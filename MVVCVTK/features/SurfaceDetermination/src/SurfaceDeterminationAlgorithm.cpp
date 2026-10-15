#include "FeatureSupport/WorkLimit.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceContracts.h"
#include "SurfaceSeedBuilder.h"
#include "SurfaceEdgeTopology.h"
#include "SurfaceCudaCompute.h"
#include "SurfaceGpuFinalizer.h"
#include "SurfaceBackendCost.h"
#include <chrono>
#include "Data/DataPayloads.h"
#include "MappedArrayStorage.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkType.h>
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cfenv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <new>
#include <optional>
#include <string>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xmmintrin.h>

namespace {

using Point3 = std::array<double, 3>;

constexpr std::uint32_t algorithmRevision = surfaceAlgorithmRevision;
constexpr std::size_t histogramBinCount = 512;
constexpr double geometryEpsilon = 1.0e-12;
constexpr double qualityRatioThreshold = 0.5;

using Triangle = SurfaceSeedTriangle;
struct SurfaceCancelled final
{
};
void CheckCancellation(std::size_t &count, const SurfaceCancelCheck &cancelled)
{
    if ((count++ & 1023U) == 0 && cancelled && cancelled())
        throw SurfaceCancelled{};
}

struct CpuSamplingPlan final {
    unsigned physicalCores = 0;
    unsigned logicalThreads = 0;
    unsigned threadLimit = 1;
};

CpuSamplingPlan GetCpuSamplingPlan()
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
    // 仅协调 Surface 点采样，多个请求不各自取满同一核心预算。
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


// 新 worker 的默认栈来自当前宿主 PE，不把固定线程栈大小当作所有调用方的事实。
std::optional<std::size_t> GetComponentWorkerStackReserve() {
    MODULEINFO module{};
    const auto handle=GetModuleHandleW(nullptr);
    if (!handle || !GetModuleInformation(GetCurrentProcess(),handle,&module,sizeof(module)) ||
        module.SizeOfImage<std::max(sizeof(IMAGE_DOS_HEADER),sizeof(IMAGE_NT_HEADERS64))) return {};
    const auto *base=static_cast<const unsigned char*>(module.lpBaseOfDll);
    const auto *dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 ||
        std::size_t(dos->e_lfanew)>module.SizeOfImage-sizeof(IMAGE_NT_HEADERS64)) return {};
    const auto *header=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (header->Signature!=IMAGE_NT_SIGNATURE || header->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        !header->OptionalHeader.SizeOfStackReserve) return {};
    return std::size_t(header->OptionalHeader.SizeOfStackReserve);
}

template<class ComponentCallback>
unsigned RunComponentJobs(std::size_t begin, std::size_t end, unsigned count,
                          const SurfaceCancelCheck &cancelled, const ComponentCallback &process) {
    constexpr auto capacity=sizeof(KAFFINITY)*CHAR_BIT/2;
    std::array<std::thread,capacity-1> workers;
    std::array<std::exception_ptr,capacity> failures{};
    std::atomic<std::size_t> next{begin};
    std::atomic<bool> stopped{false};
    const auto run=[&](unsigned worker) noexcept {
        try {
            while (!stopped.load(std::memory_order_acquire)) {
                if (cancelled && cancelled()) throw SurfaceCancelled{};
                // 动态领取小批次，避免海量组件产生一项任务一次原子操作。
                const auto first=next.fetch_add(16,std::memory_order_relaxed);
                if (first>=end) break;
                const auto last=std::min(end,first+16);
                for (auto position=first;position<last;++position) {
                    if (stopped.load(std::memory_order_acquire)) return;
                    process(position,worker);
                }
            }
        } catch (...) {
            failures[worker]=std::current_exception();
            stopped.store(true,std::memory_order_release);
        }
    };
    unsigned started=1;
    for (;started<count;++started) {
        try { workers[started-1]=std::thread(run,started); }
        catch (const std::system_error &) { break; }
        catch (const std::bad_alloc &) { break; }
    }
    run(0);
    for (unsigned worker=1;worker<started;++worker) workers[worker-1].join();
    for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);
    return started;
}

struct ImageGeometry final {
    std::array<int, 6> extent{};
    std::array<int, 3> dimensions{};
    std::array<double, 3> spacing{};
    std::array<double, 3> origin{};
    std::array<double, 9> direction{};
    std::array<double, 9> indexToModel{};
    std::array<double, 9> modelToIndex{};
    double voxelVolume = 0.0;
};

template <class T> double ReadScalar(const void *values, const std::size_t index)
{
    return static_cast<double>(static_cast<const T *>(values)[index]);
}

struct ScalarView final {
    const void* values = nullptr;
    std::size_t valueCount = 0;
    int vtkType = VTK_VOID;
    double (*read)(const void *, std::size_t) = nullptr;
    std::optional<std::array<double, 2>> materialRange;
    double GetValue(std::size_t index) const noexcept
    {
        const auto value = read(values, index);
        // 非有限原始节点仍属缺失支持，不得饱和成有效材料。
        return materialRange && std::isfinite(value)
                   ? std::clamp(value, (*materialRange)[0], (*materialRange)[1]) : value;
    }
    void SetReader()
    {
        switch (vtkType)
        {
        case VTK_CHAR:
            read = ReadScalar<char>;
            break;
        case VTK_SIGNED_CHAR:
            read = ReadScalar<signed char>;
            break;
        case VTK_UNSIGNED_CHAR:
            read = ReadScalar<unsigned char>;
            break;
        case VTK_SHORT:
            read = ReadScalar<short>;
            break;
        case VTK_UNSIGNED_SHORT:
            read = ReadScalar<unsigned short>;
            break;
        case VTK_INT:
            read = ReadScalar<int>;
            break;
        case VTK_UNSIGNED_INT:
            read = ReadScalar<unsigned int>;
            break;
        case VTK_LONG:
            read = ReadScalar<long>;
            break;
        case VTK_UNSIGNED_LONG:
            read = ReadScalar<unsigned long>;
            break;
        case VTK_LONG_LONG:
            read = ReadScalar<long long>;
            break;
        case VTK_UNSIGNED_LONG_LONG:
            read = ReadScalar<unsigned long long>;
            break;
        case VTK_FLOAT:
            read = ReadScalar<float>;
            break;
        case VTK_DOUBLE:
            read = ReadScalar<double>;
            break;
        }
    }
};

struct VolumeView final {
    std::array<double, 2> sourceRange{};
    ImageGeometry geometry;
    ScalarView scalars;
    const unsigned char* validity = nullptr;
    std::size_t voxelCount = 0;
};

struct ResolvedParams final
{
    SurfaceComponentSelection componentSelection = SurfaceComponentSelection::Largest;
    SurfaceDeterminationMethod method = SurfaceDeterminationMethod::GlobalAutomatic;
    double initialIsoValue = 0.0;
    double boundaryToleranceModel = 0.0, haloModel = 0.0;
    std::optional<SurfaceIsoEstimate> isoEstimate;
    std::optional<Point3> seedModelPoint;
    RoiReadSnapshot roi;
    std::vector<RoiPlane> clipPlanes;
    std::uint64_t minimumObjectVoxels = 1;
    double sharpCornerAngleDeg = 75.0;
};

enum class SampleStatus : std::uint8_t {
    Valid,
    Clipped,
    InvalidSupport
};

struct ScalarSample final {
    SampleStatus status = SampleStatus::Clipped;
    double value = 0.0;
};

struct TopologyMetrics final {
    double signedVolume = 0.0;
    std::uint64_t boundaryEdgeCount = 0;
    std::uint64_t nonManifoldEdgeCount = 0;
    std::uint64_t degenerateTriangleCount = 0;
    bool isOrientationValid = true;
};


// C++17 私有可写区间；最终 vector 已定长，worker 不改变容器结构。
struct SurfaceRecordRange final {
    SurfacePointRecord *data = nullptr;
    std::size_t count = 0;
    SurfacePointRecord &operator[](std::size_t index) const { return data[index]; }
    SurfacePointRecord *begin() const { return data; }
    SurfacePointRecord *end() const { return data+count; }
    std::size_t size() const { return count; }
};
struct ComponentTotals final {
    std::uint64_t accepted=0, rejected=0, lowContrast=0, truncated=0;
    std::uint32_t nonManifold=0;
    unsigned cpuThreads=1;
    double preparationMs=0, samplingMs=0, outputMs=0, cpuSamplingMs=0;
};
void MergeComponentTotals(SurfaceAlgorithmResult &result, const ComponentTotals &totals) {
    result.acceptedPointCount+=totals.accepted;
    result.rejectedPointCount+=totals.rejected;
    result.lowContrastPointCount+=totals.lowContrast;
    result.truncatedPointCount+=totals.truncated;
    result.nonManifoldObjectCount+=totals.nonManifold;
    result.cpuThreadCount=std::max(result.cpuThreadCount,totals.cpuThreads);
    result.meshPreparationMs+=totals.preparationMs;
    result.pointSamplingMs+=totals.samplingMs;
    result.outputMs+=totals.outputMs;
    result.cpuSamplingMs+=totals.cpuSamplingMs;
}

struct MeshComponent final {
    DataArray<std::uint32_t> sourcePointIds;
    DataArray<Triangle> triangles;
    std::uint32_t minimumPointId = 0;
    std::uint64_t estimatedVoxelCount = 0;
    bool isClosed = false;
    TopologyMetrics topology;
};

bool GetCancelled(const SurfaceCancelCheck& getCancelled)
{
    return getCancelled && getCancelled();
}

void SendProgress(
    const SurfaceProgressCallback& onProgress,
    const SurfaceDeterminationStage stage,
    const double progress)
{
    if (onProgress) onProgress(stage, std::clamp(progress, 0.0, 1.0));
}

bool GetProduct(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product)
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

bool GetSum(
    const std::size_t left,
    const std::size_t right,
    std::size_t& sum)
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

Point3 Add(const Point3& left, const Point3& right)
{
    return {
        left[0] + right[0],
        left[1] + right[1],
        left[2] + right[2]
    };
}

Point3 Subtract(const Point3& left, const Point3& right)
{
    return {
        left[0] - right[0],
        left[1] - right[1],
        left[2] - right[2]
    };
}

Point3 Scale(const Point3& point, const double scale)
{
    return { point[0] * scale, point[1] * scale, point[2] * scale };
}

double Dot(const Point3& left, const Point3& right)
{
    return left[0] * right[0]
        + left[1] * right[1]
        + left[2] * right[2];
}

Point3 Cross(const Point3& left, const Point3& right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]
    };
}

double GetLength(const Point3& point)
{
    return std::sqrt(std::max(0.0, Dot(point, point)));
}

bool Normalize(Point3& point)
{
    const double length = GetLength(point);
    if (!std::isfinite(length) || length <= geometryEpsilon) return false;
    point = Scale(point, 1.0 / length);
    return true;
}

Point3 Multiply(
    const std::array<double, 9>& matrix,
    const Point3& point)
{
    return {
        matrix[0] * point[0] + matrix[1] * point[1]
            + matrix[2] * point[2],
        matrix[3] * point[0] + matrix[4] * point[1]
            + matrix[5] * point[2],
        matrix[6] * point[0] + matrix[7] * point[1]
            + matrix[8] * point[2]
    };
}

bool GetInverse(
    const std::array<double, 9>& matrix,
    std::array<double, 9>& inverse,
    double& determinant)
{
    determinant = matrix[0] * (matrix[4] * matrix[8]
        - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8]
            - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7]
            - matrix[4] * matrix[6]);
    if (!std::isfinite(determinant)
        || std::abs(determinant) <= geometryEpsilon) {
        return false;
    }
    const double scale = 1.0 / determinant;
    inverse = {
        (matrix[4] * matrix[8] - matrix[5] * matrix[7]) * scale,
        (matrix[2] * matrix[7] - matrix[1] * matrix[8]) * scale,
        (matrix[1] * matrix[5] - matrix[2] * matrix[4]) * scale,
        (matrix[5] * matrix[6] - matrix[3] * matrix[8]) * scale,
        (matrix[0] * matrix[8] - matrix[2] * matrix[6]) * scale,
        (matrix[2] * matrix[3] - matrix[0] * matrix[5]) * scale,
        (matrix[3] * matrix[7] - matrix[4] * matrix[6]) * scale,
        (matrix[1] * matrix[6] - matrix[0] * matrix[7]) * scale,
        (matrix[0] * matrix[4] - matrix[1] * matrix[3]) * scale
    };
    return true;
}

Point3 GetModelPoint(
    const ImageGeometry& geometry,
    const Point3& continuousIndex)
{
    return Add(
        geometry.origin,
        Multiply(geometry.indexToModel, continuousIndex));
}

Point3 GetContinuousIndex(
    const ImageGeometry& geometry,
    const Point3& modelPoint)
{
    return Multiply(
        geometry.modelToIndex,
        Subtract(modelPoint, geometry.origin));
}

bool GetScalarSupported(const int vtkType)
{
    switch (vtkType) {
    case VTK_CHAR:
    case VTK_SIGNED_CHAR:
    case VTK_UNSIGNED_CHAR:
    case VTK_SHORT:
    case VTK_UNSIGNED_SHORT:
    case VTK_INT:
    case VTK_UNSIGNED_INT:
    case VTK_LONG:
    case VTK_UNSIGNED_LONG:
    case VTK_LONG_LONG:
    case VTK_UNSIGNED_LONG_LONG:
    case VTK_FLOAT:
    case VTK_DOUBLE:
        return true;
    default:
        return false;
    }
}

bool GetSameGeometry(
    vtkImageData& image,
    const ImageGeometry& geometry)
{
    int extent[6]{};
    double spacing[3]{};
    double origin[3]{};
    image.GetExtent(extent);
    image.GetSpacing(spacing);
    image.GetOrigin(origin);
    for (std::size_t index = 0; index < geometry.extent.size(); ++index) {
        if (extent[index] != geometry.extent[index]) return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (spacing[axis] != geometry.spacing[axis]
            || origin[axis] != geometry.origin[axis]) {
            return false;
        }
    }
    const auto* direction = image.GetDirectionMatrix();
    if (!direction) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            if (direction->GetElement(
                    static_cast<int>(row),
                    static_cast<int>(column))
                != geometry.direction[row * 3 + column]) {
                return false;
            }
        }
    }
    return true;
}

