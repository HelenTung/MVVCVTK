#include "PartSegmentationTestCases.h"

#include "Algorithms/ClassicalPartSegmenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool GetCaseResult(const bool passed, const std::string_view name)
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

PartVolumeData BuildVolume(
    const std::array<int, 3>& dimensions,
    std::vector<double> values)
{
    PartVolumeData volume;
    volume.dimensions = dimensions;
    volume.extent = {
        0, dimensions[0] - 1,
        0, dimensions[1] - 1,
        0, dimensions[2] - 1
    };
    volume.values = std::move(values);
    return volume;
}

bool GetNearlyEqual(const double left, const double right)
{
    return std::abs(left - right) < 1e-12;
}

} // namespace

int GetPartAlgorithmFailCount()
{
    int failureCount = 0;
    PartAlgorithmParams params;

    const auto invalid = ClassicalPartSegmenter::BuildLabels(
        PartVolumeData{}, params);
    failureCount += GetCaseResult(
        invalid.error == PartAlgorithmError::InvalidInput,
        "Empty volume is rejected") ? 0 : 1;

    const auto background = ClassicalPartSegmenter::BuildLabels(
        BuildVolume({ 2, 2, 2 }, std::vector<double>(8, 0.0)), params);
    failureCount += GetCaseResult(
        background.error == PartAlgorithmError::None
            && background.parts.empty()
            && std::all_of(
                background.labels.begin(), background.labels.end(),
                [](const std::uint32_t label) { return label == 0; }),
        "All-background volume returns no parts") ? 0 : 1;

    auto singleVolume = BuildVolume({ 1, 1, 1 }, { 1.0 });
    singleVolume.spacing = { 2.0, 3.0, 4.0 };
    const auto single = ClassicalPartSegmenter::BuildLabels(
        singleVolume, params);
    failureCount += GetCaseResult(
        single.error == PartAlgorithmError::None
            && single.labels == std::vector<std::uint32_t>{ 1 }
            && single.parts.size() == 1
            && single.parts[0].voxelCount == 1
            && GetNearlyEqual(single.parts[0].physicalVolumeMM3, 24.0),
        "Single foreground voxel produces one deterministic label") ? 0 : 1;

    const auto separatedVolume = BuildVolume(
        { 4, 1, 1 }, { 1.0, 1.0, 0.0, 1.0 });
    const auto separated = ClassicalPartSegmenter::BuildLabels(
        separatedVolume, params);
    failureCount += GetCaseResult(
        separated.error == PartAlgorithmError::None
            && separated.labels
                == std::vector<std::uint32_t>{ 1, 1, 0, 2 }
            && separated.parts.size() == 2,
        "Separated foreground regions receive stable labels") ? 0 : 1;

    const auto diagonal = ClassicalPartSegmenter::BuildLabels(
        BuildVolume({ 2, 2, 1 }, { 1.0, 0.0, 0.0, 1.0 }), params);
    failureCount += GetCaseResult(
        diagonal.error == PartAlgorithmError::None
            && diagonal.parts.size() == 2,
        "Diagonal contact stays disconnected under 6-connectivity") ? 0 : 1;

    auto maskedVolume = BuildVolume({ 2, 1, 1 }, { 1.0, 1.0 });
    maskedVolume.validity = { 1, 0 };
    const auto masked = ClassicalPartSegmenter::BuildLabels(
        maskedVolume, params);
    failureCount += GetCaseResult(
        masked.error == PartAlgorithmError::None
            && masked.labels == std::vector<std::uint32_t>{ 1, 0 },
        "Validity mask excludes invalid foreground") ? 0 : 1;

    const auto nonFinite = ClassicalPartSegmenter::BuildLabels(
        BuildVolume({ 2, 1, 1 }, {
            std::numeric_limits<double>::quiet_NaN(), 1.0 }), params);
    failureCount += GetCaseResult(
        nonFinite.error == PartAlgorithmError::None
            && nonFinite.labels == std::vector<std::uint32_t>{ 0, 1 },
        "Non-finite scalar is background") ? 0 : 1;

    PartAlgorithmParams filteredParams = params;
    filteredParams.minPartVoxels = 2;
    const auto filtered = ClassicalPartSegmenter::BuildLabels(
        separatedVolume, filteredParams);
    failureCount += GetCaseResult(
        filtered.error == PartAlgorithmError::None
            && filtered.parts.size() == 1
            && filtered.labels
                == std::vector<std::uint32_t>{ 1, 1, 0, 0 },
        "Minimum voxel rule removes small parts without label gaps") ? 0 : 1;

    auto offsetVolume = BuildVolume({ 1, 1, 1 }, { 1.0 });
    offsetVolume.extent = { 5, 5, 7, 7, 9, 9 };
    offsetVolume.spacing = { 2.0, 1.0, 1.0 };
    offsetVolume.origin = { 1.0, 2.0, 3.0 };
    offsetVolume.direction = {
        -1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    const auto offset = ClassicalPartSegmenter::BuildLabels(
        offsetVolume, params);
    failureCount += GetCaseResult(
        offset.error == PartAlgorithmError::None
            && offset.parts.size() == 1
            && offset.parts[0].voxelExtent
                == std::array<int, 6>{ 5, 5, 7, 7, 9, 9 }
            && GetNearlyEqual(offset.parts[0].centroidWorld[0], -9.0)
            && GetNearlyEqual(offset.parts[0].centroidWorld[1], 9.0)
            && GetNearlyEqual(offset.parts[0].centroidWorld[2], 12.0),
        "Non-zero extent and direction map catalog to world") ? 0 : 1;

    PartAlgorithmParams budgetParams = params;
    budgetParams.maxWorkingBytes = 1;
    const auto budget = ClassicalPartSegmenter::BuildLabels(
        singleVolume, budgetParams);
    failureCount += GetCaseResult(
        budget.error == PartAlgorithmError::BudgetExceeded,
        "Working-set budget rejects before allocation") ? 0 : 1;

    std::vector<double> checkerValues(27, 0.0);
    for (int z = 0; z < 3; ++z) {
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                if ((x + y + z) % 2 == 0) {
                    checkerValues[static_cast<std::size_t>(
                        x + 3 * (y + 3 * z))] = 1.0;
                }
            }
        }
    }
    PartAlgorithmParams catalogBudgetParams = params;
    catalogBudgetParams.maxWorkingBytes = 27U
        * (sizeof(double)
            + sizeof(std::uint32_t)
            + sizeof(std::size_t));
    const auto catalogBudget = ClassicalPartSegmenter::BuildLabels(
        BuildVolume({ 3, 3, 3 }, std::move(checkerValues)),
        catalogBudgetParams);
    failureCount += GetCaseResult(
        catalogBudget.error == PartAlgorithmError::BudgetExceeded,
        "Working-set budget includes worst-case PartCatalog storage") ? 0 : 1;

    PartAlgorithmParams overflowParams = params;
    overflowParams.maxPartCount = 1;
    const auto overflow = ClassicalPartSegmenter::BuildLabels(
        separatedVolume, overflowParams);
    failureCount += GetCaseResult(
        overflow.error == PartAlgorithmError::LabelOverflow,
        "PartId limit rejects label overflow") ? 0 : 1;

    constexpr std::uint32_t exactPartLimit = 4096;
    std::vector<double> exactLimitValues(
        static_cast<std::size_t>(exactPartLimit) * 2U - 1U,
        0.0);
    for (std::size_t index = 0; index < exactLimitValues.size(); index += 2) {
        exactLimitValues[index] = 1.0;
    }
    PartAlgorithmParams exactLimitParams = params;
    exactLimitParams.maxPartCount = exactPartLimit;
    const int exactLimitWidth = static_cast<int>(exactLimitValues.size());
    const auto exactLimit = ClassicalPartSegmenter::BuildLabels(
        BuildVolume({ exactLimitWidth, 1, 1 },
            std::move(exactLimitValues)),
        exactLimitParams);
    failureCount += GetCaseResult(
        exactLimit.error == PartAlgorithmError::None
            && exactLimit.parts.size() == exactPartLimit
            && exactLimit.parts.back().partId == exactPartLimit,
        "PartId limit accepts the exact configured count") ? 0 : 1;

    const auto cancelled = ClassicalPartSegmenter::BuildLabels(
        separatedVolume, params, [] { return true; });
    failureCount += GetCaseResult(
        cancelled.error == PartAlgorithmError::Cancelled
            && cancelled.labels.empty()
            && cancelled.parts.empty(),
        "Cancellation publishes no partial labels") ? 0 : 1;

    const auto repeated = ClassicalPartSegmenter::BuildLabels(
        separatedVolume, params);
    failureCount += GetCaseResult(
        repeated.error == PartAlgorithmError::None
            && repeated.labels == separated.labels
            && repeated.parts.size() == separated.parts.size(),
        "Repeated run preserves deterministic partition") ? 0 : 1;

    return failureCount;
}
