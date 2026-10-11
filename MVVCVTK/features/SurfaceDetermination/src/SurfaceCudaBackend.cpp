#include "SurfaceCudaTypes.h"
#include "SurfaceCudaPtx.h"
#include <cuda.h>
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <chrono>

#ifndef MVVCVTK_SURFACE_CUDA_SLAB_REUSE
#define MVVCVTK_SURFACE_CUDA_SLAB_REUSE 0
#endif

namespace {
std::size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                sizeof(memory)) ? memory.PrivateUsage : 0;
}
struct Slot final {
    CUdeviceptr points = 0, samples = 0;
    void *host = nullptr;
    CUstream stream = nullptr;
    CUevent done = nullptr;
    std::array<CUevent,4> profileEvents{};
    bool profileRecorded = false;
    bool prepared = false, pending = false;
    std::uint64_t generation = 0;
    std::size_t count = 0;
    double *Indices() const { return static_cast<double *>(host); }
    SurfaceCudaSample *Samples() const {
        return reinterpret_cast<SurfaceCudaSample *>(Indices() + surfaceCudaBatchCapacity * 3);
    }
};
struct Session final {
    SurfaceCudaVolume source{};
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUfunction function = nullptr;
    CUdeviceptr values = 0, mask = 0;
    Slot slots[surfaceCudaSlotCount];
    std::size_t planeCount = 0, planeBytes = 0, sliceCapacity = 0;
    int cachedFirst = 0, cachedCount = 0;
    bool createdCurrent = false, failed = false;
    bool reuseSlab = false;
    SurfaceCudaProfile profile;
    bool profileInitialized = false;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    unsigned testFault = 0;
#endif
    DWORD ownerThread = GetCurrentThreadId();
    ~Session() {
        if (!context) return;
        CUcontext popped = nullptr;
        if (createdCurrent) cuCtxPopCurrent(&popped);
        const bool current = cuCtxPushCurrent(context) == CUDA_SUCCESS;
        if (current) {
            // 先结束全部异步访问，再释放页锁定区、设备区和 context。
            bool drained = cuCtxSynchronize() == CUDA_SUCCESS;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
            if (testFault == 3) drained = false;
#endif
            if (!drained) {
                // 同步失败时先销毁 context，停止设备访问，之后才允许释放锁页区。
                cuCtxPopCurrent(&popped);
                const auto destroyed = cuCtxDestroy(context);
                if (destroyed == CUDA_SUCCESS)
                    for (auto &slot : slots) if (slot.host) cuMemFreeHost(slot.host);
                // 严重驱动错误且无法确认销毁时保留有界锁页区，避免潜在 DMA 悬空访问。
                return;
            }
            for (auto &slot : slots) {
                for (const auto event : slot.profileEvents) if (event) cuEventDestroy(event);
                if (slot.done) cuEventDestroy(slot.done);
                if (slot.stream) cuStreamDestroy(slot.stream);
                if (slot.samples) cuMemFree(slot.samples);
                if (slot.points) cuMemFree(slot.points);
                if (slot.host) cuMemFreeHost(slot.host);
            }
            if (mask) cuMemFree(mask);
            if (values) cuMemFree(values);
            if (module) cuModuleUnload(module);
            cuCtxPopCurrent(&popped);
        }
        const auto destroyed = cuCtxDestroy(context);
        if (!current && destroyed == CUDA_SUCCESS)
            for (auto &slot : slots) if (slot.host) cuMemFreeHost(slot.host);
    }
};
struct CurrentContext final {
    bool active = false;
    explicit CurrentContext(Session &s) : active(cuCtxPushCurrent(s.context) == CUDA_SUCCESS) {}
    ~CurrentContext() { if (active) { CUcontext popped = nullptr; cuCtxPopCurrent(&popped); } }
};
bool GetOwned(const Session &s) { return s.ownerThread == GetCurrentThreadId(); }