SurfaceFailureReason BuildVolumeView(
    const VtkImageGridSnapshot& source,
    VolumeView& volume,
    std::string& message)
{
    if (!source || !source->image || !source->data
        || !GetDataRevisionRefValid(source->data->self)) {
        message = "Surface source is unavailable.";
        return SurfaceFailureReason::InvalidSource;
    }
    auto* image = source->image.GetPointer();
    const auto* payload = dynamic_cast<const ImageGrid3DPayload*>(
        source->data->payload.get());
    if (!payload) return SurfaceFailureReason::InvalidSource;
    const auto& sourceGeometry = payload->GetGeometry();
    volume.sourceRange = payload->GetScalarRange();
    int dimensions[3]{};
    int extent[6]{};
    double spacing[3]{};
    double origin[3]{};
    image->GetDimensions(dimensions);
    image->GetExtent(extent);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);
    std::size_t voxelCount = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::int64_t extentSize =
            static_cast<std::int64_t>(extent[axis * 2 + 1])
            - static_cast<std::int64_t>(extent[axis * 2]) + 1;
        if (dimensions[axis] < 2 || extentSize != dimensions[axis] ||
            sourceGeometry.dimensions[axis] != dimensions[axis] ||
            sourceGeometry.extent[axis * 2] != extent[axis * 2] ||
            sourceGeometry.extent[axis * 2 + 1] != extent[axis * 2 + 1] || !std::isfinite(spacing[axis]) ||
            spacing[axis] <= 0.0 || !std::isfinite(origin[axis]) ||
            spacing[axis] != sourceGeometry.spacing[axis] || origin[axis] != sourceGeometry.origin[axis] ||
            !GetProduct(voxelCount, static_cast<std::size_t>(dimensions[axis]), voxelCount))
        {
            message = "Surface source geometry is invalid.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        volume.geometry.dimensions[axis] = dimensions[axis];
        volume.geometry.spacing[axis] = spacing[axis];
        volume.geometry.origin[axis] = origin[axis];
        volume.geometry.extent[axis * 2] = extent[axis * 2];
        volume.geometry.extent[axis * 2 + 1] = extent[axis * 2 + 1];
    }

    const auto* direction = image->GetDirectionMatrix();
    if (!direction) {
        message = "Surface source direction is unavailable.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const double value = direction->GetElement(
                static_cast<int>(row), static_cast<int>(column));
            if (!std::isfinite(value) || value != sourceGeometry.direction[row * 3 + column])
            {
                message = "Surface source direction is invalid.";
                return SurfaceFailureReason::InvalidGeometry;
            }
            volume.geometry.direction[row * 3 + column] = value;
            volume.geometry.indexToModel[row * 3 + column] =
                value * spacing[column];
        }
    }
    double determinant = 0.0;
    if (!GetInverse(
            volume.geometry.indexToModel,
            volume.geometry.modelToIndex,
            determinant)) {
        message = "Surface source direction is not invertible.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    volume.geometry.voxelVolume = std::abs(determinant);

    auto* scalars = image->GetPointData()
        ? image->GetPointData()->GetScalars() : nullptr;
    if (!scalars
        || scalars->GetNumberOfComponents() != 1
        || !scalars->HasStandardMemoryLayout()
        || !GetScalarSupported(scalars->GetDataType())) {
        message = "Surface source requires one supported contiguous scalar.";
        return SurfaceFailureReason::UnsupportedScalar;
    }
    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    if (tupleCount <= 0
        || static_cast<unsigned long long>(tupleCount)
            > std::numeric_limits<std::size_t>::max()
        || static_cast<std::size_t>(tupleCount) != voxelCount) {
        message = "Surface source tuple count is inconsistent.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    const void* values = scalars->GetVoidPointer(0);
    if (!values) {
        message = "Surface source scalar buffer is unavailable.";
        return SurfaceFailureReason::UnsupportedScalar;
    }
    volume.scalars = { values, voxelCount, scalars->GetDataType() };
    volume.scalars.SetReader();
    volume.voxelCount = voxelCount;

    if (source->validityMask) {
        auto* mask = source->validityMask.GetPointer();
        auto* maskScalars = mask && mask->GetPointData()
            ? mask->GetPointData()->GetScalars() : nullptr;
        if (!mask || !GetSameGeometry(*mask, volume.geometry)
            || (maskScalars
                && maskScalars->GetNumberOfTuples() != tupleCount)) {
            message = "Surface validity mask is inconsistent.";
            return SurfaceFailureReason::InvalidGeometry;
        }
        if (!maskScalars
            || maskScalars->GetDataType() != VTK_UNSIGNED_CHAR
            || maskScalars->GetNumberOfComponents() != 1
            || !maskScalars->HasStandardMemoryLayout()) {
            message = "Surface validity mask requires one contiguous uint8 scalar.";
            return SurfaceFailureReason::UnsupportedScalar;
        }
        volume.validity = static_cast<const unsigned char*>(
            maskScalars->GetVoidPointer(0));
        if (!volume.validity) {
            message = "Surface validity mask buffer is unavailable.";
            return SurfaceFailureReason::InvalidGeometry;
        }
    }
    return SurfaceFailureReason::None;
}

std::array<double, 6> GetDataBounds(const ImageGeometry& geometry)
{
    std::array<double, 6> bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    for (int corner = 0; corner < 8; ++corner) {
        Point3 index{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            index[axis] = static_cast<double>(
                geometry.extent[axis * 2 + ((corner >> axis) & 1)]);
        }
        const Point3 point = GetModelPoint(geometry, index);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            bounds[axis * 2] = std::min(bounds[axis * 2], point[axis]);
            bounds[axis * 2 + 1] =
                std::max(bounds[axis * 2 + 1], point[axis]);
        }
    }
    return bounds;
}

std::size_t GetTupleIndex(
    const ImageGeometry& geometry,
    const int x,
    const int y,
    const int z)
{
    const std::size_t localX = static_cast<std::size_t>(
        x - geometry.extent[0]);
    const std::size_t localY = static_cast<std::size_t>(
        y - geometry.extent[2]);
    const std::size_t localZ = static_cast<std::size_t>(
        z - geometry.extent[4]);
    return (localZ * static_cast<std::size_t>(geometry.dimensions[1])
        + localY) * static_cast<std::size_t>(geometry.dimensions[0])
        + localX;
}

template <bool Interpolate = true>
ScalarSample GetScalarAtIndex(
    const VolumeView& volume,
    const Point3& continuousIndex)
{
    std::array<int, 3> lower{};
    Point3 fraction{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double minimum = static_cast<double>(
            volume.geometry.extent[axis * 2]);
        const double maximum = static_cast<double>(
            volume.geometry.extent[axis * 2 + 1]);
        const double value = continuousIndex[axis];
        const double tolerance = 32.0
            * std::numeric_limits<double>::epsilon()
            * std::max({ 1.0, std::abs(minimum), std::abs(maximum) });
        if (!std::isfinite(value)
            || value < minimum - tolerance
            || value > maximum + tolerance) {
            return { SampleStatus::Clipped, 0.0 };
        }
        const double bounded = std::clamp(value, minimum, maximum);
        int base = static_cast<int>(std::floor(bounded));
        if (base >= volume.geometry.extent[axis * 2 + 1]) {
            base = volume.geometry.extent[axis * 2 + 1] - 1;
        }
        if (base < volume.geometry.extent[axis * 2]) {
            base = volume.geometry.extent[axis * 2];
        }
        lower[axis] = base;
        if constexpr (Interpolate)
            fraction[axis] = std::clamp(bounded - static_cast<double>(base), 0.0, 1.0);
    }

    // 原采样状态只取决于坐标和完整角点 mask，NaN 标量不会改变该状态。
    if constexpr (!Interpolate)
        if (!volume.validity) return {SampleStatus::Valid, 0.0};
    double value = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
        const int x = lower[0] + (corner & 1);
        const int y = lower[1] + ((corner >> 1) & 1);
        const int z = lower[2] + ((corner >> 2) & 1);
        const std::size_t tupleIndex = GetTupleIndex(
            volume.geometry, x, y, z);
        if (volume.validity && volume.validity[tupleIndex] == 0) {
            return { SampleStatus::InvalidSupport, 0.0 };
        }
        if constexpr (Interpolate) {
            const double xWeight = (corner & 1) ? fraction[0] : 1.0 - fraction[0];
            const double yWeight = (corner & 2) ? fraction[1] : 1.0 - fraction[1];
            const double zWeight = (corner & 4) ? fraction[2] : 1.0 - fraction[2];
            value += xWeight * yWeight * zWeight * volume.scalars.GetValue(tupleIndex);
        }
    }
    return { SampleStatus::Valid, value };
}


bool GetGradientAtIndex(
    const VolumeView& volume,
    const Point3& center,
    Point3& gradientModel,
    double& magnitude)
{
    Point3 gradientIndex{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        Point3 negative = center;
        Point3 positive = center;
        negative[axis] -= 0.5;
        positive[axis] += 0.5;
        const ScalarSample before = GetScalarAtIndex(volume, negative);
        const ScalarSample after = GetScalarAtIndex(volume, positive);
        if (before.status != SampleStatus::Valid
            || after.status != SampleStatus::Valid) {
            return false;
        }
        gradientIndex[axis] = after.value - before.value;
    }
    gradientModel = {
        volume.geometry.modelToIndex[0] * gradientIndex[0]
            + volume.geometry.modelToIndex[3] * gradientIndex[1]
            + volume.geometry.modelToIndex[6] * gradientIndex[2],
        volume.geometry.modelToIndex[1] * gradientIndex[0]
            + volume.geometry.modelToIndex[4] * gradientIndex[1]
            + volume.geometry.modelToIndex[7] * gradientIndex[2],
        volume.geometry.modelToIndex[2] * gradientIndex[0]
            + volume.geometry.modelToIndex[5] * gradientIndex[1]
            + volume.geometry.modelToIndex[8] * gradientIndex[2]
    };
    magnitude = GetLength(gradientModel);
    return std::isfinite(magnitude) && magnitude > geometryEpsilon;
}

bool GetGradient(const VolumeView &volume, const Point3 &modelPoint, Point3 &gradientModel, double &magnitude)
{
    return GetGradientAtIndex(volume, GetContinuousIndex(volume.geometry, modelPoint), gradientModel, magnitude);
}

bool GetVoxelUsed(
    const VolumeView& volume,
    const ResolvedParams& params,
    const int x,
    const int y,
    const int z,
    const std::size_t tupleIndex)
{
    if (volume.validity && volume.validity[tupleIndex] == 0) return false;
    (void)params;(void)x;(void)y;(void)z;
    // ROI 是评价选区；材料估计与剖面支撑保持原有效域。
    return true;
}

SurfaceFailureReason GetMaterialRange(
    const VolumeView& volume,
    const ResolvedParams& params,
    const SurfaceCancelCheck& getCancelled,
    double& isoValue,
    std::string& message,
    std::optional<SurfaceIsoEstimate>& estimate)
{
    double minimum = std::numeric_limits<double>::max(), maximum = std::numeric_limits<double>::lowest();
    std::uint64_t validCount = 0, excludedCount = 0;
    const auto &extent = volume.geometry.extent;
    std::array<std::int64_t, 3> sampleSteps{};
    for (unsigned a = 0; a < 3; ++a)
    {
        sampleSteps[a] = (static_cast<std::int64_t>(volume.geometry.dimensions[a]) + 127) / 128;
    }
    // 范围和直方图使用同一采样集合。独立的 16^3 预采样会避开薄壁材料，
    // 随后把真正的材料峰作为越界值丢弃，并把背景附近的次峰误认作材料。
    // 只保留每侧 17 个极值，以固定空间排除最多 16 个孤立极端样本。
    constexpr std::size_t tailCapacity = 17;
    std::array<double, tailCapacity> lowest{}, highest{};
    lowest.fill(std::numeric_limits<double>::max());
    highest.fill(std::numeric_limits<double>::lowest());
    for (std::int64_t z = extent[4] + sampleSteps[2] / 2; z <= extent[5]; z += sampleSteps[2])
        for (std::int64_t y = extent[2] + sampleSteps[1] / 2; y <= extent[3]; y += sampleSteps[1])
        {
            if (GetCancelled(getCancelled))
                return SurfaceFailureReason::Cancelled;
            for (std::int64_t x = extent[0] + sampleSteps[0] / 2; x <= extent[1]; x += sampleSteps[0])
            {
                const auto id = GetTupleIndex(volume.geometry, static_cast<int>(x), static_cast<int>(y),
                                              static_cast<int>(z));
                const auto value = volume.scalars.GetValue(id);
                if (GetVoxelUsed(volume, params, static_cast<int>(x), static_cast<int>(y),
                                 static_cast<int>(z), id) &&
                    std::isfinite(value)) {
                    ++validCount;
                    if (value < lowest.back()) {
                        const auto at = std::lower_bound(lowest.begin(), lowest.end(), value);
                        std::move_backward(at, lowest.end()-1, lowest.end()); *at = value;
                    }
                    if (value > highest.back()) {
                        const auto at = std::lower_bound(highest.begin(), highest.end(), value, std::greater<double>{});
                        std::move_backward(at, highest.end()-1, highest.end()); *at = value;
                    }
                }
            }
        }
    if (validCount >= 64)
    {
        const auto trim = std::min<std::uint64_t>(tailCapacity-1, validCount / 1000);
        minimum = lowest[trim];
        maximum = highest[trim];
    }
    if (validCount < 64 || !std::isfinite(minimum)
        || !std::isfinite(maximum) || !std::isfinite(maximum - minimum)
        || maximum - minimum <= geometryEpsilon
            * std::max({ 1.0, std::abs(minimum), std::abs(maximum) })) {
        message = "Surface material range estimation requires a non-degenerate bimodal histogram.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    validCount = 0;
    std::array<std::uint64_t, histogramBinCount> histogram{};
    const double scale = static_cast<double>(histogramBinCount - 1)
        / (maximum - minimum);
    for (std::int64_t zValue = extent[4] + sampleSteps[2] / 2;
        zValue <= static_cast<std::int64_t>(extent[5]); zValue += sampleSteps[2]) {
        const int z = static_cast<int>(zValue);
        if (GetCancelled(getCancelled)) {
            message = "Surface threshold estimation was cancelled.";
            return SurfaceFailureReason::Cancelled;
        }
        for (std::int64_t yValue = extent[2] + sampleSteps[1] / 2;
            yValue <= static_cast<std::int64_t>(extent[3]); yValue += sampleSteps[1]) {
            const int y = static_cast<int>(yValue);
            for (std::int64_t xValue = extent[0] + sampleSteps[0] / 2;
                xValue <= static_cast<std::int64_t>(extent[1]); xValue += sampleSteps[0]) {
                const int x = static_cast<int>(xValue);
                const std::size_t tupleIndex = GetTupleIndex(
                    volume.geometry, x, y, z);
                if (!GetVoxelUsed(
                        volume, params, x, y, z, tupleIndex)) {
                    continue;
                }
                const double value = volume.scalars.GetValue(tupleIndex);
                if (!std::isfinite(value) || value < minimum || value > maximum)
                {
                    ++excludedCount;
                    continue;
                }
                ++validCount;
                const auto bin = static_cast<std::size_t>(std::clamp(
                    std::llround((value - minimum) * scale),
                    0LL,
                    static_cast<long long>(histogramBinCount - 1)));
                ++histogram[bin];
            }
        }
    }

    std::array<double, histogramBinCount> smooth{};
    for (std::size_t index = 0; index < histogramBinCount; ++index) {
        double sum = 0.0;
        double weight = 0.0;
        const std::size_t begin = index > 2 ? index - 2 : 0;
        const std::size_t end = std::min(
            histogramBinCount - 1, index + 2);
        for (std::size_t sample = begin; sample <= end; ++sample) {
            const double localWeight = sample == index ? 3.0
                : (sample + 1 == index || sample == index + 1 ? 2.0 : 1.0);
            sum += localWeight * static_cast<double>(histogram[sample]);
            weight += localWeight;
        }
        smooth[index] = sum / weight;
    }

    struct Peak final {
        std::size_t index = 0;
        double height = 0.0;
    };
    std::vector<Peak> peaks;
    peaks.reserve(histogramBinCount);
    for (std::size_t index = 0; index < histogramBinCount; ++index) {
        const double left = index == 0 ? -1.0 : smooth[index - 1];
        const double right = index + 1 == histogramBinCount
            ? -1.0 : smooth[index + 1];
        if (smooth[index] >= left && smooth[index] >= right
            && smooth[index] > 0.0) {
            peaks.push_back({ index, smooth[index] });
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& left, const Peak& right) {
        if (left.height != right.height) return left.height > right.height;
        return left.index < right.index;
    });

    std::optional<std::pair<Peak, Peak>> selected;
    const std::size_t minimumSeparation = histogramBinCount / 10;
    const double minimumPeakHeight = std::max(
        2.0, static_cast<double>(validCount) * 1.0e-5);
    if (!peaks.empty()) {
        // 按谷分群；近背景起伏和同一材料上的小尖峰合并，
        // 非空气材料按原始直方图积分样本数比较，而不是按峰高或离背景的距离比较。
        std::vector<Peak> groups;
        for (const auto& peak : peaks) {
            if (peak.height < minimumPeakHeight) continue;
            const bool separated = std::all_of(groups.begin(), groups.end(), [&](const Peak& group) {
                const auto low = std::min(peak.index, group.index), high = std::max(peak.index, group.index);
                if (high-low < minimumSeparation) return false;
                const auto valley = *std::min_element(smooth.begin()+low, smooth.begin()+high+1);
                return valley <= 0.7 * std::min(peak.height, group.height);
            });
            if (separated) groups.push_back(peak);
        }
        std::sort(groups.begin(), groups.end(), [](const Peak& a, const Peak& b) { return a.index < b.index; });
        DataArray<std::uint64_t> counts;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const auto left = i == 0 ? std::ptrdiff_t{0}
                : std::min_element(smooth.begin()+groups[i-1].index, smooth.begin()+groups[i].index+1)-smooth.begin();
            const auto right = i+1 < groups.size()
                ? std::min_element(smooth.begin()+groups[i].index, smooth.begin()+groups[i+1].index+1)-smooth.begin()
                : static_cast<std::ptrdiff_t>(histogramBinCount);
            counts.push_back(std::accumulate(histogram.begin()+left, histogram.begin()+right, std::uint64_t{0}));
        }
        if (!counts.empty()) {
            // 背景取有显著峰高与样本支持的最低灰度群；最高峰可能属于材料。
            // 低值小伪影不能顶替空气。范围需包含足够背景，才能识别这对灰度群。
            const auto maximumCount = *std::max_element(counts.begin(), counts.end());
            std::size_t background = 0;
            while (background < groups.size()
                && (groups[background].height < 0.01*peaks.front().height || counts[background] < 0.05*maximumCount)) ++background;
            std::uint64_t largestCount = 0;
            for (std::size_t i = background+1; i < groups.size(); ++i) {
                if (counts[i] > largestCount) { largestCount = counts[i]; selected = std::make_pair(groups[background], groups[i]); }
            }
        }
    }
    if (!selected) {
        message = "Surface material range estimation could not identify two reliable peaks.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }

    const double binWidth = (maximum - minimum)
        / static_cast<double>(histogramBinCount - 1);
    const double background = minimum
        + static_cast<double>(selected->first.index) * binWidth;
    const double material = minimum
        + static_cast<double>(selected->second.index) * binWidth;
    isoValue = 0.5 * (background + material);
    if (!std::isfinite(isoValue) || material <= background) {
        message = "Surface material range estimation produced an invalid threshold.";
        return SurfaceFailureReason::ThresholdUnreliable;
    }
    estimate = SurfaceIsoEstimate{
        isoValue,      background,
        material,      validCount,
        excludedCount, double(selected->second.index - selected->first.index) / (histogramBinCount - 1)};
    estimate->histogramCounts=histogram;
    estimate->histogramEdges.front()=minimum;
    estimate->histogramEdges.back()=maximum;
    for(std::size_t i=1;i<histogramBinCount;++i) estimate->histogramEdges[i]=minimum+(static_cast<double>(i)-0.5)*binWidth;
    for(std::size_t axis=0;axis<3;++axis) estimate->histogramSampleSteps[axis]=static_cast<int>(sampleSteps[axis]);
    return SurfaceFailureReason::None;
}

void AddFingerprint(
    std::uint64_t& fingerprint,
    const std::uint64_t value)
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        fingerprint ^= (value >> (byte * 8U)) & 0xffU;
        fingerprint *= prime;
    }
}

