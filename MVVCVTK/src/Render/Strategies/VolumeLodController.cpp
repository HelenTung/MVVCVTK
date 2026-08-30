#include "Render/Internal/VolumeLodController.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double lowRatio = 0.25;
constexpr double highRatio = 0.50;
constexpr double xHighRatio = 0.75;
constexpr double ultraRatio = 1.0;
constexpr double minimumRatio = lowRatio;
constexpr double ratioStep = 0.05;
constexpr int minimumLevel = 5;
constexpr int maximumLevel = 20;
constexpr long double workingCopies = 2.0L;
constexpr long double fallbackCopies = 2.0L;
constexpr long double autoMemoryFraction = 0.65L;
constexpr std::array qualityOrder{
    VolumeQuality::Auto,
    VolumeQuality::Low,
    VolumeQuality::High,
    VolumeQuality::XHigh,
    VolumeQuality::Ultra
};
}

VolumeLodController::VolumeLodController()
{
    (void)Reset();
}

bool VolumeLodController::Reset()
{
    m_source = {};
    m_source.cpuThreadCount = 1;
    for (std::size_t index = 0; index < m_profiles.size(); ++index) {
        m_profiles[index] = BuildProfile(qualityOrder[index]);
    }
    return GetQualityIndex(m_quality).has_value();
}

bool VolumeLodController::SetQuality(const VolumeQuality quality)
{
    if (!GetQualityIndex(quality)) return false;
    m_quality = quality;
    return true;
}

bool VolumeLodController::SetSource(const Source& source)
{
    if (source.dimensions[0] <= 0
        || source.dimensions[1] <= 0
        || source.dimensions[2] <= 0
        || source.nativeBytes == 0
        || !GetVoxelCount(source.dimensions)
        || source.maskBytes
            > std::numeric_limits<std::uint64_t>::max()
                - source.nativeBytes) {
        return false;
    }

    const Source oldSource = m_source;
    const auto oldProfiles = m_profiles;
    m_source = source;
    m_source.cpuThreadCount = std::max(
        1U, source.cpuThreadCount);
    for (std::size_t index = 0; index < m_profiles.size(); ++index) {
        const auto profile = BuildProfile(qualityOrder[index]);
        if (profile.outputDimensions[0] <= 0
            || profile.outputDimensions[1] <= 0
            || profile.outputDimensions[2] <= 0) {
            m_source = oldSource;
            m_profiles = oldProfiles;
            return false;
        }
        m_profiles[index] = profile;
    }
    return true;
}

bool VolumeLodController::SetActiveRatio(const double dimensionRatio)
{
    const double targetRatio = GetTargetRatio();
    constexpr double ratioEpsilon = 1e-9;
    if (!std::isfinite(dimensionRatio)
        || dimensionRatio <= 0.0
        || dimensionRatio > 1.0
        || std::abs(dimensionRatio - targetRatio) > ratioEpsilon) {
        return false;
    }
    return true;
}

VolumeQuality VolumeLodController::GetQuality() const noexcept
{
    return m_quality;
}

VolumeLodController::Profile
VolumeLodController::GetProfile() const noexcept
{
    return GetProfile(m_quality);
}

VolumeLodController::Profile VolumeLodController::GetProfile(
    const VolumeQuality quality) const noexcept
{
    const auto index = GetQualityIndex(quality);
    return index ? m_profiles[*index] : Profile{};
}

double VolumeLodController::GetTargetRatio() const noexcept
{
    return GetProfile().dimensionRatio;
}

std::optional<std::size_t> VolumeLodController::GetQualityIndex(
    const VolumeQuality quality) noexcept
{
    switch (quality) {
    case VolumeQuality::Auto:
        return 0;
    case VolumeQuality::Low:
        return 1;
    case VolumeQuality::High:
        return 2;
    case VolumeQuality::XHigh:
        return 3;
    case VolumeQuality::Ultra:
        return 4;
    }
    return std::nullopt;
}

