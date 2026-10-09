#include "SurfaceCudaTypes.h"
#include "SurfaceCudaPtx.h"
#include <cuda.h>
#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

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
bool SetSlab(Session &s, int first, int last) {
    if (s.cachedCount && first >= s.cachedFirst && std::int64_t(last)-s.cachedFirst < s.cachedCount) return true;
    // 两个 stream 共用体素 slab；所有旧读者完成后才能覆写。
    for (const auto &slot : s.slots)
        if (slot.pending && cuEventSynchronize(slot.done) != CUDA_SUCCESS) return false;
    s.cachedCount = 0;
    const auto count = int(std::min(s.sliceCapacity, std::size_t(s.source.extent[5] - first + 1)));
    const auto offset = std::size_t(first - s.source.extent[4]);
    if (cuMemcpyHtoD(s.values, static_cast<const unsigned char *>(s.source.values) + offset * s.planeBytes,
                     std::size_t(count) * s.planeBytes) != CUDA_SUCCESS ||
        (s.mask && cuMemcpyHtoD(s.mask, s.source.validity + offset * s.planeCount,
                               std::size_t(count) * s.planeCount) != CUDA_SUCCESS)) return false;
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
    if (cuMemcpyHtoDAsync(slot.points,slot.Indices(),accepted*3*sizeof(double),slot.stream) != CUDA_SUCCESS) {
        s.failed = true; return 0;
    }
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    if (s.testFault == 1) {s.testFault=0;s.failed=true;return 0;}
#endif
    if (
        cuLaunchKernel(s.function,unsigned((accepted+127)/128),1,1,128,1,1,0,slot.stream,arguments,nullptr) != CUDA_SUCCESS ||
        cuMemcpyDtoHAsync(slot.Samples(),slot.samples,accepted*sizeof(SurfaceCudaSample),slot.stream) != CUDA_SUCCESS ||
        cuEventRecord(slot.done,slot.stream) != CUDA_SUCCESS) { s.failed = true; return 0; }
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
    slot.pending=false; *samples=slot.Samples(); return 1;
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
    if (!handle || fault>3) return 0;
    auto &s=*static_cast<Session *>(handle);
    if (!GetOwned(s)) return 0;
    s.testFault=fault;return 1;
}
#endif