SurfaceFailureReason ResolveParams(
    const VolumeView& volume,
    const SurfaceDeterminationStartParams& input,
    const SurfaceCancelCheck& getCancelled,
    ResolvedParams& params,
    std::string& message)
{
    message = SurfaceRecipeCodec::GetError(input);
    if (!message.empty() || input.seedBlockDepth == 0 || input.seedBlockDepth > 4096)
        return SurfaceFailureReason::InvalidGeometry;
    params.sharpCornerAngleDeg = input.sharpCornerAngleDeg;
    params.method = input.method;
    params.componentSelection = input.componentSelection;
    params.seedModelPoint = input.seedModelPoint;

    params.minimumObjectVoxels = input.minimumObjectVoxels;
    if (params.componentSelection == SurfaceComponentSelection::Seeded
        && !params.seedModelPoint) {
        message = "Surface seeded selection requires a model-space seed.";
        return SurfaceFailureReason::InvalidGeometry;
    }
    if (params.seedModelPoint) {
        for (const double value : *params.seedModelPoint) {
            if (!std::isfinite(value)) {
                message = "Surface seed contains a non-finite coordinate.";
                return SurfaceFailureReason::InvalidGeometry;
            }
        }
    }
    if (params.roi) {
        const auto planes=params.roi->GetClipPlanes();
        if (planes.error==RoiError::None) params.clipPlanes=planes.planes;
    }

    params.boundaryToleranceModel = 0.25 * *std::min_element(volume.geometry.spacing.begin(), volume.geometry.spacing.end());
    params.haloModel = 4 * *std::max_element(volume.geometry.spacing.begin(), volume.geometry.spacing.end());
    if (input.method == SurfaceDeterminationMethod::GlobalAutomatic)
    {
        if (input.materialRange)
        {
            SurfaceIsoEstimate estimate;
            estimate.backgroundValue = (*input.materialRange)[0];
            estimate.materialValue = (*input.materialRange)[1];
            params.isoEstimate = estimate;
        }
        else if (input.initialIsoValue)
        {
            // 显式阈值的缺省饱和范围来自同一正式输入，不能被另一次峰估计否决。
            SurfaceIsoEstimate estimate;
            estimate.backgroundValue = volume.sourceRange[0];
            estimate.materialValue = volume.sourceRange[1];
            if (!std::isfinite(estimate.backgroundValue) || !std::isfinite(estimate.materialValue) ||
                !std::isfinite(estimate.materialValue - estimate.backgroundValue))
                return SurfaceFailureReason::ThresholdUnreliable;
            params.isoEstimate = estimate;
        }
        else
        {
            const auto status = GetMaterialRange(volume, params, getCancelled, params.initialIsoValue,
                                                message, params.isoEstimate);
            if (status != SurfaceFailureReason::None)
                return status;
        }
        if (!params.isoEstimate)
            return SurfaceFailureReason::ThresholdUnreliable;
        auto &estimate = *params.isoEstimate;
        params.initialIsoValue = input.initialIsoValue.value_or(
            (1 - input.seedFraction) * estimate.backgroundValue + input.seedFraction * estimate.materialValue);
        if (!(estimate.backgroundValue < params.initialIsoValue &&
              params.initialIsoValue < estimate.materialValue))
        {
            message = "Material ISO threshold must lie strictly between the material values.";
            return SurfaceFailureReason::ThresholdUnreliable;
        }
        estimate.isoValue = params.initialIsoValue;
        return SurfaceFailureReason::None;
    }
    return SurfaceFailureReason::InvalidGeometry;
}

bool AddWorkingBytes(
    const std::size_t count,
    const std::size_t itemBytes,
    std::size_t& workingBytes)
{
    std::size_t bytes = 0;
    return GetProduct(count, itemBytes, bytes)
        && GetSum(workingBytes, bytes, workingBytes);
}

class DisjointSet final {
public:
    explicit DisjointSet(const std::size_t size)
        : m_parent(size)
        , m_rank(size, 0)
    {
        std::iota(m_parent.begin(), m_parent.end(), 0U);
    }

    std::uint32_t GetRoot(const std::uint32_t value)
    {
        std::uint32_t current = value;
        while (m_parent[current] != current) current = m_parent[current];
        std::uint32_t next = value;
        while (m_parent[next] != next) {
            const std::uint32_t parent = m_parent[next];
            m_parent[next] = current;
            next = parent;
        }
        return current;
    }

    void SetJoined(const std::uint32_t left, const std::uint32_t right)
    {
        std::uint32_t leftRoot = GetRoot(left);
        std::uint32_t rightRoot = GetRoot(right);
        if (leftRoot == rightRoot) return;
        if (m_rank[leftRoot] < m_rank[rightRoot]) {
            std::swap(leftRoot, rightRoot);
        }
        m_parent[rightRoot] = leftRoot;
        if (m_rank[leftRoot] == m_rank[rightRoot]) ++m_rank[leftRoot];
    }

    DataArray<std::uint32_t> ReleaseParents() { return std::move(m_parent); }
    DataArray<std::uint8_t> ReleaseRanks() { return std::move(m_rank); }

private:
    DataArray<std::uint32_t> m_parent;
    DataArray<std::uint8_t> m_rank;
};

std::uint64_t GetEdgeKey(
    const std::uint32_t left,
    const std::uint32_t right)
{
    const std::uint32_t minimum = std::min(left, right);
    const std::uint32_t maximum = std::max(left, right);
    return (static_cast<std::uint64_t>(minimum) << 32U)
        | static_cast<std::uint64_t>(maximum);
}

TopologyMetrics GetTopologyMetrics(const DataArray<Point3> &points, const DataArray<Triangle> &triangles,
                                   const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    struct EdgeRecord final {
        std::uint32_t count = 0;
        int directionSum = 0;
    };
    const bool useCompact = points.size() <= (std::uint64_t(1) << 31U);
    DataArray<std::uint64_t> edgeKeys;
    using EdgeMap=std::unordered_map<std::uint64_t,EdgeRecord,std::hash<std::uint64_t>,std::equal_to<std::uint64_t>,
        DataArrayAllocator<std::pair<const std::uint64_t,EdgeRecord>>>;
    std::optional<EdgeMap> edges;
    if (useCompact) {
        if (triangles.size() > edgeKeys.max_size() / 3U) throw std::bad_alloc{};
        edgeKeys.reserve(triangles.size() * 3U);
    } else {
        edges.emplace();
    if (triangles.size()
        <= std::numeric_limits<std::size_t>::max() / 2U) {
        edges->reserve(triangles.size() * 2U);
    }
    }
    TopologyMetrics metrics;
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto& ids = triangle.vertices;
        if (ids[0] >= points.size()
            || ids[1] >= points.size()
            || ids[2] >= points.size()) {
            ++metrics.degenerateTriangleCount;
            metrics.isOrientationValid = false;
            continue;
        }
        const Point3 edge01 = Subtract(points[ids[1]], points[ids[0]]);
        const Point3 edge02 = Subtract(points[ids[2]], points[ids[0]]);
        const Point3 areaVector = Cross(edge01, edge02);
        const double doubleArea = GetLength(areaVector);
        if (!std::isfinite(doubleArea) || doubleArea <= geometryEpsilon) {
            ++metrics.degenerateTriangleCount;
        }
        else {
            metrics.signedVolume += Dot(
                points[ids[0]],
                Cross(points[ids[1]], points[ids[2]])) / 6.0;
        }
        for (std::size_t edge = 0; edge < 3; ++edge) {
            CheckCancellation(cancellationBatch, cancelled);
            const std::uint32_t from = ids[edge];
            const std::uint32_t to = ids[(edge + 1) % 3];
            if (useCompact) edgeKeys.push_back(GetSurfaceDirectedEdgeKey(from, to));
            else {
                auto& record = (*edges)[GetEdgeKey(from, to)];
                ++record.count;
                record.directionSum += from < to ? 1 : -1;
            }
        }
    }
    if (useCompact) {
        const auto topology = GetSortedSurfaceEdgeTopology(edgeKeys, [&] {
            CheckCancellation(cancellationBatch, cancelled);
            if (cancelled && cancelled()) throw SurfaceCancelled{};
        });
        metrics.boundaryEdgeCount = topology.boundaryEdgeCount;
        metrics.nonManifoldEdgeCount = topology.nonManifoldEdgeCount;
        metrics.isOrientationValid = metrics.isOrientationValid && topology.isOrientationValid;
    } else for (const auto& item : *edges) {
        CheckCancellation(cancellationBatch, cancelled);
        const EdgeRecord& edge = item.second;
        if (edge.count == 1) ++metrics.boundaryEdgeCount;
        else if (edge.count > 2) ++metrics.nonManifoldEdgeCount;
        if (edge.count == 2 && edge.directionSum != 0) {
            metrics.isOrientationValid = false;
        }
        if (edge.count > 2) metrics.isOrientationValid = false;
    }
    if (metrics.degenerateTriangleCount != 0) {
        metrics.isOrientationValid = false;
    }
    return metrics;
}

