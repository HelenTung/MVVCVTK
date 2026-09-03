#include "Render/Internal/IsoLodController.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double lowRatio = 0.25;
constexpr double highRatio = 0.50;
constexpr double xHighRatio = 0.75;
constexpr double ultraRatio = 1.0;
constexpr double ratioStep = 0.05;
constexpr int minimumLevel = 5;
constexpr int maximumLevel = 20;
// Auto 最多使用当前可用物理内存的 20%；三份工作集近似覆盖重采样输出、
// FlyingEdges 扫描工作区和候选输出。mesh 上界不可由输入体素精确预测，最终仍由
// IsoSurfaceStrategy 的候选物化结果决定是否提交。
constexpr long double autoMemoryFraction = 0.20L;
constexpr long double estimatedWorkingCopies = 3.0L;
constexpr std::array qualityOrder{
    VolumeQuality::Auto,
    VolumeQuality::Low,
    VolumeQuality::High,
    VolumeQuality::XHigh,
    VolumeQuality::Ultra
};
}

IsoLodController::IsoLodController()
{
    (void)Reset();
}

bool IsoLodController::Reset()
{
    m_source = {};
    m_source.cpuThreadCount = 1;
    for (std::size_t index = 0; index < m_profiles.size(); ++index) {
        m_profiles[index] = BuildProfile(qualityOrder[index]);
    }
    return GetQualityIndex(m_quality).has_value();
}

bool IsoLodController::SetQuality(const VolumeQuality quality)
{
    if (!GetQualityIndex(quality)) return false;
    m_quality = quality;
    return true;
}

bool IsoLodController::SetSource(const Source& source)
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

VolumeQuality IsoLodController::GetQuality() const noexcept
{
    return m_quality;
}

IsoLodController::Profile IsoLodController::GetProfile() const noexcept
{
    return GetProfile(m_quality);
}

IsoLodController::Profile IsoLodController::GetProfile(
    const VolumeQuality quality) const noexcept
{
    const auto index = GetQualityIndex(quality);
    return index ? m_profiles[*index] : Profile{};
}

std::optional<std::size_t> IsoLodController::GetQualityIndex(
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

double IsoLodController::GetQuantizedRatio(
    const double dimensionRatio) noexcept
{
    if (!std::isfinite(dimensionRatio)) return lowRatio;
    const double clampedRatio = std::clamp(
        dimensionRatio, lowRatio, ultraRatio);
    constexpr double levelEpsilon = 1e-9;
    const int level = std::clamp(
        static_cast<int>(std::floor(
            clampedRatio / ratioStep + levelEpsilon)),
        minimumLevel,
        maximumLevel);
    return static_cast<double>(level) * ratioStep;
}

std::optional<std::uint64_t> IsoLodController::GetVoxelCount(
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
IsoLodController::GetScaledDimensions(
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
            || scaled > static_cast<double>(dimensions[axis])) {
            return std::nullopt;
        }
        scaledDimensions[axis] = std::max(
            1, static_cast<int>(scaled));
    }
    return scaledDimensions;
}

IsoLodController::Profile IsoLodController::BuildProfile(
    const VolumeQuality quality) const noexcept
{
    Profile profile;
    switch (quality) {
    case VolumeQuality::Auto:
        profile.dimensionRatio = GetAutoRatio();
        break;
    case VolumeQuality::Low:
        profile.dimensionRatio = lowRatio;
        break;
    case VolumeQuality::High:
        profile.dimensionRatio = highRatio;
        break;
    case VolumeQuality::XHigh:
        profile.dimensionRatio = xHighRatio;
        break;
    case VolumeQuality::Ultra:
        profile.dimensionRatio = ultraRatio;
        break;
    }
    if (m_source.nativeBytes == 0) return profile;
    const auto dimensions = GetScaledDimensions(
        m_source.dimensions, profile.dimensionRatio);
    if (dimensions) profile.outputDimensions = *dimensions;
    return profile;
}

double IsoLodController::GetAutoRatio() const noexcept
{
    if (m_source.nativeBytes == 0) return lowRatio;
    const long double sourceBytes = static_cast<long double>(
        m_source.nativeBytes + m_source.maskBytes);
    const long double fallbackBytes =
        sourceBytes * estimatedWorkingCopies;
    const long double availableBytes = m_source.systemMemoryBytes > 0
        ? static_cast<long double>(m_source.systemMemoryBytes)
        : fallbackBytes;
    const long double memoryBudget =
        availableBytes * autoMemoryFraction;
    const long double ratioCube = memoryBudget
        / (sourceBytes * estimatedWorkingCopies);
    const double memoryRatio = ratioCube > 0.0L
        ? std::cbrt(static_cast<double>(ratioCube)) : lowRatio;

    double cpuRatio = ultraRatio;
    if (m_source.cpuThreadCount <= 2) cpuRatio = lowRatio;
    else if (m_source.cpuThreadCount <= 4) cpuRatio = highRatio;
    else if (m_source.cpuThreadCount <= 8) cpuRatio = xHighRatio;
    return GetQuantizedRatio(std::min(memoryRatio, cpuRatio));
}