double VolumeLodController::GetQuantizedRatio(
    const double dimensionRatio) noexcept
{
    if (!std::isfinite(dimensionRatio)) return minimumRatio;
    const double clampedRatio = std::clamp(
        dimensionRatio, minimumRatio, 1.0);
    constexpr double levelEpsilon = 1e-9;
    const int level = std::clamp(
        static_cast<int>(std::floor(
            clampedRatio / ratioStep + levelEpsilon)),
        minimumLevel,
        maximumLevel);
    return static_cast<double>(level) * ratioStep;
}

std::optional<std::uint64_t> VolumeLodController::GetVoxelCount(
    const std::array<int, 3>& dimensions) noexcept
{
    std::uint64_t voxelCount = 1;
    for (const int dimension : dimensions) {
        if (dimension <= 0
            || voxelCount
                > std::numeric_limits<std::uint64_t>::max()
                    / static_cast<std::uint64_t>(dimension)) {
            return std::nullopt;
        }
        voxelCount *= static_cast<std::uint64_t>(dimension);
    }
    return voxelCount;
}

std::optional<std::array<int, 3>>
VolumeLodController::GetScaledDimensions(
    const std::array<int, 3>& dimensions,
    const double dimensionRatio) noexcept
{
    if (!std::isfinite(dimensionRatio)
        || dimensionRatio <= 0.0
        || dimensionRatio > 1.0) {
        return std::nullopt;
    }
    std::array<int, 3> scaledDimensions{};
    for (std::size_t axis = 0;
        axis < scaledDimensions.size(); ++axis) {
        const double scaled = std::ceil(
            static_cast<double>(dimensions[axis])
            * dimensionRatio);
        if (!std::isfinite(scaled)
            || scaled <= 0.0
            || scaled > static_cast<double>(
                std::numeric_limits<int>::max())
            || scaled > static_cast<double>(dimensions[axis])) {
            return std::nullopt;
        }
        scaledDimensions[axis] = std::max(
            1, static_cast<int>(scaled));
    }
    return scaledDimensions;
}

double VolumeLodController::GetTargetFps(
    const VolumeQuality quality) noexcept
{
    switch (quality) {
    case VolumeQuality::Auto:
        return 20.0;
    case VolumeQuality::Low:
        return 30.0;
    case VolumeQuality::High:
        return 20.0;
    case VolumeQuality::XHigh:
        return 12.0;
    case VolumeQuality::Ultra:
        return 8.0;
    }
    return 20.0;
}

VolumeLodController::Profile VolumeLodController::BuildProfile(
    const VolumeQuality quality) const noexcept
{
    Profile profile;
    profile.targetFps = GetTargetFps(quality);
    switch (quality) {
    case VolumeQuality::Auto:
        profile.stillRayStepFactor = 0.75;
        profile.maxImageDistance = 2.5;
        profile.previewImageDistance = 3.0;
        profile.previewRayFactor = 3.0;
        profile.isAutoSampling = true;
        break;
    case VolumeQuality::Low:
        profile.dimensionRatio = lowRatio;
        profile.stillRayStepFactor = 1.0;
        profile.maxImageDistance = 3.0;
        profile.previewImageDistance = 2.0;
        profile.previewRayFactor = 2.0;
        break;
    case VolumeQuality::High:
        profile.dimensionRatio = highRatio;
        profile.stillRayStepFactor = 0.75;
        profile.maxImageDistance = 2.5;
        profile.previewImageDistance = 3.0;
        profile.previewRayFactor = 3.0;
        break;
    case VolumeQuality::XHigh:
        profile.dimensionRatio = xHighRatio;
        profile.stillRayStepFactor = 0.5;
        profile.maxImageDistance = 2.0;
        profile.previewImageDistance = 3.0;
        profile.previewRayFactor = 4.0;
        break;
    case VolumeQuality::Ultra:
        profile.dimensionRatio = ultraRatio;
        profile.stillRayStepFactor = 0.4;
        profile.maxImageDistance = 1.5;
        profile.previewImageDistance = 4.0;
        profile.previewRayFactor = 4.0;
        break;
    }
    if (m_source.nativeBytes == 0) return profile;
    if (quality == VolumeQuality::Auto) {
        profile.dimensionRatio = GetAutoRatio();
        // Auto 只解析一次数据档位；交互采样随后复用相邻显式档，
        // 不根据逐帧耗时反向改变 dimensions。
        if (profile.dimensionRatio <= lowRatio) {
            profile.previewImageDistance = 2.0;
            profile.previewRayFactor = 2.0;
        }
        else if (profile.dimensionRatio <= highRatio) {
            profile.previewImageDistance = 3.0;
            profile.previewRayFactor = 3.0;
        }
        else if (profile.dimensionRatio <= xHighRatio) {
            profile.previewImageDistance = 3.0;
            profile.previewRayFactor = 4.0;
        }
        else {
            profile.previewImageDistance = 4.0;
            profile.previewRayFactor = 4.0;
        }
    }
    const auto dimensions = GetScaledDimensions(
        m_source.dimensions, profile.dimensionRatio);
    if (dimensions) profile.outputDimensions = *dimensions;
    return profile;
}