using ProfileClock = std::chrono::steady_clock;
double ProfileElapsed(const ProfileClock::time_point start) {
    return std::chrono::duration<double,std::milli>(ProfileClock::now()-start).count();
}
void InitializeProfile(Session &s) {
    if (!s.profile.requested || s.profileInitialized) return;
    s.profileInitialized = true;
    const auto started = ProfileClock::now();
    bool available = true;
    // Start 的计算资源准入已完成；首次 Submit 才尝试固定的八个可选事件。
    for (auto &slot : s.slots) for (auto &event : slot.profileEvents) {
        if (available && cuEventCreate(&event,CU_EVENT_DEFAULT) != CUDA_SUCCESS) available = false;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
        if (s.testFault == 4) { s.testFault=0; available=false; }
#endif
    }
    if (!available) {
        for (auto &slot : s.slots) for (auto &event : slot.profileEvents) {
            if (event) cuEventDestroy(event);
            event = nullptr;
        }
        s.profile.failed = 1;
    }
    s.profile.eventsAvailable = available ? 1 : 0;
    s.profile.eventSetupMs += ProfileElapsed(started);
}
void RecordProfile(Session &s, Slot &slot, const unsigned stage) {
    if (!slot.profileRecorded) return;
    bool recorded = true;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    if (s.testFault == 5 && stage == 1) { s.testFault=0; recorded=false; }
#endif
    if (!recorded || cuEventRecord(slot.profileEvents[stage],slot.stream) != CUDA_SUCCESS) {
        slot.profileRecorded = false;
        s.profile.failed = 1;
    }
}
void CollectProfile(Session &s, const Slot &slot) {
    if (!slot.profileRecorded || s.profile.failed) return;
    // 必需的 done 已同步，四个更早记录的同 stream 事件也已完成；失败只影响诊断。
    float upload=0, kernel=0, download=0;
    bool readable = true;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    if (s.testFault == 6) { s.testFault=0; readable=false; }
#endif
    if (!readable || cuEventElapsedTime(&upload,slot.profileEvents[0],slot.profileEvents[1]) != CUDA_SUCCESS ||
        cuEventElapsedTime(&kernel,slot.profileEvents[1],slot.profileEvents[2]) != CUDA_SUCCESS ||
        cuEventElapsedTime(&download,slot.profileEvents[2],slot.profileEvents[3]) != CUDA_SUCCESS ||
        !std::isfinite(upload) || !std::isfinite(kernel) || !std::isfinite(download) ||
        upload<0 || kernel<0 || download<0) {
        s.profile.failed = 1;
        return;
    }
    ++s.profile.measuredBatchCount;
    s.profile.measuredPointCount += slot.count;
    s.profile.pointUploadMs += upload;
    s.profile.kernelMs += kernel;
    s.profile.sampleDownloadMs += download;
}

