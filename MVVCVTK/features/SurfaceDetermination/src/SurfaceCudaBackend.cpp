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
                                sizeof(memory))
               ? memory.PrivateUsage
               : 0;
}
struct Session final {
    SurfaceCudaVolume source{};
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUfunction function = nullptr;
    CUdeviceptr values = 0, mask = 0, points = 0, samples = 0;
    std::size_t planeCount = 0, planeBytes = 0, sliceCapacity = 0;
    int cachedFirst = 0, cachedCount = 0;
    bool createdCurrent = false;
    DWORD ownerThread = GetCurrentThreadId();
    ~Session() {
        if (!context)
            return;
        CUcontext popped = nullptr;
        if (createdCurrent)
            cuCtxPopCurrent(&popped);
        if (cuCtxPushCurrent(context) != CUDA_SUCCESS) {
            cuCtxDestroy(context);
            return;
        }
        if (samples)
            cuMemFree(samples);
        if (points)
            cuMemFree(points);
        if (mask)
            cuMemFree(mask);
        if (values)
            cuMemFree(values);
        if (module)
            cuModuleUnload(module);
        cuCtxPopCurrent(&popped);
        cuCtxDestroy(context);
    }
};
} // namespace
extern "C" __declspec(dllexport) int SurfaceCudaCreate(const SurfaceCudaVolume *source,
                                                       std::size_t deviceLimit, std::size_t hostLimit,
                                                       void **handle, std::size_t *deviceBytes) {
    *handle = nullptr;
    *deviceBytes = 0;
    const auto before = PrivateBytes();
    try {
        if ((source->scalarBytes != 4 && source->scalarBytes != 8) || cuInit(0) != CUDA_SUCCESS)
            return 0;
        CUdevice device;
        if (cuDeviceGet(&device, 0) != CUDA_SUCCESS)
            return 0;
        auto s = std::make_unique<Session>();
        s->source = *source;
#if CUDA_VERSION >= 13000
        if (cuCtxCreate(&s->context, nullptr, 0, device) != CUDA_SUCCESS)
            return 0;
#else
        if (cuCtxCreate(&s->context, 0, device) != CUDA_SUCCESS)
            return 0;
#endif
        s->createdCurrent = true;
        if (cuModuleLoadData(&s->module, surfaceCudaPtx) != CUDA_SUCCESS ||
            cuModuleGetFunction(&s->function, s->module, "SurfaceSamplePoints") != CUDA_SUCCESS)
            return 0;
        const auto fixed = surfaceCudaBatchCapacity * (3 * sizeof(double) + sizeof(SurfaceCudaSample));
        s->planeCount = std::size_t(source->dimensions[0]) * source->dimensions[1];
        s->planeBytes = s->planeCount * source->scalarBytes;
        if (!s->planeBytes || deviceLimit <= fixed)
            return 0;
        const auto perSlice = s->planeBytes + (source->validity ? s->planeCount : 0);
        s->sliceCapacity = std::min(std::size_t(source->dimensions[2]), (deviceLimit - fixed) / perSlice);
        if (s->sliceCapacity < 4)
            return 0;
        if (cuMemAlloc(&s->values, s->planeBytes * s->sliceCapacity) != CUDA_SUCCESS ||
            (source->validity && cuMemAlloc(&s->mask, s->planeCount * s->sliceCapacity) != CUDA_SUCCESS) ||
            cuMemAlloc(&s->points, surfaceCudaBatchCapacity * 3 * sizeof(double)) != CUDA_SUCCESS ||
            cuMemAlloc(&s->samples, surfaceCudaBatchCapacity * sizeof(SurfaceCudaSample)) != CUDA_SUCCESS)
            return 0;
        const auto after = PrivateBytes();
        if (!before || !after || (after > before && after - before > hostLimit))
            return 0;
        *deviceBytes = fixed + perSlice * s->sliceCapacity;
        CUcontext popped = nullptr;
        if (cuCtxPopCurrent(&popped) != CUDA_SUCCESS)
            return 0;
        s->createdCurrent = false;
        *handle = s.release();
        return 1;
    } catch (...) {
        return 0;
    }
}
extern "C" __declspec(dllexport) int SurfaceCudaReadPoints(void *handle, const double *points,
                                                           std::size_t count, SurfaceCudaSample *output) {
    auto &s = *static_cast<Session *>(handle);
    if (GetCurrentThreadId() != s.ownerThread || !count || count > surfaceCudaBatchCapacity)
        return 0;
    struct Restore final {
        bool pushed = false;
        ~Restore() {
            if (pushed) {
                CUcontext popped = nullptr;
                cuCtxPopCurrent(&popped);
            }
        }
    } restore;
    if (cuCtxPushCurrent(s.context) != CUDA_SUCCESS)
        return 0;
    restore.pushed = true;
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (std::size_t i = 0; i < count; ++i) {
        const auto z = points[3 * i + 2];
        low = std::min(low, z);
        high = std::max(high, z);
    }
    if (!std::isfinite(low) || !std::isfinite(high))
        return 0;
    const int first = int(std::max(double(s.source.extent[4]), std::floor(low) - 2));
    const int last = int(std::min(double(s.source.extent[5]), std::ceil(high) + 2));
    if (last < first || std::size_t(last - first + 1) > s.sliceCapacity)
        return 0;
    if (!s.cachedCount || first < s.cachedFirst || last >= s.cachedFirst + s.cachedCount) {
        s.cachedFirst = first;
        s.cachedCount = int(std::min(s.sliceCapacity, std::size_t(s.source.extent[5] - first + 1)));
        const auto offset = std::size_t(first - s.source.extent[4]);
        if (cuMemcpyHtoD(s.values,
                         static_cast<const unsigned char *>(s.source.values) + offset * s.planeBytes,
                         std::size_t(s.cachedCount) * s.planeBytes) != CUDA_SUCCESS ||
            (s.mask && cuMemcpyHtoD(s.mask, s.source.validity + offset * s.planeCount,
                                    std::size_t(s.cachedCount) * s.planeCount) != CUDA_SUCCESS))
            return 0;
    }
    auto v = s.source;
    v.firstSlice = s.cachedFirst;
    v.sliceCount = s.cachedCount;
    if (cuMemcpyHtoD(s.points, points, count * 3 * sizeof(double)) != CUDA_SUCCESS)
        return 0;
    v.values = reinterpret_cast<const void *>(s.values);
    v.validity = s.mask ? reinterpret_cast<const unsigned char *>(s.mask) : nullptr;
    auto pointAddress = s.points, sampleAddress = s.samples;
    void *arguments[]{&v, &pointAddress, &count, &sampleAddress};
    if (cuLaunchKernel(s.function, unsigned((count + 127) / 128), 1, 1, 128, 1, 1, 0, nullptr, arguments,
                       nullptr) != CUDA_SUCCESS ||
        cuCtxSynchronize() != CUDA_SUCCESS ||
        cuMemcpyDtoH(output, s.samples, count * sizeof(SurfaceCudaSample)) != CUDA_SUCCESS)
        return 0;
    return 1;
}
extern "C" __declspec(dllexport) void SurfaceCudaDestroy(void *handle) noexcept {
    delete static_cast<Session *>(handle);
}
