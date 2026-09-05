#include "QtHostMethodCases.h"

#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "App/Services/AppPorts.h"
#include "App/Services/AppServiceFactory.h"
#include "DataConverters.h"
#include "Data/DataManager.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"
#include "ImageProcessor.h"
#include "CompositeStrategy.h"
#include "IsoSurfaceStrategy.h"
#include "Render/Internal/IsoLodController.h"
#include "Render/Internal/VolumeLodController.h"
#include "VolumeStrategy.h"

#include <vtkActor.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataObject.h>
#include <vtkDoubleArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkIdTypeArray.h>
#include <vtkImageResample.h>
#include <vtkImageReslice.h>
#include <vtkInformation.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkTable.h>
#include <vtkTriangle.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkVolume.h>
#include <vtkVolumeMapper.h>
#include <vtkVolumeProperty.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace {

static_assert(static_cast<int>(VolumeQuality::Auto) == 0);
static_assert(static_cast<int>(VolumeQuality::Low) == 1);
static_assert(static_cast<int>(VolumeQuality::High) == 2);
static_assert(static_cast<int>(VolumeQuality::XHigh) == 3);
static_assert(static_cast<int>(VolumeQuality::Ultra) == 4);

class GpuProbeStrategy final : public VolumeStrategy {
public:
    bool SetProbeBytes(const std::uint64_t freeBytes) noexcept
    {
        m_freeBytes = freeBytes;
        return true;
    }

    bool GetQueryOrderValid() const noexcept
    {
        return m_isQueryOrderValid;
    }

private:
    std::optional<std::uint64_t> GetGpuFreeBytes() const override
    {
        const std::uint64_t releaseCount = GetGpuReleaseCount();
        m_isQueryOrderValid = m_isQueryOrderValid
            && releaseCount > m_lastQueryReleaseCount;
        m_lastQueryReleaseCount = releaseCount;
        return m_freeBytes;
    }

    std::uint64_t m_freeBytes = 0;
    mutable std::uint64_t m_lastQueryReleaseCount = 0;
    mutable bool m_isQueryOrderValid = true;
};

double GetRenderRate(const bool isInteracting) noexcept
{
    return isInteracting ? 15.0 : 0.001;
}

vtkSmartPointer<vtkImageData> BuildFloatImage(
    const std::vector<float>& values)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(
        static_cast<int>(values.size()), 1, 1);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::copy(
        values.begin(),
        values.end(),
        static_cast<float*>(image->GetScalarPointer()));
    return image;
}

struct RoiStats final {
    double leftMean = 0.0;
    double rightMean = 0.0;
    double leftVariance = 0.0;
    double rightVariance = 0.0;
};

RoiStats GetRoiStats(vtkImageData* image)
{
    RoiStats result;
    if (!image) return result;
    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    const auto* scalars =
        static_cast<const float*>(image->GetScalarPointer());
    std::vector<double> left;
    std::vector<double> right;
    for (int z = 1; z < dims[2] - 1; ++z) {
        for (int y = 1; y < dims[1] - 1; ++y) {
            for (int x = 2; x < dims[0] - 2; ++x) {
                const vtkIdType index =
                    x + dims[0] * (y + dims[1] * z);
                if (x < dims[0] / 2 - 4) {
                    left.push_back(scalars[index]);
                }
                else if (x >= dims[0] / 2 + 4) {
                    right.push_back(scalars[index]);
                }
            }
        }
    }
    const auto getMean = [](const std::vector<double>& values) {
        if (values.empty()) return 0.0;
        return std::accumulate(
            values.begin(), values.end(), 0.0)
            / static_cast<double>(values.size());
    };
    const auto getVariance = [&getMean](
        const std::vector<double>& values) {
        if (values.empty()) return 0.0;
        const double mean = getMean(values);
        double sum = 0.0;
        for (const double value : values) {
            const double delta = value - mean;
            sum += delta * delta;
        }
        return sum / static_cast<double>(values.size());
    };
    result.leftMean = getMean(left);
    result.rightMean = getMean(right);
    result.leftVariance = getVariance(left);
    result.rightVariance = getVariance(right);
    return result;
}

int GetHistogramFailCount()
{
    int failureCount = 0;
    HistogramConverter converter;
    converter.SetBinCount(4);
    auto image = BuildFloatImage({ 0.0f, 1.0f, 2.0f, 3.0f });
    auto table = converter.GetOutputData(image);
    const std::uint64_t firstBuildCount = converter.GetBuildCount();
    auto* intensity = table
        ? vtkDoubleArray::SafeDownCast(
            table->GetColumnByName("Intensity"))
        : nullptr;
    auto* frequency = table
        ? vtkIdTypeArray::SafeDownCast(
            table->GetColumnByName("Frequency"))
        : nullptr;
    vtkIdType frequencySum = 0;
    if (frequency) {
        for (vtkIdType index = 0;
            index < frequency->GetNumberOfValues();
            ++index) {
            frequencySum += frequency->GetValue(index);
        }
    }
    failureCount += GetCaseResult(
        intensity && frequency
            && intensity->GetValue(3) == 3.0
            && frequencySum == 4
            && frequency->GetValue(3) == 1,
        "Histogram keeps exact vtkIdType counts and includes scalar max") ? 0 : 1;

    const auto median = converter.GetHistogramPercentile(image, 0.5);
    failureCount += GetCaseResult(
        converter.GetHistogramPercentile(image, 0.0) == 0.0
            && median == 1.0
            && converter.GetHistogramPercentile(image, 1.0) == 3.0
            && !converter.GetHistogramPercentile(image, -0.1)
            && firstBuildCount == 1
            && converter.GetBuildCount() == firstBuildCount,
        "Histogram reuses one build for percentile boundaries") ? 0 : 1;

    auto constantImage = BuildFloatImage(
        { 7.0f, 7.0f, 7.0f, 7.0f });
    failureCount += GetCaseResult(
        converter.GetHistogramPercentile(constantImage, 0.37) == 7.0
            && converter.GetBuildCount() == firstBuildCount + 1,
        "Histogram constant-volume percentile") ? 0 : 1;

    constexpr int largeCount = 16'777'217;
    auto largeImage = vtkSmartPointer<vtkImageData>::New();
    largeImage->SetDimensions(largeCount, 1, 1);
    largeImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(
            largeImage->GetScalarPointer()),
        largeCount,
        static_cast<unsigned char>(0));
    auto largeTable = converter.GetOutputData(largeImage);
    auto* largeFrequency = largeTable
        ? vtkIdTypeArray::SafeDownCast(
            largeTable->GetColumnByName("Frequency"))
        : nullptr;
    failureCount += GetCaseResult(
        largeFrequency
            && largeFrequency->GetValue(0)
                == static_cast<vtkIdType>(largeCount)
            && converter.GetBuildCount() == firstBuildCount + 2,
        "Histogram count remains exact above 2^24") ? 0 : 1;

    const auto tempDir = std::filesystem::temp_directory_path();
    const std::string fileId = std::to_string(
        reinterpret_cast<std::uintptr_t>(image.GetPointer()));
    const auto pngPath = tempDir / std::filesystem::u8path(
        u8"mvvcvtk-直方图-é-" + fileId + ".png");
    const auto jpegPath = tempDir / std::filesystem::u8path(
        u8"mvvcvtk-直方图-é-" + fileId + ".other");
    converter.ExportHistogram(image, pngPath.u8string());
    converter.ExportHistogram(image, jpegPath.u8string());
    failureCount += GetCaseResult(
        std::filesystem::exists(pngPath)
            && std::filesystem::exists(jpegPath),
        "Histogram UTF-8 PNG and fallback JPEG export") ? 0 : 1;
    std::error_code error;
    std::filesystem::remove(pngPath, error);
    error.clear();
    std::filesystem::remove(jpegPath, error);
    return failureCount;
}

int GetResampleFailCount()
{
    int failureCount = 0;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(900, 450, 225);
    image->SetSpacing(0.5, 1.0, 2.0);
    image->SetOrigin(4.0, 5.0, 6.0);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    constexpr double qualityRatio = 0.8;
    constexpr double customRatio = 256.0 / 900.0;
    auto quality = ImageProcessor::CreateScaledImage(
        image, qualityRatio);
    auto custom = ImageProcessor::CreateScaledImage(
        image, customRatio);
    if (quality) quality->UpdateInformation();
    if (custom) custom->UpdateInformation();
    int qualityDims[3] = { 0, 0, 0 };
    int customDims[3] = { 0, 0, 0 };
    int qualityExtent[6] = { 0, -1, 0, -1, 0, -1 };
    int customExtent[6] = { 0, -1, 0, -1, 0, -1 };
    double qualityOrigin[3] = { 0.0, 0.0, 0.0 };
    double qualitySpacing[3] = { 0.0, 0.0, 0.0 };
    if (quality && quality->GetOutputInformation(0)) {
        auto* outputInfo = quality->GetOutputInformation(0);
        outputInfo->Get(
            vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
            qualityExtent);
        outputInfo->Get(vtkDataObject::ORIGIN(), qualityOrigin);
        outputInfo->Get(vtkDataObject::SPACING(), qualitySpacing);
    }
    if (custom && custom->GetOutputInformation(0)) {
        custom->GetOutputInformation(0)->Get(
            vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
            customExtent);
    }
    for (int axis = 0; axis < 3; ++axis) {
        qualityDims[axis] =
            qualityExtent[2 * axis + 1]
            - qualityExtent[2 * axis] + 1;
        customDims[axis] =
            customExtent[2 * axis + 1]
            - customExtent[2 * axis] + 1;
    }
    const bool isGeometryValid =
        quality && custom
            && qualityDims[0] == 720
            && qualityDims[1] == 360
            && qualityDims[2] == 180
            && customDims[0] == 256
            && customDims[1] == 128
            && customDims[2] == 64
            && quality->GetInterpolationMode()
                == VTK_RESLICE_LINEAR
            && qualityOrigin[0] == 4.0
            && std::abs(
                qualitySpacing[0]
                - 0.5 / qualityRatio) < 1e-12
            && std::abs(
                qualityOrigin[0]
                + (qualityDims[0] - 1) * qualitySpacing[0]
                - (4.0 + 719.0
                    * (0.5 / qualityRatio))) < 1e-9;
    if (!isGeometryValid) {
        std::cerr << "Resample actual quality="
            << qualityDims[0] << 'x' << qualityDims[1] << 'x'
            << qualityDims[2] << " custom="
            << customDims[0] << 'x' << customDims[1]
            << 'x' << customDims[2] << " mode="
            << (quality ? quality->GetInterpolationMode() : -1)
            << " origin="
            << qualityOrigin[0]
            << '\n';
    }
    failureCount += GetCaseResult(
        isGeometryValid,
        "Resample preserves original axis ratios and geometry") ? 0 : 1;

    auto mask = ImageProcessor::CreateScaledMask(
        image, customRatio);
    failureCount += GetCaseResult(
        mask && mask->GetInterpolationMode()
            == VTK_RESLICE_NEAREST,
        "Validity mask uses nearest interpolation") ? 0 : 1;
    failureCount += GetCaseResult(
        !ImageProcessor::CreateScaledImage(image, 0.0)
            && !ImageProcessor::CreateScaledImage(nullptr, 1.0)
            && !ImageProcessor::CreateScaledImage(image, 1.01),
        "Resample rejects null input and ratios outside (0,1]") ? 0 : 1;

    bool hasInvalidAxisRejected = true;
    constexpr std::array invalidDimensions{
        std::array{ 0, 64, 32 },
        std::array{ 128, 0, 32 },
        std::array{ 128, 64, 0 }
    };
    for (const auto& dimensions : invalidDimensions) {
        auto invalidImage = vtkSmartPointer<vtkImageData>::New();
        invalidImage->SetDimensions(
            dimensions[0], dimensions[1], dimensions[2]);
        hasInvalidAxisRejected =
            !ImageProcessor::CreateScaledImage(
                invalidImage, 0.5)
            && hasInvalidAxisRejected;
    }
    failureCount += GetCaseResult(
        hasInvalidAxisRejected,
        "Resample rejects every non-positive source axis") ? 0 : 1;

    auto smallImage = vtkSmartPointer<vtkImageData>::New();
    smallImage->SetDimensions(128, 64, 32);
    smallImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto smallResample =
        ImageProcessor::CreateScaledImage(smallImage, 1.0);
    smallResample->UpdateInformation();
    int smallExtent[6] = { 0, -1, 0, -1, 0, -1 };
    smallResample->GetOutputInformation(0)->Get(
        vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
        smallExtent);
    failureCount += GetCaseResult(
        smallExtent[1] - smallExtent[0] + 1 == 128
            && smallExtent[3] - smallExtent[2] + 1 == 64
            && smallExtent[5] - smallExtent[4] + 1 == 32,
        "Ratio 1.0 preserves native dimensions") ? 0 : 1;
    return failureCount;
}

int GetLodControlFailCount()
{
    int failureCount = 0;
    VolumeLodController controller;
    VolumeLodController::Source source;
    source.dimensions = { 1000, 500, 250 };
    source.nativeBytes = 500'000'000ULL;
    source.maskBytes = 125'000'000ULL;
    source.systemMemoryBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    source.gpuMemoryBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    source.cpuThreadCount = 8;
    const bool isSourceSet = controller.SetSource(source);
    const auto autoProfile = controller.GetProfile(VolumeQuality::Auto);
    const auto lowProfile = controller.GetProfile(VolumeQuality::Low);
    const auto highProfile = controller.GetProfile(VolumeQuality::High);
    const auto xHighProfile = controller.GetProfile(VolumeQuality::XHigh);
    const auto ultraProfile = controller.GetProfile(VolumeQuality::Ultra);
    failureCount += GetCaseResult(
        isSourceSet
            && autoProfile.dimensionRatio >= 0.25
            && autoProfile.dimensionRatio <= 1.0
            && lowProfile.dimensionRatio == 0.25
            && lowProfile.outputDimensions
                == std::array<int, 3>{ 250, 125, 63 }
            && highProfile.dimensionRatio == 0.50
            && highProfile.outputDimensions
                == std::array<int, 3>{ 500, 250, 125 }
            && highProfile.previewImageDistance == 3.0
            && highProfile.previewRayFactor == 3.0
            && !highProfile.isPreviewJitterOn
            && xHighProfile.dimensionRatio == 0.75
            && xHighProfile.outputDimensions
                == std::array<int, 3>{ 750, 375, 188 }
            && xHighProfile.previewImageDistance == 3.0
            && xHighProfile.previewRayFactor == 4.0
            && !xHighProfile.isPreviewJitterOn
            && ultraProfile.dimensionRatio == 1.0
            && ultraProfile.outputDimensions == source.dimensions
            && lowProfile.previewImageDistance == 2.0
            && lowProfile.previewRayFactor == 2.0
            && !lowProfile.isPreviewJitterOn
            && ultraProfile.previewImageDistance == 4.0
            && ultraProfile.previewRayFactor == 4.0
            && !ultraProfile.isPreviewJitterOn,
        "Explicit LOD tiers keep fixed data and preview sampling profiles")
        ? 0 : 1;

    VolumeLodController::Source abundantSource = source;
    abundantSource.systemMemoryBytes =
        8ULL * 1024ULL * 1024ULL * 1024ULL;
    abundantSource.gpuMemoryBytes =
        4ULL * 1024ULL * 1024ULL * 1024ULL;
    abundantSource.cpuThreadCount = 16;
    abundantSource.isNativeAliasAllowed = false;
    (void)controller.SetSource(abundantSource);
    const double abundantRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    auto lowRamSource = abundantSource;
    lowRamSource.systemMemoryBytes = 700ULL * 1024ULL * 1024ULL;
    (void)controller.SetSource(lowRamSource);
    const double lowRamRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    auto lowGpuSource = abundantSource;
    lowGpuSource.gpuMemoryBytes = 700ULL * 1024ULL * 1024ULL;
    (void)controller.SetSource(lowGpuSource);
    const double lowGpuRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    auto lowCpuSource = abundantSource;
    lowCpuSource.systemMemoryBytes =
        2100ULL * 1024ULL * 1024ULL;
    lowCpuSource.cpuThreadCount = 1;
    (void)controller.SetSource(lowCpuSource);
    const double lowCpuRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    auto highCpuSource = lowCpuSource;
    highCpuSource.cpuThreadCount = 16;
    (void)controller.SetSource(highCpuSource);
    const double highCpuRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    failureCount += GetCaseResult(
        lowRamRatio < abundantRatio
            && lowGpuRatio < abundantRatio
            && lowCpuRatio < highCpuRatio,
        "Load-time Auto plan consumes RAM GPU and CPU resources")
        ? 0 : 1;

    VolumeLodController::Source firstShape;
    firstShape.dimensions = { 9, 5, 3 };
    firstShape.nativeBytes = 540ULL;
    firstShape.systemMemoryBytes = 1000ULL;
    firstShape.gpuMemoryBytes = 1000ULL;
    firstShape.cpuThreadCount = 2;
    VolumeLodController::Source secondShape = firstShape;
    secondShape.dimensions = { 15, 3, 3 };
    const bool isFirstShapeSet = controller.SetSource(firstShape);
    const auto firstShapeProfile =
        controller.GetProfile(VolumeQuality::High);
    const bool isSecondShapeSet = controller.SetSource(secondShape);
    const auto secondShapeProfile =
        controller.GetProfile(VolumeQuality::High);
    VolumeLodController::Source overflowSource;
    overflowSource.dimensions = { 1, 1, 1 };
    overflowSource.nativeBytes =
        std::numeric_limits<std::uint64_t>::max();
    overflowSource.maskBytes = 1ULL;
    overflowSource.systemMemoryBytes = 1024ULL;
    overflowSource.gpuMemoryBytes = 1024ULL;
    overflowSource.cpuThreadCount = 1;
    failureCount += GetCaseResult(
        isFirstShapeSet
            && isSecondShapeSet
            && firstShapeProfile.outputDimensions
                != secondShapeProfile.outputDimensions
            && !controller.SetSource(overflowSource),
        "LOD plan uses original dimensions and rejects byte overflow")
        ? 0 : 1;

    (void)controller.SetSource(source);
    (void)controller.SetQuality(VolumeQuality::High);
    const auto stableProfile = controller.GetProfile();
    (void)controller.SetQuality(VolumeQuality::Low);
    (void)controller.SetQuality(VolumeQuality::XHigh);
    (void)controller.SetQuality(VolumeQuality::High);
    const auto selectedProfile = controller.GetProfile();
    failureCount += GetCaseResult(
        stableProfile.dimensionRatio
                == selectedProfile.dimensionRatio
            && stableProfile.outputDimensions
                == selectedProfile.outputDimensions,
        "Quality switches reuse the immutable load-time LOD plan")
        ? 0 : 1;

    const bool isUltraSet =
        controller.SetQuality(VolumeQuality::Ultra);
    const bool isReducedActiveRejected =
        !controller.SetActiveRatio(0.5);
    failureCount += GetCaseResult(
        isUltraSet
            && isReducedActiveRejected
            && controller.GetTargetRatio() == 1.0
            && controller.GetProfile().outputDimensions
                == source.dimensions,
        "Ultra is a strict native-resolution invariant") ? 0 : 1;
    return failureCount;
}