DataArray<MeshComponent> BuildComponents(const DataArray<Point3> &points,
                                           const DataArray<Triangle> &triangles, const double voxelVolume,
                                           const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    DisjointSet sets(points.size());
    for (const auto &triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        sets.SetJoined(triangle.vertices[0], triangle.vertices[1]);
        sets.SetJoined(triangle.vertices[1], triangle.vertices[2]);
    }
    using Group = std::pair<std::uint32_t, DataArray<Triangle>>;
    using GroupMap = std::map<std::uint32_t, DataArray<Triangle>,std::less<std::uint32_t>,
        DataArrayAllocator<std::pair<const std::uint32_t,DataArray<Triangle>>>>;
    DataArray<Group> groups;
    std::optional<GroupMap> legacyGroups;
    DataArray<std::uint32_t> localIds;
    // Union 已结束，GetRoot 不读取 rank；把 rank 缓冲改作根和点的标记。
    auto seen = sets.ReleaseRanks();
    for (auto &marker : seen) { CheckCancellation(cancellationBatch, cancelled); marker = 0; }
    constexpr auto groupTag = std::uint32_t(1) << 31U;
    if (points.size() <= std::uint64_t(groupTag)) {
        std::size_t groupCount = 0;
        for (const auto &triangle : triangles) {
            CheckCancellation(cancellationBatch, cancelled);
            const auto root = sets.GetRoot(triangle.vertices[0]);
            if (!seen[root]) { seen[root] = 1; ++groupCount; }
        }
        groups.reserve(groupCount);
        localIds = sets.ReleaseParents();
        const auto getGroup = [&](std::uint32_t point) {
            auto root = point;
            while (localIds[root] != root && !(localIds[root] & groupTag)) root = localIds[root];
            auto encoded = localIds[root];
            if (!(encoded & groupTag)) {
                encoded = groupTag | static_cast<std::uint32_t>(groups.size());
                groups.emplace_back(root, DataArray<Triangle>{});
                localIds[root] = encoded;
            }
            while (!(localIds[point] & groupTag)) {
                const auto next = localIds[point];
                localIds[point] = encoded;
                point = next;
            }
            return encoded & ~groupTag;
        };
        for (const auto &triangle : triangles) {
            CheckCancellation(cancellationBatch, cancelled);
            const auto group = getGroup(triangle.vertices[0]);
            groups[group].second.push_back(triangle);
        }
        // 恢复原 map 的根编号顺序，保留组件顺序及每组件的浮点累加顺序。
        std::sort(groups.begin(), groups.end(), [&](const auto &left, const auto &right) {
            CheckCancellation(cancellationBatch, cancelled);
            return left.first < right.first;
        });
        for (const auto &group : groups) {
            CheckCancellation(cancellationBatch, cancelled);
            seen[group.first] = 0;
        }
    } else {
        // 32 位完整编号保留原分组容器，避免标签占用合法点编号。
        legacyGroups.emplace();
        for (const auto &triangle : triangles) {
            CheckCancellation(cancellationBatch, cancelled);
            (*legacyGroups)[sets.GetRoot(triangle.vertices[0])].push_back(triangle);
        }
        localIds = sets.ReleaseParents();
    }
    DataArray<MeshComponent> components;
    components.reserve(legacyGroups ? legacyGroups->size() : groups.size());
    const auto appendComponent = [&](auto &item) {
        CheckCancellation(cancellationBatch, cancelled);
        MeshComponent component;
        component.triangles = std::move(item.second);
        DataArray<std::uint32_t> pointIds;
        pointIds.reserve(component.triangles.size());
        for (const Triangle& triangle : component.triangles) {
            CheckCancellation(cancellationBatch, cancelled);
            for(auto id:triangle.vertices) {
                if(!seen[id]) {seen[id]=1;pointIds.push_back(id);}
            }
        }
        std::sort(pointIds.begin(), pointIds.end());
        component.sourcePointIds = std::move(pointIds);
        component.minimumPointId = component.sourcePointIds.front();

        const bool hasAllPoints = component.sourcePointIds.size() == points.size();
        DataArray<Point3> localPoints;
        if (!hasAllPoints) localPoints.reserve(component.sourcePointIds.size());
        for (std::size_t index = 0;
            index < component.sourcePointIds.size(); ++index) {
            CheckCancellation(cancellationBatch, cancelled);
            localIds[component.sourcePointIds[index]]=static_cast<std::uint32_t>(index);
            if (!hasAllPoints) localPoints.push_back(points[component.sourcePointIds[index]]);
        }
        // 来源点映射已经固定，直接在组件三角形缓冲中完成局部重编号。
        for (Triangle& triangle : component.triangles) {
            CheckCancellation(cancellationBatch, cancelled);
            for (std::uint32_t& pointId : triangle.vertices) {
                CheckCancellation(cancellationBatch, cancelled);
                pointId = localIds[pointId];
            }
        }
        const TopologyMetrics topology = GetTopologyMetrics(hasAllPoints ? points : localPoints,
                                                           component.triangles, cancelled);
        component.topology=topology;
        // 保留已经生成的局部编号，避免输出阶段再次建立同一 remap。
        // 拓扑哈希已释放后再压紧容量，避免 push_back 的余量常驻到结果生成阶段。
        CheckCancellation(cancellationBatch, cancelled);
        component.triangles.shrink_to_fit();
        component.isClosed = topology.boundaryEdgeCount == 0
            && topology.nonManifoldEdgeCount == 0;
        if (component.isClosed && voxelVolume > geometryEpsilon) {
            const double voxelCount = std::abs(topology.signedVolume)
                / voxelVolume;
            if (std::isfinite(voxelCount) && voxelCount >= 1.0) {
                component.estimatedVoxelCount = voxelCount
                    >= static_cast<double>(
                        std::numeric_limits<long long>::max())
                    ? std::numeric_limits<std::uint64_t>::max()
                    : static_cast<std::uint64_t>(std::llround(voxelCount));
            }
        }
        components.push_back(std::move(component));
    };
    if (legacyGroups) {
        for (auto &group : *legacyGroups) appendComponent(group);
    } else {
        for (auto &group : groups) appendComponent(group);
    }
    return components;
}

double GetComponentDistanceSquared(const MeshComponent &component, const DataArray<Point3> &points,
                                   const Point3 &seed, const SurfaceCancelCheck &cancelled = {},
                                   std::uint64_t *visited = nullptr)
{
    std::size_t cancellationBatch = 0;
    double distance = std::numeric_limits<double>::max();
    for (const std::uint32_t pointId : component.sourcePointIds) {
        CheckCancellation(cancellationBatch, cancelled);
        const Point3 delta = Subtract(points[pointId], seed);
        distance = std::min(distance, Dot(delta, delta));
    }
    if(visited)*visited+=component.sourcePointIds.size();
    return distance;
}

DataArray<std::size_t> GetSelectedComponents(const DataArray<MeshComponent> &components,
                                               const DataArray<Point3> &points,
                                               const ResolvedParams &params,
                                               const SurfaceCancelCheck &cancelled,
                                               std::uint64_t &candidateCount,std::uint64_t &pointVisits)
{
    std::size_t cancellationBatch = 0;
    DataArray<std::size_t> eligible;
    const auto score = [&components](const std::size_t index) {
        const MeshComponent& component = components[index];
        return std::make_pair(component.estimatedVoxelCount,
                              static_cast<std::uint64_t>(component.triangles.size()));
    };
    const auto stableOrder = [&components, &score](const std::size_t left,const std::size_t right) {
        if (score(left) != score(right)) return score(left) > score(right);
        return components[left].minimumPointId < components[right].minimumPointId;
    };
    std::optional<std::size_t> best;
    double bestDistance = 0;
    bool distanceKnown = false;
    for (std::size_t index = 0; index < components.size(); ++index) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto &component = components[index];
        // 开放表面没有可信体积，保留原准入规则。
        if (component.isClosed && component.estimatedVoxelCount < params.minimumObjectVoxels) continue;
        ++candidateCount;
        if (params.componentSelection == SurfaceComponentSelection::All) {
            eligible.push_back(index);
            continue;
        }
        if (!best) { best = index; continue; }
        if (params.componentSelection == SurfaceComponentSelection::Largest) {
            // 与原 sort 的首项比较器相同；只输出一项时不存储、排序其余候选。
            if (stableOrder(index,*best)) best=index;
            continue;
        }
        // 仅一个候选时无需计算距离；存在竞争时，每件组件至多扫描一次。
        const auto &seed=*params.seedModelPoint;
        if (!distanceKnown) {
            bestDistance=GetComponentDistanceSquared(components[*best],points,seed,cancelled,&pointVisits);
            distanceKnown=true;
        }
        const auto distance=GetComponentDistanceSquared(component,points,seed,cancelled,&pointVisits);
        if (distance<bestDistance || (distance==bestDistance &&
                component.minimumPointId<components[*best].minimumPointId)) {
            best=index;bestDistance=distance;
        }
    }
    if (params.componentSelection != SurfaceComponentSelection::All)
        return best?DataArray<std::size_t>{*best}:DataArray<std::size_t>{};
    // All 仍按原规则排序，保留全部对象和点、面的发布顺序。
    std::sort(eligible.begin(),eligible.end(),stableOrder);
    return eligible;
}

void GetLocalMesh(MeshComponent &component, DataArray<Point3> &sourcePoints,
                  DataArray<Point3> &points, DataArray<Triangle> &triangles,
                  const bool canConsumeSource,
                  const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    if (canConsumeSource && component.sourcePointIds.size() == sourcePoints.size()) {
        CheckCancellation(cancellationBatch, cancelled);
        // 唯一组件的升序去重点编号覆盖整个源数组，局部编号与源编号相同。
        points = std::move(sourcePoints);
    } else {
        points.reserve(component.sourcePointIds.size());
        for (std::size_t index = 0; index < component.sourcePointIds.size(); ++index) {
            CheckCancellation(cancellationBatch, cancelled);
            points.push_back(sourcePoints[component.sourcePointIds[index]]);
        }
    }
    triangles = std::move(component.triangles);
}

DataArray<Point3> GetVertexNormals(const DataArray<Point3> &points,
                                     const DataArray<Triangle> &triangles,
                                     const CpuSamplingPlan &cpuPlan, unsigned &normalizationThreads,
                                     const SurfaceCancelCheck &cancelled = {}, const bool ownsCpuQuota = false)
{
    std::size_t cancellationBatch = 0;
    DataArray<Point3> normals(points.size(), Point3{});
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const Point3 normal = Cross(
            Subtract(
                points[triangle.vertices[1]],
                points[triangle.vertices[0]]),
            Subtract(
                points[triangle.vertices[2]],
                points[triangle.vertices[0]]));
        for (const std::uint32_t pointId : triangle.vertices) {
            CheckCancellation(cancellationBatch, cancelled);
            normals[pointId] = Add(normals[pointId], normal);
        }
    }
    const auto normalize = [&](std::size_t begin, std::size_t end) {
            std::size_t batch = 0;
            for (auto i = begin; i < end; ++i) {
                CheckCancellation(batch, cancelled);
                if (!Normalize(normals[i])) normals[i] = {0.0, 0.0, 1.0};
            }
        };
    if (ownsCpuQuota) {
        // 外层组件池已租用配额；内部串行执行，不能再次申请 limit=1 的嵌套 lease。
        normalize(0,normals.size());
        normalizationThreads=1;
    } else normalizationThreads=RunPointSampling(normals.size(),cpuPlan.threadLimit,cancelled,normalize);
    return normals;
}

void SetOutwardWinding(
    const VolumeView& volume,
    const DataArray<Point3>& points,
    DataArray<Triangle>& triangles)
{
    double orientationScore = 0.0;
    std::size_t sampleCount = 0;
    const std::size_t stride = std::max<std::size_t>(
        1, triangles.size() / 256U);
    for (std::size_t index = 0; index < triangles.size(); index += stride) {
        const Triangle& triangle = triangles[index];
        const Point3& first = points[triangle.vertices[0]];
        const Point3& second = points[triangle.vertices[1]];
        const Point3& third = points[triangle.vertices[2]];
        Point3 triangleNormal = Cross(
            Subtract(second, first), Subtract(third, first));
        if (!Normalize(triangleNormal)) continue;
        const Point3 center = Scale(Add(Add(first, second), third), 1.0 / 3.0);
        Point3 gradient{};
        double magnitude = 0.0;
        if (!GetGradient(volume, center, gradient, magnitude)
            || !Normalize(gradient)) {
            continue;
        }
        const Point3 outward = Scale(gradient, -1.0);
        orientationScore += Dot(triangleNormal, outward);
        ++sampleCount;
    }
    if (sampleCount != 0 && orientationScore < 0.0) {
        for (Triangle& triangle : triangles) {
            std::swap(triangle.vertices[1], triangle.vertices[2]);
        }
    }
}

float GetFiniteFloat(const double value)
{
    if (!std::isfinite(value)) return 0.0F;
    const double bounded = std::clamp(
        value,
        -static_cast<double>(std::numeric_limits<float>::max()),
        static_cast<double>(std::numeric_limits<float>::max()));
    return static_cast<float>(bounded);
}

template<bool DeferRoiBoundary=false>
bool SetMaterialPoint(const VolumeView &volume, const ResolvedParams &params, const Point3 &initialPoint,
                      const Point3 &initialNormal, SurfacePointRecord &record,
                      SurfacePointDiagnostic *diagnostic = nullptr,
                      const SurfaceCudaSample* deviceSample = nullptr)
{
    record.seedPositionModel = record.positionModel = initialPoint;
    auto normal = initialNormal;
    Point3 gradient{};
    double magnitude = 0;
    // ROI 边缘不改变材料点细化或测量资格。
    Point3 center{};
    bool hasGradient = false;
    if (deviceSample) {
        std::copy_n(deviceSample->gradient, 3, gradient.begin());
        magnitude = deviceSample->magnitude;
        hasGradient = deviceSample->gradientValid != 0;
        if (hasGradient && Normalize(gradient)) normal = Scale(gradient, -1);
    } else {
        center = GetContinuousIndex(volume.geometry, initialPoint);
        hasGradient = GetGradientAtIndex(volume, center, gradient, magnitude);
        // CPU magnitude 已由同一梯度的 GetLength 得到；保留随后 normal 的归一化。
        if (hasGradient) normal = Scale(Scale(gradient, 1.0 / magnitude), -1);
    }
    if (!Normalize(normal)) {
        record.flags |= SurfacePointFlags::FitRejected | SurfacePointFlags::SeedRetained;
        return false;
    }
    record.seedNormalModel = normal;
    // 原始分支的饱和节点梯度、位置和支持规则；不再保留局部剖面拟合路径。
    // 不可变输入下首次采样已给出相同 magnitude，无需重复 6 次插值。
    record.normalModel = {GetFiniteFloat(normal[0]), GetFiniteFloat(normal[1]), GetFiniteFloat(normal[2])};
    record.localThreshold = GetFiniteFloat(params.initialIsoValue);
    record.gradientMagnitude = GetFiniteFloat(magnitude);
    const auto support = deviceSample
        ? (deviceSample->support == 1 ? SampleStatus::Valid : SampleStatus::InvalidSupport)
        : GetScalarAtIndex<false>(volume, center).status;
    record.validSupportRatio = support == SampleStatus::Valid ? 1.0F : 0.0F;
    if (support != SampleStatus::Valid) record.flags |= SurfacePointFlags::InvalidSupport;
    if (diagnostic) {
        diagnostic->isAvailable = true;
        diagnostic->point = record;
        diagnostic->message = "Material threshold has no local refinement profile.";
    }
    return true;
}

bool GetPointAtDataBoundary(
    const ImageGeometry& geometry,
    const Point3& modelPoint,
    const double toleranceModel)
{
    const Point3 index = GetContinuousIndex(geometry, modelPoint);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double toleranceIndex = toleranceModel
            / geometry.spacing[axis];
        if (std::abs(index[axis]
                - static_cast<double>(geometry.extent[axis * 2]))
                <= toleranceIndex
            || std::abs(index[axis]
                - static_cast<double>(geometry.extent[axis * 2 + 1]))
                <= toleranceIndex) {
            return true;
        }
    }
    return false;
}

void SetMaterialTriangleFlags(const DataArray<Point3> &points, const DataArray<Triangle> &triangles,
                              const DataArray<Point3> &normals, const double cornerCosine,
                              SurfaceRecordRange records,
                              const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    for (const Triangle& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto& ids = triangle.vertices;
        auto face = Cross(Subtract(points[ids[1]],points[ids[0]]),
                          Subtract(points[ids[2]],points[ids[0]]));
        const double length = GetLength(face);
        // 材料流程的前后位置相同；复用尖角检测的原面及长度，保留原比较而非用 Normalize 失败代替。
        if (length <= geometryEpsilon || Dot(face,face) <= 0.0) {
            for (const std::uint32_t pointId : ids) {
                CheckCancellation(cancellationBatch, cancelled);
                records[pointId].flags |=
                    SurfacePointFlags::TriangleFlipRisk | SurfacePointFlags::SeedRetained;
            }
        }
        if (!std::isfinite(length) || length <= geometryEpsilon) continue;
        face = Scale(face,1.0/length);
        for (const auto pointId : ids) {
            if (Dot(face,normals[pointId]) < cornerCosine)
                records[pointId].flags |= SurfacePointFlags::SharpCorner;
        }
    }
}

SurfaceMetricValidity GetVolumeValidity(
    const TopologyMetrics& topology,
    const bool isTruncated,
    const bool hasQuality)
{
    if (topology.nonManifoldEdgeCount != 0) {
        return SurfaceMetricValidity::NonManifold;
    }
    if (isTruncated) return SurfaceMetricValidity::Truncated;
    if (topology.boundaryEdgeCount != 0) {
        return SurfaceMetricValidity::OpenSurface;
    }
    if (!topology.isOrientationValid
        || topology.degenerateTriangleCount != 0) {
        return SurfaceMetricValidity::NonManifold;
    }
    if (!hasQuality || !std::isfinite(topology.signedVolume)) return SurfaceMetricValidity::InsufficientQuality;
    return SurfaceMetricValidity::Valid;
}

