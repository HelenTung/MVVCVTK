#include "PartSegmentationTestCases.h"

#include "Algorithms/ClassicalPartSegmenter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

bool GetCaseResult(const bool passed, const std::string_view name)
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

template<typename Scalar>
PartVolumeView BuildView(
    const std::array<int, 3>& dimensions,
    const std::vector<Scalar>& values,
    const PartScalarType scalarType)
{
    PartVolumeView volume;
    volume.dimensions = dimensions;
    volume.extent = {
        0, dimensions[0] - 1,
        0, dimensions[1] - 1,
        0, dimensions[2] - 1
    };
    volume.values = { values.data(), values.size(), scalarType };
    return volume;
}

} // namespace

int GetPartScaleFailCount()
{
    int failureCount = 0;
    PartAlgorithmParams params;

    std::vector<float> denseValues(32U * 32U * 32U, 1.0F);
    const auto* const sourceData = denseValues.data();
    const auto dense = ClassicalPartSegmenter::BuildLabels(
        BuildView({ 32, 32, 32 }, denseValues, PartScalarType::Float32),
        params);
    const bool hasNoSentinel = std::none_of(
        dense.labels.begin(), dense.labels.end(),
        [](const std::uint32_t label) {
            return label >= std::numeric_limits<std::uint32_t>::max() - 1U;
        });
    failureCount += GetCaseResult(
        dense.error == PartAlgorithmError::None
            && dense.parts.size() == 1
            && dense.parts[0].voxelCount == denseValues.size()
            && denseValues.data() == sourceData
            && hasNoSentinel
            && dense.metrics.labelBytes
                >= denseValues.size() * sizeof(std::uint32_t)
            && dense.metrics.frontierPeakBytes > 0
            && dense.metrics.peakWorkingBytes <= params.maxWorkingBytes,
        "Dense typed view uses bounded measured working storage") ? 0 : 1;

    PartAlgorithmParams exactBudget = params;
    exactBudget.maxWorkingBytes = dense.metrics.peakWorkingBytes;
    const auto exact = ClassicalPartSegmenter::BuildLabels(
        BuildView({ 32, 32, 32 }, denseValues, PartScalarType::Float32),
        exactBudget);
    failureCount += GetCaseResult(
        exact.error == PartAlgorithmError::None
            && exact.labels == dense.labels,
        "Measured peak is sufficient as the exact rerun budget") ? 0 : 1;

    if (dense.metrics.peakWorkingBytes > 0) {
        exactBudget.maxWorkingBytes = dense.metrics.peakWorkingBytes - 1U;
        const auto limited = ClassicalPartSegmenter::BuildLabels(
            BuildView(
                { 32, 32, 32 }, denseValues,
                PartScalarType::Float32),
            exactBudget);
        failureCount += GetCaseResult(
            limited.error == PartAlgorithmError::BudgetExceeded
                && limited.labels.empty()
                && limited.parts.empty()
                && limited.requiredBytes > exactBudget.maxWorkingBytes,
            "One byte below the measured peak is rejected transactionally")
            ? 0 : 1;
    }

    std::vector<double> filteredValues(4096, 1.0);
    PartAlgorithmParams filteredParams = params;
    filteredParams.minPartVoxels = filteredValues.size() + 1U;
    const auto filtered = ClassicalPartSegmenter::BuildLabels(
        BuildView(
            { static_cast<int>(filteredValues.size()), 1, 1 },
            filteredValues,
            PartScalarType::Float64),
        filteredParams);
    failureCount += GetCaseResult(
        filtered.error == PartAlgorithmError::None
            && filtered.parts.empty()
            && std::all_of(
                filtered.labels.begin(), filtered.labels.end(),
                [](const std::uint32_t label) { return label == 0; })
            && filtered.metrics.filteredPeakBytes
                >= filteredValues.size() * sizeof(std::size_t),
        "All-filtered component restores labels within measured storage")
        ? 0 : 1;

    std::vector<std::uint8_t> thinValues(64U * 64U, 1U);
    std::vector<double> progressValues;
    const auto thin = ClassicalPartSegmenter::BuildLabels(
        BuildView(
            { 64, 64, 1 }, thinValues, PartScalarType::UInt8),
        params,
        nullptr,
        [&](const double progress) {
            progressValues.push_back(progress);
        });
    failureCount += GetCaseResult(
        thin.error == PartAlgorithmError::None
            && thin.parts.size() == 1
            && thin.parts[0].voxelCount == thinValues.size()
            && !progressValues.empty()
            && std::is_sorted(
                progressValues.begin(), progressValues.end())
            && progressValues.front() == 0.0
            && progressValues.back() == 1.0,
        "Thin plane keeps deterministic output and monotonic progress")
        ? 0 : 1;

    constexpr int snakeWidth = 33;
    constexpr int snakeHeight = 17;
    std::vector<std::int16_t> snakeValues(
        static_cast<std::size_t>(snakeWidth * snakeHeight), 0);
    for (int y = 0; y < snakeHeight; ++y) {
        if (y % 2 == 0) {
            std::fill_n(
                snakeValues.begin()
                    + static_cast<std::size_t>(y * snakeWidth),
                snakeWidth,
                static_cast<std::int16_t>(1));
        }
        else {
            const int x = y % 4 == 1 ? snakeWidth - 1 : 0;
            snakeValues[static_cast<std::size_t>(
                x + y * snakeWidth)] = 1;
        }
    }
    const auto snake = ClassicalPartSegmenter::BuildLabels(
        BuildView(
            { snakeWidth, snakeHeight, 1 },
            snakeValues,
            PartScalarType::Int16),
        params);
    failureCount += GetCaseResult(
        snake.error == PartAlgorithmError::None
            && snake.parts.size() == 1
            && std::all_of(
                snake.labels.begin(), snake.labels.end(),
                [](const std::uint32_t label) {
                    return label == 0U || label == 1U;
                }),
        "Serpentine one-voxel path remains one Face6 component") ? 0 : 1;

    std::vector<float> cancelValues(8192, 1.0F);
    int classificationChecks = 0;
    const auto classifyCancelled = ClassicalPartSegmenter::BuildLabels(
        BuildView(
            { static_cast<int>(cancelValues.size()), 1, 1 },
            cancelValues,
            PartScalarType::Float32),
        params,
        [&] {
            ++classificationChecks;
            return classificationChecks >= 3;
        });
    failureCount += GetCaseResult(
        classifyCancelled.error == PartAlgorithmError::Cancelled
            && classifyCancelled.labels.empty()
            && classifyCancelled.parts.empty(),
        "Classification cancellation publishes no partial result") ? 0 : 1;

    bool stopConnectivity = false;
    const auto connectivityCancelled =
        ClassicalPartSegmenter::BuildLabels(
            BuildView(
                { static_cast<int>(cancelValues.size()), 1, 1 },
                cancelValues,
                PartScalarType::Float32),
            params,
            [&] { return stopConnectivity; },
            [&](const double progress) {
                if (progress > 0.30) stopConnectivity = true;
            });
    failureCount += GetCaseResult(
        connectivityCancelled.error == PartAlgorithmError::Cancelled
            && connectivityCancelled.labels.empty()
            && connectivityCancelled.parts.empty(),
        "Frontier cancellation publishes no partial result") ? 0 : 1;

    const std::vector<float> restoreValues{ 1.0F };
    PartAlgorithmParams restoreParams = params;
    restoreParams.minPartVoxels = 2;
    int restoreChecks = 0;
    const auto restoreCancelled = ClassicalPartSegmenter::BuildLabels(
        BuildView(
            { 1, 1, 1 }, restoreValues, PartScalarType::Float32),
        restoreParams,
        [&] {
            ++restoreChecks;
            return restoreChecks >= 5;
        });
    failureCount += GetCaseResult(
        restoreCancelled.error == PartAlgorithmError::Cancelled
            && restoreCancelled.labels.empty()
            && restoreCancelled.parts.empty(),
        "Filtered-label restoration cancellation is transactional") ? 0 : 1;

    return failureCount;
}
