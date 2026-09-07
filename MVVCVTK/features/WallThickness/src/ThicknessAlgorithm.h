#pragma once

#include "Host/WallThicknessHostTypes.h"
#include "Data/DataPayloads.h"

#include <atomic>
#include <chrono>
#include <limits>

namespace ThicknessAlgorithm
{
inline constexpr std::size_t noNeighbor = std::numeric_limits<std::size_t>::max();
using Neighbors = std::vector<std::array<std::size_t, 3>>;
struct Work final
{
    ThicknessArchive archive;
    std::shared_ptr<const ImageGrid3DPayload> source;
    std::shared_ptr<const LabelMap3DPayload> labels;
    std::shared_ptr<const SurfaceMeshPayload> mesh;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::chrono::steady_clock::time_point deadline;
};
struct Field final
{
    std::shared_ptr<const std::vector<ThicknessSample>> samples;
    std::shared_ptr<const Neighbors> neighbors;
    std::uint32_t subdivisions = 0;
};
struct Candidate final
{
    ThicknessStatus status = ThicknessStatus::InternalError;
    Field field;
    ThicknessStatistics statistics;
    std::vector<ThicknessRegion> regions;
    std::string message;
};
bool GetParamsValid(const ThicknessParams &params) noexcept;
bool GetEvaluationValid(const ThicknessEvaluation &evaluation) noexcept;
bool GetDisplayValid(const ThicknessDisplay &display) noexcept;
bool GetConfigValid(const ThicknessConfig &config) noexcept;
Candidate BuildField(const Work &work) noexcept;
Candidate BuildEvaluation(const Field &field, const ThicknessEvaluation &evaluation,
                          const ThicknessParams &params, const ThicknessConfig &config,
                          const std::shared_ptr<std::atomic<bool>> &cancelled,
                          std::chrono::steady_clock::time_point deadline) noexcept;
ThicknessPoint GetSampleCorner(const SurfaceMeshPayload &mesh, const ThicknessSample &sample,
                               std::size_t corner);
} // namespace ThicknessAlgorithm
