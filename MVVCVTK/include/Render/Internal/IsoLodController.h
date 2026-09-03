#pragma once

#include "App/VolumePresentationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// 等值面独立 LOD 计划器；只解析提取输入尺寸，不携带体渲染采样或 GPU 策略。
class IsoLodController final {
public:
    struct Source final {
        std::array<int, 3> dimensions{};
        std::uint64_t nativeBytes = 0;
        std::uint64_t maskBytes = 0;
        std::uint64_t systemMemoryBytes = 0;
        unsigned int cpuThreadCount = 1;
    };

    struct Profile final {
        std::array<int, 3> outputDimensions{};
        double dimensionRatio = 1.0;
    };

    IsoLodController();

    bool Reset();
    bool SetQuality(VolumeQuality quality);
    bool SetSource(const Source& source);

    VolumeQuality GetQuality() const noexcept;
    Profile GetProfile() const noexcept;
    Profile GetProfile(VolumeQuality quality) const noexcept;

private:
    static std::optional<std::size_t> GetQualityIndex(
        VolumeQuality quality) noexcept;
    static double GetQuantizedRatio(double dimensionRatio) noexcept;
    static std::optional<std::uint64_t> GetVoxelCount(
        const std::array<int, 3>& dimensions) noexcept;
    static std::optional<std::array<int, 3>> GetScaledDimensions(
        const std::array<int, 3>& dimensions,
        double dimensionRatio) noexcept;
    Profile BuildProfile(VolumeQuality quality) const noexcept;
    double GetAutoRatio() const noexcept;

    Source m_source;
    std::array<Profile, 5> m_profiles{};
    VolumeQuality m_quality = VolumeQuality::Auto;
};
