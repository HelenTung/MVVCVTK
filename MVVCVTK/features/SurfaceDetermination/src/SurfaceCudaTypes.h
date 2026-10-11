#pragma once
#include <cstddef>
#include <cstdint>
#include <array>

// 仅用于私有 DLL/设备核之间；不携带 VTK、Host 或发布对象身份。
struct SurfaceCudaVolume final {
    const void *values = nullptr;
    const unsigned char *validity = nullptr;
    int extent[6]{};
    int dimensions[3]{};
    double origin[3]{};
    double modelToIndex[9]{};
    double materialRange[2]{};
    int scalarBytes = 4;
    int hasMaterialRange = 0;
    int firstSlice = 0;
    int sliceCount = 0;
};
struct SurfaceCudaSample final {
    double gradient[3]{};
    double magnitude = 0;
    int gradientValid = 0;
    int support = 0; // 0=clipped, 1=valid, 2=invalid mask, 3=tile miss。
};
inline constexpr std::size_t surfaceCudaBatchCapacity = 8192;
inline constexpr unsigned surfaceCudaSlotCount = 2;
inline constexpr std::size_t surfaceCudaHostBufferBytes =
    surfaceCudaSlotCount * surfaceCudaBatchCapacity * (3 * sizeof(double) + sizeof(SurfaceCudaSample));
struct SurfaceCudaTicket final {
    unsigned slot = 0;
    std::uint64_t generation = 0;
    std::size_t count = 0;
    const void *owner = nullptr;
    std::uint64_t epoch = 0;
};
// 可选仓内剖析快照；不改变既有 ticket、kernel 参数或计算函数的私有 ABI。
struct SurfaceCudaProfile final {
    int requested = 0, eventsAvailable = 0, complete = 0, failed = 0;
    std::uint64_t submittedBatchCount = 0, measuredBatchCount = 0, measuredPointCount = 0;
    std::uint64_t slabUploadCount = 0, slabUploadBytes = 0;
    double eventSetupMs = 0, slabWaitMs = 0, slabUploadHostMs = 0;
    double pointUploadMs = 0, kernelMs = 0, sampleDownloadMs = 0;
};
using SurfaceCudaOpen = int (*)(const SurfaceCudaVolume *, std::size_t, std::size_t, void **, std::size_t *);
using SurfaceCudaRead = int (*)(void *, const double *, std::size_t, SurfaceCudaSample *);
using SurfaceCudaPrepareCallback = int (*)(void *, unsigned, double **);
using SurfaceCudaSubmitCallback = int (*)(void *, unsigned, std::size_t, SurfaceCudaTicket *);
using SurfaceCudaCollectCallback = int (*)(void *, const SurfaceCudaTicket *, const SurfaceCudaSample **);
using SurfaceCudaTestFaultCallback = int (*)(void *, unsigned);
using SurfaceCudaProfileCallback = int (*)(void *, SurfaceCudaProfile *);
using SurfaceCudaClose = void (*)(void *) noexcept;