SurfaceMetricValidity GetAreaValidity(
    const TopologyMetrics& topology,
    const bool isTruncated,
    const bool hasQuality)
{
    if (!hasQuality) return SurfaceMetricValidity::InsufficientQuality;
    if (topology.nonManifoldEdgeCount != 0
        || topology.degenerateTriangleCount != 0
        || !topology.isOrientationValid) {
        return SurfaceMetricValidity::NonManifold;
    }
    if (isTruncated) return SurfaceMetricValidity::Truncated;
    if (topology.boundaryEdgeCount != 0) {
        return SurfaceMetricValidity::OpenSurface;
    }
    return SurfaceMetricValidity::Valid;
}

void WriteObjectResult(const VolumeView &volume, const ResolvedParams &params, const std::uint32_t objectIndex,
                     const DataArray<Point3> &originalPoints, const DataArray<Triangle> &triangles,
                     SurfaceRecordRange records, DataArray<Point3>& classifications,
                     const TopologyMetrics& cachedTopology, SurfaceAlgorithmResult &result, ComponentTotals &totals,
                     const SurfaceCancelCheck &cancelled = {})
{
    std::size_t cancellationBatch = 0;
    const auto firstPoint=result.objects[objectIndex].firstPoint;
    const auto firstTriangle=result.objects[objectIndex].firstTriangle;
    // 当前材料流程保留 seed 位置；直接复用同一几何，省去整组件副本。
    const auto& refinedPoints=originalPoints;
    // 整体翻转不改变边关联、退化和方向一致性；体积仍按最终顺序重新累加。
    auto topology=cachedTopology;
    topology.signedVolume=0;

    std::uint64_t acceptedCount = 0;
    std::uint64_t lowContrastCount = 0;
    bool isTruncated = false;
    const double boundaryTolerance = std::max(
        params.boundaryToleranceModel, 1.0e-8);
    std::array<double, 6> bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    std::size_t pointIndex=0;
    for (SurfacePointRecord& record : records) {
        CheckCancellation(cancellationBatch, cancelled);
        // 风险已在三角面准备时判定；恢复仍在采样后执行，使用采样完成的 seedNormalModel。
        if (GetSurfaceFlag(record.flags,SurfacePointFlags::TriangleFlipRisk)) {
            record.positionModel=originalPoints[pointIndex];
            record.offsetFromSeed=0;
            record.normalModel={GetFiniteFloat(record.seedNormalModel[0]),
                                GetFiniteFloat(record.seedNormalModel[1]),
                                GetFiniteFloat(record.seedNormalModel[2])};
        }
        record.objectIndex = objectIndex;
        bool roiBoundary=false;
        auto& classified=classifications[pointIndex++];
        classified={SurfaceContract::GetPointValid(record,params.method)?1.:0.,
            GetPointAtDataBoundary(volume.geometry,record.positionModel,boundaryTolerance)?1.:0.,
            roiBoundary?1.:0.};
        if (classified[0]!=0) ++acceptedCount;
        if (GetSurfaceFlag(
                record.flags, SurfacePointFlags::LowContrast)) {
            ++lowContrastCount;
        }
        if (GetSurfaceFlag(record.flags, SurfacePointFlags::ProfileClipped) ||
            GetSurfaceFlag(record.flags, SurfacePointFlags::RoiBoundary) ||
            classified[1]!=0 || classified[2]!=0)
        {
            isTruncated = true;
            ++totals.truncated;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            CheckCancellation(cancellationBatch, cancelled);
            bounds[axis * 2] = std::min(
                bounds[axis * 2], record.positionModel[axis]);
            bounds[axis * 2 + 1] = std::max(
                bounds[axis * 2 + 1], record.positionModel[axis]);
        }
    }
    double validArea = 0.0;
    double totalArea = 0.0;
    bool hasCompleteSupport = !triangles.empty();
    // 无体积输出的对象不再累加体积；面积、质量判定和有效体积的顺序保持原样。
    const bool calculateVolume = topology.nonManifoldEdgeCount == 0 && !isTruncated &&
        topology.boundaryEdgeCount == 0 && topology.isOrientationValid && topology.degenerateTriangleCount == 0;
    std::size_t triangleIndex=0;
    for (const auto& triangle : triangles) {
        CheckCancellation(cancellationBatch, cancelled);
        const auto& ids = triangle.vertices;
        const auto edgeA = Subtract(records[ids[1]].positionModel, records[ids[0]].positionModel);
        const auto edgeB = Subtract(records[ids[2]].positionModel, records[ids[0]].positionModel);
        const double doubleArea=GetLength(Cross(edgeA,edgeB));
        const double area = 0.5 * doubleArea;
        if(calculateVolume && hasCompleteSupport && std::isfinite(doubleArea)&&doubleArea>geometryEpsilon)
            topology.signedVolume+=Dot(refinedPoints[ids[0]],Cross(refinedPoints[ids[1]],refinedPoints[ids[2]]))/6.0;
        bool isValid = std::isfinite(area) && area > geometryEpsilon;
        for (const auto id : ids) {
            CheckCancellation(cancellationBatch, cancelled);
            isValid = isValid && classifications[id][0]!=0 && classifications[id][1]==0 && classifications[id][2]==0;
        }
        if (std::isfinite(area)) { totalArea += area; if (isValid) validArea += area; }
        hasCompleteSupport = hasCompleteSupport && isValid;
        result.triangleValidity[std::size_t(firstTriangle)+triangleIndex++]=isValid ? 1 : 0;
    }
    const bool hasFiniteArea = std::isfinite(totalArea) && std::isfinite(validArea);
    const double validAreaRatio = hasFiniteArea && totalArea > 0.0 ? validArea / totalArea : 0.0;
    hasCompleteSupport = hasCompleteSupport && hasFiniteArea;
    const bool hasQuality = hasFiniteArea && validAreaRatio >= qualityRatioThreshold;

    SurfaceObjectRecord object;
    object.objectIndex = objectIndex;
    object.firstPoint = firstPoint;
    object.pointCount = records.size();
    object.firstTriangle = firstTriangle;
    object.triangleCount = triangles.size();
    object.boundsModel = bounds;
    object.validAreaRatio = validAreaRatio;
    object.isClosed = topology.boundaryEdgeCount == 0;
    object.isManifold = topology.nonManifoldEdgeCount == 0
        && topology.degenerateTriangleCount == 0;
    object.isTruncated = isTruncated;
    object.isOrientationValid = topology.isOrientationValid;
    object.areaValidity = GetAreaValidity(
        topology, isTruncated, hasQuality);
    if (object.areaValidity != SurfaceMetricValidity::InsufficientQuality
        && object.areaValidity != SurfaceMetricValidity::NonManifold) {
        object.areaModelUnit2 = validArea;
    }
    object.volumeValidity = GetVolumeValidity(
        topology, isTruncated, hasCompleteSupport);
    if (object.volumeValidity == SurfaceMetricValidity::Valid) {
        object.volumeModelUnit3 = std::abs(topology.signedVolume);
    }

    if (!object.isManifold) ++totals.nonManifold;
    totals.accepted+=acceptedCount;
    totals.lowContrast+=lowContrastCount;
    totals.rejected+=records.size()-acceptedCount;
    const auto pointOffset=static_cast<std::uint32_t>(firstPoint);
    auto indexOffset=std::size_t(firstTriangle)*3;
    for (const auto &triangle : triangles) {
        CheckCancellation(cancellationBatch,cancelled);
        for (const auto pointId : triangle.vertices) {
            CheckCancellation(cancellationBatch,cancelled);
            result.triangleIndices[indexOffset++]=pointOffset+pointId;
        }
    }
    result.objects[objectIndex]=std::move(object);
}

SurfaceFailureReason SetAdditionalInputs(const VtkImageGridSnapshot &source,
                                         const SurfaceDeterminationStartParams &params,
                                         const SurfaceAlgorithmInputs &inputs, VolumeView &volume,
                                         std::string &message)
{
    if (params.analysisRoi.has_value() != static_cast<bool>(inputs.roi)
        || (inputs.roi && (inputs.roi->GetRevision() != *params.analysisRoi
                           || inputs.roi->GetSource() != source->data->self))) {
        message = "Surface ROI snapshot does not match the request/source.";
        return SurfaceFailureReason::InvalidRoi;
    }
    return SurfaceFailureReason::None;
}


