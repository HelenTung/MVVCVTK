#include "SurfaceHistogramReference.h"
#include "SurfaceDeterminationTestSupport.h"
#include "ImagePeakEstimator.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

using SurfaceTest::Checks;

struct HistogramCase final {
    std::string name;
    std::array<int, 3> dimensions{};
    std::vector<double> values;
    std::vector<unsigned char> validity;
    bool succeeds = true;
    bool symmetric = false;
    double expectedBackground = 0;
    double expectedMaterial = 0;
    double expectedIso = 0;
    std::uint64_t expectedSamples = 0;
    std::uint64_t expectedExcluded = 0;
};

double ReadDouble(const void* data, const std::size_t index)
{
    return static_cast<const double*>(data)[index];
}

SurfaceHistogramInput MakeSurfaceInput(const HistogramCase& testCase, const bool allowParallel)
{
    return {testCase.dimensions, testCase.values.data(), ReadDouble,
        testCase.validity.empty() ? nullptr : testCase.validity.data(), allowParallel};
}

ImagePeakInput MakeImagePeakInput(const HistogramCase& testCase)
{
    return {testCase.dimensions, testCase.values.data(), ReadDouble,
        testCase.validity.empty() ? nullptr : testCase.validity.data()};
}

ImagePeakFailure ToImagePeakFailure(const SurfaceFailureReason reason)
{
    switch (reason) {
    case SurfaceFailureReason::None: return ImagePeakFailure::None;
    case SurfaceFailureReason::InvalidSource: return ImagePeakFailure::InvalidSource;
    case SurfaceFailureReason::InvalidGeometry: return ImagePeakFailure::InvalidGeometry;
    case SurfaceFailureReason::ThresholdUnreliable: return ImagePeakFailure::ThresholdUnreliable;
    case SurfaceFailureReason::Cancelled: return ImagePeakFailure::Cancelled;
    default: return ImagePeakFailure::ThresholdUnreliable;
    }
}

template<class Left, class Right>
bool SameEstimate(const std::optional<Left>& left, const std::optional<Right>& right)
{
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return left->isoValue == right->isoValue
        && left->backgroundValue == right->backgroundValue
        && left->materialValue == right->materialValue
        && left->sampleCount == right->sampleCount
        && left->excludedSampleCount == right->excludedSampleCount
        && left->separationRatio == right->separationRatio
        && left->histogramCounts == right->histogramCounts
        && left->histogramEdges == right->histogramEdges
        && left->histogramSampleSteps == right->histogramSampleSteps;
}

void CompareCase(Checks& checks, const HistogramCase& testCase)
{
    std::optional<SurfaceIsoEstimate> reference;
    std::optional<SurfaceIsoEstimate> surfaceSerial;
    std::optional<SurfaceIsoEstimate> surfaceParallel;
    std::optional<ImagePeakEstimate> shared;
    std::string message;
    const auto neverCancelled = [] { return false; };
    const auto referenceFailure = SurfaceHistogramReference::BuildEstimate(
        MakeSurfaceInput(testCase, false), neverCancelled, reference, message);
    const auto serialFailure = SurfaceHistogramEstimator::BuildEstimate(
        MakeSurfaceInput(testCase, false), neverCancelled, surfaceSerial, message);
    const auto parallelFailure = SurfaceHistogramEstimator::BuildEstimate(
        MakeSurfaceInput(testCase, true), neverCancelled, surfaceParallel, message);
    const auto sharedFailure = ImagePeakEstimator::BuildEstimate(
        MakeImagePeakInput(testCase), neverCancelled, shared, message);

    checks.Get(referenceFailure == serialFailure && referenceFailure == parallelFailure
        && ToImagePeakFailure(referenceFailure) == sharedFailure,
        testCase.name + ": frozen reference, Surface serial/parallel, and shared failure status match");
    checks.Get(SameEstimate(reference, surfaceSerial)
        && SameEstimate(reference, surfaceParallel) && SameEstimate(reference, shared),
        testCase.name + ": optional state and every published estimate field match exactly");

    if (!testCase.succeeds) {
        checks.Get(referenceFailure == SurfaceFailureReason::ThresholdUnreliable
            && !reference && !surfaceSerial && !surfaceParallel && !shared,
            testCase.name + ": rejected input publishes no estimate");
        return;
    }
    checks.Get(referenceFailure == SurfaceFailureReason::None && sharedFailure == ImagePeakFailure::None
        && reference && surfaceSerial && surfaceParallel && shared,
        testCase.name + ": expected successful status and estimates are present");
    if (!reference) return;

    checks.Get(reference->sampleCount == testCase.expectedSamples
        && reference->excludedSampleCount == testCase.expectedExcluded,
        testCase.name + ": independently known included and excluded sample counts");
    if (testCase.symmetric) {
        const auto coarseWidth = (reference->histogramEdges.back()
            - reference->histogramEdges.front())
            / static_cast<double>(reference->histogramCounts.size() - 1);
        checks.Get(reference->backgroundValue < 0 && reference->materialValue > 0
            && std::abs(reference->isoValue) <= 0.5 * coarseWidth,
            testCase.name + ": independent symmetric-mode signs and midpoint near zero");
    } else {
        checks.Get(reference->backgroundValue == testCase.expectedBackground
            && reference->materialValue == testCase.expectedMaterial
            && reference->isoValue == testCase.expectedIso,
            testCase.name + ": independently known background/material/ISO values");
        if (testCase.name == "robust-range-outlier") {
            checks.Get(0.5 * -900.0 + 0.5 * 1000.0 == 50.0
                && reference->isoValue == 500.0,
                "single -900 outlier is trimmed: robust ISO is 500, whole-range midpoint is 50");
        }
    }
}

