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

template<typename Scalar>
PartVolumeView BuildVolume(
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
        PartVolumeView{}, params);
    failureCount += GetCaseResult(
        invalid.error == PartAlgorithmError::InvalidInput,
        "Empty volume is rejected") ? 0 : 1;

    const std::vector<double> backgroundValues(8, 0.0);
    const auto background = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 2, 2, 2 }, backgroundValues, PartScalarType::Float64),
        params);
    failureCount += GetCaseResult(
        background.error == PartAlgorithmError::None
            && background.parts.empty()
            && std::all_of(
                background.labels.begin(), background.labels.end(),
                [](const std::uint32_t label) { return label == 0; }),
        "All-background volume returns no parts") ? 0 : 1;

    const std::vector<double> singleValues{ 1.0 };
    auto singleVolume = BuildVolume(
        { 1, 1, 1 }, singleValues, PartScalarType::Float64);
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

    const std::vector<double> separatedValues{
        1.0, 1.0, 0.0, 1.0
    };
    const auto separatedVolume = BuildVolume(
        { 4, 1, 1 }, separatedValues, PartScalarType::Float64);
    const auto separated = ClassicalPartSegmenter::BuildLabels(
        separatedVolume, params);
    failureCount += GetCaseResult(
        separated.error == PartAlgorithmError::None
            && separated.labels
                == std::vector<std::uint32_t>{ 1, 1, 0, 2 }
            && separated.parts.size() == 2,
        "Separated foreground regions receive stable labels") ? 0 : 1;

    const std::vector<double> diagonalValues{ 1.0, 0.0, 0.0, 1.0 };
    const auto diagonal = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 2, 2, 1 }, diagonalValues, PartScalarType::Float64),
        params);
    failureCount += GetCaseResult(
        diagonal.error == PartAlgorithmError::None
            && diagonal.parts.size() == 2,
        "Diagonal contact stays disconnected under 6-connectivity") ? 0 : 1;

    const std::vector<float> maskedValues{ 1.0F, 1.0F };
    const std::vector<std::uint16_t> maskValues{ 256U, 0U };
    auto maskedVolume = BuildVolume(
        { 2, 1, 1 }, maskedValues, PartScalarType::Float32);
    maskedVolume.validity = PartScalarView{
        maskValues.data(), maskValues.size(), PartScalarType::UInt16
    };
    const auto masked = ClassicalPartSegmenter::BuildLabels(
        maskedVolume, params);
    failureCount += GetCaseResult(
        masked.error == PartAlgorithmError::None
            && masked.labels == std::vector<std::uint32_t>{ 1, 0 },
        "UInt16 mask keeps finite nonzero value 256") ? 0 : 1;

    const std::vector<double> nonFiniteValues{
        std::numeric_limits<double>::quiet_NaN(), 1.0
    };
    const auto nonFinite = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 2, 1, 1 }, nonFiniteValues, PartScalarType::Float64),
        params);
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

    auto offsetVolume = BuildVolume(
        { 1, 1, 1 }, singleValues, PartScalarType::Float64);
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
        budget.error == PartAlgorithmError::BudgetExceeded
            && budget.requiredBytes > budgetParams.maxWorkingBytes,
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
    catalogBudgetParams.maxWorkingBytes =
        checkerValues.size() * sizeof(std::uint32_t);
    const auto catalogBudget = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 3, 3, 3 }, checkerValues, PartScalarType::Float64),
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
        BuildVolume(
            { exactLimitWidth, 1, 1 },
            exactLimitValues,
            PartScalarType::Float64),
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

    const std::vector<float> floatValues{
        0.0F, 0.5F, 1.0F, 0.0F,
        1.0F, 1.0F, 0.0F, 0.0F
    };
    const std::vector<double> doubleValues(
        floatValues.begin(), floatValues.end());
    const auto* const floatData = floatValues.data();
    const auto* const doubleData = doubleValues.data();
    const auto floatResult = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 4, 2, 1 }, floatValues, PartScalarType::Float32),
        params);
    const auto doubleResult = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 4, 2, 1 }, doubleValues, PartScalarType::Float64),
        params);
    failureCount += GetCaseResult(
        floatResult.error == PartAlgorithmError::None
            && doubleResult.error == PartAlgorithmError::None
            && floatResult.labels == doubleResult.labels
            && floatResult.parts.size() == doubleResult.parts.size()
            && floatValues.data() == floatData
            && doubleValues.data() == doubleData,
        "Direct float and double views preserve source and match") ? 0 : 1;

    const auto getScalarPass =
        [&](const auto& values, const PartScalarType scalarType) {
            const auto result = ClassicalPartSegmenter::BuildLabels(
                BuildVolume({ 2, 1, 1 }, values, scalarType),
                params);
            return result.error == PartAlgorithmError::None
                && result.labels
                    == std::vector<std::uint32_t>{ 0U, 1U };
        };
    failureCount += GetCaseResult(
        getScalarPass(
            std::vector<std::int8_t>{ 0, 1 },
            PartScalarType::Int8)
            && getScalarPass(
                std::vector<std::uint8_t>{ 0, 1 },
                PartScalarType::UInt8)
            && getScalarPass(
                std::vector<std::int16_t>{ 0, 1 },
                PartScalarType::Int16)
            && getScalarPass(
                std::vector<std::uint16_t>{ 0, 1 },
                PartScalarType::UInt16)
            && getScalarPass(
                std::vector<std::int32_t>{ 0, 1 },
                PartScalarType::Int32)
            && getScalarPass(
                std::vector<std::uint32_t>{ 0, 1 },
                PartScalarType::UInt32)
            && getScalarPass(
                std::vector<std::int64_t>{ 0, 1 },
                PartScalarType::Int64)
            && getScalarPass(
                std::vector<std::uint64_t>{ 0, 1 },
                PartScalarType::UInt64),
        "All fixed-width integer scalar views are dispatched") ? 0 : 1;

    const std::vector<std::uint64_t> maxUnsigned{
        std::numeric_limits<std::uint64_t>::max()
    };
    PartAlgorithmParams uintBoundaryParams = params;
    uintBoundaryParams.threshold = std::ldexp(1.0, 64);
    const auto uintBoundary = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 1, 1, 1 },
            maxUnsigned,
            PartScalarType::UInt64),
        uintBoundaryParams);
    const std::vector<std::int64_t> maxSigned{
        std::numeric_limits<std::int64_t>::max()
    };
    PartAlgorithmParams intBoundaryParams = params;
    intBoundaryParams.threshold = std::ldexp(1.0, 63);
    const auto intBoundary = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 1, 1, 1 },
            maxSigned,
            PartScalarType::Int64),
        intBoundaryParams);
    failureCount += GetCaseResult(
        uintBoundary.error == PartAlgorithmError::None
            && uintBoundary.parts.empty()
            && uintBoundary.labels
                == std::vector<std::uint32_t>{ 0U }
            && intBoundary.error == PartAlgorithmError::None
            && intBoundary.parts.empty()
            && intBoundary.labels
                == std::vector<std::uint32_t>{ 0U },
        "Integer comparison rejects exclusive 64-bit upper bounds")
        ? 0 : 1;

    const std::vector<std::uint8_t> maxUInt8{
        std::numeric_limits<std::uint8_t>::max()
    };
    PartAlgorithmParams uint8FractionParams = params;
    uint8FractionParams.threshold = 255.5;
    const auto uint8Fraction = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 1, 1, 1 }, maxUInt8, PartScalarType::UInt8),
        uint8FractionParams);
    const std::vector<std::int8_t> maxInt8{
        std::numeric_limits<std::int8_t>::max()
    };
    PartAlgorithmParams int8FractionParams = params;
    int8FractionParams.threshold = 127.5;
    const auto int8Fraction = ClassicalPartSegmenter::BuildLabels(
        BuildVolume(
            { 1, 1, 1 }, maxInt8, PartScalarType::Int8),
        int8FractionParams);
    failureCount += GetCaseResult(
        uint8Fraction.error == PartAlgorithmError::None
            && uint8Fraction.parts.empty()
            && int8Fraction.error == PartAlgorithmError::None
            && int8Fraction.parts.empty(),
        "Fractional thresholds above integer maxima stay background")
        ? 0 : 1;

    auto wrongCount = singleVolume;
    wrongCount.values.valueCount = 0;
    auto badGeometry = singleVolume;
    badGeometry.spacing[0] = 0.0;
    auto badDirection = singleVolume;
    badDirection.direction[0] =
        std::numeric_limits<double>::quiet_NaN();
    const auto wrongCountResult = ClassicalPartSegmenter::BuildLabels(
        wrongCount, params);
    const auto badGeometryResult = ClassicalPartSegmenter::BuildLabels(
        badGeometry, params);
    const auto badDirectionResult = ClassicalPartSegmenter::BuildLabels(
        badDirection, params);
    const std::vector<double> overflowWorldValues{ 1.0, 1.0 };
    auto overflowWorld = BuildVolume(
        { 2, 1, 1 }, overflowWorldValues, PartScalarType::Float64);
    overflowWorld.spacing = { 1.0e308, 1.0e-308, 1.0 };
    overflowWorld.origin = { 1.0e308, 0.0, 0.0 };
    const auto overflowWorldResult = ClassicalPartSegmenter::BuildLabels(
        overflowWorld, params);
    failureCount += GetCaseResult(
        wrongCountResult.error == PartAlgorithmError::InvalidInput
            && badGeometryResult.error == PartAlgorithmError::InvalidInput
            && badDirectionResult.error == PartAlgorithmError::InvalidInput
            && overflowWorldResult.error
                == PartAlgorithmError::InvalidInput
            && wrongCountResult.labels.empty()
            && badGeometryResult.labels.empty()
            && badDirectionResult.labels.empty()
            && overflowWorldResult.labels.empty(),
        "Bad pointer count and geometry are rejected before allocation")
        ? 0 : 1;

    return failureCount;
}