SurfaceAlgorithmResult BuildSurfaceImpl(const VtkImageGridSnapshot &source,
                                        const SurfaceDeterminationStartParams &inputParams,
                                        const WorkLimit maxWorkingBytes,
                                        const SurfaceCancelCheck &getCancelled,
                                        const SurfaceProgressCallback &onProgress,
                                        const SurfaceAlgorithmInputs &inputs)
{
    ResultArrayScope resultStorage(maxWorkingBytes.GetBound(8 * 1024 * 1024));
    SurfaceAlgorithmResult result;
    // 报告只保留目录时，不让对象摘要延长整份点/面后备文件的寿命。
    auto objectStorage=std::make_shared<MappedArrayStorage>(maxWorkingBytes.GetBound(8*1024*1024));
    DataArrayAllocator<SurfaceObjectRecord> objectAllocator;objectAllocator.storage=objectStorage;
    result.objects=DataArray<SurfaceObjectRecord>(objectAllocator);
    std::optional<std::pair<SurfaceBackendCalibrationCache::Key,SurfaceBackendCalibration>> completedCalibration;
    char profilingSetting[8]{};
    result.cudaProfilingRequested =
        GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_PROFILE",profilingSetting,sizeof(profilingSetting))==1 &&
        profilingSetting[0]=='1';
    const auto profileStart = [&] {
        return result.cudaProfilingRequested ? std::chrono::steady_clock::now() :
                                              std::chrono::steady_clock::time_point{};
    };
    const auto profileRecord = [&](double &milliseconds, const std::chrono::steady_clock::time_point start) {
        if (result.cudaProfilingRequested)
            milliseconds += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
    result.method = inputParams.method;
    result.algorithmRevision = algorithmRevision;
    const auto fail = [&](SurfaceFailureReason reason, const std::string &message) {
        result.failureReason = reason;
        result.message = message;
        result.status = reason == SurfaceFailureReason::Cancelled ? SurfaceResultStatus::Cancelled
                                                                  : SurfaceResultStatus::Failed;
        DataArray<SurfacePointRecord>().swap(result.points);
        DataArray<std::uint32_t>().swap(result.triangleIndices);
        DataArray<std::uint8_t>().swap(result.triangleValidity);
        DataArray<SurfaceObjectRecord>().swap(result.objects);
        result.interfaces.clear();
        result.acceptedPointCount = 0;
        result.rejectedPointCount = 0;
        result.lowContrastPointCount = 0;
        result.truncatedPointCount = 0;
        result.nonManifoldObjectCount = 0;
        DataArray<SurfaceSelectionPiece>().swap(result.selection);
        result.sharedParent.reset();
        return std::move(result);
    };
    SendProgress(onProgress, SurfaceDeterminationStage::Preparing, 0.01);
    VolumeView volume;
    auto reason = BuildVolumeView(source, volume, result.message);
    if (reason != SurfaceFailureReason::None)
        return fail(reason, result.message);
    reason = SetAdditionalInputs(source, inputParams, inputs, volume, result.message);
    if (reason != SurfaceFailureReason::None)
        return fail(reason, result.message);
    result.requiredBytes = 64 * 1024;
    SendProgress(onProgress, SurfaceDeterminationStage::ThresholdEstimation, 0.05);
    ResolvedParams params;
    params.roi = inputs.roi;
    reason = ResolveParams(volume, inputParams, getCancelled, params, result.message);
    if (reason != SurfaceFailureReason::None)
        return fail(reason, result.message);
    result.resolvedParams = inputParams;
    if (inputParams.method == SurfaceDeterminationMethod::GlobalAutomatic)
    {
        // 估计必须先读原始灰度；此后种子、梯度和诊断统一读同一饱和节点场。
        volume.scalars.materialRange = std::array<double, 2>{params.isoEstimate->backgroundValue,
                                                            params.isoEstimate->materialValue};
        result.resolvedParams.materialRange = volume.scalars.materialRange;
    }
    result.resolvedParams.targetViews = {};
    auto &resolved = result.resolvedParams;
    resolved.purpose = SurfaceContract::GetPurpose(inputParams);
    resolved.sourceVolume = result.sourceRevision;
    resolved.initialIsoValue = params.initialIsoValue;
    result.initialIsoValue = params.initialIsoValue;
    result.isoEstimate = params.isoEstimate;
    const auto cpuPlan = GetCpuSamplingPlan();
    result.availablePhysicalCores = cpuPlan.physicalCores;
    result.availableLogicalThreads = cpuPlan.logicalThreads;
    result.plannedCpuThreads = cpuPlan.threadLimit;
    result.parameterFingerprint = 1469598103934665603ULL;
    AddFingerprint(result.parameterFingerprint, algorithmRevision);
    for (const unsigned char c : SurfaceRecipeCodec::BuildText(resolved))
        AddFingerprint(result.parameterFingerprint, c);
    AddFingerprint(result.parameterFingerprint, params.roi ? 1U : 0U);
    if (params.roi) {
        AddFingerprint(result.parameterFingerprint, roiSchemaVersion);
        for (const auto byte : params.roi->GetRevision().entityId.bytes)
            AddFingerprint(result.parameterFingerprint, byte);
        AddFingerprint(result.parameterFingerprint, params.roi->GetRevision().generation);
    }
    const double halo = params.haloModel;
    SurfaceSeedGrid grid;
    grid.extent = volume.geometry.extent;
    grid.dimensions = volume.geometry.dimensions;
    grid.origin = volume.geometry.origin;
    grid.indexToModel = volume.geometry.indexToModel;
    grid.modelToIndex = volume.geometry.modelToIndex;
    grid.values = &volume.scalars;
    grid.scalarValues = volume.scalars.values;
    grid.scalarType = volume.scalars.vtkType;
    grid.materialRange = volume.scalars.materialRange ? &*volume.scalars.materialRange : nullptr;
    grid.readScalar = [](const void *view, std::size_t index) {
        return static_cast<const ScalarView *>(view)->GetValue(index);
    };
    grid.validity = volume.validity;
    constexpr std::size_t pairIndex = 0, pairCount = 1;
    {
        SurfaceInterfaceRecord interfaceRecord;
        interfaceRecord.canonicalId = "gray";
        interfaceRecord.firstPoint = result.points.size();
        interfaceRecord.firstTriangle = result.triangleIndices.size() / 3;
        std::size_t retained = 0;
        if (!AddWorkingBytes(result.points.capacity(), sizeof(SurfacePointRecord), retained) ||
            !AddWorkingBytes(result.triangleIndices.capacity(), sizeof(std::uint32_t), retained) ||
            !AddWorkingBytes(result.triangleValidity.capacity(), sizeof(std::uint8_t), retained) ||
            !AddWorkingBytes(result.objects.capacity(), sizeof(SurfaceObjectRecord), retained))
            return fail(SurfaceFailureReason::BudgetExceeded,
                        "Surface retained interface byte count overflows.");
        DataArray<Point3> meshPoints;
        DataArray<Triangle> meshTriangles;
        SurfaceExecutionStats statistics;
        SendProgress(onProgress, SurfaceDeterminationStage::SeedExtraction,
                     0.1 + 0.8 * pairIndex / pairCount);
        SurfaceSeedAudit seedAudit;
        grid.audit = &seedAudit;
        const auto seedStatus = SurfaceSeedBuilder::BuildMesh(
            grid, params.initialIsoValue, {}, halo,
            inputParams.seedBlockDepth, maxWorkingBytes, getCancelled, meshPoints,
            meshTriangles, statistics);
        result.requiredBytes =
            std::max(result.requiredBytes, statistics.estimatedWorkingBytes + retained);
        result.seedClassificationCacheBytes = std::max(result.seedClassificationCacheBytes,
                                                       seedAudit.classificationCacheBytes);
        result.seedClassifiedNodeCount += seedAudit.classifiedNodeCount;
        result.seedUniformCellCount += seedAudit.uniformCellCount;
        result.seedCacheBudgetFallbackCount += seedAudit.budgetFallbackCount;
        result.execution.scannedCellCount += statistics.scannedCellCount;
        result.execution.skippedCellCount += statistics.skippedCellCount;
        result.execution.blockCount += statistics.blockCount;
        result.execution.processedExtent = statistics.processedExtent;
        result.execution.seedMs += statistics.seedMs;
        if (seedStatus != SurfaceSeedStatus::Succeeded)
        {
            return fail(seedStatus == SurfaceSeedStatus::Cancelled ? SurfaceFailureReason::Cancelled
                        : seedStatus == SurfaceSeedStatus::BudgetExceeded
                            ? SurfaceFailureReason::BudgetExceeded
                        : seedStatus == SurfaceSeedStatus::NoSurface ? SurfaceFailureReason::NoSurface
                                                                     : SurfaceFailureReason::InvalidGeometry,
                        "Surface seed extraction failed (status=" +
                            std::to_string(static_cast<unsigned>(seedStatus)) + ").");
        }
        if (meshPoints.size() > UINT32_MAX - result.points.size())
            return fail(SurfaceFailureReason::BudgetExceeded, "Surface output exceeds uint32 topology.");
        const auto started = std::chrono::steady_clock::now();
        SendProgress(onProgress, SurfaceDeterminationStage::SubvoxelRefinement,
                     .1 + .8 * (pairIndex + .25) / pairCount);
        auto components =
            BuildComponents(meshPoints, meshTriangles, volume.geometry.voxelVolume, getCancelled);
        result.componentsMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        [[maybe_unused]] const auto releasedTriangleBytes=meshTriangles.capacity()*sizeof(Triangle);
        DataArray<Triangle>().swap(meshTriangles);
        const auto selectionStarted=std::chrono::steady_clock::now();
        const auto selected = GetSelectedComponents(components, meshPoints, params, getCancelled,
            result.componentCandidateCount,result.componentSelectionPointVisits);
        result.componentSelectionMs+=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-selectionStarted).count();
        // 数量已确定后一次预留最终产物，避免追加时同时存活的新旧大数组。
        std::size_t outputPoints=result.points.size(),outputTriangles=result.triangleIndices.size()/3;
        std::size_t outputObjects=0,reservedBytes=0,reservationCancellation=0;
        for(const auto index:selected) {
            CheckCancellation(reservationCancellation,getCancelled);
            if(!GetSum(outputPoints,components[index].sourcePointIds.size(),outputPoints)
                ||!GetSum(outputTriangles,components[index].triangles.size(),outputTriangles))
                return fail(SurfaceFailureReason::BudgetExceeded,"Surface output counts overflow.");
        }
        if(outputTriangles>std::numeric_limits<std::size_t>::max()/3)
            return fail(SurfaceFailureReason::BudgetExceeded,"Surface output indices overflow.");
        if(!GetSum(result.objects.size(),selected.size(),outputObjects)
            ||!AddWorkingBytes(outputPoints,sizeof(SurfacePointRecord),reservedBytes)
            ||!AddWorkingBytes(outputTriangles*3,sizeof(std::uint32_t),reservedBytes)
            ||!AddWorkingBytes(outputTriangles,sizeof(std::uint8_t),reservedBytes)
            ||!AddWorkingBytes(outputObjects,sizeof(SurfaceObjectRecord),reservedBytes))
            return fail(SurfaceFailureReason::BudgetExceeded,"Surface output reservation byte count overflows.");
        if(GetCancelled(getCancelled)) return fail(SurfaceFailureReason::Cancelled,"Surface reservation was cancelled.");
        result.points.reserve(outputPoints);
        result.triangleIndices.reserve(outputTriangles*3);
        result.triangleValidity.reserve(outputTriangles);
        result.objects.reserve(outputObjects);

        const auto objectBase=result.objects.size();
        auto pointOffset=result.points.size(),triangleOffset=result.triangleIndices.size()/3;
        result.points.resize(outputPoints);
        result.triangleIndices.resize(outputTriangles*3);
        result.triangleValidity.resize(outputTriangles);
        result.objects.resize(outputObjects);
        for (std::size_t position=0;position<selected.size();++position) {
            CheckCancellation(reservationCancellation,getCancelled);
            const auto &component=components[selected[position]];
            auto &object=result.objects[objectBase+position];
            object.firstPoint=pointOffset;object.pointCount=component.sourcePointIds.size();
            object.firstTriangle=triangleOffset;object.triangleCount=component.triangles.size();
            pointOffset+=std::size_t(object.pointCount);triangleOffset+=std::size_t(object.triangleCount);
        }
        [[maybe_unused]] const auto largest=std::max_element(selected.begin(),selected.end(),[&](auto a,auto b) {
            return components[a].sourcePointIds.size()<components[b].sourcePointIds.size();
        });
        const auto processComponent=[&](std::size_t position,bool ownsCpuQuota,ComponentTotals &totals) {
            const auto selectedIndex=selected[position];
            const auto objectIndex=static_cast<std::uint32_t>(objectBase+position);
            if (GetCancelled(getCancelled))
                throw SurfaceCancelled{};
            const auto preparationStart=std::chrono::steady_clock::now();
            DataArray<Point3> points;
            DataArray<Triangle> triangles;
            GetLocalMesh(components[selectedIndex], meshPoints, points, triangles,
                         !ownsCpuQuota && components.size() == 1, getCancelled);
            SetOutwardWinding(volume, points, triangles);
            unsigned normalizationThreads = 1;
            auto normals = GetVertexNormals(points, triangles, cpuPlan, normalizationThreads, getCancelled,ownsCpuQuota);
            totals.cpuThreads = std::max(totals.cpuThreads, normalizationThreads);
            SurfaceRecordRange records{result.points.data()+std::size_t(result.objects[objectIndex].firstPoint),points.size()};
            const double cornerCosine = std::cos(params.sharpCornerAngleDeg * 0.5 * 3.141592653589793 / 180);
            SetMaterialTriangleFlags(points,triangles,normals,cornerCosine,records,getCancelled);
            std::atomic<bool> cancelled{false};
            const auto samplingStart=std::chrono::steady_clock::now();
            totals.preparationMs+=std::chrono::duration<double,std::milli>(samplingStart-preparationStart).count();
            {
                SurfaceCudaCompute cuda;
                const auto collectProfile = [&] {
                    if (!result.cudaProfilingRequested || !cuda.GetActive()) return;
                    const auto profile=cuda.GetProfile();
                    if (!profile.requested) return;
                    auto &total=result.cudaProfile;
                    const bool first=!total.requested;
                    total.eventsAvailable=first?profile.eventsAvailable:total.eventsAvailable&&profile.eventsAvailable;
                    total.complete=first?profile.complete:total.complete&&profile.complete;
                    total.requested=1; total.failed=total.failed||profile.failed;
                    total.submittedBatchCount+=profile.submittedBatchCount;
                    total.measuredBatchCount+=profile.measuredBatchCount;
                    total.measuredPointCount+=profile.measuredPointCount;
                    total.slabUploadCount+=profile.slabUploadCount;
                    total.slabUploadBytes+=profile.slabUploadBytes;
                    total.eventSetupMs+=profile.eventSetupMs;
                    total.slabWaitMs+=profile.slabWaitMs;
                    total.slabUploadHostMs+=profile.slabUploadHostMs;
                    total.pointUploadMs+=profile.pointUploadMs;
                    total.kernelMs+=profile.kernelMs;
                    total.sampleDownloadMs+=profile.sampleDownloadMs;
                };
                const auto closeCuda = [&] {
                    if (!cuda.GetActive()) return;
                    collectProfile();
                    const auto started=profileStart();
                    cuda.Reset();
                    profileRecord(result.cudaCloseMs,started);
                };
                const auto sampleCpu = [&](std::size_t begin, std::size_t end) {
                    for (auto i = begin; i < end; ++i) {
                        if (cancelled.load(std::memory_order_acquire)) return;
                        if (((i-begin)&31U)==0 && GetCancelled(getCancelled)) {
                            cancelled.store(true); return;
                        }
                        SetMaterialPoint<true>(volume,params,points[i],normals[i],records[i]);
                        records[i].interfaceIndex=static_cast<std::uint32_t>(pairIndex);
                    }
                };
                bool adaptive=false;
                std::size_t cpuPrefix=0,adaptiveVolumeBytes=0;
                std::optional<SurfaceBackendCalibrationCache::Key> calibrationKey;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                unsigned adaptiveFault=0;
                char adaptiveFaultSetting[4]{};
                if(GetEnvironmentVariableA("MVVCVTK_SURFACE_COST_TEST_FAULT",adaptiveFaultSetting,sizeof(adaptiveFaultSetting))==1 &&
                    adaptiveFaultSetting[0]>='1' && adaptiveFaultSetting[0]<='4')adaptiveFault=unsigned(adaptiveFaultSetting[0]-'0');
#endif
#ifdef MVVCVTK_SURFACE_CUDA_ENABLED
                if (!ownsCpuQuota && largest != selected.end() && selectedIndex == *largest &&
                    (volume.scalars.vtkType == VTK_FLOAT || volume.scalars.vtkType == VTK_DOUBLE)) {
                    SurfaceCudaVolume input;
                    input.values = volume.scalars.values;
                    input.validity = volume.validity;
                    std::copy(volume.geometry.extent.begin(), volume.geometry.extent.end(), input.extent);
                    std::copy(volume.geometry.dimensions.begin(), volume.geometry.dimensions.end(), input.dimensions);
                    std::copy(volume.geometry.origin.begin(), volume.geometry.origin.end(), input.origin);
                    std::copy(volume.geometry.modelToIndex.begin(), volume.geometry.modelToIndex.end(),
                              input.modelToIndex);
                    input.scalarBytes = volume.scalars.vtkType == VTK_FLOAT ? sizeof(float) : sizeof(double);
                    if (volume.scalars.materialRange) {
                        input.hasMaterialRange = 1;
                        std::copy(volume.scalars.materialRange->begin(), volume.scalars.materialRange->end(),
                                  input.materialRange);
                    }
                    if (points.size() >= surfaceCudaBatchCapacity && releasedTriangleBytes > surfaceCudaHostBufferBytes &&
                        !GetCancelled(getCancelled)) {
                        char adaptiveSetting[8]{},backendSetting[16]{};
                        GetEnvironmentVariableA("MVVCVTK_SURFACE_COMPUTE_BACKEND",backendSetting,sizeof(backendSetting));
                        adaptive=MVVCVTK_SURFACE_ADAPTIVE_BACKEND && std::string(backendSetting)!="cpu" &&
                            std::fegetround()==FE_TONEAREST && (_mm_getcsr()&0xffc0U)==0x1f80U &&
                            releasedTriangleBytes>=MVVCVTK_SURFACE_CUDA_DEVICE_BYTES &&
                            releasedTriangleBytes>=MVVCVTK_SURFACE_CUDA_HOST_BYTES &&
                            GetProduct(volume.voxelCount,std::size_t(input.scalarBytes)+(volume.validity?1:0),adaptiveVolumeBytes) &&
                            !(GetEnvironmentVariableA("MVVCVTK_SURFACE_ADAPTIVE_BACKEND",adaptiveSetting,sizeof(adaptiveSetting))==1 && adaptiveSetting[0]=='0');
                        if(adaptive){
                            // 使用最终 records 作为输出；前缀规模由实际核心计划和既有批次容量确定。
                            cpuPrefix=std::min(points.size(),std::max(surfaceCudaBatchCapacity,
                                std::min(points.size()/16,std::size_t(cpuPlan.threadLimit)*surfaceCudaBatchCapacity)));
                            const auto probeStart=std::chrono::steady_clock::now();
                            const auto threads=RunPointSampling(cpuPrefix,cpuPlan.threadLimit,getCancelled,sampleCpu);
                            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-probeStart).count();
                            result.adaptiveCpuPrefixMs+=elapsed;result.adaptiveCpuPrefixPoints+=cpuPrefix;
                            if(result.cudaProfilingRequested)totals.cpuSamplingMs+=elapsed;
                            totals.cpuThreads=std::max(totals.cpuThreads,threads);
                            result.adaptiveCpuPrefixThreads=threads;
                            if(cancelled.load())throw SurfaceCancelled{};
                            char cacheSetting[8]{};
                            bool cacheAllowed=!(GetEnvironmentVariableA("MVVCVTK_SURFACE_COST_CACHE",cacheSetting,sizeof(cacheSetting))==1 && cacheSetting[0]=='0');
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                            for(const auto name:{"MVVCVTK_SURFACE_CUDA_TEST_FAULT","MVVCVTK_SURFACE_FINALIZER_TEST_FAULT","MVVCVTK_SURFACE_COST_TEST_FAULT"}){
                                char fault[8]{};
                                if(GetEnvironmentVariableA(name,fault,sizeof(fault))==1 && fault[0]!='0')cacheAllowed=false;
                            }
#endif
                            const auto deployment=cacheAllowed?SurfaceCudaCompute::GetBackendStamp():0;
                            if(deployment){
                                SurfaceBackendCalibrationCache::Key key{};
                                std::memcpy(&key[0],result.sourceRevision.entityId.bytes.data(),8);
                                std::memcpy(&key[1],result.sourceRevision.entityId.bytes.data()+8,8);
                                key[2]=result.sourceRevision.generation;key[3]=result.parameterFingerprint;
                                key[4]=1469598103934665603ULL;
                                AddFingerprint(key[4],std::uint64_t(volume.scalars.vtkType));
                                AddFingerprint(key[4],volume.validity?1:0);AddFingerprint(key[4],volume.voxelCount);
                                for(const auto value:volume.geometry.extent)AddFingerprint(key[4],std::uint64_t(value));
                                for(const auto value:volume.geometry.origin){std::uint64_t bits=0;std::memcpy(&bits,&value,8);AddFingerprint(key[4],bits);}
                                for(const auto value:volume.geometry.spacing){std::uint64_t bits=0;std::memcpy(&bits,&value,8);AddFingerprint(key[4],bits);}
                                for(const auto value:volume.geometry.direction){std::uint64_t bits=0;std::memcpy(&bits,&value,8);AddFingerprint(key[4],bits);}
                                for(const auto value:volume.geometry.modelToIndex){std::uint64_t bits=0;std::memcpy(&bits,&value,8);AddFingerprint(key[4],bits);}
                                GROUP_AFFINITY affinity{};DWORD_PTR processMask=0,systemMask=0;
                                if(GetThreadGroupAffinity(GetCurrentThread(),&affinity)){
                                    AddFingerprint(key[4],affinity.Mask);AddFingerprint(key[4],affinity.Group);
                                }
                                if(GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask))AddFingerprint(key[4],processMask);
                                key[5]=deployment;key[6]=points.size();key[7]=maxWorkingBytes.GetBound(SIZE_MAX);
                                key[8]=std::uint64_t(cpuPlan.threadLimit)|(std::uint64_t(threads)<<16)|
                                    (std::uint64_t(cpuPlan.physicalCores)<<32)|(std::uint64_t(cpuPlan.logicalThreads)<<48);
                                char finalize[8]{},reuse[8]{};
                                const bool finalizeOn=MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL &&
                                    !(GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL",finalize,sizeof(finalize))==1 && finalize[0]=='0');
                                const bool reuseOn=!(GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_SLAB_REUSE",reuse,sizeof(reuse))==1 && reuse[0]=='0');
                                key[9]=std::uint64_t(finalizeOn)|(std::uint64_t(reuseOn)<<1)|(std::uint64_t(result.cudaProfilingRequested)<<2);
                                calibrationKey=key;result.adaptiveCalibrationBytes=sizeof(SurfaceBackendCalibrationCache);
                                const auto prior=SurfaceBackendCalibrationCache::Get().Take(key,std::chrono::steady_clock::now());
                                if(prior){
                                    const auto admission=GetSurfaceBackendAdmission(*prior,cpuPrefix,elapsed,points.size()-cpuPrefix,adaptiveVolumeBytes);
                                    result.adaptiveCalibrationHit=true;
                                    if(admission.hasEstimate && !admission.keepGpu){
                                        result.adaptiveSkippedStartup=true;result.adaptiveEstimateAvailable=true;
                                        result.adaptiveCpuEstimateMs=admission.cpuTotalMs;result.adaptiveGpuEstimateMs=admission.gpuTotalMs;
                                    }
                                }
                            }
                        }
                        const auto adaptiveStart=std::chrono::steady_clock::now();
                        const auto started=profileStart();
                        bool startAllowed=true;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                        if(adaptive && adaptiveFault==3){
                            std::fprintf(stderr,"Surface cost startup refusal fault entered.\n");startAllowed=false;
                        }
#endif
                        const bool active=cpuPrefix<points.size() && !result.adaptiveSkippedStartup && startAllowed && cuda.Start(input,releasedTriangleBytes,
                            maxWorkingBytes.GetBound(MVVCVTK_SURFACE_CUDA_DEVICE_BYTES));
                        profileRecord(result.cudaStartMs,started);
                        if(adaptive)result.adaptiveStartupMs+=std::chrono::duration<double,std::milli>(
                            std::chrono::steady_clock::now()-adaptiveStart).count();
                        if (active) {
                            result.cudaDeviceBytes = cuda.GetDeviceBytes();
                            result.cudaHostBufferBytes = surfaceCudaHostBufferBytes;
                        }
                    }
                }
#endif
                if (cuda.GetActive()) {
                    const auto pipelineStart=profileStart();
                    const auto adaptiveGpuStarted=std::chrono::steady_clock::now();
                    bool deciding=adaptive;
                    unsigned pilotSubmitted=0;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                    char faultSetting[4]{};
                    const unsigned finalizerFault=GetEnvironmentVariableA("MVVCVTK_SURFACE_FINALIZER_TEST_FAULT",
                        faultSetting,sizeof(faultSetting))==1 && faultSetting[0]>='1' && faultSetting[0]<='3'
                        ?unsigned(faultSetting[0]-'0'):0;
                    const auto cudaOwner=std::this_thread::get_id();
#endif
                    struct PendingBatch final { std::size_t begin=0; SurfaceCudaTicket ticket; };
                    std::array<PendingBatch,surfaceCudaSlotCount> pending;
                    unsigned head=0, queued=0;
                    std::size_t cursor=cpuPrefix;
                    bool healthy=true;
                    const auto finalize=[&](std::size_t begin,std::size_t count,const SurfaceCudaSample *samples) {
                        SurfaceGpuFinalizedBatch finalized;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                        if(begin==cpuPrefix && std::this_thread::get_id()!=cudaOwner && finalizerFault>=2) {
                            std::fprintf(stderr,"Surface finalizer worker fault %u entered.\n",finalizerFault);
                            if(finalizerFault==2){cancelled.store(true);return finalized;}
                            throw std::runtime_error("Surface finalizer worker failure probe.");
                        }
#endif
                        const auto started=profileStart();
                        for (std::size_t i=0;i<count;++i) {
                            if (cancelled.load(std::memory_order_acquire)) break;
                            if ((i&31U)==0 && GetCancelled(getCancelled)) {cancelled.store(true);break;}
                            const auto *accepted=samples && samples[i].support==1 && samples[i].gradientValid
                                ?samples+i:nullptr;
                            const auto id=begin+i;
                            SetMaterialPoint<true>(volume,params,points[id],normals[id],records[id],nullptr,accepted);
                            records[id].interfaceIndex=static_cast<std::uint32_t>(pairIndex);
                            if (accepted) ++finalized.accepted;
                        }
                        profileRecord(finalized.cpuMs,started);
                        return finalized;
                    };
                    using Finalizer=SurfaceGpuFinalizer<decltype(finalize)>;
                    std::optional<CpuSamplingLease> finalizerLease;
                    std::unique_ptr<Finalizer> finalizer;
                    char setting[8]{};
                    const bool allowFinalizers=MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL && cpuPlan.threadLimit>1 &&
                        !(GetEnvironmentVariableA("MVVCVTK_SURFACE_GPU_FINALIZE_PARALLEL",setting,sizeof(setting))==1 && setting[0]=='0');
                    const auto stack=allowFinalizers?GetComponentWorkerStackReserve():std::optional<std::size_t>{};
                    auto requested=stack?std::min(cpuPlan.threadLimit-1,surfaceCudaSlotCount):0U;
                    std::size_t perWorker=0,workspace=0;
                    if (requested && GetSum(*stack,64*1024,perWorker)) {
                        const auto remaining=maxWorkingBytes-result.requiredBytes;
                        const auto available=remaining.GetBound(SIZE_MAX-result.requiredBytes);
                        if (available>=Finalizer::sampleBytes) requested=unsigned(std::min<std::size_t>(requested,
                            (available-Finalizer::sampleBytes)/perWorker));
                        else requested=0;
                    } else requested=0;
                    // 未创建 worker 时 owner 仍执行索引转换和材料回填，也必须占用一个共享槽。
                    finalizerLease.emplace(requested+1,cpuPlan.threadLimit,getCancelled);
                    if (requested) {
                        try {
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                            if(finalizerFault==1){
                                std::fprintf(stderr,"Surface finalizer allocation fault entered.\n");
                                throw std::bad_alloc{};
                            }
#endif
                            // 配额包括 CUDA owner；两个可重用样本槽决定最多同时存在两个后处理任务。
                            if (finalizerLease->GetThreadCount()>1) {
                                finalizer=std::make_unique<Finalizer>(finalizerLease->GetThreadCount()-1,finalize);
                                const auto workers=finalizer->GetThreadCount();
                                if (workers) {
                                    workspace=Finalizer::sampleBytes+workers*perWorker;
                                    result.cudaFinalizeThreads=workers+1;
                                    result.cudaFinalizeWorkspaceBound=std::max(result.cudaFinalizeWorkspaceBound,workspace);
                                    result.requiredBytes+=workspace;
                                    totals.cpuThreads=std::max(totals.cpuThreads,workers+1);
                                } else finalizer.reset();
                            }
                        } catch (const std::bad_alloc &) { finalizer.reset(); }
                    }
                    const auto mergeFinalized=[&](const SurfaceGpuFinalizedBatch &batch) {
                        result.cudaPointCount+=batch.accepted;
                        // 并行时为 worker 累计 CPU 时间，不能当作独占 wall 时间相加。
                        result.cudaFinalizeMs+=batch.cpuMs;
                    };
                    const auto finishFinalizers=[&] {
                        if (finalizer) {
                            for (unsigned slot=0;slot<surfaceCudaSlotCount;++slot) mergeFinalized(finalizer->Finish(slot));
                            finalizer.reset();
                        }
                        finalizerLease.reset();
                    };
                    while (cursor<points.size() || queued) {
                        if (GetCancelled(getCancelled)) {cancelled.store(true);break;}
                        // 两批可同时入队；CPU 回填前一批时 GPU 继续处理后一批。
                        while (healthy && cursor<points.size() && queued<surfaceCudaSlotCount &&
                               (!deciding || pilotSubmitted<surfaceCudaSlotCount)) {
                            const auto slot=(head+queued)%surfaceCudaSlotCount;
                            const auto indexStart=profileStart();
                            auto *indices=cuda.Prepare(slot);
                            if (!indices) {profileRecord(result.cudaIndexMs,indexStart);healthy=false;break;}
                            const auto count=std::min(surfaceCudaBatchCapacity,points.size()-cursor);
                            for (std::size_t i=0;i<count;++i) {
                                const auto index=GetContinuousIndex(volume.geometry,points[cursor+i]);
                                std::copy(index.begin(),index.end(),indices+i*3);
                            }
                            profileRecord(result.cudaIndexMs,indexStart);
                            auto &batch=pending[slot];batch.begin=cursor;
                            const auto submitStart=profileStart();
                            const bool submitted=cuda.Submit(slot,count,batch.ticket);
                            profileRecord(result.cudaSubmitMs,submitStart);
                            if (!submitted) {healthy=false;break;}
                            cursor+=batch.ticket.count; ++queued; ++result.cudaBatchCount;
                            if(deciding)++pilotSubmitted;
                        }
                        if (queued) {
                            const auto &batch=pending[head];
                            const auto collectStart=profileStart();
                            const auto *samples=cuda.Collect(batch.ticket);
                            profileRecord(result.cudaCollectMs,collectStart);
                            if (!samples) healthy=false;
                            if (finalizer) {
                                mergeFinalized(finalizer->Finish(head));
                                finalizer->Submit(head,batch.begin,batch.ticket.count,samples);
                            } else mergeFinalized(finalize(batch.begin,batch.ticket.count,samples));
                            head=(head+1)%surfaceCudaSlotCount; --queued;
                            if (cancelled.load()) break;
                            if(deciding && !queued && healthy){
                                // 探测仅提交现有两个槽；先收完并排空回填，计时包含全部回传与CPU处理。
                                if(finalizer)for(unsigned slot=0;slot<surfaceCudaSlotCount;++slot)
                                    mergeFinalized(finalizer->Finish(slot));
                                const auto elapsed=std::chrono::duration<double,std::milli>(
                                    std::chrono::steady_clock::now()-adaptiveGpuStarted).count();
                                auto traffic=cuda.GetProfile();
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                                if(adaptiveFault==4){
                                    std::fprintf(stderr,"Surface cost missing traffic fault entered.\n");
                                    traffic={};
                                }
#endif
                                const SurfaceBackendProbe probe{cpuPrefix,cursor-cpuPrefix,points.size()-cursor,
                                    adaptiveVolumeBytes,traffic.slabUploadBytes,result.adaptiveCpuPrefixMs,
                                    elapsed,traffic.slabUploadHostMs,result.adaptiveStartupMs};
                                const auto choice=GetSurfaceBackendChoice(probe);
                                if(calibrationKey){
                                    const auto calibration=GetSurfaceBackendCalibration(probe);
                                    if(calibration)completedCalibration=std::make_pair(*calibrationKey,*calibration);
                                }
                                result.adaptiveGpuPilotPoints+=cursor-cpuPrefix;result.adaptiveGpuPilotMs+=elapsed;
                                result.adaptiveEstimateAvailable=choice.hasEstimate;
                                result.adaptiveCpuEstimateMs=choice.cpuTotalMs;result.adaptiveGpuEstimateMs=choice.gpuTotalMs;
                                bool keep=choice.keepGpu;
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
                                if(adaptiveFault==1 || adaptiveFault==2){
                                    std::fprintf(stderr,"Surface cost decision fault %u entered.\n",adaptiveFault);
                                    keep=adaptiveFault==2 && choice.hasEstimate;
                                }
#endif
                                result.adaptiveKeptGpu=keep;
                                deciding=false;
                                if(!keep)healthy=false;
                            }
                        } else if (!healthy) {
                            // 已提交批次收集完再关闭；未提交的连续余段保持原 CPU 计算。
                            finishFinalizers(); // 先释放共享配额，再申请 CPU 余段的点级配额。
                            closeCuda();
                            const auto cpuStarted=profileStart();
                            const auto threads=RunPointSampling(points.size()-cursor,cpuPlan.threadLimit,getCancelled,
                                [&](std::size_t begin,std::size_t end){sampleCpu(cursor+begin,cursor+end);});
                            profileRecord(totals.cpuSamplingMs,cpuStarted);
                            totals.cpuThreads=std::max(totals.cpuThreads,threads);
                            cursor=points.size();
                        }
                    }
                    finishFinalizers();
                    profileRecord(result.cudaPipelineMs,pipelineStart);
                } else {
                    const auto cpuStarted=profileStart();
                    unsigned threads=1;
                    if (ownsCpuQuota) sampleCpu(0,points.size());
                    else if(cpuPrefix<points.size())threads=RunPointSampling(points.size()-cpuPrefix,cpuPlan.threadLimit,getCancelled,
                        [&](std::size_t begin,std::size_t end){sampleCpu(cpuPrefix+begin,cpuPrefix+end);});
                    profileRecord(totals.cpuSamplingMs,cpuStarted);
                    totals.cpuThreads=std::max(totals.cpuThreads,threads);
                }
                closeCuda();
            } // 设备/回传工作区在拓扑分配前释放。
            if (cancelled.load())
                throw SurfaceCancelled{};
            const auto outputStart=std::chrono::steady_clock::now();
            totals.samplingMs+=std::chrono::duration<double,std::milli>(outputStart-samplingStart).count();
            if(!ownsCpuQuota && largest!=selected.end() && selectedIndex==*largest){
                result.largestPointSamplingMs+=std::chrono::duration<double,std::milli>(outputStart-samplingStart).count();
                result.largestPointCount+=points.size();result.largestTriangleCount+=triangles.size();
            }
            if (!ownsCpuQuota) SendProgress(onProgress, SurfaceDeterminationStage::TopologyValidation,
                                           .1 + .8 * (pairIndex + .85) / pairCount);
            WriteObjectResult(volume,params,objectIndex,points,triangles,records,normals,
                              components[selectedIndex].topology,result,totals,getCancelled);
            totals.outputMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-outputStart).count();
            result.objects[objectIndex].interfaceIndex=static_cast<std::uint32_t>(pairIndex);
        };

        const auto serialWorkingBound=result.requiredBytes;
        char componentSetting[8]{};
        const bool enableComponents=MVVCVTK_SURFACE_COMPONENT_PARALLEL && cpuPlan.threadLimit>1 &&
            !(GetEnvironmentVariableA("MVVCVTK_SURFACE_COMPONENT_PARALLEL",componentSetting,sizeof(componentSetting))==1 &&
              componentSetting[0]=='0');
        const auto stackReserve=enableComponents?GetComponentWorkerStackReserve():std::optional<std::size_t>{};
        const auto eligible=[&](std::size_t position) {
            return components[selected[position]].sourcePointIds.size()<=8192 &&
                (largest==selected.end() || selected[position]!=*largest);
        };
        for (std::size_t position=0;position<selected.size();) {
            auto end=position;
            std::size_t pointWork=0,maxPoints=0;
            if (enableComponents && stackReserve) while (end<selected.size() && eligible(end)) {
                CheckCancellation(reservationCancellation,getCancelled);
                const auto count=components[selected[end]].sourcePointIds.size();
                pointWork+=count;maxPoints=std::max(maxPoints,count);++end;
            }
            auto requested=unsigned(std::min<std::size_t>(cpuPlan.threadLimit,pointWork/8192));
            std::size_t perWorker=0;
            if (requested>=2) {
                // 工作区为一件组件的 points+normals、宿主默认栈及有界线程/分配 bookkeeping。
                const bool valid=GetSum(*stackReserve,64*1024,perWorker) &&
                    AddWorkingBytes(maxPoints,2*sizeof(Point3),perWorker);
                if (valid && perWorker<=SIZE_MAX/requested) requested=unsigned(
                    (maxWorkingBytes-serialWorkingBound).GetBound(std::min(
                        std::size_t(requested)*perWorker,SIZE_MAX-serialWorkingBound))/perWorker);
                else requested=0;
                if (requested<2) ++result.componentBudgetFallbackCount;
            }
            if (requested>=2) {
                CpuSamplingLease lease(requested,cpuPlan.threadLimit,getCancelled);
                const auto count=lease.GetThreadCount();
                if (count>=2) {
                    constexpr auto capacity=sizeof(KAFFINITY)*CHAR_BIT/2;
                    std::array<ComponentTotals,capacity> workerTotals{};
                    SendProgress(onProgress,SurfaceDeterminationStage::TopologyValidation,
                                 .1+.8*(pairIndex+.85)/pairCount);
                    const auto poolStarted=std::chrono::steady_clock::now();
                    const auto actual=RunComponentJobs(position,end,count,getCancelled,
                        [&](std::size_t item,unsigned worker){processComponent(item,true,workerTotals[worker]);});
                    result.componentParallelMs+=std::chrono::duration<double,std::milli>(
                        std::chrono::steady_clock::now()-poolStarted).count();
                    result.componentCpuThreads=std::max(result.componentCpuThreads,actual);
                    result.cpuThreadCount=std::max(result.cpuThreadCount,actual);
                    result.componentCpuPointCount+=pointWork;
                    const auto workspace=std::size_t(count)*perWorker;
                    result.componentWorkspaceBound=std::max(result.componentWorkspaceBound,workspace);
                    result.requiredBytes=std::max(result.requiredBytes,serialWorkingBound+workspace);
                    for (unsigned worker=0;worker<actual;++worker) MergeComponentTotals(result,workerTotals[worker]);
                    position=end;
                    continue;
                }
                // lease 只有一个可用槽时先结束租约，再走保留的点级调度。
            }
            // 不够分摊调度或预算不足时一次处理已扫描整段，避免重扫后缀形成平方复杂度。
            const auto serialEnd=end>position?end:position+1;
            ComponentTotals totals;
            while (position<serialEnd) processComponent(position++,false,totals);
            MergeComponentTotals(result,totals);
        }
        result.execution.refinementMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        interfaceRecord.pointCount = result.points.size() - interfaceRecord.firstPoint;
        interfaceRecord.triangleCount = result.triangleIndices.size() / 3 - interfaceRecord.firstTriangle;
        result.interfaces.push_back(std::move(interfaceRecord));
    }
    if (GetCancelled(getCancelled))
        return fail(SurfaceFailureReason::Cancelled, "Surface validation was cancelled.");
    if (result.points.empty() || result.triangleIndices.empty())
        return fail(SurfaceFailureReason::NoSurface, "Surface output is empty.");
    if (inputs.roi) {
        // 在完整正式表面上选片，保留父三角形及重心来源，不生成封口或移动新点。
        for (std::size_t i=0;i<result.triangleIndices.size();i+=3) {
            if (GetCancelled(getCancelled)) return fail(SurfaceFailureReason::Cancelled,"ROI surface selection cancelled.");
            const std::array<Point3,3> triangle{result.points[result.triangleIndices[i]].positionModel,
                result.points[result.triangleIndices[i+1]].positionModel,result.points[result.triangleIndices[i+2]].positionModel};
            const auto a=Subtract(triangle[1],triangle[0]),b=Subtract(triangle[2],triangle[0]);
            const double aa=Dot(a,a),bb=Dot(b,b),ab=Dot(a,b),denominator=aa*bb-ab*ab;
            if (!(denominator>0)) continue;
            const auto selected=inputs.roi->GetTriangleChunks(triangle,1024,[&](const auto& pieces){
                for(const auto& piece:pieces){
                SurfaceSelectionPiece record;record.parentTriangle=i/3;record.points=piece;
                for (int p=0;p<3;++p) {const auto q=Subtract(piece[p],triangle[0]);const double aq=Dot(a,q),bq=Dot(b,q);
                    const double v=(bb*aq-ab*bq)/denominator,w=(aa*bq-ab*aq)/denominator;record.barycentric[p]={1-v-w,v,w};}
                const auto u=Subtract(piece[1],piece[0]),v=Subtract(piece[2],piece[0]);
                result.selectedAreaMM2+=0.5*std::hypot(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]);
                result.selection.push_back(std::move(record));

                }return !GetCancelled(getCancelled);
            },0,getCancelled);
            if(selected.error!=RoiError::None)return fail(selected.error==RoiError::Cancelled?SurfaceFailureReason::Cancelled:selected.error==RoiError::TooLarge?SurfaceFailureReason::BudgetExceeded:SurfaceFailureReason::UnsupportedRoi,"ROI triangle selection failed.");
            result.selectionToleranceMM=std::max(result.selectionToleranceMM,selected.boundaryTolerance);
        }
    }
    result.execution.estimatedWorkingBytes = result.requiredBytes;
    result.execution.retainedBytes = result.points.capacity() * sizeof(SurfacePointRecord) +
                                     result.triangleIndices.capacity() * sizeof(std::uint32_t) +
                                     result.triangleValidity.capacity() +
                                     result.objects.capacity() * sizeof(SurfaceObjectRecord);
    result.status = SurfaceResultStatus::Succeeded;
    result.failureReason = SurfaceFailureReason::None;
    result.message = "Surface determination succeeded: points=" + std::to_string(result.points.size()) +
                     ", accepted=" + std::to_string(result.acceptedPointCount) + ".";
    SendProgress(onProgress, SurfaceDeterminationStage::TopologyValidation, 1);
    if(completedCalibration)SurfaceBackendCalibrationCache::Get().Store(completedCalibration->first,
        completedCalibration->second,std::chrono::steady_clock::now());
    resultStorage.Finish();
    objectStorage->Finish();
    return result;
}