void CompareRejectedInput(Checks& checks, const std::string& name,
    const SurfaceHistogramInput& surfaceInput, const ImagePeakInput& sharedInput,
    const SurfaceFailureReason expected)
{
    std::optional<SurfaceIsoEstimate> reference;
    std::optional<SurfaceIsoEstimate> serial;
    std::optional<SurfaceIsoEstimate> parallel;
    std::optional<ImagePeakEstimate> shared;
    std::string message;
    const auto neverCancelled = [] { return false; };
    const auto a = SurfaceHistogramReference::BuildEstimate(surfaceInput, neverCancelled, reference, message);
    const auto b = SurfaceHistogramEstimator::BuildEstimate(surfaceInput, neverCancelled, serial, message);
    auto parallelInput = surfaceInput;
    parallelInput.allowParallel = true;
    const auto c = SurfaceHistogramEstimator::BuildEstimate(parallelInput, neverCancelled, parallel, message);
    const auto d = ImagePeakEstimator::BuildEstimate(sharedInput, neverCancelled, shared, message);
    checks.Get(a == expected && b == expected && c == expected
        && ToImagePeakFailure(a) == d && !reference && !serial && !parallel && !shared,
        name + ": all implementations return the expected failure without an estimate");
}

void CheckImmediateCancellation(Checks& checks, const HistogramCase& testCase)
{
    std::optional<SurfaceIsoEstimate> reference;
    std::optional<SurfaceIsoEstimate> serial;
    std::optional<SurfaceIsoEstimate> parallel;
    std::optional<ImagePeakEstimate> shared;
    std::string message;
    const auto cancelled = [] { return true; };
    const auto a = SurfaceHistogramReference::BuildEstimate(
        MakeSurfaceInput(testCase, false), cancelled, reference, message);
    const auto b = SurfaceHistogramEstimator::BuildEstimate(
        MakeSurfaceInput(testCase, false), cancelled, serial, message);
    const auto c = SurfaceHistogramEstimator::BuildEstimate(
        MakeSurfaceInput(testCase, true), cancelled, parallel, message);
    const auto d = ImagePeakEstimator::BuildEstimate(
        MakeImagePeakInput(testCase), cancelled, shared, message);
    checks.Get(a == SurfaceFailureReason::Cancelled && b == a && c == a
        && ToImagePeakFailure(a) == d && !reference && !serial && !parallel && !shared,
        testCase.name + ": immediate cancellation discards every estimate");

    unsigned plans = 0;
    const ImagePeakDispatch noWorkers = [](std::size_t, unsigned,
        const std::function<bool()>&, const std::function<void(std::size_t, std::size_t)>&) { return 0U; };
    const auto planned = [&] { ++plans; return 2U; };
    const auto early = ImagePeakEstimator::BuildEstimate(MakeImagePeakInput(testCase),
        cancelled, shared, message, nullptr, 1, noWorkers, planned);
    checks.Get(early == ImagePeakFailure::Cancelled && plans == 0 && !shared,
        "pre-cancelled input never queries the parallel CPU plan");
    const auto dispatched = ImagePeakEstimator::BuildEstimate(MakeImagePeakInput(testCase),
        [] { return false; }, shared, message, nullptr, 1, noWorkers, planned);
    checks.Get(dispatched == ImagePeakFailure::Cancelled && plans == 1 && !shared,
        "a cancelled dispatcher discards its histogram rather than publishing a partial estimate");
}