int GetIsoLodControlFailCount()
{
    int failureCount = 0;
    IsoLodController controller;
    IsoLodController::Source source;
    source.dimensions = { 3, 2, 1 };
    source.nativeBytes = 128ULL;
    source.maskBytes = 64ULL;
    source.systemMemoryBytes = 64ULL * 1024ULL * 1024ULL;
    source.cpuThreadCount = 16;
    const bool isSourceSet = controller.SetSource(source);
    failureCount += GetCaseResult(
        isSourceSet
            && controller.GetProfile(VolumeQuality::Low).outputDimensions
                == std::array<int, 3>{ 1, 1, 1 }
            && controller.GetProfile(VolumeQuality::High).outputDimensions
                == std::array<int, 3>{ 2, 1, 1 }
            && controller.GetProfile(VolumeQuality::XHigh).outputDimensions
                == std::array<int, 3>{ 3, 2, 1 }
            && controller.GetProfile(VolumeQuality::Ultra).outputDimensions
                == source.dimensions,
        "Iso LOD keeps exact explicit ratios for small axes") ? 0 : 1;

    auto noProbeSource = source;
    noProbeSource.dimensions = { 1000, 500, 250 };
    noProbeSource.nativeBytes = 500'000'000ULL;
    noProbeSource.maskBytes = 125'000'000ULL;
    noProbeSource.systemMemoryBytes = 0;
    noProbeSource.cpuThreadCount = 16;
    const bool isNoProbeSet = controller.SetSource(noProbeSource);
    const double noProbeRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    auto lowCpuSource = noProbeSource;
    lowCpuSource.systemMemoryBytes =
        64ULL * 1024ULL * 1024ULL * 1024ULL;
    lowCpuSource.cpuThreadCount = 0;
    const bool isLowCpuSet = controller.SetSource(lowCpuSource);
    const double lowCpuRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    auto abundantSource = lowCpuSource;
    abundantSource.cpuThreadCount = 16;
    const bool isAbundantSet = controller.SetSource(abundantSource);
    const double abundantRatio =
        controller.GetProfile(VolumeQuality::Auto).dimensionRatio;
    failureCount += GetCaseResult(
        isNoProbeSet
            && noProbeRatio >= 0.25 && noProbeRatio <= 1.0
            && isLowCpuSet && lowCpuRatio == 0.25
            && isAbundantSet && abundantRatio == 1.0,
        "Iso Auto remains bounded across probe failure and CPU limits") ? 0 : 1;

    IsoLodController::Source overflowSource = abundantSource;
    overflowSource.dimensions = { 1, 1, 1 };
    overflowSource.nativeBytes =
        std::numeric_limits<std::uint64_t>::max();
    overflowSource.maskBytes = 1ULL;
    const auto stableProfile = controller.GetProfile(VolumeQuality::Ultra);
    const bool isOverflowRejected =
        !controller.SetSource(overflowSource);
    const bool isInvalidQualityRejected =
        !controller.SetQuality(static_cast<VolumeQuality>(99));
    failureCount += GetCaseResult(
        isOverflowRejected
            && isInvalidQualityRejected
            && controller.GetProfile(VolumeQuality::Ultra).outputDimensions
                == stableProfile.outputDimensions
            && controller.GetQuality() == VolumeQuality::Auto,
        "Iso LOD rejects overflow and invalid quality without mutation") ? 0 : 1;
    return failureCount;
}