SurfaceAlgorithmResult SelectParentSurface(const VtkImageGridSnapshot& source,const SurfaceDeterminationStartParams& params,WorkLimit maxWorkingBytes,const SurfaceCancelCheck& getCancelled,const SurfaceAlgorithmInputs& inputs)
{
    ResultArrayScope resultStorage(maxWorkingBytes.GetBound(8*1024*1024));
    SurfaceAlgorithmResult result;
    const auto fail=[&](SurfaceFailureReason reason,const std::string& message){result.failureReason=reason;result.status=reason==SurfaceFailureReason::Cancelled?SurfaceResultStatus::Cancelled:SurfaceResultStatus::Failed;result.message=message;DataArray<SurfaceSelectionPiece>().swap(result.selection);result.sharedParent.reset();return std::move(result);};
    const auto& parent=*inputs.parent;
    if(!source || !source->data || parent.sourceRevision!=source->data->self || !parent.points || !parent.triangleIndices || !parent.objects || !parent.triangleValidity
        || !params.parentGeneration || parent.dataRevision!=*params.parentGeneration || parent.algorithmRevision!=surfaceAlgorithmRevision)
        return fail(SurfaceFailureReason::InvalidGeometry,"Parent surface does not match the source/revision.");
    if(GetCancelled(getCancelled))return fail(SurfaceFailureReason::Cancelled,"Surface selection cancelled.");
    const auto inputError=SurfaceDeterminationAlgorithm::GetInputFailure(source,params,inputs);
    if(inputError!=SurfaceFailureReason::None)return fail(inputError,"Surface selection inputs are invalid.");
    std::size_t bytes=0;
    if(!AddWorkingBytes(parent.points->size(),sizeof(SurfacePointRecord),bytes) || !AddWorkingBytes(parent.triangleIndices->size(),sizeof(std::uint32_t),bytes)
        || !AddWorkingBytes(parent.triangleValidity->size(),sizeof(std::uint8_t),bytes) || !AddWorkingBytes(parent.objects->size(),sizeof(SurfaceObjectRecord),bytes))
        return fail(SurfaceFailureReason::BudgetExceeded,"Parent surface byte count overflows.");
    result.sourceRevision=parent.sourceRevision;result.algorithmRevision=parent.algorithmRevision;result.method=parent.method;result.isoEstimate=parent.isoEstimate;
    result.resolvedParams=parent.resolvedParams;result.resolvedParams.analysisRoi=params.analysisRoi;result.resolvedParams.parentGeneration=params.parentGeneration;
    result.sharedParent=inputs.parent;
    const auto& points=*parent.points;const auto& triangles=*parent.triangleIndices;
    if(parent.interfaces)result.interfaces=*parent.interfaces;
    result.parameterFingerprint=parent.parameterFingerprint;
    for(auto byte:parent.dataRevision.entityId.bytes){result.parameterFingerprint^=byte;result.parameterFingerprint*=1099511628211ULL;}
    if(inputs.roi){for(auto byte:inputs.roi->GetRevision().entityId.bytes){result.parameterFingerprint^=byte;result.parameterFingerprint*=1099511628211ULL;}result.parameterFingerprint^=inputs.roi->GetRevision().generation;}
    std::size_t checked=0;
    for(const auto& point:points){CheckCancellation(checked,getCancelled);if(SurfaceContract::GetPointValid(point,parent.method))++result.acceptedPointCount;else ++result.rejectedPointCount;if(GetSurfaceFlag(point.flags,SurfacePointFlags::LowContrast))++result.lowContrastPointCount;}
    if (inputs.roi) {
        // 在完整正式表面上选片，保留父三角形及重心来源，不生成封口或移动新点。
        for (std::size_t i=0;i<triangles.size();i+=3) {
            if (GetCancelled(getCancelled)) return fail(SurfaceFailureReason::Cancelled,"ROI surface selection cancelled.");
            const std::array<Point3,3> triangle{points[triangles[i]].positionModel,
                points[triangles[i+1]].positionModel,points[triangles[i+2]].positionModel};
            const auto a=Subtract(triangle[1],triangle[0]),b=Subtract(triangle[2],triangle[0]);
            const double aa=Dot(a,a),bb=Dot(b,b),ab=Dot(a,b),denominator=aa*bb-ab*ab;
            if (!(denominator>0)) continue;
            const auto selected=inputs.roi->GetTriangleChunks(triangle,1024,[&](const auto& pieces){
                for(const auto& piece:pieces){
                SurfaceSelectionPiece record;record.parentTriangle=i/3;record.points=piece;
                for (int p=0;p<3;++p) {const auto q=Subtract(piece[p],triangle[0]);const double aq=Dot(a,q),bq=Dot(b,q);
                    const double v=(bb*aq-ab*bq)/denominator,w=(aa*bq-ab*aq)/denominator;record.barycentric[p]={1-v-w,v,w};}
                const auto u=Subtract(piece[1],piece[0]),v=Subtract(piece[2],piece[0]);
                result.selectedAreaMM2+=0.5*std::hypot(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]);
                result.selection.push_back(std::move(record));

                }return !GetCancelled(getCancelled);
            },0,getCancelled);
            if(selected.error!=RoiError::None)return fail(selected.error==RoiError::Cancelled?SurfaceFailureReason::Cancelled:selected.error==RoiError::TooLarge?SurfaceFailureReason::BudgetExceeded:SurfaceFailureReason::UnsupportedRoi,"ROI triangle selection failed.");
            result.selectionToleranceMM=std::max(result.selectionToleranceMM,selected.boundaryTolerance);
        }
    }
    if(!AddWorkingBytes(result.selection.size(),sizeof(SurfaceSelectionPiece),bytes))return fail(SurfaceFailureReason::BudgetExceeded,"Surface selected fragment size overflows.");
    result.requiredBytes=bytes;result.execution.estimatedWorkingBytes=bytes;result.execution.retainedBytes=bytes;
    resultStorage.Finish();
    result.status=SurfaceResultStatus::Succeeded;result.failureReason=SurfaceFailureReason::None;result.message="Selected the immutable parent surface without measurement.";return result;
}

} // namespace

