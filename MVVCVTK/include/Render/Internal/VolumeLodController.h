#pragma once

#include "App/VolumePresentationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

class VolumeLodController final {
public:
    struct Source final {
        std::array<int, 3> dimensions{};
        std::uint64_t nativeBytes = 0;
        std::uint64_t maskBytes = 0;
        std::uint64_t systemMemoryBytes = 0;
        std::uint64_t gpuMemoryBytes = 0;
        unsigned int cpuThreadCount = 1;
        bool isNativeAliasAllowed = true;
    };

    struct Profile final {
        std::array<int, 3> outputDimensions{};
        double dimensionRatio = 1.0;
        double stillRayStepFactor = 0.5;
        double maxImageDistance = 2.0;
        double previewImageDistance = 2.0;
        double previewRayFactor = 2.0;
        double targetFps = 20.0;
        bool isJitterOn = true;
        bool isPreviewJitterOn = false;
        bool isAutoSampling = false;
    };

    VolumeLodController();

    bool Reset();
    bool SetQuality(VolumeQuality quality);
    bool SetSource(const Source& source);
    bool SetActiveRatio(double dimensionRatio);

    VolumeQuality GetQuality() const noexcept;
    Profile GetProfile() const noexcept;
    Profile GetProfile(VolumeQuality quality) const noexcept;
    double GetTargetRatio() const noexcept;

private:
    static std::optional<std::size_t> GetQualityIndex(
        VolumeQuality quality) noexcept;
    static double GetQuantizedRatio(double dimensionRatio) noexcept;
    static std::optional<std::uint64_t> GetVoxelCount(
        const std::array<int, 3>& dimensions) noexcept;
    static std::optional<std::array<int, 3>> GetScaledDimensions(
        const std::array<int, 3>& dimensions,
        double dimensionRatio) noexcept;
    static double GetTargetFps(VolumeQuality quality) noexcept;
    Profile BuildProfile(VolumeQuality quality) const noexcept;
    long double GetTargetBytes(double dimensionRatio) const noexcept;
    long double GetWorkingBytes(double dimensionRatio) const noexcept;
    double GetAutoRatio() const noexcept;

    Source m_source;
    std::array<Profile, 5> m_profiles{};
    VolumeQuality m_quality = VolumeQuality::Low;
};