HistogramCase MakeSymmetricMixture(const double offset)
{
    constexpr std::size_t sampleCount = 128U * 128U * 128U;
    std::array<double, 512> density{};
    std::array<std::size_t, 512> population{};
    for (std::size_t i = 0; i < density.size(); ++i) {
        const auto gray = -12.0 + 24.0 * static_cast<double>(i) / (density.size() - 1);
        const auto left = (gray + offset) / 2.5;
        const auto right = (gray - offset) / 2.5;
        density[i] = std::exp(-0.5 * left * left) + std::exp(-0.5 * right * right);
    }
    const auto totalDensity = std::accumulate(density.begin(), density.end(), 0.0);
    const auto remaining = sampleCount - 17 * population.size();
    for (std::size_t i = 0; i < population.size() / 2; ++i)
        population[i] = population[population.size() - 1 - i] = 17
            + static_cast<std::size_t>(remaining * density[i] / totalDensity);
    auto assigned = std::accumulate(population.begin(), population.end(), std::size_t{});
    for (std::size_t i = 0; assigned < sampleCount; ++i, assigned += 2) {
        ++population[i % (population.size() / 2)];
        ++population[population.size() - 1 - i % (population.size() / 2)];
    }
    HistogramCase testCase;
    testCase.name = offset == 0 ? "symmetric-unimodal" : "symmetric-shallow-valley";
    testCase.dimensions = {128, 128, 128};
    testCase.values.reserve(sampleCount);
    for (std::size_t i = 0; i < population.size(); ++i) {
        const auto gray = -12.0 + 24.0 * static_cast<double>(i) / (population.size() - 1);
        testCase.values.insert(testCase.values.end(), population[i], gray);
    }
    testCase.succeeds = offset != 0;
    testCase.symmetric = offset != 0;
    testCase.expectedSamples = sampleCount;
    return testCase;
}

} // namespace

int GetSharedHistogramFailCount()
{
    Checks checks;

    HistogramCase robust;
    robust.name = "robust-range-outlier";
    robust.dimensions = {32, 32, 32};
    robust.values.resize(32U * 32U * 32U);
    for (std::size_t i = 0; i < robust.values.size(); ++i)
        robust.values[i] = i % 32 < 16 ? 0.0 : 1000.0;
    robust.values[0] = -900.0;
    robust.expectedBackground = 0;
    robust.expectedMaterial = 1000;
    robust.expectedIso = 500;
    robust.expectedSamples = robust.values.size() - 1;
    robust.expectedExcluded = 1;
    CompareCase(checks, robust);

    HistogramCase negative;
    negative.name = "negative-bimodal";
    negative.dimensions = {32, 32, 32};
    negative.values.resize(32U * 32U * 32U);
    for (std::size_t i = 0; i < negative.values.size(); ++i)
        negative.values[i] = i % 32 < 16 ? -1000.0 : -200.0;
    negative.expectedBackground = -1000;
    negative.expectedMaterial = -200;
    negative.expectedIso = -600;
    negative.expectedSamples = negative.values.size();
    CompareCase(checks, negative);

    HistogramCase masked;
    masked.name = "masked-nonfinite-bimodal";
    masked.dimensions = {32, 32, 32};
    masked.values.resize(32U * 32U * 32U);
    for (std::size_t i = 0; i < masked.values.size(); ++i)
        masked.values[i] = i % 32 < 16 ? 0.0 : 1000.0;
    masked.validity.assign(masked.values.size(), 1);
    std::fill(masked.validity.begin(), masked.validity.begin() + 100, 0);
    masked.values[200] = std::numeric_limits<double>::quiet_NaN();
    masked.values[300] = std::numeric_limits<double>::infinity();
    masked.expectedBackground = 0;
    masked.expectedMaterial = 1000;
    masked.expectedIso = 500;
    masked.expectedSamples = masked.values.size() - 100 - 2;
    masked.expectedExcluded = 2;
    CompareCase(checks, masked);

    HistogramCase constant;
    constant.name = "constant";
    constant.dimensions = {32, 32, 32};
    constant.values.assign(32U * 32U * 32U, 10.0);
    constant.succeeds = false;
    CompareCase(checks, constant);

    const auto shallow = MakeSymmetricMixture(3.2);
    CompareCase(checks, shallow);
    const auto unimodal = MakeSymmetricMixture(0.0);
    CompareCase(checks, unimodal);

    checks.Get(SurfaceHistogramReference::GetWorkingBytes(1)
        == SurfaceHistogramEstimator::GetWorkingBytes(1)
        && SurfaceHistogramReference::GetWorkingBytes(2)
        == SurfaceHistogramEstimator::GetWorkingBytes(2)
        && ImagePeakEstimator::GetWorkingBytes(1)
        == SurfaceHistogramReference::GetWorkingBytes(1)
        && ImagePeakEstimator::GetWorkingBytes(2)
        == SurfaceHistogramReference::GetWorkingBytes(2),
        "shared, Surface, and frozen working-byte estimates retain the same formula");

    const HistogramCase& valid = robust;
    CompareRejectedInput(checks, "invalid-source",
        {valid.dimensions, nullptr, ReadDouble, nullptr, false},
        {valid.dimensions, nullptr, ReadDouble, nullptr}, SurfaceFailureReason::InvalidSource);
    CompareRejectedInput(checks, "invalid-geometry",
        {{1, 32, 32}, valid.values.data(), ReadDouble, nullptr, false},
        {{1, 32, 32}, valid.values.data(), ReadDouble, nullptr}, SurfaceFailureReason::InvalidGeometry);
    CheckImmediateCancellation(checks, robust);
    return checks.failureCount;
}
