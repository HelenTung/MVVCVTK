#include "TomoPyRingAdapter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace ArtifactReduction {
ArtifactError GetRingLayout(const GridGeometry3D& grid,
    const ArtifactRingParams& params, RingLayout& layout) noexcept
{
    const double limit = std::numeric_limits<float>::max();
    if (params.axis < 0 || params.axis > 2 || !std::isfinite(params.strength)
        || params.strength < 0.0 || params.strength > 1.0
        || !std::isfinite(params.maxCorrection) || params.maxCorrection <= 0.0 || params.maxCorrection > limit
        || !std::isfinite(params.threshMin) || !std::isfinite(params.threshMax)
        || std::abs(params.threshMin) > limit || std::abs(params.threshMax) > limit
        || !std::isfinite(params.threshold) || params.threshold <= 0.0 || params.threshold > limit
        || static_cast<float>(params.threshMin) >= static_cast<float>(params.threshMax)
        || static_cast<float>(params.threshold) == 0.0f
        || (params.mode != ArtifactRingMode::Wrap && params.mode != ArtifactRingMode::Reflect)) return ArtifactError::InvalidRequest;
    int next = 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis == params.axis) continue;
        const double local = params.centerIndex[next] - grid.extent[axis * 2];
        if (!std::isfinite(local) || local < 0.0 || local > grid.dimensions[axis] - 1) return ArtifactError::UnsupportedGeometry;
        layout.planeAxes[next] = axis;
        layout.center[next] = static_cast<float>(local);
        ++next;
    }
    const auto u = layout.planeAxes[0];
    const auto v = layout.planeAxes[1];
    if (std::abs(grid.spacing[u] - grid.spacing[v]) > 1e-6 * std::max(grid.spacing[u], grid.spacing[v]))
        return ArtifactError::UnsupportedGeometry;
    return mvvcvtk_tomopy_get_layout(grid.dimensions[u], grid.dimensions[v], layout.center[0], layout.center[1],
        params.angularMin, params.ringWidth, static_cast<int>(params.mode), &layout.native) == MVVCVTK_TOMOPY_OK
        ? ArtifactError::None : ArtifactError::UnsupportedGeometry;
}

ArtifactError BuildRingCorrection(const GridGeometry3D& grid,
    const ArtifactRingParams& params, std::vector<float>& values,
    TaskControl& control, ArtifactQuality& quality)
{
    if (params.strength == 0.0) return control.GetError();
    RingLayout layout;
    if (const auto error = GetRingLayout(grid, params, layout); error != ArtifactError::None) return error;
    const int width = grid.dimensions[layout.planeAxes[0]];
    const int height = grid.dimensions[layout.planeAxes[1]];
    std::vector<float> slice(static_cast<std::size_t>(width) * height);
    const auto indexOf = [&](int x, int y, int s) {
        std::array<int, 3> position{};
        position[layout.planeAxes[0]] = x;
        position[layout.planeAxes[1]] = y;
        position[params.axis] = s;
        return (static_cast<std::size_t>(position[2]) * grid.dimensions[1] + position[1]) * grid.dimensions[0] + position[0];
    };
    for (int s = 0; s < grid.dimensions[params.axis]; ++s) {
        if (const auto error = control.GetError(); error != ArtifactError::None) return error;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) slice[static_cast<std::size_t>(y) * width + x] = values[indexOf(x, y, s)];
        control.progress.store(1 + static_cast<unsigned int>(39.0 * s / grid.dimensions[params.axis]), std::memory_order_relaxed);
        const auto status = mvvcvtk_tomopy_remove_ring(slice.data(), width, height, layout.center[0], layout.center[1],
            static_cast<float>(params.threshMax), static_cast<float>(params.threshMin), static_cast<float>(params.threshold),
            params.angularMin, params.ringWidth, static_cast<int>(params.mode), nullptr);
        if (status != MVVCVTK_TOMOPY_OK) return status == MVVCVTK_TOMOPY_ALLOCATION_FAILED ? ArtifactError::TooLarge : ArtifactError::KernelFailed;
        if (const auto error = control.GetError(); error != ArtifactError::None) return error;
        quality.ringSampleCount += static_cast<std::size_t>(layout.native.polar_width) * layout.native.polar_height;
        quality.ringRadiusCount += layout.native.polar_width;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto index = indexOf(x, y, s);
                const double oldValue = values[index];
                const double delta = static_cast<double>(slice[static_cast<std::size_t>(y) * width + x]) - oldValue;
                const double correction = params.strength * std::clamp(delta, -params.maxCorrection, params.maxCorrection);
                const double candidate = oldValue + correction;
                if (!std::isfinite(candidate) || std::abs(candidate) > std::numeric_limits<float>::max()) {
                    ++quality.guardedCount;
                    continue;
                }
                values[index] = static_cast<float>(candidate);
                if (values[index] != oldValue) {
                    ++quality.ringVoxelCount;
                    const double ratio = 1.0 / static_cast<double>(quality.ringVoxelCount);
                    quality.ringCorrectionRms = std::hypot(quality.ringCorrectionRms * std::sqrt(1.0 - ratio),
                        (values[index] - oldValue) * std::sqrt(ratio));
                }
            }
        }
        control.progress.store(static_cast<unsigned int>(40.0 * (s + 1) / grid.dimensions[params.axis]), std::memory_order_relaxed);
    }
    return ArtifactError::None;
}
} // namespace ArtifactReduction