SurfaceAlgorithmResult SurfaceDeterminationAlgorithm::BuildSurface(
    const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &params,
    const WorkLimit maxWorkingBytes, const SurfaceCancelCheck &getCancelled,
    const SurfaceProgressCallback &onProgress, const SurfaceAlgorithmInputs &inputs)
{
    try {
        if(inputs.parent)return SelectParentSurface(source,params,maxWorkingBytes,getCancelled,inputs);
        return BuildSurfaceImpl(source, params, maxWorkingBytes, getCancelled, onProgress, inputs);
    }
    catch (const SurfaceCancelled &)
    {
        SurfaceAlgorithmResult result;
        result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
        result.status = SurfaceResultStatus::Cancelled;
        result.failureReason = SurfaceFailureReason::Cancelled;
        result.message = "Surface topology operation was cancelled.";
        return result;
    }
    catch (const std::bad_alloc&) {
        SurfaceAlgorithmResult result;
        result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
        result.method = params.method;
        result.failureReason = SurfaceFailureReason::BudgetExceeded;
        result.message = "Surface storage allocation failed.";
        return result;
    }
    catch (...) {
        SurfaceAlgorithmResult result;
        result.sourceRevision = source && source->data ? source->data->self : DataRevisionRef{};
        result.method = params.method;
        result.failureReason = SurfaceFailureReason::InternalError;
        result.message = "Surface determination raised an internal error.";
        return result;
    }
}

SurfacePointDiagnostic SurfaceDeterminationAlgorithm::GetPointDiagnostic(
    const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &resolved,
    const SurfacePointRecord &point, const SurfaceAlgorithmInputs &inputs)
{
    SurfacePointDiagnostic diagnostic;
    try
    {
        VolumeView volume;
        if (BuildVolumeView(source, volume, diagnostic.message) != SurfaceFailureReason::None)
            return diagnostic;
        if (SetAdditionalInputs(source, resolved, inputs, volume, diagnostic.message) !=
            SurfaceFailureReason::None)
            return diagnostic;
        if (!resolved.initialIsoValue)
        {
            diagnostic.message = "Diagnostic replay requires a resolved recipe.";
            return diagnostic;
        }
        ResolvedParams params;
        params.roi = inputs.roi;
        if (ResolveParams(volume, resolved, {}, params, diagnostic.message) != SurfaceFailureReason::None)
            return diagnostic;
        if (resolved.method == SurfaceDeterminationMethod::GlobalAutomatic)
            volume.scalars.materialRange = std::array<double, 2>{params.isoEstimate->backgroundValue,
                                                                params.isoEstimate->materialValue};
        SurfacePointRecord replay;
        const Point3 normal{point.seedNormalModel[0], point.seedNormalModel[1], point.seedNormalModel[2]};
        SetMaterialPoint(volume, params, point.seedPositionModel, normal, replay, &diagnostic);
        // 拓扑/覆盖区边界是网格级判定；保留发布记录，使调用方能区分节点定位与最终质量。
        diagnostic.point = point;
        diagnostic.message = "Point replay uses the frozen source, resolved recipe and original seed; "
                             "point includes final mesh quality flags.";
    }
    catch (const std::bad_alloc &)
    {
        diagnostic = {};
        diagnostic.message = "Diagnostic workspace allocation failed.";
    }
    catch (...)
    {
        diagnostic = {};
        diagnostic.message = "Diagnostic replay failed.";
    }
    return diagnostic;
}

SurfaceFailureReason SurfaceDeterminationAlgorithm::GetInputFailure(
    const VtkImageGridSnapshot &source, const SurfaceDeterminationStartParams &params,
    const SurfaceAlgorithmInputs &inputs)
{
    try
    {
        VolumeView volume;
        std::string message;
        const auto reason = BuildVolumeView(source, volume, message);
        return reason == SurfaceFailureReason::None
                   ? SetAdditionalInputs(source, params, inputs, volume, message)
                   : reason;
    }
    catch (...)
    {
        return SurfaceFailureReason::InvalidSource;
    }
}