long double VolumeLodController::GetTargetBytes(
    const double dimensionRatio) const noexcept
{
    const auto nativeVoxels = GetVoxelCount(m_source.dimensions);
    const auto scaledDimensions = GetScaledDimensions(
        m_source.dimensions, dimensionRatio);
    const auto scaledVoxels = scaledDimensions
        ? GetVoxelCount(*scaledDimensions) : std::nullopt;
    if (!nativeVoxels || !scaledVoxels
        || *nativeVoxels == 0
        || m_source.maskBytes
            > std::numeric_limits<std::uint64_t>::max()
                - m_source.nativeBytes) {
        return std::numeric_limits<long double>::infinity();
    }
    const std::uint64_t sourceBytes =
        m_source.nativeBytes + m_source.maskBytes;
    const long double voxelRatio =
        static_cast<long double>(*scaledVoxels)
        / static_cast<long double>(*nativeVoxels);
    return static_cast<long double>(sourceBytes) * voxelRatio;
}

long double VolumeLodController::GetWorkingBytes(
    const double dimensionRatio) const noexcept
{
    if (m_source.isNativeAliasAllowed
        && std::abs(dimensionRatio - 1.0) <= 1e-9) {
        return 0.0L;
    }
    return GetTargetBytes(dimensionRatio) * workingCopies;
}

double VolumeLodController::GetAutoRatio() const noexcept
{
    if (m_source.nativeBytes == 0) return 1.0;
    const long double sourceBytes = GetTargetBytes(1.0);
    if (!std::isfinite(sourceBytes) || sourceBytes <= 0.0L) {
        return minimumRatio;
    }
    const long double fallbackBytes = std::min(
        sourceBytes * fallbackCopies,
        static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max()));
    const long double systemBytes = m_source.systemMemoryBytes > 0
        ? static_cast<long double>(m_source.systemMemoryBytes)
        : fallbackBytes;
    const long double gpuBytes = m_source.gpuMemoryBytes > 0
        ? static_cast<long double>(m_source.gpuMemoryBytes)
        : fallbackBytes;
    const long double threadCount = static_cast<long double>(
        std::max(1U, m_source.cpuThreadCount));
    // 核心数只在加载时折算系统工作集余量；不进入逐帧反馈，也不编码绝对 dimensions。
    const long double cpuFactor = threadCount / (threadCount + 1.0L);
    const long double systemBudget =
        systemBytes * autoMemoryFraction * cpuFactor;
    const long double gpuBudget = gpuBytes * autoMemoryFraction;

    for (int level = maximumLevel;
        level >= minimumLevel; --level) {
        const double dimensionRatio =
            static_cast<double>(level) * ratioStep;
        if (GetWorkingBytes(dimensionRatio) <= systemBudget
            && GetTargetBytes(dimensionRatio) <= gpuBudget) {
            return GetQuantizedRatio(dimensionRatio);
        }
    }
    return minimumRatio;
}
