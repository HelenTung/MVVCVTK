// Surface 各计算阶段共享同一核心规划、配额与资源不足时的串行接管。
#pragma once
#include "Data/DataPayloads.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <xmmintrin.h>
namespace SurfaceCpu {
struct SurfaceCancelled final {};
using SurfaceCancelCheck = std::function<bool()>;
struct CpuSamplingPlan final {
    unsigned physicalCores = 0;
    unsigned logicalThreads = 0;
    unsigned threadLimit = 1;
};

inline CpuSamplingPlan GetCpuSamplingPlan()
{
    CpuSamplingPlan plan;
    GROUP_AFFINITY affinity{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &affinity) || !affinity.Mask)
        return plan;
    // 只在调用线程的可用组内保守规划，不扩展进程/线程亲和性。
    USHORT groupCount = 1, group = 0;
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessGroupAffinity(GetCurrentProcess(), &groupCount, &group) && groupCount == 1 &&
        group == affinity.Group && GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        affinity.Mask &= processMask;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return plan;
    try {
        DataArray<std::uint64_t> storage((std::size_t(bytes) + 7) / 8);
        auto *data = reinterpret_cast<unsigned char *>(storage.data());
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data), &bytes))
            return plan;
        std::size_t offset = 0;
        while (offset < bytes) {
            constexpr auto header = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
            constexpr auto masks = header + offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
            if (bytes - offset < masks) return CpuSamplingPlan{};
            const auto *info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(data + offset);
            if (info->Size < masks || info->Size > bytes - offset ||
                info->Processor.GroupCount > (info->Size - masks) / sizeof(GROUP_AFFINITY))
                return CpuSamplingPlan{};
            KAFFINITY allowed = 0;
            for (unsigned g = 0; g < info->Processor.GroupCount; ++g) {
                const auto &mask = info->Processor.GroupMask[g];
                if (mask.Group == affinity.Group) allowed |= mask.Mask & affinity.Mask;
            }
            if (allowed) {
                ++plan.physicalCores;
                while (allowed) { allowed &= allowed - 1; ++plan.logicalThreads; }
            }
            offset += info->Size;
        }
    } catch (const std::bad_alloc &) {
        return CpuSamplingPlan{};
    }
    // 保留至少一半物理核心和逻辑线程；查询失败或只有一个核心时不新增线程。
    plan.threadLimit = std::max(1U, std::min(plan.physicalCores / 2, plan.logicalThreads / 2));
    char setting[8]{};
    // 新线程使用默认浮点控制；调用方采用其他舍入/非规格化处理时保留原串行计算。
    if (!MVVCVTK_SURFACE_CPU_PARALLEL || std::fegetround() != FE_TONEAREST ||
        (_mm_getcsr() & 0xffc0U) != 0x1f80U ||
        (GetEnvironmentVariableA("MVVCVTK_SURFACE_CPU_PARALLEL", setting, sizeof(setting)) == 1 && setting[0] == '0'))
        plan.threadLimit = 1;
    return plan;
}

struct CpuSamplingQuota final {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned activeThreads = 0;
};

class CpuSamplingLease final {
public:
    CpuSamplingLease(unsigned requested, unsigned limit, const SurfaceCancelCheck &cancelled)
    {
        std::unique_lock<std::mutex> lock(m_quota.mutex);
        while (m_quota.activeThreads >= limit) {
            if (cancelled && cancelled()) throw SurfaceCancelled{};
            m_quota.changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        m_threads = std::min(requested, limit - m_quota.activeThreads);
        m_quota.activeThreads += m_threads;
    }
    ~CpuSamplingLease()
    {
        {
            std::lock_guard<std::mutex> lock(m_quota.mutex);
            m_quota.activeThreads -= m_threads;
        }
        m_quota.changed.notify_all();
    }
    unsigned GetThreadCount() const noexcept { return m_threads; }
    CpuSamplingLease(const CpuSamplingLease &) = delete;
    CpuSamplingLease &operator=(const CpuSamplingLease &) = delete;
private:
    static CpuSamplingQuota &GetQuota()
    {
        static CpuSamplingQuota quota;
        return quota;
    }
    // 协调 Surface 直方图、点采样和组件阶段，多个请求共享同一核心配额。
    CpuSamplingQuota &m_quota = GetQuota();
    unsigned m_threads = 0;
};

template <class RangeCallback>
unsigned RunPointSampling(const std::size_t pointCount, const unsigned threadLimit,
                          const SurfaceCancelCheck &cancelled, const RangeCallback &sample)
{
    // 每个线程至少负责 8192 个点，避免小组件反复创建线程。
    constexpr std::size_t minimumPointsPerThread = 8192;
    const auto requested = unsigned(std::min<std::size_t>(threadLimit,
                                                        std::max<std::size_t>(1, pointCount / minimumPointsPerThread)));
    CpuSamplingLease lease(requested, threadLimit, cancelled);
    const auto count = lease.GetThreadCount();
    if (count == 1) {
        sample(0, pointCount);
        return 1;
    }
    // 单组 KAFFINITY 的位数只限定存储容量，不固定实际并发数。
    constexpr auto groupCapacity = sizeof(KAFFINITY) * CHAR_BIT / 2;
    std::array<std::thread, groupCapacity - 1> workers;
    std::array<std::exception_ptr, groupCapacity> failures{};
    const auto run = [&](unsigned worker) noexcept {
        try {
            const auto width = pointCount / count, extra = pointCount % count;
            const auto begin = worker * width + std::min<std::size_t>(worker, extra);
            sample(begin, begin + width + (worker < extra ? 1 : 0));
        } catch (...) {
            failures[worker] = std::current_exception();
        }
    };
    unsigned started = 1;
    for (; started < count; ++started) {
        try {
            workers[started - 1] = std::thread(run, started);
        } catch (const std::system_error &) {
            break;
        } catch (const std::bad_alloc &) {
            break;
        }
    }
    run(0);
    // 创建资源不足时，由调用线程完成未派出的区间；已派出的区间仍只写一次。
    for (unsigned worker = started; worker < count; ++worker)
        run(worker);
    for (unsigned worker = 1; worker < started; ++worker)
        workers[worker - 1].join();
    for (const auto &failure : failures)
        if (failure) std::rethrow_exception(failure);
    return started;
}


}