int GetRenderContractFailCount()
{
    int failureCount = 0;
    constexpr std::array configuredQualities{
        VolumeQuality::Auto,
        VolumeQuality::Low,
        VolumeQuality::High,
        VolumeQuality::XHigh,
        VolumeQuality::Ultra
    };
    failureCount += GetCaseResult(
        configuredQualities.size() == 5,
        "Volume quality exposes Auto through strict native Ultra") ? 0 : 1;

    auto eventSource = std::make_shared<SharedStateBroadcaster>();
    auto appState = std::make_shared<SharedInteractionState>(
        eventSource);
    AppServiceArgs serviceArgs;
    serviceArgs.dataManager =
        std::make_shared<RawVolumeDataManager>();
    serviceArgs.interactionState = appState;
    serviceArgs.eventSource = eventSource;
    auto servicePorts = CreateAppPorts(
        std::move(serviceArgs));
    const FeatureSource firstSource{ "feature.first" };
    const FeatureSource secondSource{ "feature.second" };
    AppViewUpdate qualityUpdate;
    qualityUpdate.volumeQuality = VolumeQuality::XHigh;
    const bool isQualityAccepted = servicePorts.app.view
        && servicePorts.app.feature
        && servicePorts.app.view->SendViewUpdate(qualityUpdate);
    const bool hasFeatureAggregate =
        isQualityAccepted
        && servicePorts.app.feature->SetFeatureActive(
            firstSource, true)
        && servicePorts.app.feature->SetFeatureActive(
            secondSource, true)
        && servicePorts.app.feature->SetFeatureActive(
            firstSource, false)
        && servicePorts.app.view->GetViewState().isFeatureActive
        && servicePorts.app.feature->SetFeatureActive(
            secondSource, false)
        && !servicePorts.app.view->GetViewState().isFeatureActive
        && servicePorts.app.view->GetViewState().volumeQuality
            == VolumeQuality::Auto;
    failureCount += GetCaseResult(
        hasFeatureAggregate,
        "Feature sources aggregate while unapplied quality remains unchanged") ? 0 : 1;

    VolumeTransferFunction transferFunction;
    transferFunction.colorNodes = {
        { 0.0, 0.0, 0.0, 0.0 },
        { 1.0, 1.0, 1.0, 1.0 }
    };
    transferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 1.0, 0.6 }
    };
    AppViewUpdate transferUpdate;
    transferUpdate.volumeTransferFunction = transferFunction;
    const bool isTransferSet = servicePorts.app.view
        ->SendViewUpdate(transferUpdate);
    const auto transferState =
        servicePorts.app.view->GetViewState();
    auto incompleteFunction = transferFunction;
    incompleteFunction.opacityNodes.resize(1);
    transferUpdate.volumeTransferFunction = incompleteFunction;
    const bool isIncompleteRejected =
        !servicePorts.app.view->SendViewUpdate(transferUpdate);
    auto singleColorFunction = transferFunction;
    singleColorFunction.colorNodes.resize(1);
    transferUpdate.volumeTransferFunction = singleColorFunction;
    const bool isSingleColorRejected =
        !servicePorts.app.view->SendViewUpdate(transferUpdate);
    auto duplicateFunction = transferFunction;
    duplicateFunction.colorNodes[1].scalar =
        duplicateFunction.colorNodes[0].scalar;
    transferUpdate.volumeTransferFunction = duplicateFunction;
    const bool isDuplicateRejected =
        !servicePorts.app.view->SendViewUpdate(transferUpdate);
    auto descendingColor = transferFunction;
    descendingColor.colorNodes[1].scalar = -1.0;
    transferUpdate.volumeTransferFunction = descendingColor;
    const bool isColorOrderRejected =
        !servicePorts.app.view->SendViewUpdate(transferUpdate);
    auto descendingOpacity = transferFunction;
    descendingOpacity.opacityNodes[1].scalar = -1.0;
    transferUpdate.volumeTransferFunction = descendingOpacity;
    const bool isOpacityOrderRejected =
        !servicePorts.app.view->SendViewUpdate(transferUpdate);
    const auto rejectedState =
        servicePorts.app.view->GetViewState();
    failureCount += GetCaseResult(
        isTransferSet
            && isIncompleteRejected
            && isSingleColorRejected
            && isDuplicateRejected
            && isColorOrderRejected
            && isOpacityOrderRejected
            && rejectedState.revision == transferState.revision
            && rejectedState.volumeTransferFunction.colorNodes.size()
                == transferState.volumeTransferFunction.colorNodes.size()
            && rejectedState.volumeTransferFunction.opacityNodes.size()
                == transferState.volumeTransferFunction.opacityNodes.size()
            && rejectedState.volumeTransferFunction.colorNodes[1].scalar
                == transferState.volumeTransferFunction.colorNodes[1].scalar
            && rejectedState.volumeTransferFunction.opacityNodes[1].scalar
                == transferState.volumeTransferFunction.opacityNodes[1].scalar
            && std::abs(
                transferState.volumeTransferFunction
                    .opacityNodes.back().opacity - 0.6) < 1e-12,
        "ViewSet keeps one complete scalar transfer snapshot") ? 0 : 1;

    auto dimensionImage = vtkSmartPointer<vtkImageData>::New();
    dimensionImage->SetDimensions(1200, 1, 1);
    dimensionImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    VolumeStrategy dimensionStrategy;
    const bool isDimensionInputSet = dimensionStrategy.SetInputData(
        dimensionImage, nullptr);
    auto* dimensionVolume = vtkVolume::SafeDownCast(
        dimensionStrategy.GetMainProp());
    auto* dimensionMapper = dimensionVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            dimensionVolume->GetMapper())
        : nullptr;
    const auto getMaxDimension =
        [](vtkGPUVolumeRayCastMapper* mapper) {
        if (!mapper) return 0;
        mapper->Update();
        auto* input = vtkImageData::SafeDownCast(
            mapper->GetInput());
        if (!input) return 0;
        int dimensions[3] = {};
        input->GetDimensions(dimensions);
        return std::max(
            { dimensions[0], dimensions[1], dimensions[2] });
    };
    const auto getDimensions =
        [](vtkGPUVolumeRayCastMapper* mapper) {
        std::array<int, 3> dimensions{};
        if (!mapper) return dimensions;
        mapper->Update();
        auto* input = vtkImageData::SafeDownCast(mapper->GetInput());
        if (input) {
            std::copy_n(
                input->GetDimensions(),
                dimensions.size(),
                dimensions.begin());
        }
        return dimensions;
    };
    RenderParams dimensionParams;
    constexpr std::array qualityProfiles{
        std::pair{ VolumeQuality::Auto, 2.5 },
        std::pair{ VolumeQuality::Low, 3.0 },
        std::pair{ VolumeQuality::High, 2.5 },
        std::pair{ VolumeQuality::XHigh, 2.0 },
        std::pair{ VolumeQuality::Ultra, 1.5 }
    };
    bool areQualityProfilesSet = dimensionMapper != nullptr;
    bool isUltraNative = false;
    const std::uint64_t dimensionPlanCount =
        dimensionStrategy.GetLodPlanCount();
    for (const auto& [quality, maximumImageDistance] :
        qualityProfiles) {
        dimensionParams.volumeQuality = quality;
        const std::uint64_t resampleCountBefore =
            dimensionStrategy.GetResampleBuildCount();
        const bool isQualitySet = dimensionStrategy.SetVisualState(
            dimensionParams, UpdateFlags::Quality);
        const auto plannedDimensions =
            dimensionStrategy.GetLodDimensions(quality);
        const bool hasNativeDimensions =
            plannedDimensions == std::array<int, 3>{ 1200, 1, 1 };
        const std::uint64_t expectedResampleCount =
            resampleCountBefore + (hasNativeDimensions ? 0ULL : 1ULL);
        if (quality == VolumeQuality::Ultra) {
            isUltraNative = dimensionMapper
                && dimensionMapper->GetInput()
                    == dimensionImage.GetPointer()
            && dimensionStrategy.GetResampleBuildCount()
                == resampleCountBefore;
        }
        areQualityProfilesSet = areQualityProfilesSet
            && isQualitySet
            && getDimensions(dimensionMapper)
                == plannedDimensions
            && dimensionStrategy.GetResampleBuildCount()
                == expectedResampleCount
            && std::abs(
                dimensionMapper->GetMaximumImageSampleDistance()
                    - maximumImageDistance) < 1e-12;
    }
    failureCount += GetCaseResult(
        isDimensionInputSet
            && areQualityProfilesSet
            && dimensionPlanCount == 1
            && dimensionStrategy.GetLodPlanCount()
                == dimensionPlanCount + 1
            && isUltraNative
            && dimensionStrategy.GetLodDimensions(
                VolumeQuality::Low)
                == std::array<int, 3>{ 300, 1, 1 }
            && dimensionStrategy.GetLodDimensions(
                VolumeQuality::High)
                == std::array<int, 3>{ 600, 1, 1 }
            && dimensionStrategy.GetLodDimensions(
                VolumeQuality::XHigh)
                == std::array<int, 3>{ 900, 1, 1 }
            && dimensionStrategy.GetLodDimensions(
                VolumeQuality::Ultra)
                == std::array<int, 3>{ 1200, 1, 1 },
        "Quality tiers keep fixed dimensions while explicit Auto re-resolves") ? 0 : 1;

    auto gpuGateImage = vtkSmartPointer<vtkImageData>::New();
    gpuGateImage->SetDimensions(128, 128, 128);
    gpuGateImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(gpuGateImage->GetScalarPointer()),
        gpuGateImage->GetNumberOfPoints(),
        static_cast<unsigned char>(128));
    GpuProbeStrategy gpuGateStrategy;
    RenderParams gpuGateParams;
    gpuGateParams.volumeQuality = VolumeQuality::Low;
    gpuGateParams.volumeTransferFunction.colorNodes = {
        { 0.0, 0.0, 0.0, 0.0 },
        { 255.0, 1.0, 1.0, 1.0 }
    };
    gpuGateParams.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 255.0, 1.0 }
    };
    const bool isGpuGateConfigured = gpuGateStrategy.SetVisualState(
        gpuGateParams,
        UpdateFlags::VolumeTransfer
            | UpdateFlags::Material
            | UpdateFlags::Quality);
    const bool isGpuGateInputSet = gpuGateStrategy.SetInputData(
        gpuGateImage, nullptr);
    auto* gpuGateVolume = vtkVolume::SafeDownCast(
        gpuGateStrategy.GetMainProp());
    auto* gpuGateMapper = gpuGateVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            gpuGateVolume->GetMapper())
        : nullptr;
    auto gpuGateRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto gpuGateWindow = vtkSmartPointer<vtkRenderWindow>::New();
    gpuGateWindow->SetOffScreenRendering(1);
    gpuGateWindow->AddRenderer(gpuGateRenderer);
    gpuGateStrategy.AttachRenderer(gpuGateRenderer);
    gpuGateRenderer->ResetCamera();
    gpuGateWindow->Render();
    gpuGateWindow->WaitForCompletion();
    const auto gpuGateLowPartitions =
        gpuGateStrategy.GetGpuPartitions();
    vtkSmartPointer<vtkAlgorithmOutput> gpuGateLowInput =
        gpuGateMapper
            ? gpuGateMapper->GetInputConnection(0, 0) : nullptr;
    const std::uint64_t releaseCountBefore =
        gpuGateStrategy.GetGpuReleaseCount();
    const std::uint64_t preloadCountBefore =
        gpuGateStrategy.GetGpuPreloadCount();
    const std::uint64_t queryCountBefore =
        gpuGateStrategy.GetGpuQueryCount();
    const vtkIdType configuredGpuBytes = gpuGateMapper
        ? gpuGateMapper->GetMaxMemoryInBytes() : 0;
    if (gpuGateMapper) {
        // 运行期有释放后驱动余量时，不得再次被 VTK 的固定 128 MiB
        // fallback（这里压成 1 byte）覆盖。
        gpuGateMapper->SetMaxMemoryInBytes(1);
    }
    (void)gpuGateStrategy.SetProbeBytes(100ULL * 1024ULL);
    gpuGateParams.volumeQuality = VolumeQuality::High;
    const bool isGpuGateQualitySet = gpuGateStrategy.SetVisualState(
        gpuGateParams, UpdateFlags::Quality);
    if (isGpuGateQualitySet) {
        gpuGateWindow->Render();
        gpuGateWindow->WaitForCompletion();
    }
    const auto gpuGateHighPartitions =
        gpuGateStrategy.GetGpuPartitions();
    const bool isGpuGateSwitched = gpuGateMapper
        && gpuGateMapper->GetInputConnection(0, 0)
            != gpuGateLowInput.GetPointer()
        && getDimensions(gpuGateMapper)
            == std::array<int, 3>{ 64, 64, 64 };
    gpuGateParams.volumeQuality = VolumeQuality::XHigh;
    (void)gpuGateStrategy.SetProbeBytes(200ULL * 1024ULL);
    const bool isGpuGateXHighSet = gpuGateStrategy.SetVisualState(
        gpuGateParams, UpdateFlags::Quality);
    if (isGpuGateXHighSet) {
        gpuGateWindow->Render();
        gpuGateWindow->WaitForCompletion();
    }
    const auto gpuGateXHighPartitions =
        gpuGateStrategy.GetGpuPartitions();
    const bool isGpuGateXHighExact = gpuGateMapper
        && getDimensions(gpuGateMapper)
            == std::array<int, 3>{ 96, 96, 96 };
    gpuGateParams.volumeQuality = VolumeQuality::Ultra;
    (void)gpuGateStrategy.SetProbeBytes(300ULL * 1024ULL);
    const bool isGpuGateUltraSet = gpuGateStrategy.SetVisualState(
        gpuGateParams, UpdateFlags::Quality);
    if (isGpuGateUltraSet) {
        gpuGateWindow->Render();
        gpuGateWindow->WaitForCompletion();
    }
    const auto gpuGateUltraPartitions =
        gpuGateStrategy.GetGpuPartitions();
    const bool isGpuGateUltraExact = gpuGateMapper
        && getDimensions(gpuGateMapper)
            == std::array<int, 3>{ 128, 128, 128 };
    auto* gpuGateUltraInput = gpuGateMapper
        ? gpuGateMapper->GetInput() : nullptr;
    (void)gpuGateStrategy.SetProbeBytes(0);
    gpuGateParams.volumeQuality = VolumeQuality::Low;
    const bool isTinyBlockRejected =
        !gpuGateStrategy.SetVisualState(
            gpuGateParams, UpdateFlags::Quality);
    const bool isUltraPreserved = gpuGateMapper
        && gpuGateMapper->GetInput() == gpuGateUltraInput
        && gpuGateStrategy.GetGpuPartitions()
            == gpuGateUltraPartitions;
    if (gpuGateMapper) {
        gpuGateMapper->SetMaxMemoryInBytes(configuredGpuBytes);
    }
    failureCount += GetCaseResult(
        isGpuGateConfigured
            && isGpuGateInputSet
            && gpuGateMapper
            && isGpuGateQualitySet
            && isGpuGateSwitched
            && gpuGateLowPartitions
                == std::array<unsigned short, 3>{ 1, 1, 1 }
            && gpuGateHighPartitions
                == std::array<unsigned short, 3>{ 2, 2, 2 }
            && gpuGateStrategy.GetLodDimensions(
                VolumeQuality::High)
                == std::array<int, 3>{ 64, 64, 64 }
            && isGpuGateXHighSet
            && isGpuGateXHighExact
            && gpuGateXHighPartitions
                == std::array<unsigned short, 3>{ 3, 3, 2 }
            && isGpuGateUltraSet
            && isGpuGateUltraExact
            && gpuGateUltraPartitions
                == std::array<unsigned short, 3>{ 3, 3, 3 }
            && gpuGateStrategy.GetGpuReleaseCount()
                == releaseCountBefore + 4
            && gpuGateStrategy.GetGpuPreloadCount()
                == preloadCountBefore
            && gpuGateStrategy.GetGpuQueryCount()
                == queryCountBefore + 4
            && gpuGateStrategy.GetQueryOrderValid()
            && isTinyBlockRejected
            && isUltraPreserved,
        "Runtime LOD sizes blocks from free VRAM after eviction") ? 0 : 1;

    VolumeStrategy loadQualityStrategy;
    RenderParams loadQualityParams = gpuGateParams;
    loadQualityParams.volumeQuality = VolumeQuality::High;
    const bool isLoadQualitySet = loadQualityStrategy.SetVisualState(
        loadQualityParams,
        UpdateFlags::Quality | UpdateFlags::Denoise);
    const std::uint64_t loadBuildCount =
        loadQualityStrategy.GetResampleBuildCount();
    const bool isLoadQualityInputSet = loadQualityStrategy.SetInputData(
        dimensionImage, nullptr);
    const std::uint64_t loadInputBuildCount =
        loadQualityStrategy.GetResampleBuildCount();
    const bool isLoadSnapshotSet = loadQualityStrategy.SetVisualState(
        loadQualityParams, UpdateFlags::All);
    failureCount += GetCaseResult(
        isLoadQualitySet
            && isLoadQualityInputSet
            && isLoadSnapshotSet
            && loadInputBuildCount == loadBuildCount + 1
            && loadQualityStrategy.GetResampleBuildCount()
                == loadInputBuildCount
            && loadQualityStrategy.GetResampleUpdateCount() == 1
            && loadQualityStrategy.GetLodDimensions(
                VolumeQuality::High)
                == std::array<int, 3>{ 600, 1, 1 },
        "Load candidates build the requested Volume quality only once") ? 0 : 1;

    auto lodCacheImage = vtkSmartPointer<vtkImageData>::New();
    lodCacheImage->SetDimensions(64, 64, 64);
    lodCacheImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(
            lodCacheImage->GetScalarPointer()),
        lodCacheImage->GetNumberOfPoints(),
        static_cast<unsigned char>(128));
    VolumeStrategy lodCacheStrategy;
    RenderParams lodCacheParams;
    lodCacheParams.volumeQuality = VolumeQuality::High;
    const bool isHighConfigured = lodCacheStrategy.SetVisualState(
        lodCacheParams, UpdateFlags::Quality);
    const bool isLodCacheInputSet = lodCacheStrategy.SetInputData(
        lodCacheImage, nullptr);
    auto* lodCacheVolume = vtkVolume::SafeDownCast(
        lodCacheStrategy.GetMainProp());
    auto* lodCacheMapper = lodCacheVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            lodCacheVolume->GetMapper())
        : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> firstHighInput =
        lodCacheMapper
            ? lodCacheMapper->GetInputConnection(0, 0) : nullptr;
    vtkSmartPointer<vtkImageData> firstHighProduct = lodCacheMapper
        ? vtkImageData::SafeDownCast(lodCacheMapper->GetInput()) : nullptr;
    const std::uint64_t firstHighBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    const std::uint64_t firstHighPlanCount =
        lodCacheStrategy.GetLodPlanCount();
    lodCacheParams.volumeQuality = VolumeQuality::Low;
    const bool isFirstLowSet = lodCacheStrategy.SetVisualState(
        lodCacheParams, UpdateFlags::Quality);
    const std::uint64_t firstLowBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    vtkSmartPointer<vtkAlgorithmOutput> firstLowInput =
        lodCacheMapper
            ? lodCacheMapper->GetInputConnection(0, 0) : nullptr;
    lodCacheParams.volumeQuality = VolumeQuality::High;
    const bool isHighReused = lodCacheStrategy.SetVisualState(
        lodCacheParams, UpdateFlags::Quality);
    const std::uint64_t reusedHighBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    const bool isHighInputReused = lodCacheMapper
        && vtkImageData::SafeDownCast(lodCacheMapper->GetInput())
            == firstHighProduct.GetPointer();

    lodCacheImage->Modified();
    const bool isLodCacheInputReset = lodCacheStrategy.SetInputData(
        lodCacheImage, nullptr);
    const std::uint64_t resetHighBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    lodCacheParams.volumeQuality = VolumeQuality::Low;
    const bool isLowRebuilt = lodCacheStrategy.SetVisualState(
        lodCacheParams, UpdateFlags::Quality);
    const std::uint64_t resetLowBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    lodCacheParams.isDenoiseOn = true;
    const bool isDenoiseSet = lodCacheStrategy.SetVisualState(
        lodCacheParams, UpdateFlags::Denoise);
    const std::uint64_t denoiseBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    lodCacheParams.isDenoiseOn = false;
    const bool isDenoiseReset = lodCacheStrategy.SetVisualState(
        lodCacheParams, UpdateFlags::Denoise);
    const std::uint64_t resetDenoiseBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    auto lodCacheMask = vtkSmartPointer<vtkImageData>::New();
    lodCacheMask->CopyStructure(lodCacheImage);
    lodCacheMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(
            lodCacheMask->GetScalarPointer()),
        lodCacheMask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    const bool isLodCacheMaskSet = lodCacheStrategy.SetInputData(
        lodCacheImage, lodCacheMask);
    const std::uint64_t maskBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    const std::uint64_t maskUpdateCount =
        lodCacheStrategy.GetResampleUpdateCount();
    const bool isLodCacheMaskReused = lodCacheStrategy.SetInputData(
        lodCacheImage, lodCacheMask);
    const std::uint64_t reusedMaskBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    const std::uint64_t reusedMaskUpdateCount =
        lodCacheStrategy.GetResampleUpdateCount();
    auto* lodMaskScalars = static_cast<unsigned char*>(
        lodCacheMask->GetScalarPointer());
    if (lodMaskScalars) lodMaskScalars[0] = 0;
    lodCacheMask->Modified();
    const bool isLodCacheMaskReset = lodCacheStrategy.SetInputData(
        lodCacheImage, lodCacheMask);
    const std::uint64_t resetMaskBuildCount =
        lodCacheStrategy.GetResampleBuildCount();
    const std::uint64_t resetMaskUpdateCount =
        lodCacheStrategy.GetResampleUpdateCount();
    failureCount += GetCaseResult(
        isHighConfigured
            && isLodCacheInputSet
            && lodCacheMapper
            && firstHighInput
            && firstHighBuildCount == 1
            && firstHighPlanCount == 1
            && isFirstLowSet
            && firstLowInput
            && firstLowInput != firstHighInput
            && firstLowBuildCount == firstHighBuildCount + 1
            && isHighReused
            && isHighInputReused
            && reusedHighBuildCount == firstLowBuildCount
            && isLodCacheInputReset
            && resetHighBuildCount == reusedHighBuildCount + 1
            && isLowRebuilt
            && resetLowBuildCount == resetHighBuildCount + 1
            && isDenoiseSet
            && denoiseBuildCount == resetLowBuildCount + 1
            && isDenoiseReset
            && resetDenoiseBuildCount == denoiseBuildCount
            && isLodCacheMaskSet
            && maskBuildCount == resetDenoiseBuildCount + 2
            && maskUpdateCount == maskBuildCount
            && isLodCacheMaskReused
            && reusedMaskBuildCount == maskBuildCount
            && reusedMaskUpdateCount == maskUpdateCount
            && isLodCacheMaskReset
            && resetMaskBuildCount == maskBuildCount + 2
            && resetMaskUpdateCount == maskUpdateCount + 2
            && lodCacheStrategy.GetLodPlanCount()
                == firstHighPlanCount + 5,
        "LOD cache builds on first use and invalidates by data mask and denoise")
        ? 0 : 1;

    VolumeStrategy boundedCacheStrategy;
    RenderParams boundedCacheParams;
    boundedCacheParams.volumeQuality = VolumeQuality::High;
    const bool isBoundedHighSet = boundedCacheStrategy.SetVisualState(
        boundedCacheParams, UpdateFlags::Quality);
    const bool isBoundedInputSet = boundedCacheStrategy.SetInputData(
        lodCacheImage, nullptr);
    auto* boundedVolume = vtkVolume::SafeDownCast(
        boundedCacheStrategy.GetMainProp());
    auto* boundedMapper = boundedVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            boundedVolume->GetMapper())
        : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> boundedHighInput =
        boundedMapper
            ? boundedMapper->GetInputConnection(0, 0) : nullptr;
    vtkSmartPointer<vtkImageData> boundedHighProduct = boundedMapper
        ? vtkImageData::SafeDownCast(boundedMapper->GetInput()) : nullptr;
    boundedCacheParams.volumeQuality = VolumeQuality::Low;
    const bool isBoundedLowSet = boundedCacheStrategy.SetVisualState(
        boundedCacheParams, UpdateFlags::Quality);
    boundedCacheParams.volumeQuality = VolumeQuality::High;
    const bool isBoundedHighReused = boundedCacheStrategy.SetVisualState(
        boundedCacheParams, UpdateFlags::Quality);
    boundedCacheParams.volumeQuality = VolumeQuality::XHigh;
    const bool isBoundedXHighSet = boundedCacheStrategy.SetVisualState(
        boundedCacheParams, UpdateFlags::Quality);
    boundedCacheParams.volumeQuality = VolumeQuality::Ultra;
    const bool isBoundedUltraSet = boundedCacheStrategy.SetVisualState(
        boundedCacheParams, UpdateFlags::Quality);
    const std::uint64_t fullCacheBuildCount =
        boundedCacheStrategy.GetResampleBuildCount();
    boundedCacheParams.volumeQuality = VolumeQuality::Low;
    const bool isEvictedLowRebuilt = boundedCacheStrategy.SetVisualState(
        boundedCacheParams, UpdateFlags::Quality);
    const std::uint64_t evictedLowBuildCount =
        boundedCacheStrategy.GetResampleBuildCount();
    boundedCacheParams.volumeQuality = VolumeQuality::High;
    const bool isProtectedHighReused =
        boundedCacheStrategy.SetVisualState(
            boundedCacheParams, UpdateFlags::Quality);
    failureCount += GetCaseResult(
        isBoundedHighSet
            && isBoundedInputSet
            && boundedMapper
            && boundedHighInput
            && isBoundedLowSet
            && isBoundedHighReused
            && isBoundedXHighSet
            && isBoundedUltraSet
            && fullCacheBuildCount == 3
            && isEvictedLowRebuilt
            && evictedLowBuildCount == fullCacheBuildCount + 1
            && isProtectedHighReused
            && boundedCacheStrategy.GetResampleBuildCount()
                == evictedLowBuildCount + 1
            && vtkImageData::SafeDownCast(boundedMapper->GetInput())
                != boundedHighProduct.GetPointer(),
        "Bounded typed LOD cache evicts the oldest unowned product")
        ? 0 : 1;

    auto cacheMask = vtkSmartPointer<vtkImageData>::New();
    cacheMask->CopyStructure(dimensionImage);
    cacheMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* cacheMaskScalars = static_cast<unsigned char*>(
        cacheMask->GetScalarPointer());
    for (vtkIdType index = 0;
        index < dimensionImage->GetNumberOfPoints(); ++index) {
        cacheMaskScalars[index] = index % 2 == 0 ? 0 : 255;
    }
    VolumeStrategy cacheStrategy;
    const bool isCacheInputSet = cacheStrategy.SetInputData(
        dimensionImage, cacheMask);
    auto* cacheVolume = vtkVolume::SafeDownCast(
        cacheStrategy.GetMainProp());
    auto* cacheMapper = cacheVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            cacheVolume->GetMapper())
        : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> qualityInput =
        cacheMapper ? cacheMapper->GetInputConnection(0, 0) : nullptr;
    vtkSmartPointer<vtkImageData> qualityMask =
        cacheMapper ? cacheMapper->GetMaskInput() : nullptr;
    const double qualitySampleDistance =
        cacheMapper ? cacheMapper->GetSampleDistance() : 0.0;
    const std::uint64_t qualityInputCount =
        cacheStrategy.GetMapperInputCount();
    const std::uint64_t qualityBuildCount =
        cacheStrategy.GetResampleBuildCount();
    const std::uint64_t qualityUpdateCount =
        cacheStrategy.GetResampleUpdateCount();
    const std::uint64_t qualityPlanCount =
        cacheStrategy.GetLodPlanCount();
    const bool isNativeAuto =
        cacheStrategy.GetLodDimensions(VolumeQuality::Auto)
            == std::array<int, 3>{ 1200, 1, 1 };
    auto* qualityVolume = cacheMapper
        ? vtkImageData::SafeDownCast(cacheMapper->GetInput())
        : nullptr;
    bool isMaskBinary = qualityMask != nullptr;
    auto* qualityMaskScalars = qualityMask
        ? static_cast<unsigned char*>(qualityMask->GetScalarPointer())
        : nullptr;
    for (vtkIdType index = 0;
        isMaskBinary && index < qualityMask->GetNumberOfPoints(); ++index) {
        isMaskBinary = qualityMaskScalars
            && (qualityMaskScalars[index] == 0
                || qualityMaskScalars[index] == 255);
    }
    const bool isQualityStable =
        isCacheInputSet
        && cacheMapper
        && qualityInput
        && qualityMask
        && qualityVolume
        && isNativeAuto
        && qualityVolume == dimensionImage.GetPointer()
        && qualityMask == cacheMask.GetPointer()
        && qualityInputCount == 1
        && qualityPlanCount == 1
        && qualityBuildCount == 0
        && qualityUpdateCount == 0
        && std::equal(
            qualityVolume->GetExtent(),
            qualityVolume->GetExtent() + 6,
            qualityMask->GetExtent())
        && isMaskBinary
        && cacheMapper->GetAutoAdjustSampleDistances() != 0
        && std::abs(
            cacheMapper->GetImageSampleDistance() - 1.0) < 1e-12
        && std::abs(
            cacheMapper->GetMinimumImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(
            cacheMapper->GetMaximumImageSampleDistance() - 2.5)
            < 1e-12
        && cacheMapper->GetUseJittering() != 0;
    RenderParams cacheParams;
    cacheParams.isFeatureActive = true;
    cacheStrategy.SetVisualState(
        cacheParams, UpdateFlags::Quality);
    const bool isFeatureCacheReused =
        cacheMapper
        && cacheMapper->GetInputConnection(0, 0)
            == qualityInput.GetPointer()
        && cacheMapper->GetMaskInput()
            == qualityMask.GetPointer()
        && getMaxDimension(cacheMapper) == 1200
        && cacheMapper->GetAutoAdjustSampleDistances() != 0
        && std::abs(
            cacheMapper->GetImageSampleDistance() - 1.0) < 1e-12
        && std::abs(
            cacheMapper->GetSampleDistance()
                - qualitySampleDistance) < 1e-12
        && cacheMapper->GetUseJittering() != 0
        && cacheStrategy.GetMapperInputCount()
            == qualityInputCount
        && cacheStrategy.GetResampleBuildCount()
            == qualityBuildCount
        && cacheStrategy.GetResampleUpdateCount()
            == qualityUpdateCount
        && cacheStrategy.GetLodPlanCount()
            == qualityPlanCount + 1;
    failureCount += GetCaseResult(
        isQualityStable && isFeatureCacheReused,
        "Explicit Auto re-resolves without rebuilding unchanged products") ? 0 : 1;

    VolumeStrategy retryStrategy;
    retryStrategy.SetInputData(dimensionImage);
    auto* retryVolume = vtkVolume::SafeDownCast(
        retryStrategy.GetMainProp());
    auto* retryMapper = retryVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            retryVolume->GetMapper())
        : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> oldRetryInput =
        retryMapper ? retryMapper->GetInputConnection(0, 0) : nullptr;
    auto retryImage = vtkSmartPointer<vtkImageData>::New();
    retryStrategy.SetInputData(retryImage);
    const bool isFailedInputPreserved =
        retryMapper
        && retryMapper->GetInputConnection(0, 0)
            == oldRetryInput.GetPointer();
    retryImage->SetDimensions(8, 1, 1);
    retryImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    retryStrategy.SetInputData(retryImage);
    const int retryDimension = getMaxDimension(retryMapper);
    failureCount += GetCaseResult(
        isFailedInputPreserved
            && retryMapper->GetInputConnection(0, 0)
                != oldRetryInput.GetPointer()
            && retryDimension == 8,
        "Failed producer input preserves the old cache and allows retry") ? 0 : 1;

    auto keyImage = vtkSmartPointer<vtkImageData>::New();
    keyImage->SetDimensions(32, 4, 2);
    keyImage->SetSpacing(1.0, 1.0, 1.0);
    keyImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    VolumeStrategy keyStrategy;
    keyStrategy.SetInputData(keyImage);
    auto* keyVolume = vtkVolume::SafeDownCast(
        keyStrategy.GetMainProp());
    auto* keyMapper = keyVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            keyVolume->GetMapper())
        : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> firstKeyInput =
        keyMapper ? keyMapper->GetInputConnection(0, 0) : nullptr;
    keyImage->SetSpacing(2.0, 3.0, 4.0);
    keyImage->Modified();
    keyStrategy.SetInputData(keyImage);
    vtkSmartPointer<vtkAlgorithmOutput> spacingKeyInput =
        keyMapper ? keyMapper->GetInputConnection(0, 0) : nullptr;
    if (keyMapper) {
        keyMapper->Update();
    }
    auto* spacingKeyOutput = keyMapper
        ? vtkImageData::SafeDownCast(keyMapper->GetInput())
        : nullptr;
    const bool isSpacingKeyUpdated =
        firstKeyInput
        && spacingKeyInput
        && spacingKeyOutput
        && std::abs(spacingKeyOutput->GetSpacing()[0] - 2.0)
            < 1e-12
        && std::abs(spacingKeyOutput->GetSpacing()[1] - 3.0)
            < 1e-12
        && std::abs(spacingKeyOutput->GetSpacing()[2] - 4.0)
            < 1e-12;

    vtkSmartPointer<vtkAlgorithmOutput> dataKeyInput =
        keyMapper ? keyMapper->GetInputConnection(0, 0) : nullptr;
    auto* keyScalars = static_cast<unsigned char*>(
        keyImage->GetScalarPointer());
    if (keyScalars) {
        keyScalars[0] = 17;
    }
    keyImage->Modified();
    keyStrategy.SetInputData(keyImage);
    if (keyMapper) keyMapper->Update();
    auto* dataKeyOutput = keyMapper
        ? vtkImageData::SafeDownCast(keyMapper->GetInput())
        : nullptr;
    const bool isDataKeyUpdated =
        keyMapper
        && dataKeyInput
        && dataKeyOutput
        && *static_cast<unsigned char*>(
            dataKeyOutput->GetScalarPointer()) == 17;

    vtkSmartPointer<vtkAlgorithmOutput> extentKeyInput =
        keyMapper ? keyMapper->GetInputConnection(0, 0) : nullptr;
    keyImage->SetDimensions(16, 4, 2);
    keyImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    keyImage->Modified();
    keyStrategy.SetInputData(keyImage);
    const bool isExtentKeyUpdated =
        keyMapper
        && extentKeyInput
        && getMaxDimension(keyMapper) == 16;

    auto keyMask = vtkSmartPointer<vtkImageData>::New();
    keyMask->CopyStructure(keyImage);
    keyMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(keyMask->GetScalarPointer()),
        keyMask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    keyStrategy.SetInputMask(keyMask);
    vtkSmartPointer<vtkImageData> firstKeyMask =
        keyMapper ? keyMapper->GetMaskInput() : nullptr;
    auto* keyMaskScalars = static_cast<unsigned char*>(
        keyMask->GetScalarPointer());
    if (keyMaskScalars) keyMaskScalars[0] = 0;
    keyMask->Modified();
    keyStrategy.SetInputMask(keyMask);
    vtkSmartPointer<vtkImageData> nextKeyMask =
        keyMapper ? keyMapper->GetMaskInput() : nullptr;
    keyImage->Modified();
    keyStrategy.SetInputMask(nullptr);
    auto* failedClearMask =
        keyMapper ? keyMapper->GetMaskInput() : nullptr;
    failureCount += GetCaseResult(
        isSpacingKeyUpdated
            && isDataKeyUpdated
            && isExtentKeyUpdated
            && firstKeyMask
            && nextKeyMask
            && nextKeyMask == firstKeyMask.GetPointer()
            && *static_cast<unsigned char*>(
                nextKeyMask->GetScalarPointer()) == 0
            && failedClearMask == nullptr
            && keyStrategy.GetLodPlanCount() == 7,
        "Same-pointer mutations rebuild plans while native aliases stay stable") ? 0 : 1;

    auto image = BuildFloatImage(
        { 0.0f, 0.0f, 1.0f, 1.0f });
    image->SetDimensions(4, 1, 1);

    VolumeStrategy volumeStrategy;
    volumeStrategy.SetInputData(image);
    auto* volume = vtkVolume::SafeDownCast(
        volumeStrategy.GetMainProp());
    failureCount += GetCaseResult(
        volume && volume->GetProperty()
            && volume->GetProperty()->GetShade() == 0,
        "Volume constructor follows default ShadeOff") ? 0 : 1;

    constexpr std::array<double, 3> isoColor = {
        0.75, 0.75, 0.75
    };
    const auto getColorMatches = [&isoColor](const double* color) {
        return color
            && std::equal(
                isoColor.begin(),
                isoColor.end(),
                color,
                [](double expected, double actual) {
                    return std::abs(expected - actual) < 1e-12;
                });
    };
    VolumeTransferFunction isoFunction;
    isoFunction.colorNodes = {
        { 10.0, 0.75, 0.75, 0.75 },
        { 20.0, 0.75, 0.75, 0.75 },
        { 27.0, 0.75, 0.75, 0.75 },
        { 30.0, 0.75, 0.75, 0.75 }
    };
    isoFunction.opacityNodes = {
        { 10.0, 0.0 },
        { 20.0, 0.0 },
        { 27.0, 0.8 },
        { 30.0, 1.0 }
    };

    auto* colorMapper = volume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            volume->GetMapper())
        : nullptr;
    const vtkMTimeType blendMTime =
        colorMapper ? colorMapper->GetMTime() : 0;
    const bool isCompositeBlend = colorMapper
        && colorMapper->GetBlendMode()
            == vtkVolumeMapper::COMPOSITE_BLEND;
    if (colorMapper) {
        colorMapper->SetBlendModeToComposite();
    }
    failureCount += GetCaseResult(
        isCompositeBlend
            && colorMapper
            && colorMapper->GetBlendMode()
                == vtkVolumeMapper::COMPOSITE_BLEND
            && colorMapper->GetMTime() == blendMTime,
        "Volume blend mode is explicit Composite and repeated setting is idempotent") ? 0 : 1;
    vtkAlgorithmOutput* colorInput = colorMapper
        ? colorMapper->GetInputConnection(0, 0) : nullptr;
    const double colorSampleDistance = colorMapper
        ? colorMapper->GetSampleDistance() : 0.0;
    const double colorImageDistance = colorMapper
        ? colorMapper->GetImageSampleDistance() : 0.0;
    const int colorAutoAdjust = colorMapper
        ? colorMapper->GetAutoAdjustSampleDistances() : -1;
    const int colorJitter = colorMapper
        ? colorMapper->GetUseJittering() : -1;
    const std::uint64_t tfMapperInputCount =
        volumeStrategy.GetMapperInputCount();
    const std::uint64_t tfResampleBuildCount =
        volumeStrategy.GetResampleBuildCount();
    const std::uint64_t tfResampleUpdateCount =
        volumeStrategy.GetResampleUpdateCount();
    RenderParams colorParams;
    colorParams.volumeTransferFunction = isoFunction;
    colorParams.scalarRange[0] = 10.0;
    colorParams.scalarRange[1] = 30.0;
    colorParams.material.opacity = 0.4;
    volumeStrategy.SetVisualState(
        colorParams, UpdateFlags::VolumeTransfer);
    auto* colorFunction = volume && volume->GetProperty()
        ? volume->GetProperty()->GetRGBTransferFunction()
        : nullptr;
    bool hasVolumeRgb =
        colorFunction
        && colorFunction->GetSize()
            == static_cast<int>(isoFunction.colorNodes.size());
    auto* opacityFunction = volume && volume->GetProperty()
        ? volume->GetProperty()->GetScalarOpacity()
        : nullptr;
    hasVolumeRgb = hasVolumeRgb
        && opacityFunction
        && opacityFunction->GetSize()
            == static_cast<int>(isoFunction.opacityNodes.size());
    for (int index = 0;
        hasVolumeRgb && index < colorFunction->GetSize();
        ++index) {
        double colorNode[6] = {};
        double opacityNode[4] = {};
        colorFunction->GetNodeValue(index, colorNode);
        opacityFunction->GetNodeValue(index, opacityNode);
        const auto& sourceColor =
            isoFunction.colorNodes[static_cast<std::size_t>(index)];
        const auto& sourceOpacity =
            isoFunction.opacityNodes[static_cast<std::size_t>(index)];
        hasVolumeRgb =
            getColorMatches(colorNode + 1)
            && std::abs(
                colorNode[0] - sourceColor.scalar) < 1e-12
            && std::abs(
                opacityNode[0] - sourceOpacity.scalar) < 1e-12
            && std::abs(
                opacityNode[1]
                    - sourceOpacity.opacity
                        * colorParams.material.opacity) < 1e-12;
    }
    failureCount += GetCaseResult(
        hasVolumeRgb
            && colorMapper
            && colorInput
            && colorMapper->GetInputConnection(0, 0)
                == colorInput,
        "Volume TF maps real scalar, RGB, and opacity without rebuilding input") ? 0 : 1;

    const std::vector<VolumeTransferFunction::ColorNode> customNodes{
        { 10.0, 1.0, 0.0, 0.0 },
        { 30.0, 0.0, 0.0, 1.0 }
    };
    colorParams.volumeTransferFunction.colorNodes = customNodes;
    auto* oldOpacityFunction = opacityFunction;
    const bool isCompleteTransferSet =
        volumeStrategy.SetVisualState(
            colorParams, UpdateFlags::VolumeTransfer);
    colorFunction = volume && volume->GetProperty()
        ? volume->GetProperty()->GetRGBTransferFunction()
        : nullptr;
    double firstCustom[6] = {};
    double lastCustom[6] = {};
    if (colorFunction && colorFunction->GetSize() == 2) {
        colorFunction->GetNodeValue(0, firstCustom);
        colorFunction->GetNodeValue(1, lastCustom);
    }
    failureCount += GetCaseResult(
        colorFunction
            && colorFunction->GetSize() == 2
            && colorMapper
            && std::abs(firstCustom[1] - 1.0) < 1e-12
            && std::abs(firstCustom[2]) < 1e-12
            && std::abs(firstCustom[3]) < 1e-12
            && std::abs(lastCustom[1]) < 1e-12
            && std::abs(lastCustom[2]) < 1e-12
            && std::abs(lastCustom[3] - 1.0) < 1e-12
            && std::abs(
                colorMapper->GetSampleDistance()
                    - colorSampleDistance) < 1e-12
            && std::abs(
                colorMapper->GetImageSampleDistance()
                    - colorImageDistance) < 1e-12
            && colorMapper->GetAutoAdjustSampleDistances()
                == colorAutoAdjust
            && colorMapper->GetUseJittering() == colorJitter
            && colorMapper->GetInputConnection(0, 0)
                == colorInput
            && volume->GetProperty()->GetScalarOpacity()
                != oldOpacityFunction
            && volumeStrategy.GetMapperInputCount()
                == tfMapperInputCount
            && volumeStrategy.GetResampleBuildCount()
                == tfResampleBuildCount
            && volumeStrategy.GetResampleUpdateCount()
                == tfResampleUpdateCount
            && isCompleteTransferSet,
        "A complete TF snapshot replaces both curves without changing LOD input") ? 0 : 1;

    auto invalidTransferParams = colorParams;
    auto* oldColorFunction = colorFunction;
    oldOpacityFunction = volume->GetProperty()->GetScalarOpacity();
    invalidTransferParams.volumeTransferFunction
        .opacityNodes[1].scalar =
            invalidTransferParams.volumeTransferFunction
                .opacityNodes[0].scalar;
    const bool isInvalidTransferRejected =
        !volumeStrategy.SetVisualState(
            invalidTransferParams,
            UpdateFlags::VolumeTransfer);
    failureCount += GetCaseResult(
        isInvalidTransferRejected
            && volume->GetProperty()->GetRGBTransferFunction()
                == oldColorFunction
            && volume->GetProperty()->GetScalarOpacity()
                == oldOpacityFunction
            && colorMapper->GetInputConnection(0, 0) == colorInput
            && volumeStrategy.GetMapperInputCount()
                == tfMapperInputCount
            && volumeStrategy.GetResampleBuildCount()
                == tfResampleBuildCount
            && volumeStrategy.GetResampleUpdateCount()
                == tfResampleUpdateCount,
        "An invalid opacity curve cannot partially commit a new color curve") ? 0 : 1;

    auto dragParams = colorParams;
    dragParams.isInteracting = true;
    volumeStrategy.SetVisualState(
        dragParams, UpdateFlags::RenderRate);
    const std::uint64_t dragMapperInputCount =
        volumeStrategy.GetMapperInputCount();
    const std::uint64_t dragResampleBuildCount =
        volumeStrategy.GetResampleBuildCount();
    const std::uint64_t dragResampleUpdateCount =
        volumeStrategy.GetResampleUpdateCount();
    for (int revision = 1; revision <= 1000; ++revision) {
        dragParams.volumeTransferFunction.colorNodes.back().b =
            static_cast<double>(revision) / 1000.0;
        volumeStrategy.SetVisualState(
            dragParams, UpdateFlags::VolumeTransfer);
    }
    const bool isDragPipelineStable =
        volumeStrategy.GetMapperInputCount()
            == dragMapperInputCount
        && volumeStrategy.GetResampleBuildCount()
            == dragResampleBuildCount
        && volumeStrategy.GetResampleUpdateCount()
            == dragResampleUpdateCount
        && colorMapper->GetInputConnection(0, 0) == colorInput;
    dragParams.isInteracting = false;
    volumeStrategy.SetVisualState(
        dragParams, UpdateFlags::RenderRate);
    failureCount += GetCaseResult(
        isDragPipelineStable
            && volumeStrategy.GetMapperInputCount()
                == dragMapperInputCount
            && volumeStrategy.GetResampleBuildCount()
                == dragResampleBuildCount
            && volumeStrategy.GetResampleUpdateCount()
                == dragResampleUpdateCount,
        "One thousand TF drag snapshots never rebind or rebuild the 3D LOD") ? 0 : 1;

    RenderParams absoluteParams = colorParams;
    absoluteParams.material.opacity = 0.5;
    absoluteParams.volumeTransferFunction.colorNodes = {
        { 12.0, 1.0, 0.0, 0.0 },
        { 20.0, 0.0, 1.0, 0.0 },
        { 28.0, 0.0, 0.0, 1.0 }
    };
    absoluteParams.volumeTransferFunction.opacityNodes = {
        { 10.0, 0.0 },
        { 18.0, 0.3 },
        { 25.0, 0.8 },
        { 30.0, 1.0 }
    };
    const bool isAbsoluteSet = volumeStrategy.SetVisualState(
        absoluteParams,
        UpdateFlags::VolumeTransfer);
    auto* absoluteColor =
        volume->GetProperty()->GetRGBTransferFunction();
    auto* absoluteOpacity =
        volume->GetProperty()->GetScalarOpacity();
    failureCount += GetCaseResult(
        isAbsoluteSet && absoluteColor && absoluteOpacity
            && absoluteColor->GetSize() == 3
            && absoluteOpacity->GetSize() == 4
            && std::abs(absoluteColor->GetRange()[0] - 12.0)
                < 1e-12
            && std::abs(absoluteColor->GetRange()[1] - 28.0)
                < 1e-12
            && std::abs(absoluteOpacity->GetRange()[0] - 10.0)
                < 1e-12
            && std::abs(absoluteOpacity->GetRange()[1] - 30.0)
                < 1e-12
            && std::abs(
                volume->GetProperty()
                    ->GetScalarOpacityUnitDistance() - 1.0)
                < 1e-12
            && volume->GetVisibility() != 0
            && colorMapper->GetInputConnection(0, 0) == colorInput,
        "Real scalar nodes keep one stable linear TF semantic") ? 0 : 1;

    constexpr std::array scalarQualities{
        VolumeQuality::Low,
        VolumeQuality::High,
        VolumeQuality::Ultra
    };
    bool isAbsoluteLodStable = true;
    for (std::size_t index = 0;
        index < scalarQualities.size(); ++index) {
        auto scalarParams = absoluteParams;
        scalarParams.volumeQuality = scalarQualities[index];
        dimensionStrategy.SetVisualState(
            scalarParams,
            UpdateFlags::Quality
                | UpdateFlags::VolumeTransfer);
        auto* scalarProperty = dimensionVolume
            ? dimensionVolume->GetProperty() : nullptr;
        auto* scalarColor = scalarProperty
            ? scalarProperty->GetRGBTransferFunction() : nullptr;
        auto* scalarOpacity = scalarProperty
            ? scalarProperty->GetScalarOpacity() : nullptr;
        isAbsoluteLodStable = isAbsoluteLodStable
            && getDimensions(dimensionMapper)
                == dimensionStrategy.GetLodDimensions(
                    scalarQualities[index])
            && scalarColor && scalarOpacity
            && std::abs(scalarColor->GetRange()[0] - 12.0)
                < 1e-12
            && std::abs(scalarColor->GetRange()[1] - 28.0)
                < 1e-12
            && std::abs(scalarOpacity->GetRange()[0] - 10.0)
                < 1e-12
            && std::abs(scalarOpacity->GetRange()[1] - 30.0)
                < 1e-12;
    }
    failureCount += GetCaseResult(
        isAbsoluteLodStable,
        "Absolute scalar nodes stay fixed across quality tiers") ? 0 : 1;

    IsoSurfaceStrategy isoStrategy;
    isoStrategy.SetInputData(image);
    auto* actor = vtkActor::SafeDownCast(
        isoStrategy.GetMainProp());
    failureCount += GetCaseResult(
        actor && actor->GetProperty()
            && actor->GetProperty()->GetInterpolation()
                == VTK_FLAT,
        "Iso constructor follows default Flat interpolation") ? 0 : 1;

    auto isoImage = vtkSmartPointer<vtkImageData>::New();
    isoImage->SetDimensions(1200, 40, 40);
    isoImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(isoImage->GetScalarPointer()),
        isoImage->GetNumberOfPoints(),
        static_cast<unsigned char>(128));
    auto isoMask = vtkSmartPointer<vtkImageData>::New();
    isoMask->SetDimensions(1200, 40, 40);
    isoMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(isoMask->GetScalarPointer()),
        isoMask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    IsoSurfaceStrategy isoQualityStrategy;
    RenderParams isoQualityParams;
    isoQualityParams.volumeQuality = VolumeQuality::Low;
    const bool isIsoLowSet = isoQualityStrategy.SetVisualState(
        isoQualityParams, UpdateFlags::Quality);
    const bool isIsoInputSet = isoQualityStrategy.SetInputData(
        isoImage, isoMask);
    auto* isoQualityActor = vtkActor::SafeDownCast(
        isoQualityStrategy.GetMainProp());
    auto* isoMapper = isoQualityActor
        ? vtkPolyDataMapper::SafeDownCast(
            isoQualityActor->GetMapper())
        : nullptr;
    vtkMapper* isoMapperIdentity = isoMapper;
    const std::array isoProfiles{
        std::pair{
            VolumeQuality::Low,
            std::array<int, 3>{ 300, 10, 10 } },
        std::pair{
            VolumeQuality::High,
            std::array<int, 3>{ 600, 20, 20 } },
        std::pair{
            VolumeQuality::XHigh,
            std::array<int, 3>{ 900, 30, 30 } },
        std::pair{
            VolumeQuality::Ultra,
            std::array<int, 3>{ 1200, 40, 40 } }
    };
    bool areIsoProfilesValid = isIsoLowSet && isIsoInputSet;
    for (const auto& [quality, expectedDimensions] : isoProfiles) {
        isoQualityParams.volumeQuality = quality;
        const bool isQualitySet = isoQualityStrategy.SetVisualState(
            isoQualityParams, UpdateFlags::Quality);
        areIsoProfilesValid = areIsoProfilesValid
            && isQualitySet
            && isoQualityStrategy.GetLodDimensions(quality)
                == expectedDimensions
            && isoQualityStrategy.GetInputDimensions()
                == expectedDimensions
            && isoQualityStrategy.GetMaskDimensions()
                == expectedDimensions
            && isoQualityActor->GetMapper() == isoMapperIdentity;
    }
    const auto autoDimensions =
        isoQualityStrategy.GetLodDimensions(VolumeQuality::Auto);
    const bool isAutoIsoBounded =
        autoDimensions[0] >= 300 && autoDimensions[0] <= 1200
        && autoDimensions[1] >= 10 && autoDimensions[1] <= 40
        && autoDimensions[2] >= 10 && autoDimensions[2] <= 40;
    vtkSmartPointer<vtkAlgorithmOutput> ultraIsoInput =
        isoMapper ? isoMapper->GetInputConnection(0, 0) : nullptr;
    isoQualityParams.volumeQuality =
        static_cast<VolumeQuality>(99);
    const bool isInvalidIsoRejected =
        !isoQualityStrategy.SetVisualState(
            isoQualityParams, UpdateFlags::Quality);
    auto misalignedMask = vtkSmartPointer<vtkImageData>::New();
    misalignedMask->SetDimensions(1200, 40, 40);
    misalignedMask->SetSpacing(2.0, 1.0, 1.0);
    misalignedMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    const bool isMisalignedMaskRejected =
        !isoQualityStrategy.SetInputData(
            isoImage, misalignedMask);
    const bool isMisalignedRollbackValid = isoMapper
        && isoMapper->GetInputConnection(0, 0)
            == ultraIsoInput.GetPointer();
    isoImage->GetPointData()->GetScalars()->Modified();
    const bool isSamePointerRebuilt =
        isoQualityStrategy.SetInputData(isoImage, isoMask);
    const bool hasSamePointerNewPipeline = isoMapper
        && isoMapper->GetInputConnection(0, 0)
            != ultraIsoInput.GetPointer();
    failureCount += GetCaseResult(
        areIsoProfilesValid
            && isAutoIsoBounded
            && isInvalidIsoRejected
            && isMisalignedMaskRejected
            && isMisalignedRollbackValid
            && isSamePointerRebuilt
            && hasSamePointerNewPipeline
            && isoQualityStrategy.GetQuality()
                == VolumeQuality::Ultra
            && isoQualityActor->GetMapper() == isoMapperIdentity,
        "Iso tiers remove 766 while input and mask failures stay transactional") ? 0 : 1;

    auto polyData = vtkSmartPointer<vtkPolyData>::New();
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->InsertNextPoint(0.0, 0.0, 0.0);
    points->InsertNextPoint(1.0, 0.0, 0.0);
    points->InsertNextPoint(0.0, 1.0, 0.0);
    auto triangle = vtkSmartPointer<vtkTriangle>::New();
    triangle->GetPointIds()->SetId(0, 0);
    triangle->GetPointIds()->SetId(1, 1);
    triangle->GetPointIds()->SetId(2, 2);
    auto cells = vtkSmartPointer<vtkCellArray>::New();
    cells->InsertNextCell(triangle);
    polyData->SetPoints(points);
    polyData->SetPolys(cells);
    IsoSurfaceStrategy polyStrategy;
    polyStrategy.SetInputData(polyData);
    auto* polyActor = vtkActor::SafeDownCast(
        polyStrategy.GetMainProp());
    failureCount += GetCaseResult(
        polyActor && polyActor->GetProperty()
            && getColorMatches(
                polyActor->GetProperty()->GetColor())
            && polyActor->GetProperty()->GetInterpolation()
                == VTK_FLAT,
        "Iso polydata input exposes the base RGB and Flat interpolation") ? 0 : 1;

    RenderParams params;
    params.volumeQuality = VolumeQuality::High;
    params.gradientOpacity = {
        { 0.0, 0.0 }, { 10.0, 0.5 }, { 20.0, 1.0 }
    };
    volumeStrategy.SetVisualState(
        params,
        UpdateFlags::Quality
            | UpdateFlags::GradientOpacity);
    auto* mapper = volume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            volume->GetMapper())
        : nullptr;
    auto* gradient = volume && volume->GetProperty()
        ? volume->GetProperty()->GetGradientOpacity()
        : nullptr;
    double gradientRange[2] = {};
    if (gradient) {
        gradient->GetRange(gradientRange);
    }
    vtkAlgorithmOutput* customInput =
        mapper ? mapper->GetInputConnection(0, 0) : nullptr;
    failureCount += GetCaseResult(
        mapper
            && mapper->GetAutoAdjustSampleDistances() == 0
            && std::abs(mapper->GetImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(
                mapper->GetMinimumImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(
                mapper->GetMaximumImageSampleDistance() - 2.5)
                < 1e-12
            && std::abs(mapper->GetSampleDistance() - 0.75)
                < 1e-12
            && mapper->GetUseJittering() != 0
            && volume->GetProperty()->HasGradientOpacity()
            && mapper->GetGradientOpacityRangeType()
                == vtkGPUVolumeRayCastMapper::SCALAR
            && gradient && gradient->GetSize() == 3
            && std::abs(gradientRange[0]) < 1e-12
            && std::abs(gradientRange[1] - 20.0) < 1e-12
            && std::abs(image->GetScalarRange()[0]) < 1e-12
            && std::abs(image->GetScalarRange()[1] - 1.0)
                < 1e-12
            && std::abs(image->GetSpacing()[0] - 1.0) < 1e-12,
        "High quality and SCALAR gradient opacity reach VTK properties") ? 0 : 1;

    params.isFeatureActive = true;
    volumeStrategy.SetVisualState(
        params, UpdateFlags::Quality);
    vtkAlgorithmOutput* featureQualityInput =
        mapper ? mapper->GetInputConnection(0, 0) : nullptr;
    const bool isFeatureQuality =
        mapper
        && featureQualityInput
        && featureQualityInput == customInput
        && mapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(mapper->GetImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(mapper->GetSampleDistance() - 0.75)
            < 1e-12
        && mapper->GetUseJittering() != 0;

    params.isFeatureActive = false;
    volumeStrategy.SetVisualState(
        params, UpdateFlags::Quality);
    const bool isCustomRestored =
        mapper
        && mapper->GetInputConnection(0, 0) == customInput
        && mapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(mapper->GetImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(mapper->GetSampleDistance() - 0.75)
            < 1e-12
        && mapper->GetUseJittering() != 0;
    failureCount += GetCaseResult(
        isFeatureQuality
            && isCustomRestored,
        "Feature state leaves the configured quality unchanged") ? 0 : 1;

    params.gradientOpacity.clear();
    volumeStrategy.SetVisualState(
        params, UpdateFlags::GradientOpacity);
    const bool hasEmptyGradient =
        volume->GetProperty()->HasGradientOpacity();
    gradient = volume->GetProperty()->GetGradientOpacity();
    failureCount += GetCaseResult(
        !hasEmptyGradient
            && gradient
            && gradient->GetSize() == 2,
        "Empty gradient disables the custom function before VTK creates its default") ? 0 : 1;

    auto previewImage = vtkSmartPointer<vtkImageData>::New();
    previewImage->SetDimensions(32, 32, 32);
    previewImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(previewImage->GetScalarPointer()),
        previewImage->GetNumberOfPoints(),
        static_cast<unsigned char>(128));
    auto previewMask = vtkSmartPointer<vtkImageData>::New();
    previewMask->CopyStructure(previewImage);
    previewMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(previewMask->GetScalarPointer()),
        previewMask->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    VolumeStrategy previewStrategy;
    previewStrategy.SetInputData(previewImage);
    previewStrategy.SetInputMask(previewMask);
    RenderParams previewParams;
    previewParams.volumeQuality = VolumeQuality::XHigh;
    previewParams.isDenoiseOn = true;
    previewParams.scalarRange[0] = 0.0;
    previewParams.scalarRange[1] = 255.0;
    previewParams.volumeTransferFunction.colorNodes = {
        { 0.0, 0.1, 0.2, 0.3 },
        { 255.0, 0.8, 0.7, 0.6 }
    };
    previewParams.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 255.0, 0.9 }
    };
    previewParams.gradientOpacity = {
        { 0.0, 0.0 }, { 64.0, 0.7 }, { 255.0, 1.0 }
    };
    previewParams.material = {
        0.08, 0.65, 0.65, 40.0, 0.85, true
    };
    previewStrategy.SetVisualState(
        previewParams,
        UpdateFlags::Quality
            | UpdateFlags::Denoise
            | UpdateFlags::VolumeTransfer
            | UpdateFlags::GradientOpacity
            | UpdateFlags::Material);
    auto* previewVolume = vtkVolume::SafeDownCast(
        previewStrategy.GetMainProp());
    auto* previewMapper = previewVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            previewVolume->GetMapper())
        : nullptr;
    auto previewRenderer = vtkSmartPointer<vtkRenderer>::New();
    auto previewWindow = vtkSmartPointer<vtkRenderWindow>::New();
    previewWindow->SetOffScreenRendering(1);
    previewWindow->AddRenderer(previewRenderer);
    previewStrategy.AttachRenderer(previewRenderer);
    if (previewVolume) {
        previewRenderer->AddVolume(previewVolume);
    }
    // 首次 Render 会让 VTK producer 完成初始化；预热后再保存 MTime，
    // 避免把管线初始化误判为交互预览重建。
    previewWindow->SetDesiredUpdateRate(GetRenderRate(false));
    previewWindow->Render();
    const auto setPreviewState = [
        &previewParams,
        &previewStrategy,
        &previewWindow](
            const bool isInteracting,
            const double desiredRate) {
        auto interactionParams = previewParams;
        interactionParams.isInteracting = isInteracting;
        const bool isSet = previewStrategy.SetVisualState(
            interactionParams, UpdateFlags::RenderRate);
        // 交互状态负责材质语义；mapper 在 GPURender owner thread 根据
        // 同一边界发布的 DesiredUpdateRate 选择 preview/still 采样。
        previewWindow->SetDesiredUpdateRate(desiredRate);
        previewWindow->Render();
        return isSet;
    };
    constexpr std::array previewProfiles{
        std::pair{
            VolumeQuality::Low,
            std::array<double, 2>{ 2.0, 2.0 } },
        std::pair{
            VolumeQuality::High,
            std::array<double, 2>{ 3.0, 3.0 } },
        std::pair{
            VolumeQuality::XHigh,
            std::array<double, 2>{ 3.0, 4.0 } },
        std::pair{
            VolumeQuality::Ultra,
            std::array<double, 2>{ 4.0, 4.0 } }
    };
    bool arePreviewProfilesValid = true;
    for (const auto& [quality, previewQuality] : previewProfiles) {
        previewParams.volumeQuality = quality;
        previewParams.isInteracting = false;
        const bool isQualitySet = previewStrategy.SetVisualState(
            previewParams,
            UpdateFlags::Quality | UpdateFlags::RenderRate);
        previewWindow->Render();
        auto* profileInput = previewMapper
            ? previewMapper->GetInputConnection(0, 0) : nullptr;
        const double profileStillRay = previewMapper
            ? previewMapper->GetSampleDistance() : 0.0;
        const bool isPreviewSet = setPreviewState(
            true, GetRenderRate(true));
        const bool isPreviewValid = previewMapper
            && std::abs(
                previewMapper->GetImageSampleDistance()
                    - previewQuality[0])
                < 1e-12
            && previewMapper->GetAutoAdjustSampleDistances() == 0
            && std::abs(
                previewMapper->GetMinimumImageSampleDistance()
                    - previewQuality[0])
                < 1e-12
            && std::abs(
                previewMapper->GetMaximumImageSampleDistance()
                    - previewQuality[0])
                < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance()
                    - previewQuality[1] * profileStillRay) < 1e-12
            && previewMapper->GetUseJittering() == 0
            && previewVolume
            && previewVolume->GetProperty()
            && previewVolume->GetProperty()->GetShade() == 0
            && previewMapper->GetInputConnection(0, 0) == profileInput;
        const bool isStillSet = setPreviewState(
            false, GetRenderRate(false));
        const bool isStillValid = previewMapper
            && std::abs(
                previewMapper->GetImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance() - profileStillRay)
                < 1e-12
            && previewMapper->GetUseJittering() != 0
            && previewVolume
            && previewVolume->GetProperty()
            && previewVolume->GetProperty()->GetShade() != 0
            && previewMapper->GetInputConnection(0, 0) == profileInput;
        arePreviewProfilesValid = arePreviewProfilesValid
            && isQualitySet && isPreviewSet && isPreviewValid
            && isStillSet && isStillValid;
    }
    previewParams.volumeQuality = VolumeQuality::XHigh;
    previewParams.isInteracting = false;
    previewStrategy.SetVisualState(
        previewParams,
        UpdateFlags::Quality | UpdateFlags::RenderRate);
    previewWindow->Render();
    auto gpuDragParams = previewParams;
    gpuDragParams.isInteracting = true;
    previewStrategy.SetVisualState(
        gpuDragParams, UpdateFlags::RenderRate);
    const std::uint64_t gpuDragInputCount =
        previewStrategy.GetMapperInputCount();
    const std::uint64_t gpuDragBuildCount =
        previewStrategy.GetResampleBuildCount();
    const std::uint64_t gpuDragUpdateCount =
        previewStrategy.GetResampleUpdateCount();
    for (int revision = 1; revision <= 1000; ++revision) {
        gpuDragParams.volumeTransferFunction.colorNodes.back().b =
            static_cast<double>(revision) / 1000.0;
        previewStrategy.SetVisualState(
            gpuDragParams, UpdateFlags::VolumeTransfer);
    }
    gpuDragParams.isInteracting = false;
    previewStrategy.SetVisualState(
        gpuDragParams, UpdateFlags::RenderRate);
    const bool isGpuTfStable =
        previewStrategy.GetMapperInputCount()
            == gpuDragInputCount
        && previewStrategy.GetResampleBuildCount()
            == gpuDragBuildCount
        && previewStrategy.GetResampleUpdateCount()
            == gpuDragUpdateCount;
    const int stillAuto = previewMapper
        ? previewMapper->GetAutoAdjustSampleDistances() : -1;
    const double stillImage = previewMapper
        ? previewMapper->GetImageSampleDistance() : 0.0;
    const double stillMinImage = previewMapper
        ? previewMapper->GetMinimumImageSampleDistance() : 0.0;
    const double stillMaxImage = previewMapper
        ? previewMapper->GetMaximumImageSampleDistance() : 0.0;
    const double stillRay = previewMapper
        ? previewMapper->GetSampleDistance() : 0.0;
    const int stillJitter = previewMapper
        ? previewMapper->GetUseJittering() : -1;
    auto* previewInput = previewMapper
        ? previewMapper->GetInputConnection(0, 0) : nullptr;
    auto* previewMaskInput = previewMapper
        ? previewMapper->GetMaskInput() : nullptr;
    const vtkMTimeType previewProducerTime =
        previewInput && previewInput->GetProducer()
            ? previewInput->GetProducer()->GetMTime() : 0;
    const vtkMTimeType previewMaskTime =
        previewMaskInput ? previewMaskInput->GetMTime() : 0;
    auto* previewProperty = previewVolume
        ? previewVolume->GetProperty() : nullptr;
    auto* previewColor = previewProperty
        ? previewProperty->GetRGBTransferFunction() : nullptr;
    auto* previewOpacity = previewProperty
        ? previewProperty->GetScalarOpacity() : nullptr;
    auto* previewGradient = previewProperty
        ? previewProperty->GetGradientOpacity() : nullptr;
    const vtkMTimeType previewColorTime =
        previewColor ? previewColor->GetMTime() : 0;
    const vtkMTimeType previewOpacityTime =
        previewOpacity ? previewOpacity->GetMTime() : 0;
    const vtkMTimeType previewGradientTime =
        previewGradient ? previewGradient->GetMTime() : 0;
    const vtkIdType previewMemoryBytes = previewMapper
        ? previewMapper->GetMaxMemoryInBytes() : 0;
    const double previewMemoryFraction = previewMapper
        ? previewMapper->GetMaxMemoryFraction() : 0.0;
    const bool isPreviewEntered = setPreviewState(
        true, GetRenderRate(true));
    const bool isInteractiveMapperQuality =
        isPreviewEntered
        && previewMapper
        && std::abs(
            previewMapper->GetImageSampleDistance() - 3.0)
            < 1e-12
        && previewMapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(
            previewMapper->GetMinimumImageSampleDistance()
                - 3.0) < 1e-12
        && std::abs(
            previewMapper->GetMaximumImageSampleDistance()
                - 3.0) < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - 4.0 * stillRay)
                < 1e-12
        && previewMapper->GetUseJittering() == 0
        && previewMapper->GetInputConnection(0, 0) == previewInput
        && previewMapper->GetMaskInput() == previewMaskInput
        && previewProperty
        && previewProperty->GetRGBTransferFunction()
            == previewColor
        && previewProperty->GetScalarOpacity()
            == previewOpacity
        && previewProperty->GetGradientOpacity()
            == previewGradient
        && previewProperty->GetShade() == 0
        && previewMapper->GetMaxMemoryInBytes()
            == previewMemoryBytes
        && std::abs(
            previewMapper->GetMaxMemoryFraction()
                - previewMemoryFraction) < 1e-12;
    const bool isPreviewExited = setPreviewState(
        false, GetRenderRate(false));
    const bool isStillMapperQuality =
        isPreviewExited
        && previewMapper
        && previewMapper->GetAutoAdjustSampleDistances() == stillAuto
        && std::abs(
            previewMapper->GetImageSampleDistance()
                - stillImage) < 1e-12
        && std::abs(
            previewMapper->GetMinimumImageSampleDistance()
                - stillMinImage) < 1e-12
        && std::abs(
            previewMapper->GetMaximumImageSampleDistance()
                - stillMaxImage) < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance()
                - stillRay) < 1e-12
        && previewMapper->GetUseJittering() == stillJitter
        && previewMapper->GetInputConnection(0, 0) == previewInput
        && previewMapper->GetMaskInput() == previewMaskInput
        && previewInput
        && previewInput->GetProducer()
        && previewInput->GetProducer()->GetMTime()
            == previewProducerTime
        && previewMaskInput
        && previewMaskInput->GetMTime() == previewMaskTime
        && previewColor
        && previewColor->GetMTime() == previewColorTime
        && previewOpacity
        && previewOpacity->GetMTime() == previewOpacityTime
        && previewGradient
        && previewGradient->GetMTime() == previewGradientTime
        && previewProperty
        && previewProperty->GetShade() != 0
        && previewMapper->GetMaxMemoryInBytes()
            == previewMemoryBytes
        && std::abs(
            previewMapper->GetMaxMemoryFraction()
                - previewMemoryFraction) < 1e-12;

    const bool isRollbackPreviewSet = setPreviewState(
        true, GetRenderRate(true));
    const auto* previewCustomInput = previewMapper
        ? previewMapper->GetInputConnection(0, 0) : nullptr;
    auto* rollbackColor = previewProperty
        ? previewProperty->GetRGBTransferFunction() : nullptr;
    auto* rollbackOpacity = previewProperty
        ? previewProperty->GetScalarOpacity() : nullptr;
    auto* rollbackGradient = previewProperty
        ? previewProperty->GetGradientOpacity() : nullptr;
    const vtkMTimeType rollbackPropertyTime =
        previewProperty ? previewProperty->GetMTime() : 0;
    RenderParams invalidPreviewParams = previewParams;
    invalidPreviewParams.volumeQuality =
        static_cast<VolumeQuality>(99);
    invalidPreviewParams.volumeTransferFunction.colorNodes = {
        { 0.0, 1.0, 0.0, 0.0 },
        { 255.0, 0.0, 1.0, 0.0 }
    };
    invalidPreviewParams.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 255.0, 1.0 }
    };
    invalidPreviewParams.gradientOpacity = {
        { 0.0, 1.0 }, { 255.0, 0.0 }
    };
    invalidPreviewParams.material = {
        0.4, 0.4, 0.4, 4.0, 0.4, false
    };
    previewStrategy.SetVisualState(
        invalidPreviewParams,
        UpdateFlags::Quality
            | UpdateFlags::VolumeTransfer
            | UpdateFlags::GradientOpacity
            | UpdateFlags::Material);
    const bool isInvalidPreviewRolledBack =
        isRollbackPreviewSet
        && previewMapper
        && previewMapper->GetInputConnection(0, 0)
            == previewCustomInput
        && std::abs(
            previewMapper->GetImageSampleDistance() - 3.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - 4.0 * stillRay)
            < 1e-12
        && previewMapper->GetUseJittering() == 0
        && previewProperty
        && previewProperty->GetRGBTransferFunction()
            == rollbackColor
        && previewProperty->GetScalarOpacity()
            == rollbackOpacity
        && previewProperty->GetGradientOpacity()
            == rollbackGradient
        && previewProperty->GetShade() == 0
        && previewProperty->GetMTime()
            == rollbackPropertyTime;
    if (!isInvalidPreviewRolledBack) {
        std::cerr
            << "DIAG_VOLUME_ROLLBACK:"
            << " input="
            << (previewMapper
                && previewMapper->GetInputConnection(0, 0)
                    == previewCustomInput)
            << " image="
            << (previewMapper
                ? previewMapper->GetImageSampleDistance()
                : -1.0)
            << " ray="
            << (previewMapper
                ? previewMapper->GetSampleDistance()
                : -1.0)
            << " jitter="
            << (previewMapper
                ? previewMapper->GetUseJittering() : -1)
            << " color="
            << (previewProperty
                && previewProperty->GetRGBTransferFunction()
                    == rollbackColor)
            << " opacity="
            << (previewProperty
                && previewProperty->GetScalarOpacity()
                    == rollbackOpacity)
            << " gradient="
            << (previewProperty
                && previewProperty->GetGradientOpacity()
                    == rollbackGradient)
            << " property_time="
            << (previewProperty
                && previewProperty->GetMTime()
                    == rollbackPropertyTime)
            << '\n';
    }

    previewParams.isFeatureActive = true;
    previewStrategy.SetVisualState(
        previewParams, UpdateFlags::Quality);
    const auto* featureInput = previewMapper
        ? previewMapper->GetInputConnection(0, 0) : nullptr;
    const bool isFeaturePreview =
        previewMapper
        && featureInput
        && featureInput == previewCustomInput
        && std::abs(
            previewMapper->GetImageSampleDistance() - 3.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - 4.0 * stillRay)
            < 1e-12;
    previewParams.isFeatureActive = false;
    previewStrategy.SetVisualState(
        previewParams, UpdateFlags::Quality);
    const bool isFeaturePreviewRestored =
        previewMapper
        && previewMapper->GetInputConnection(0, 0)
            == previewCustomInput
        && previewMapper->GetMaskInput() == previewMaskInput
        && std::abs(
            previewMapper->GetSampleDistance() - 4.0 * stillRay)
            < 1e-12
        && previewMapper->GetUseJittering() == 0
        && previewProperty
        && previewProperty->GetShade() == 0;

    previewParams.volumeTransferFunction.colorNodes = {
        { 0.0, 0.75, 0.75, 0.75 },
        { 255.0, 0.75, 0.75, 0.75 }
    };
    previewParams.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 255.0, 1.0 }
    };
    previewParams.gradientOpacity = {
        { 0.0, 0.0 }, { 255.0, 0.9 }
    };
    previewParams.material = {
        0.25, 0.65, 0.10, 8.0, 0.8, false
    };
    previewStrategy.SetVisualState(
        previewParams,
        UpdateFlags::VolumeTransfer
            | UpdateFlags::GradientOpacity
            | UpdateFlags::Material);
    auto* updatedColor = previewProperty
        ? previewProperty->GetRGBTransferFunction() : nullptr;
    auto* updatedOpacity = previewProperty
        ? previewProperty->GetScalarOpacity() : nullptr;
    auto* updatedGradient = previewProperty
        ? previewProperty->GetGradientOpacity() : nullptr;
    const bool isVisualStillSet = setPreviewState(
        false, GetRenderRate(false));
    const bool isVisualStateRestored =
        isVisualStillSet
        && previewMapper
        && std::abs(
            previewMapper->GetImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - stillRay)
            < 1e-12
        && previewMapper->GetUseJittering() == stillJitter
        && previewMapper->GetInputConnection(0, 0)
            == previewCustomInput
        && previewMapper->GetMaskInput() == previewMaskInput
        && previewProperty
        && previewProperty->GetRGBTransferFunction()
            == updatedColor
        && previewProperty->GetScalarOpacity()
            == updatedOpacity
        && previewProperty->GetGradientOpacity()
            == updatedGradient
        && previewProperty->GetShade() == 0
        && std::abs(previewProperty->GetAmbient() - 0.25)
            < 1e-12
        && std::abs(previewProperty->GetDiffuse() - 0.65)
            < 1e-12
        && std::abs(previewProperty->GetSpecular() - 0.10)
            < 1e-12
        && std::abs(
            previewProperty->GetSpecularPower() - 8.0)
            < 1e-12;

    previewParams.volumeQuality = VolumeQuality::Auto;
    previewStrategy.SetVisualState(
        previewParams, UpdateFlags::Quality);
    previewWindow->Render();
    vtkSmartPointer<vtkAlgorithmOutput> qualityPreviewInput;
    if (previewMapper) {
        qualityPreviewInput =
            previewMapper->GetInputConnection(0, 0);
    }
    const double qualityStillRay = previewMapper
        ? previewMapper->GetSampleDistance() : 0.0;
    const bool isQualityPreviewSet = setPreviewState(
        true, GetRenderRate(true));
    const bool isQualityPreview =
        isQualityPreviewSet
        && previewMapper
        && qualityPreviewInput.GetPointer()
        && getMaxDimension(previewMapper) == 32
        && previewMapper->GetInputConnection(0, 0)
            == qualityPreviewInput.GetPointer()
        && std::abs(
            previewMapper->GetImageSampleDistance() - 4.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance()
                - qualityStillRay * 4.0)
            < 1e-12
        && previewMapper->GetUseJittering() == 0;
    const bool isQualityStillSet = setPreviewState(
        false, GetRenderRate(false));
    const bool isQualityRestored =
        isQualityStillSet
        && previewMapper
        && previewMapper->GetInputConnection(0, 0)
            == qualityPreviewInput.GetPointer()
        && getMaxDimension(previewMapper) == 32
        && std::abs(
            previewMapper->GetImageSampleDistance() - 1.0)
            < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance() - qualityStillRay)
            < 1e-12;
    if (!isQualityPreview || !isQualityRestored) {
        std::cerr
            << "DIAG_LAZY_QUALITY: max="
            << getMaxDimension(previewMapper)
            << " input="
            << (previewMapper
                && previewMapper->GetInputConnection(0, 0)
                    == qualityPreviewInput.GetPointer())
            << " image="
            << (previewMapper
                ? previewMapper->GetImageSampleDistance() : -1.0)
            << " ray="
            << (previewMapper
                ? previewMapper->GetSampleDistance() : -1.0)
            << " still_ray=" << qualityStillRay
            << '\n';
    }

    previewParams.volumeQuality = VolumeQuality::XHigh;
    const std::array<MaterialParams, 3> previewMaterials{{
        { 0.25, 0.65, 0.10, 8.0, 0.80, false },
        { 0.10, 0.85, 0.25, 20.0, 1.0, false },
        { 0.08, 0.65, 0.65, 40.0, 1.0, true }
    }};
    bool areMaterialsStable = true;
    for (const auto& material : previewMaterials) {
        previewParams.material = material;
        previewStrategy.SetVisualState(
            previewParams,
            UpdateFlags::Quality | UpdateFlags::Material);
        const double materialRay = previewMapper
            ? previewMapper->GetSampleDistance() : 0.0;
        const bool isMaterialPreviewSet = setPreviewState(
            true, GetRenderRate(true));
        const bool isPreviewStable =
            isMaterialPreviewSet
            && previewMapper
            && previewProperty
            && std::abs(
                previewMapper->GetImageSampleDistance() - 3.0)
                < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance() - 4.0 * materialRay)
                < 1e-12
            && previewMapper->GetUseJittering() == 0
            && previewProperty->GetShade() == 0
            && std::abs(
                previewProperty->GetAmbient() - material.ambient)
                < 1e-12
            && std::abs(
                previewProperty->GetDiffuse() - material.diffuse)
                < 1e-12
            && std::abs(
                previewProperty->GetSpecular() - material.specular)
                < 1e-12
            && std::abs(
                previewProperty->GetSpecularPower()
                    - material.specularPower)
                < 1e-12;
        const bool isMaterialStillSet = setPreviewState(
            false, GetRenderRate(false));
        const bool isStillStable =
            isMaterialStillSet
            && previewMapper
            && std::abs(
                previewMapper->GetImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance() - materialRay)
                < 1e-12
            && previewMapper->GetUseJittering() != 0
            && previewProperty->GetShade()
                == static_cast<int>(material.isShadeOn);
        areMaterialsStable =
            isPreviewStable && isStillStable
            && areMaterialsStable;
    }

    CompositeStrategy compositeStrategy(
        std::make_shared<VolumeStrategy>());
    compositeStrategy.SetInputData(previewImage);
    compositeStrategy.SetInputMask(previewMask);
    compositeStrategy.SetVisualState(
        previewParams, UpdateFlags::All);
    auto compositeRenderer =
        vtkSmartPointer<vtkRenderer>::New();
    auto compositeWindow =
        vtkSmartPointer<vtkRenderWindow>::New();
    compositeWindow->SetOffScreenRendering(1);
    compositeWindow->AddRenderer(compositeRenderer);
    compositeStrategy.AttachRenderer(compositeRenderer);
    compositeWindow->SetDesiredUpdateRate(
        GetRenderRate(false));
    compositeWindow->Render();
    auto* compositeVolume = vtkVolume::SafeDownCast(
        compositeStrategy.GetMainProp());
    auto* compositeMapper = compositeVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            compositeVolume->GetMapper())
        : nullptr;
    const double compositeStillRay = compositeMapper
        ? compositeMapper->GetSampleDistance() : 0.0;
    std::vector<vtkProp*> referenceProps;
    std::vector<vtkMTimeType> referenceTimes;
    std::vector<int> referenceVisible;
    auto* viewProps = compositeRenderer->GetViewProps();
    if (viewProps) {
        viewProps->InitTraversal();
        while (auto* prop = viewProps->GetNextProp()) {
            if (prop != compositeVolume) {
                referenceProps.push_back(prop);
                referenceTimes.push_back(prop->GetMTime());
                referenceVisible.push_back(
                    prop->GetVisibility());
            }
        }
    }
    auto compositeParams = previewParams;
    compositeParams.isInteracting = true;
    const bool isCompositePreviewSet = compositeStrategy.SetVisualState(
        compositeParams, UpdateFlags::RenderRate);
    compositeWindow->SetDesiredUpdateRate(
        GetRenderRate(true));
    compositeWindow->Render();
    bool areReferencePropsStable =
        !referenceProps.empty()
        && referenceProps.size()
            == referenceTimes.size();
    for (std::size_t index = 0;
        areReferencePropsStable
            && index < referenceProps.size();
        ++index) {
        areReferencePropsStable =
            referenceProps[index]
            && referenceProps[index]->GetMTime()
                == referenceTimes[index]
            && referenceProps[index]->GetVisibility()
                == referenceVisible[index];
    }
    const bool isCompositePreview =
        isCompositePreviewSet
        && compositeMapper
        && std::abs(
            compositeMapper->GetImageSampleDistance() - 3.0)
            < 1e-12
        && std::abs(
            compositeMapper->GetSampleDistance()
                - 4.0 * compositeStillRay) < 1e-12
        && compositeMapper->GetUseJittering() == 0
        && compositeVolume
        && compositeVolume->GetProperty()
        && compositeVolume->GetProperty()->GetShade() == 0
        && areReferencePropsStable;
    compositeParams.isInteracting = false;
    const bool isCompositeStillSet = compositeStrategy.SetVisualState(
        compositeParams, UpdateFlags::RenderRate);
    compositeWindow->SetDesiredUpdateRate(
        GetRenderRate(false));
    compositeWindow->Render();
    const bool isCompositeRestored =
        isCompositeStillSet
        && compositeMapper
        && std::abs(
            compositeMapper->GetImageSampleDistance()
                - 1.0) < 1e-12
        && std::abs(
            compositeMapper->GetSampleDistance()
                - compositeStillRay) < 1e-12
        && compositeMapper->GetUseJittering() != 0
        && compositeVolume
        && compositeVolume->GetProperty()
        && compositeVolume->GetProperty()->GetShade() != 0;
    compositeStrategy.DetachRenderer(
        compositeRenderer);
    if (!(arePreviewProfilesValid
        && isInteractiveMapperQuality
        && isStillMapperQuality
        && isInvalidPreviewRolledBack
        && isFeaturePreview
        && isFeaturePreviewRestored
        && isVisualStateRestored
        && isGpuTfStable
        && isQualityPreview
        && isQualityRestored
        && areMaterialsStable
        && isCompositePreview
        && isCompositeRestored)) {
        std::cerr
            << "DIAG_VOLUME_PREVIEW:"
            << " profiles=" << arePreviewProfilesValid
            << " interactive=" << isInteractiveMapperQuality
            << " still=" << isStillMapperQuality
            << " rollback=" << isInvalidPreviewRolledBack
            << " rollback_input="
            << (previewMapper
                && previewMapper->GetInputConnection(0, 0)
                    == previewCustomInput)
            << " rollback_image="
            << (previewMapper
                ? previewMapper->GetImageSampleDistance()
                : -1.0)
            << " rollback_ray="
            << (previewMapper
                ? previewMapper->GetSampleDistance()
                : -1.0)
            << " rollback_jitter="
            << (previewMapper
                ? previewMapper->GetUseJittering() : -1)
            << " rollback_color="
            << (previewProperty
                && previewProperty->GetRGBTransferFunction()
                    == previewColor)
            << " rollback_opacity="
            << (previewProperty
                && previewProperty->GetScalarOpacity()
                    == previewOpacity)
            << " rollback_gradient="
            << (previewProperty
                && previewProperty->GetGradientOpacity()
                    == previewGradient)
            << " rollback_shade="
            << (previewProperty
                ? previewProperty->GetShade() : -1)
            << " feature=" << isFeaturePreview
        << " feature_restore="
        << isFeaturePreviewRestored
        << " visual_restore="
        << isVisualStateRestored
        << " gpu_tf_stable=" << isGpuTfStable
        << " quality=" << isQualityPreview
        << " quality_restore=" << isQualityRestored
        << " materials=" << areMaterialsStable
        << " composite=" << isCompositePreview
            << " composite_restore="
            << isCompositeRestored
            << '\n';
    }
    failureCount += GetCaseResult(
        arePreviewProfilesValid
            && isInteractiveMapperQuality
            && isStillMapperQuality
            && isInvalidPreviewRolledBack
            && isFeaturePreview
            && isFeaturePreviewRestored
            && isVisualStateRestored
            && isGpuTfStable
            && isQualityPreview
            && isQualityRestored
            && areMaterialsStable
            && isCompositePreview
            && isCompositeRestored,
        "Volume mapper uses tiered preview sampling without changing active input") ? 0 : 1;

    params.gradientOpacity = {
        { 5.0, 0.2 }, { 5.0, 0.8 }, { 10.0, 1.0 }
    };
    volumeStrategy.SetVisualState(
        params, UpdateFlags::GradientOpacity);
    gradient = volume->GetProperty()->GetGradientOpacity();
    failureCount += GetCaseResult(
        volume->GetProperty()->HasGradientOpacity()
            && gradient
            && gradient->GetSize() == 2
            && std::abs(gradient->GetValue(5.0) - 0.8)
                < 1e-12,
        "Duplicate gradient positions deterministically keep the last opacity") ? 0 : 1;

    RenderParams zeroRangeParams = colorParams;
    zeroRangeParams.scalarRange[0] = 12.0;
    zeroRangeParams.scalarRange[1] = 12.0;
    zeroRangeParams.volumeTransferFunction.colorNodes = customNodes;
    zeroRangeParams.volumeTransferFunction.opacityNodes = {
        { 10.0, 0.0 },
        { 30.0, 1.0 }
    };
    volumeStrategy.SetVisualState(
        zeroRangeParams, UpdateFlags::VolumeTransfer);
    colorFunction = volume->GetProperty()->GetRGBTransferFunction();
    opacityFunction = volume->GetProperty()->GetScalarOpacity();
    failureCount += GetCaseResult(
        colorFunction
            && opacityFunction
            && colorFunction->GetSize() == 2
            && opacityFunction->GetSize() == 2
            && std::abs(colorFunction->GetRange()[0] - 10.0)
                < 1e-12
            && std::abs(colorFunction->GetRange()[1] - 30.0)
                < 1e-12,
        "Data-range changes never reinterpret real scalar TF nodes") ? 0 : 1;

    constexpr int denoiseDims[3] = { 64, 32, 16 };
    auto noisyImage = vtkSmartPointer<vtkImageData>::New();
    noisyImage->SetDimensions(
        denoiseDims[0], denoiseDims[1], denoiseDims[2]);
    noisyImage->SetSpacing(0.5, 0.75, 1.25);
    noisyImage->SetOrigin(2.0, 3.0, 4.0);
    noisyImage->AllocateScalars(VTK_FLOAT, 1);
    auto* noisyScalars =
        static_cast<float*>(noisyImage->GetScalarPointer());
    std::mt19937 random(20260721);
    std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
    const vtkIdType voxelCount =
        static_cast<vtkIdType>(denoiseDims[0])
        * denoiseDims[1] * denoiseDims[2];
    for (vtkIdType index = 0; index < voxelCount; ++index) {
        const int x = static_cast<int>(
            index % denoiseDims[0]);
        noisyScalars[index] =
            (x < denoiseDims[0] / 2 ? 0.25f : 0.75f)
            + noise(random);
    }
    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    maskImage->CopyStructure(noisyImage);
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(
            maskImage->GetScalarPointer()),
        voxelCount,
        static_cast<unsigned char>(255));

    double inputBounds[6] = { 0.0 };
    double inputRange[2] = { 0.0 };
    noisyImage->GetBounds(inputBounds);
    noisyImage->GetScalarRange(inputRange);
    const RoiStats inputStats = GetRoiStats(noisyImage);

    VolumeStrategy denoiseStrategy;
    denoiseStrategy.SetInputData(noisyImage);
    denoiseStrategy.SetInputMask(maskImage);
    auto* denoiseVolume = vtkVolume::SafeDownCast(
        denoiseStrategy.GetMainProp());
    auto* denoiseMapper = denoiseVolume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            denoiseVolume->GetMapper())
        : nullptr;
    vtkAbstractVolumeMapper* mapperIdentity =
        denoiseVolume ? denoiseVolume->GetMapper() : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> imageBeforeDenoise =
        denoiseMapper
        ? denoiseMapper->GetInputConnection(0, 0) : nullptr;
    vtkSmartPointer<vtkImageData> maskBeforeDenoise =
        denoiseMapper ? denoiseMapper->GetMaskInput() : nullptr;
    RenderParams denoiseParams;
    denoiseParams.isDenoiseOn = true;
    denoiseStrategy.SetVisualState(
        denoiseParams, UpdateFlags::Denoise);
    if (denoiseMapper) denoiseMapper->Update();
    vtkImageData* denoisedImage =
        denoiseMapper
        ? vtkImageData::SafeDownCast(denoiseMapper->GetInput())
        : nullptr;
    const RoiStats denoisedStats = GetRoiStats(denoisedImage);
    double denoisedBounds[6] = { 0.0 };
    if (denoisedImage) denoisedImage->GetBounds(denoisedBounds);
    int denoisedGeometry[3] = { 0, 0, 0 };
    if (denoisedImage) {
        denoisedImage->GetDimensions(denoisedGeometry);
    }
    const double inputContrast =
        inputStats.rightMean - inputStats.leftMean;
    const double denoisedContrast =
        denoisedStats.rightMean - denoisedStats.leftMean;
    const bool hasSameBounds = std::equal(
        std::begin(inputBounds),
        std::end(inputBounds),
        std::begin(denoisedBounds),
        [](double left, double right) {
            return std::abs(left - right) < 1e-9;
        });
    const bool isDenoiseValid = denoisedImage
            && denoisedGeometry[0] == denoiseDims[0]
            && denoisedGeometry[1] == denoiseDims[1]
            && denoisedGeometry[2] == denoiseDims[2]
            && hasSameBounds
            && denoiseVolume->GetMapper() == mapperIdentity
            && denoiseMapper->GetInputConnection(0, 0)
                != imageBeforeDenoise.GetPointer()
            && maskBeforeDenoise
            && denoiseMapper->GetMaskInput()
            && denoisedStats.leftVariance
                <= inputStats.leftVariance * 0.8
            && denoisedStats.rightVariance
                <= inputStats.rightVariance * 0.8
            && denoisedContrast >= inputContrast * 0.9
            && noisyImage->GetScalarRange()[0] == inputRange[0]
            && noisyImage->GetScalarRange()[1] == inputRange[1];
    if (!isDenoiseValid) {
        std::cerr
            << "DIAG_LAZY_DENOISE: image="
            << (denoisedImage != nullptr)
            << " dims=" << denoisedGeometry[0]
            << ',' << denoisedGeometry[1]
            << ',' << denoisedGeometry[2]
            << " bounds=" << hasSameBounds
            << " mapper="
            << (denoiseVolume
                && denoiseVolume->GetMapper() == mapperIdentity)
            << " input="
            << (denoiseMapper
                && denoiseMapper->GetInputConnection(0, 0)
                    != imageBeforeDenoise.GetPointer())
            << " mask="
            << (denoiseMapper && denoiseMapper->GetMaskInput())
            << " left_var=" << denoisedStats.leftVariance
            << " right_var=" << denoisedStats.rightVariance
            << " contrast=" << denoisedContrast
            << '\n';
    }
    failureCount += GetCaseResult(
        isDenoiseValid,
        "Denoise reduces ROI noise without changing source geometry or mapper identity") ? 0 : 1;
    return failureCount;
}

}