bool GetPrefix(const Session &s, const double *points, std::size_t requested,
               std::size_t &accepted, int &first, int &last) {
    accepted = 0;
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (std::size_t i = 0; i < requested; ++i) {
        const auto z = points[3 * i + 2];
        if (!std::isfinite(z)) break;
        const auto nextLow = std::min(low, z), nextHigh = std::max(high, z);
        const double a = std::max(double(s.source.extent[4]), std::floor(nextLow) - 2);
        const double b = std::min(double(s.source.extent[5]), std::ceil(nextHigh) + 2);
        if (a > b || a > s.source.extent[5] || b < s.source.extent[4] ||
            b - a + 1 > double(s.sliceCapacity)) break;
        low = nextLow; high = nextHigh; first = int(a); last = int(b); ++accepted;
    }
    return accepted != 0;
}
bool MoveSlabOverlap(Session &s, const std::size_t from, const std::size_t to, const std::size_t count) {
    if (from==to || !count) return true;
    const bool forward=to<from;
    const auto gap=forward?from-to:to-from;
    // 各 DMA 区间不重叠；同一 stream 按方向串行搬移，实现原分配内的 memmove。
    for (auto remaining=count;remaining;) {
        const auto chunk=std::min(remaining,gap);
        const auto begin=forward?count-remaining:remaining-chunk;
        if (cuMemcpyDtoDAsync(s.values+(to+begin)*s.planeBytes,s.values+(from+begin)*s.planeBytes,
                             chunk*s.planeBytes,s.slots[0].stream)!=CUDA_SUCCESS ||
            (s.mask && cuMemcpyDtoDAsync(s.mask+(to+begin)*s.planeCount,s.mask+(from+begin)*s.planeCount,
                                        chunk*s.planeCount,s.slots[0].stream)!=CUDA_SUCCESS)) return false;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
        if (s.testFault==7) {s.testFault=0;return false;}
#endif
        remaining-=chunk;
    }
    // non-blocking stream 与同步 HtoD 之间不依赖隐式 default-stream 屏障。
    return cuStreamSynchronize(s.slots[0].stream)==CUDA_SUCCESS;
}
bool SetSlab(Session &s, int first, int last) {
    if (s.cachedCount && first >= s.cachedFirst && std::int64_t(last)-s.cachedFirst < s.cachedCount) return true;
    // 两个 stream 共用体素 slab；所有旧读者完成后才能覆写。
    const auto waitStarted = s.profile.requested ? ProfileClock::now() : ProfileClock::time_point{};
    for (const auto &slot : s.slots)
        if (slot.pending && cuEventSynchronize(slot.done) != CUDA_SUCCESS) return false;
    if (s.profile.requested) s.profile.slabWaitMs += ProfileElapsed(waitStarted);
    const auto oldFirst=std::int64_t(s.cachedFirst),oldEnd=oldFirst+s.cachedCount;
    const auto count=int(std::min(s.sliceCapacity,std::size_t(std::int64_t(s.source.extent[5])-first+1)));
    const auto overlapFirst=s.reuseSlab?std::max(oldFirst,std::int64_t(first)):std::int64_t(first);
    const auto overlapEnd=s.reuseSlab?std::min(oldEnd,std::int64_t(first)+count):overlapFirst;
    const auto overlapCount=std::size_t(std::max<std::int64_t>(0,overlapEnd-overlapFirst));
    const auto overlapOffset=overlapCount?std::size_t(overlapFirst-first):0;
    s.cachedCount = 0;
    if (overlapCount && !MoveSlabOverlap(s,std::size_t(overlapFirst-oldFirst),overlapOffset,overlapCount)) return false;
    // 轻量 host 传输计数也供成本选择读取，不申请 CUDA timing events。
    const auto uploadStarted = ProfileClock::now();
    const auto upload=[&](std::size_t begin,std::size_t planes) {
        if (!planes) return true;
        const auto offset=std::size_t(std::int64_t(first)-s.source.extent[4])+begin;
        return cuMemcpyHtoD(s.values+begin*s.planeBytes,
            static_cast<const unsigned char *>(s.source.values)+offset*s.planeBytes,planes*s.planeBytes)==CUDA_SUCCESS &&
            (!s.mask || cuMemcpyHtoD(s.mask+begin*s.planeCount,s.source.validity+offset*s.planeCount,
                                    planes*s.planeCount)==CUDA_SUCCESS);
    };
    const auto suffix=overlapOffset+overlapCount;
    if (!upload(0,overlapOffset) || !upload(suffix,std::size_t(count)-suffix)) return false;
    s.profile.slabUploadHostMs += ProfileElapsed(uploadStarted);
    ++s.profile.slabUploadCount;
    s.profile.slabUploadBytes += (std::uint64_t(count)-overlapCount) *
        (s.planeBytes + (s.mask ? s.planeCount : 0));
    s.cachedFirst = first; s.cachedCount = count;
    return true;
}
} // namespace

