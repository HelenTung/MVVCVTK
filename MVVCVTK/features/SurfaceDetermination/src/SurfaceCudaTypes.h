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
inline constexpr std::size_t surfaceCudaBatchCapacity = 1024;
struct SurfaceCudaBatch final {
    std::array<std::array<double, 3>, surfaceCudaBatchCapacity> indices{};
    std::array<SurfaceCudaSample, surfaceCudaBatchCapacity> samples{};
};
using SurfaceCudaOpen = int (*)(const SurfaceCudaVolume *, std::size_t, std::size_t, void **, std::size_t *);
using SurfaceCudaRead = int (*)(void *, const double *, std::size_t, SurfaceCudaSample *);
using SurfaceCudaClose = void (*)(void *) noexcept;