int GetViewFailCount()
{
    HostSessionConfig config;
    HostRenderViewConfig view;
    view.id = "view";
    view.role = HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode = HostRenderMode::Volume;
    view.window.viewInit.material = {
        0.2, 0.6, 0.3, 12.0, 0.8, true
    };
    view.window.viewInit.volumeTransferFunction.colorNodes = {
        { 0.0, 0.2, 0.3, 0.4 },
        { 1.0, 0.8, 0.7, 0.6 }
    };
    view.window.viewInit.volumeTransferFunction.opacityNodes = {
        { 0.0, 0.1 },
        { 1.0, 0.9 }
    };
    view.window.viewInit.hasVolumeTransferFunction = true;
    view.window.viewInit.isoThreshold = 0.25;
    view.window.viewInit.hasIso = true;
    view.window.viewInit.background = { 0.05, 0.1, 0.15 };
    view.window.viewInit.hasBackground = true;
    view.window.viewInit.windowLevel = { 80.0, 20.0 };
    view.window.viewInit.hasWindowLevel = true;
    HostRenderViewConfig linkedView = view;
    linkedView.id = "linked-view";
    linkedView.role = HostRenderViewRole::Composite3D;
    config.renderViews.push_back(std::move(view));
    config.renderViews.push_back(std::move(linkedView));
    VtkAppHostSession session(std::move(config));

    int failureCount = 0;
    HostViewTarget stateTarget;
    stateTarget.viewId = "view";
    const HostViewTarget linkedTarget{
        "linked-view", false, HostRenderViewRole::Composite3D };
    const bool isSceneSessionBuilt = session.BuildSession();
    const auto initialScene = session.GetSceneViewState(stateTarget);
    const auto linkedSceneByRole = session.GetSceneViewState(
        HostViewTarget{
            "", true, HostRenderViewRole::Composite3D });
    const auto initialScenes = session.GetSceneViewStates();
    const bool isInitialSceneValid = isSceneSessionBuilt
        && initialScene
        && initialScene->id == "view"
        && initialScene->role == HostRenderViewRole::Primary3D
        && initialScene->isAvailable
        && initialScene->presentation
        && initialScene->camera
        && linkedSceneByRole
        && linkedSceneByRole->id == "linked-view"
        && initialScenes.size() == 2
        && initialScenes[0].id == "view"
        && initialScenes[1].id == "linked-view";
    failureCount += GetCaseResult(
        isInitialSceneValid,
        "Scene snapshot keeps topology and value-only View state") ? 0 : 1;

    failureCount += GetCaseResult(
        !session.GetSceneViewState(HostViewTarget{
            "missing-view", true, HostRenderViewRole::Primary3D }),
        "Scene snapshot rejects missing ID without role fallback") ? 0 : 1;

    HostViewSetRequest value;
    value.targetView.viewId = "missing-view";
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(value)),
        "View missing target rejection") ? 0 : 1;

    HostSessionSetRequest sessionValue;
    sessionValue.spacing =
        std::array<double, 3>{ 1.0, 0.0, 1.0 };
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(sessionValue)),
        "Session invalid spacing atomic rejection") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.volumeTransferFunction = HostVolumeTransferFunction{};
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(value)),
        "View explicit empty transfer rejection") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.background = HostBackgroundColor{
        std::numeric_limits<double>::infinity(), 0.0, 0.0 };
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(value)),
        "View non-finite background rejection") ? 0 : 1;

    const auto firstScene = session.GetSceneViewState(stateTarget);
    const auto firstLinked = session.GetSceneViewState(linkedTarget);
    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.mode = HostRenderMode::IsoSurface;
    value.material = HostMaterialParams{
        0.3, 0.5, 0.4, 18.0, 0.6, false
    };
    HostVolumeTransferFunction viewFunction;
    viewFunction.colorNodes = {
        { 0.0, 0.8, 0.1, 0.2 },
        { 1.0, 0.2, 0.8, 0.4 }
    };
    viewFunction.opacityNodes = {
        { 0.0, 0.2 },
        { 1.0, 0.7 }
    };
    value.volumeTransferFunction = viewFunction;
    value.iso = 0.42;
    value.background = HostBackgroundColor{ 0.2, 0.3, 0.4 };
    value.windowLevel = HostWindowLevelParams{ 120.0, 60.0 };
    value.visibility = HostVisibilityParams{
        true, false, true
    };
    value.isAxesVisible = true;
    value.volumeQuality = HostVolumeQuality::XHigh;
    const bool isViewSet = session.SendRequest(std::move(value));
    failureCount += GetCaseResult(
        isViewSet,
        "Qt Host can set one View presentation state") ? 0 : 1;
    const auto nextScene = session.GetSceneViewState(stateTarget);
    const auto nextLinked = session.GetSceneViewState(linkedTarget);
    failureCount += GetCaseResult(
        isViewSet
            && firstScene && nextScene && firstLinked && nextLinked
            && nextScene->presentationRevision
                > firstScene->presentationRevision
            && nextLinked->presentationRevision
                == firstLinked->presentationRevision,
        "Scene presentation revision changes only for the target View") ? 0 : 1;

    sessionValue = HostSessionSetRequest{};
    sessionValue.spacing =
        std::array<double, 3>{ 0.5, 1.0, 1.5 };
    sessionValue.cursor = HostCursorParams{
        { 1.0, 2.0, 3.0 }, -1
    };
    failureCount += GetCaseResult(
        session.SendRequest(std::move(sessionValue)),
        "Qt Host can set Session spacing and cursor") ? 0 : 1;

    const auto state = session.GetRenderViewState(stateTarget);
    const auto allStates = session.GetRenderViewStates();
    const auto linkedState = std::find_if(
        allStates.begin(), allStates.end(),
        [](const HostRenderViewState& current) {
            return current.id == "linked-view";
        });
    const bool isStateReadBack = state.has_value()
        && state->id == "view"
        && state->role == HostRenderViewRole::Primary3D
        && state->viewMode == HostRenderMode::IsoSurface
        // 当前 fixture 没有数据；质量已接收但尚未提交，回读必须保持 applied。
        && state->volumeQuality == HostVolumeQuality::Auto
        && std::abs(state->material.opacity - 0.6) < 1e-12
        && !state->material.isShadeOn
        && state->volumeTransferFunction.colorNodes.size() == 2
        && state->volumeTransferFunction.opacityNodes.size() == 2
        && std::abs(
            state->volumeTransferFunction.opacityNodes[1].opacity - 0.7)
            < 1e-12
        && std::abs(state->isoThreshold - 0.42) < 1e-12
        && std::abs(state->background.r - 0.2) < 1e-12
        && std::abs(state->spacing[2] - 1.5) < 1e-12
        && std::abs(state->windowLevel.windowWidth - 120.0) < 1e-12
        && std::abs(state->scalarRange[0]) < 1e-12
        && std::abs(state->scalarRange[1] - 255.0) < 1e-12
        // 显式 Host cursor 请求是值状态，即使尚未加载体数据也应可完整回读。
        && state->cursorWorld == std::array<double, 3>{ 1.0, 2.0, 3.0 }
        && state->visibilityMask
            == (VisFlags::Planes3D | VisFlags::Ruler)
        && state->isAxesVisible
        && allStates.size() == 2
        && linkedState != allStates.end()
        && linkedState->viewMode == HostRenderMode::Volume
        && std::abs(linkedState->material.opacity - 0.8) < 1e-12
        && linkedState->material.isShadeOn
        && linkedState->volumeTransferFunction.colorNodes.size() == 2
        && linkedState->volumeTransferFunction.opacityNodes.size() == 2
        && std::abs(
            linkedState->volumeTransferFunction.opacityNodes[1].opacity
                - 0.9) < 1e-12
        && std::abs(linkedState->isoThreshold - 0.25) < 1e-12
        && std::abs(linkedState->background.r - 0.05) < 1e-12
        && std::abs(linkedState->windowLevel.windowWidth - 80.0) < 1e-12
        && std::abs(linkedState->spacing[2] - 1.5) < 1e-12
        && linkedState->cursorWorld
            == std::array<double, 3>{ 1.0, 2.0, 3.0 }
        && linkedState->visibilityMask == VisFlags::Crosshair
        && !linkedState->isAxesVisible;
    failureCount += GetCaseResult(
        isStateReadBack,
        "View presentation stays private while Session coordinates propagate") ? 0 : 1;

    HostVolumeTransferFunction independentFunction;
    independentFunction.colorNodes = {
        { 0.0, 0.0, 0.0, 0.0 },
        { 255.0, 1.0, 1.0, 1.0 }
    };
    independentFunction.opacityNodes = {
        { 0.0, 0.0 },
        { 128.0, 0.4 },
        { 255.0, 1.0 }
    };
    value = HostViewSetRequest{};
    value.targetView = stateTarget;
    value.volumeTransferFunction = independentFunction;
    const bool isIndependentSet =
        session.SendRequest(std::move(value));
    const auto independentState =
        session.GetRenderViewState(stateTarget);
    failureCount += GetCaseResult(
        isIndependentSet
            && independentState
            && independentState->volumeTransferFunction.colorNodes.size() == 2
            && independentState->volumeTransferFunction.opacityNodes.size() == 3
            && std::abs(
                independentState->volumeTransferFunction
                    .opacityNodes[1].scalar - 128.0)
                < 1e-12,
        "State keeps one complete scalar transfer function") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView = stateTarget;
    value.mode = HostRenderMode::CompositeIsoSurface;
    value.volumeQuality = HostVolumeQuality::Ultra;
    const bool isCompositeIsoQualitySet =
        session.SendRequest(std::move(value));
    const auto compositeIsoState =
        session.GetRenderViewState(stateTarget);
    value = HostViewSetRequest{};
    value.targetView = stateTarget;
    value.mode = HostRenderMode::SliceTopDown;
    value.volumeQuality = HostVolumeQuality::Low;
    const bool isSliceQualityRejected =
        !session.SendRequest(std::move(value));
    const auto preservedIsoState =
        session.GetRenderViewState(stateTarget);
    failureCount += GetCaseResult(
        isCompositeIsoQualitySet
            && compositeIsoState
            && compositeIsoState->viewMode
                == HostRenderMode::CompositeIsoSurface
            && compositeIsoState->volumeQuality
                == HostVolumeQuality::Auto
            && isSliceQualityRejected
            && preservedIsoState
            && preservedIsoState->viewMode
                == HostRenderMode::CompositeIsoSurface
            && preservedIsoState->volumeQuality
                == HostVolumeQuality::Auto,
        "Iso modes accept requested quality while applied state stays transactional") ? 0 : 1;

    HostViewResetRequest reset;
    reset.targetView.viewId = "view";
    failureCount += GetCaseResult(
        session.SendRequest(std::move(reset)),
        "Qt Host can reset the target camera") ? 0 : 1;

    failureCount += GetHistogramFailCount();
    failureCount += GetResampleFailCount();
    failureCount += GetLodControlFailCount();
    failureCount += GetIsoLodControlFailCount();
    failureCount += GetRenderContractFailCount();
    return failureCount;
}
