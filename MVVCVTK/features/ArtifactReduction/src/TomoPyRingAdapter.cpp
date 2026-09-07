#include "TomoPyRingAdapter.h"
#include <vtkSMPTools.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
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
    if (mvvcvtk_tomopy_get_layout(grid.dimensions[u], grid.dimensions[v], layout.center[0], layout.center[1],
        params.angularMin, params.ringWidth, static_cast<int>(params.mode), &layout.native) != MVVCVTK_TOMOPY_OK)
        return ArtifactError::UnsupportedGeometry;
    // 固定并行上限，预检与执行一致，不依赖其他功能对全局 SMP 线程数的设置。
    layout.workerCount = std::min(8, grid.dimensions[params.axis]);
    return ArtifactError::None;
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
    struct WorkerResult final {
        ArtifactQuality quality;
        ArtifactError error = ArtifactError::None;
        double squaredCorrection = 0.0;
        double compensation = 0.0;
    };
    std::vector<WorkerResult> workers(static_cast<std::size_t>(layout.workerCount));
    std::atomic<bool> stopped{false};
    std::atomic<unsigned int> completed{0};
    const auto indexOf = [&](int x, int y, int s) {
        std::array<int, 3> position{};
        position[layout.planeAxes[0]] = x;
        position[layout.planeAxes[1]] = y;
        position[params.axis] = s;
        return (static_cast<std::size_t>(position[2]) * grid.dimensions[1] + position[1]) * grid.dimensions[0] + position[0];
    };
    vtkSMPTools::For(0, layout.workerCount, 1, [&](vtkIdType first, vtkIdType last) {
        for (auto workerIndex = first; workerIndex < last; ++workerIndex) {
            auto& worker = workers[static_cast<std::size_t>(workerIndex)];
            auto& local = worker.quality;
            const auto fail = [&](ArtifactError error) {
                worker.error = error;
                stopped.store(true, std::memory_order_relaxed);
            };
            try {
                std::vector<float> slice(static_cast<std::size_t>(width) * height);
                for (int s = static_cast<int>(workerIndex); s < grid.dimensions[params.axis]; s += layout.workerCount) {
                    if (stopped.load(std::memory_order_relaxed)) break;
                    if (const auto error = control.GetError(); error != ArtifactError::None) { fail(error); break; }
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x) slice[static_cast<std::size_t>(y) * width + x] = values[indexOf(x, y, s)];
                    const auto status = mvvcvtk_tomopy_remove_ring(slice.data(), width, height, layout.center[0], layout.center[1],
                        static_cast<float>(params.threshMax), static_cast<float>(params.threshMin), static_cast<float>(params.threshold),
                        params.angularMin, params.ringWidth, static_cast<int>(params.mode), nullptr);
                    if (status != MVVCVTK_TOMOPY_OK) {
                        fail(status == MVVCVTK_TOMOPY_ALLOCATION_FAILED ? ArtifactError::TooLarge : ArtifactError::KernelFailed); break;
                    }
                    if (const auto error = control.GetError(); error != ArtifactError::None) { fail(error); break; }
                    local.ringSampleCount += static_cast<std::size_t>(layout.native.polar_width) * layout.native.polar_height;
                    local.ringRadiusCount += layout.native.polar_width;
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            const auto index = indexOf(x, y, s);
                            const double oldValue = values[index];
                            const double delta = static_cast<double>(slice[static_cast<std::size_t>(y) * width + x]) - oldValue;
                            const double correction = params.strength * std::clamp(delta, -params.maxCorrection, params.maxCorrection);
                            const double candidate = oldValue + correction;
                            if (!std::isfinite(candidate) || std::abs(candidate) > std::numeric_limits<float>::max()) {
                                ++local.guardedCount;
                                continue;
                            }
                            values[index] = static_cast<float>(candidate);
                            if (values[index] != oldValue) {
                                ++local.ringVoxelCount;
                                const double change = values[index] - oldValue;
                                // float32 差值平方及网格上限内的总和均在 double 范围内。
                                // 补偿求和避免每个变化体素都执行 sqrt/hypot。
                                const double term = change * change - worker.compensation;
                                const double sum = worker.squaredCorrection + term;
                                worker.compensation = (sum - worker.squaredCorrection) - term;
                                worker.squaredCorrection = sum;
                            }
                        }
                    }
                    const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                    const auto progress = 10 + static_cast<unsigned int>(30.0 * done / grid.dimensions[params.axis]);
                    auto prior = control.progress.load(std::memory_order_relaxed);
                    while (prior < progress && !control.progress.compare_exchange_weak(prior, progress, std::memory_order_relaxed)) {}
                }
            }
            catch (const std::bad_alloc&) { fail(ArtifactError::TooLarge); }
            catch (...) { fail(ArtifactError::KernelFailed); }
        }
    });
    // 固定 worker 顺序归并统计；不同切片写回的体素互不重叠。
    for (const auto& worker : workers) {
        if (worker.error != ArtifactError::None) return worker.error;
        const auto& local = worker.quality;
        quality.ringSampleCount += local.ringSampleCount;
        quality.ringRadiusCount += local.ringRadiusCount;
        quality.guardedCount += local.guardedCount;
        const auto total = quality.ringVoxelCount + local.ringVoxelCount;
        if (total != 0) {
            quality.ringCorrectionRms = std::hypot(
                quality.ringCorrectionRms * std::sqrt(static_cast<double>(quality.ringVoxelCount) / total),
                std::sqrt(worker.squaredCorrection / static_cast<double>(total)));
        }
        quality.ringVoxelCount = total;
    }
    return control.GetError();
}
} // namespace ArtifactReduction
