#include "ArtifactQualityEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ArtifactReduction {
namespace {
// 在线总体方差；极端值使差分无法用 double 表示时显式拒绝报告。
struct Moments final {
    std::size_t count = 0;
    double mean = 0.0;
    double variance = 0.0;
    bool Add(double value) noexcept
    {
        ++count;
        const double ratio = 1.0 / static_cast<double>(count);
        const double delta = value - mean;
        const double nextMean = mean * (1.0 - ratio) + value * ratio;
        variance = variance * (1.0 - ratio) + (delta * std::sqrt(ratio)) * ((value - nextMean) * std::sqrt(ratio));
        mean = nextMean;
        return std::isfinite(mean) && std::isfinite(variance);
    }
};

double AddRms(double rms, double value, std::size_t count) noexcept
{
    const double ratio = 1.0 / static_cast<double>(count);
    return std::hypot(rms * std::sqrt(1.0 - ratio), value * std::sqrt(ratio));
}
} // namespace

ArtifactError BuildQuality(const AlgorithmInput& input,
    const std::vector<float>& output, TaskControl& control,
    ArtifactQuality& quality, std::array<double, 2>& scalarRange)
{
    VolumeView before(input);
    VolumeView after(input);
    after.SetValues(output);
    Moments materialBefore;
    Moments materialAfter;
    scalarRange = { std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest() };
    const auto& grid = before.GetGeometry();
    const auto& dims = grid.dimensions;
    const std::array<std::size_t, 3> strides{ 1, static_cast<std::size_t>(dims[0]), static_cast<std::size_t>(dims[0]) * dims[1] };
    for (int z = 0; z < dims[2]; ++z) {
        for (int y = 0; y < dims[1]; ++y) {
            for (int x = 0; x < dims[0]; ++x) {
                if ((x & 255) == 0) {
                    if (const auto error = control.GetError(); error != ArtifactError::None) return error;
                }
                const auto index = before.GetIndex(x, y, z);
                if (!before.GetValid(index)) continue;
                if (!after.GetValid(index)) return ArtifactError::InvalidData;
                const double oldValue = before.GetValue(index);
                const double newValue = after.GetValue(index);
                const double delta = newValue - oldValue;
                if (!std::isfinite(delta)) return ArtifactError::InvalidData;
                ++quality.validCount;
                if (delta != 0.0) ++quality.changedCount;
                const double ratio = 1.0 / static_cast<double>(quality.validCount);
                quality.meanBefore = quality.meanBefore * (1.0 - ratio) + oldValue * ratio;
                quality.meanAfter = quality.meanAfter * (1.0 - ratio) + newValue * ratio;
                quality.meanDelta = quality.meanDelta * (1.0 - ratio) + delta * ratio;
                quality.rmsDelta = AddRms(quality.rmsDelta, delta, quality.validCount);
                quality.maxDelta = std::max(quality.maxDelta, std::abs(delta));
                scalarRange[0] = std::min(scalarRange[0], newValue);
                scalarRange[1] = std::max(scalarRange[1], newValue);
                if (before.GetMaterial(index)) {
                    if (!materialBefore.Add(oldValue) || !materialAfter.Add(newValue)) return ArtifactError::InvalidData;
                }
                const std::array<int, 3> position{ x, y, z };
                for (int axis = 0; axis < 3; ++axis) {
                    if (position[axis] + 1 >= dims[axis]) continue;
                    const auto next = index + strides[axis];
                    if (!before.GetValid(next)) continue;
                    const double nextDelta = after.GetValue(next) - before.GetValue(next);
                    const double gradientDelta = (nextDelta - delta) / grid.spacing[axis];
                    if (!std::isfinite(gradientDelta)) return ArtifactError::InvalidData;
                    ++quality.gradientCount;
                    quality.gradientDeltaRms = AddRms(quality.gradientDeltaRms, gradientDelta, quality.gradientCount);
                }
            }
        }
        control.progress.store(80 + static_cast<unsigned int>(15.0 * (z + 1) / dims[2]), std::memory_order_relaxed);
    }
    quality.materialCount = materialBefore.count;
    quality.hasMaterial = quality.materialCount != 0;
    quality.materialMeanBefore = materialBefore.mean;
    quality.materialMeanAfter = materialAfter.mean;
    quality.materialStdBefore = std::sqrt(std::max(0.0, materialBefore.variance));
    quality.materialStdAfter = std::sqrt(std::max(0.0, materialAfter.variance));
    if (quality.validCount == 0 || !std::isfinite(quality.meanBefore) || !std::isfinite(quality.meanAfter)
        || !std::isfinite(quality.meanDelta) || !std::isfinite(quality.rmsDelta)) return ArtifactError::InvalidData;
    return ArtifactError::None;
}

std::shared_ptr<const RecordTablePayload> CreateQualityReport(const ArtifactQuality& quality)
{
    std::vector<std::string> names;
    std::vector<double> values;
    std::vector<std::string> units;
    const auto add = [&](const char* name, double value, const char* unit) {
        names.emplace_back(name); values.push_back(value); units.emplace_back(unit);
    };
    add("valid-count", static_cast<double>(quality.validCount), "voxel");
    add("changed-count", static_cast<double>(quality.changedCount), "voxel");
    add("material-count", static_cast<double>(quality.materialCount), "voxel");
    add("gradient-count", static_cast<double>(quality.gradientCount), "pair");
    add("guarded-count", static_cast<double>(quality.guardedCount), "operation");
    add("mean-before", quality.meanBefore, "stored");
    add("mean-after", quality.meanAfter, "stored");
    add("mean-delta", quality.meanDelta, "stored");
    add("rms-delta", quality.rmsDelta, "stored");
    add("max-delta", quality.maxDelta, "stored");
    add("material-mean-before", quality.materialMeanBefore, "stored");
    add("material-mean-after", quality.materialMeanAfter, "stored");
    add("material-std-before", quality.materialStdBefore, "stored");
    add("material-std-after", quality.materialStdAfter, "stored");
    add("gradient-delta-rms", quality.gradientDeltaRms, "stored/physical");
    add("ring-sample-count", static_cast<double>(quality.ringSampleCount), "sample");
    add("ring-radius-count", static_cast<double>(quality.ringRadiusCount), "radius");
    add("ring-voxel-count", static_cast<double>(quality.ringVoxelCount), "voxel");
    add("ring-skipped-slices", static_cast<double>(quality.ringSkippedSlices), "slice");
    add("ring-correction-rms", quality.ringCorrectionRms, "stored");
    add("has-material", quality.hasMaterial ? 1.0 : 0.0, "bool");
    add("fidelity-verified", 0.0, "bool");
    add("warning-no-reference", 1.0, "bool");
    add("warning-material-assumption", quality.ringRadiusCount != 0 ? 1.0 : 0.0, "bool");
    add("warning-ring-inscribed-coverage", quality.ringRadiusCount != 0 ? 1.0 : 0.0, "bool");
    add("warning-mask-writeback-only", 1.0, "bool");
    add("warning-native-cancel-granularity", 1.0, "bool");
    std::vector<RecordColumn> columns;
    columns.push_back({ "metric", std::move(names) });
    columns.push_back({ "value", std::move(values) });
    columns.push_back({ "unit", std::move(units) });
    return std::make_shared<const RecordTablePayload>(DataTypes::recordTable,
        "artifact-reduction-quality-v1", std::move(columns));
}
} // namespace ArtifactReduction