extern "C" __declspec(dllexport) int SurfaceCudaCreate(const SurfaceCudaVolume *source,
                                                       std::size_t deviceLimit, std::size_t hostLimit,
                                                       void **handle, std::size_t *deviceBytes) {
    if (!source || !handle || !deviceBytes) return 0;
    *handle = nullptr; *deviceBytes = 0;
    const auto before = PrivateBytes();
    try {
        if (!source->values || (source->scalarBytes != 4 && source->scalarBytes != 8) || cuInit(0) != CUDA_SUCCESS) return 0;
        for (unsigned a = 0; a < 3; ++a)
            if (source->dimensions[a] < 2 ||
                std::int64_t(source->extent[2*a+1])-source->extent[2*a]+1 != source->dimensions[a]) return 0;
        CUdevice device;
        if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) return 0;
        auto s = std::make_unique<Session>(); s->source = *source;
        char profiling[8]{};
        s->profile.requested = GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_PROFILE",profiling,sizeof(profiling))==1 &&
                               profiling[0]=='1' ? 1 : 0;
        char reuse[8]{};
        s->reuseSlab = MVVCVTK_SURFACE_CUDA_SLAB_REUSE &&
            !(GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_SLAB_REUSE",reuse,sizeof(reuse))==1 && reuse[0]=='0');
#if CUDA_VERSION >= 13000
        if (cuCtxCreate(&s->context, nullptr, 0, device) != CUDA_SUCCESS) return 0;
#else
        if (cuCtxCreate(&s->context, 0, device) != CUDA_SUCCESS) return 0;
#endif
        s->createdCurrent = true;
        if (cuModuleLoadData(&s->module, surfaceCudaPtx) != CUDA_SUCCESS ||
            cuModuleGetFunction(&s->function, s->module, "SurfaceSamplePoints") != CUDA_SUCCESS) return 0;
        constexpr auto fixed = surfaceCudaHostBufferBytes;
        const auto nx = std::size_t(source->dimensions[0]), ny = std::size_t(source->dimensions[1]);
        if (nx > SIZE_MAX / ny || nx*ny > SIZE_MAX / std::size_t(source->scalarBytes)) return 0;
        s->planeCount = nx*ny; s->planeBytes = s->planeCount * source->scalarBytes;
        if (deviceLimit <= fixed || s->planeBytes > SIZE_MAX - (source->validity ? s->planeCount : 0)) return 0;
        const auto perSlice = s->planeBytes + (source->validity ? s->planeCount : 0);
        s->sliceCapacity = std::min(std::size_t(source->dimensions[2]), (deviceLimit-fixed)/perSlice);
        if (s->sliceCapacity < 4) return 0;
        if (cuMemAlloc(&s->values, s->planeBytes*s->sliceCapacity) != CUDA_SUCCESS ||
            (source->validity && cuMemAlloc(&s->mask,s->planeCount*s->sliceCapacity) != CUDA_SUCCESS)) return 0;
        for (auto &slot : s->slots) {
            if (cuMemHostAlloc(&slot.host, surfaceCudaHostBufferBytes/surfaceCudaSlotCount,
                               CU_MEMHOSTALLOC_PORTABLE) != CUDA_SUCCESS ||
                cuMemAlloc(&slot.points,surfaceCudaBatchCapacity*3*sizeof(double)) != CUDA_SUCCESS ||
                cuMemAlloc(&slot.samples,surfaceCudaBatchCapacity*sizeof(SurfaceCudaSample)) != CUDA_SUCCESS ||
                cuStreamCreate(&slot.stream,CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS ||
                cuEventCreate(&slot.done,CU_EVENT_DISABLE_TIMING|CU_EVENT_BLOCKING_SYNC) != CUDA_SUCCESS) return 0;
        }
        const auto after = PrivateBytes();
        if (!before || !after || (after > before && after-before > hostLimit)) return 0;
        *deviceBytes = fixed + perSlice*s->sliceCapacity;
        CUcontext popped = nullptr;
        if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS) return 0;
        s->createdCurrent = false; *handle = s.release(); return 1;
    } catch (...) { return 0; }
}
extern "C" __declspec(dllexport) int SurfaceCudaPrepare(void *handle, unsigned index, double **points) {
    if (!handle || !points || index >= surfaceCudaSlotCount) return 0;
    auto &s = *static_cast<Session *>(handle); auto &slot = s.slots[index];
    if (!GetOwned(s) || s.failed || slot.pending) return 0;
    slot.prepared = true; *points = slot.Indices(); return 1;
}
extern "C" __declspec(dllexport) int SurfaceCudaSubmit(void *handle, unsigned index,
                                                       std::size_t count, SurfaceCudaTicket *ticket) {
    if (!handle || !ticket || index >= surfaceCudaSlotCount || !count || count > surfaceCudaBatchCapacity) return 0;
    auto &s = *static_cast<Session *>(handle); auto &slot = s.slots[index];
    if (!GetOwned(s) || s.failed || slot.pending || !slot.prepared) return 0;
    CurrentContext current(s); if (!current.active) return 0;
    InitializeProfile(s);
    std::size_t accepted = 0; int first = 0, last = 0;
    if (!GetPrefix(s,slot.Indices(),count,accepted,first,last)) return 0;
    if (!SetSlab(s,first,last)) { s.failed = true; return 0; }
    auto v = s.source; v.firstSlice = s.cachedFirst; v.sliceCount = s.cachedCount;
    v.values = reinterpret_cast<const void *>(s.values);
    v.validity = s.mask ? reinterpret_cast<const unsigned char *>(s.mask) : nullptr;
    auto pointAddress = slot.points, sampleAddress = slot.samples;
    void *arguments[]{&v,&pointAddress,&accepted,&sampleAddress};
    // 先标 pending，使任何部分入队失败仍由 Reset 的 context drain 覆盖。
    slot.pending = true; slot.prepared = false; slot.count = accepted; ++slot.generation;
    slot.profileRecorded = s.profile.eventsAvailable && !s.profile.failed;
    RecordProfile(s,slot,0);
    if (cuMemcpyHtoDAsync(slot.points,slot.Indices(),accepted*3*sizeof(double),slot.stream) != CUDA_SUCCESS) {
        s.failed = true; return 0;
    }
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    if (s.testFault == 1) {s.testFault=0;s.failed=true;return 0;}
#endif
    RecordProfile(s,slot,1);
    if (cuLaunchKernel(s.function,unsigned((accepted+127)/128),1,1,128,1,1,0,slot.stream,arguments,nullptr) != CUDA_SUCCESS) {
        s.failed = true; return 0;
    }
    RecordProfile(s,slot,2);
    if (cuMemcpyDtoHAsync(slot.Samples(),slot.samples,accepted*sizeof(SurfaceCudaSample),slot.stream) != CUDA_SUCCESS) {
        s.failed = true; return 0;
    }
    RecordProfile(s,slot,3);
    if (cuEventRecord(slot.done,slot.stream) != CUDA_SUCCESS) { s.failed = true; return 0; }
    if (s.profile.requested) ++s.profile.submittedBatchCount;
    *ticket = {}; ticket->slot=index; ticket->generation=slot.generation; ticket->count=accepted;
    return 1;
}
extern "C" __declspec(dllexport) int SurfaceCudaCollect(void *handle, const SurfaceCudaTicket *ticket,
                                                        const SurfaceCudaSample **samples) {
    if (!handle || !ticket || !samples || ticket->slot >= surfaceCudaSlotCount) return 0;
    auto &s = *static_cast<Session *>(handle); auto &slot = s.slots[ticket->slot];
    if (!GetOwned(s) || !slot.pending || slot.generation!=ticket->generation || slot.count!=ticket->count) return 0;
    CurrentContext current(s); if (!current.active) return 0;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    if (s.testFault == 2) {s.testFault=0;s.failed=true;return 0;}
#endif
    if (cuEventSynchronize(slot.done) != CUDA_SUCCESS) { s.failed = true; return 0; }
    CollectProfile(s,slot);
    slot.pending=false; *samples=slot.Samples(); return 1;
}
extern "C" __declspec(dllexport) int SurfaceCudaGetProfile(void *handle, SurfaceCudaProfile *profile) {
    if (!handle || !profile) return 0;
    const auto &s = *static_cast<const Session *>(handle);
    if (!GetOwned(s)) return 0;
    *profile = s.profile;
    const bool pending = std::any_of(std::begin(s.slots),std::end(s.slots),[](const auto &slot){return slot.pending;});
    profile->complete = profile->eventsAvailable && !profile->failed && !s.failed && !pending &&
        profile->submittedBatchCount && profile->submittedBatchCount==profile->measuredBatchCount ? 1 : 0;
    return 1;
}
extern "C" __declspec(dllexport) int SurfaceCudaReadPoints(void *handle, const double *points,
                                                           std::size_t count, SurfaceCudaSample *output) {
    if (!points || !output || !count || count>surfaceCudaBatchCapacity) return 0;
    double *staging=nullptr;
    if (!SurfaceCudaPrepare(handle,0,&staging)) return 0;
    std::copy_n(points,count*3,staging);
    SurfaceCudaTicket ticket;
    if (!SurfaceCudaSubmit(handle,0,count,&ticket)) return 0;
    const SurfaceCudaSample *samples=nullptr;
    if (!SurfaceCudaCollect(handle,&ticket,&samples) || ticket.count!=count) return 0;
    std::copy_n(samples,count,output); return 1;
}
extern "C" __declspec(dllexport) void SurfaceCudaDestroy(void *handle) noexcept {
    delete static_cast<Session *>(handle);
}
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
extern "C" __declspec(dllexport) int SurfaceCudaTestSetFault(void *handle,unsigned fault) {
    if (!handle || fault>7) return 0;
    auto &s=*static_cast<Session *>(handle);
    if (!GetOwned(s)) return 0;
    s.testFault=fault;return 1;
}
#endif
