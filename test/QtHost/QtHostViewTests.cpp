#include "QtHostMethodCases.h"

#include "AppService.h"
#include "DataConverters.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"
#include "ImageProcessor.h"
#include "IsoSurfaceStrategy.h"
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
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkTable.h>
#include <vtkTriangle.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

namespace {

static_assert(static_cast<int>(VolumeQuality::Quality) == 0);
static_assert(static_cast<int>(VolumeQuality::Custom) == 1);
static_assert(
    VolumeQualityParams{}.quality == VolumeQuality::Quality);

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
            && !converter.GetHistogramPercentile(image, -0.1),
        "Histogram nearest-rank percentile boundaries") ? 0 : 1;

    auto constantImage = BuildFloatImage(
        { 7.0f, 7.0f, 7.0f, 7.0f });
    failureCount += GetCaseResult(
        converter.GetHistogramPercentile(constantImage, 0.37) == 7.0,
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
                == static_cast<vtkIdType>(largeCount),
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
    auto quality = ImageProcessor::GetDownsampledImage(image, 766);
    auto custom = ImageProcessor::GetDownsampledImage(image, 256);
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
            && qualityDims[0] == 766
            && qualityDims[1] == 383
            && qualityDims[2] == 191
            && customDims[0] == 256
            && customDims[1] == 128
            && customDims[2] == 64
            && quality->GetInterpolationMode()
                == VTK_RESLICE_LINEAR
            && qualityOrigin[0] == 4.0
            && std::abs(
                qualitySpacing[0]
                - 0.5 / (766.0 / 900.0)) < 1e-12
            && std::abs(
                qualityOrigin[0]
                + (qualityDims[0] - 1) * qualitySpacing[0]
                - (4.0 + 765.0
                    * (0.5 / (766.0 / 900.0)))) < 1e-9;
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
        "Resample Quality/Custom dimensions and geometry") ? 0 : 1;

    auto mask = ImageProcessor::GetDownsampledMask(image, 256);
    failureCount += GetCaseResult(
        mask && mask->GetInterpolationMode()
            == VTK_RESLICE_NEAREST,
        "Validity mask uses nearest interpolation") ? 0 : 1;
    failureCount += GetCaseResult(
        !ImageProcessor::GetDownsampledImage(image, 0)
            && !ImageProcessor::GetDownsampledImage(nullptr, 256),
        "Resample rejects null input and non-positive target") ? 0 : 1;

    auto smallImage = vtkSmartPointer<vtkImageData>::New();
    smallImage->SetDimensions(128, 64, 32);
    smallImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto smallResample =
        ImageProcessor::GetDownsampledImage(smallImage, 256);
    smallResample->UpdateInformation();
    int smallExtent[6] = { 0, -1, 0, -1, 0, -1 };
    smallResample->GetOutputInformation(0)->Get(
        vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
        smallExtent);
    failureCount += GetCaseResult(
        smallExtent[1] - smallExtent[0] + 1 == 128
            && smallExtent[3] - smallExtent[2] + 1 == 64
            && smallExtent[5] - smallExtent[4] + 1 == 32,
        "Resample does not enlarge input below target") ? 0 : 1;
    return failureCount;
}

int GetRenderContractFailCount()
{
    int failureCount = 0;
    const std::array configuredModes{
        VolumeQuality::Quality,
        VolumeQuality::Custom
    };
    bool hasEffectiveMatrix = true;
    for (const auto mode : configuredModes) {
        VolumeQualityParams configured;
        configured.quality = mode;
        hasEffectiveMatrix = hasEffectiveMatrix
            && GetVolumeQuality(configured, false) == mode
            && GetVolumeQuality(configured, true)
                == VolumeQuality::Quality
            && GetVolumeQuality(configured, false) == mode;
    }
    failureCount += GetCaseResult(
        hasEffectiveMatrix
            && GetRenderRate(false) == 0.001
            && GetRenderRate(true) == 15.0,
        "Quality and Custom are the only effective volume modes") ? 0 : 1;

    VizService featureService(nullptr, nullptr, nullptr);
    const FeatureSource firstSource{ "feature.first" };
    const FeatureSource secondSource{ "feature.second" };
    const VolumeQualityParams custom1000{
        VolumeQuality::Custom, 1000, 0.25, true
    };
    const bool isCustomAccepted =
        featureService.SetVolumeQuality(custom1000);
    const bool hasFeatureAggregate =
        isCustomAccepted
        && featureService.SetFeatureActive(firstSource, true)
        && featureService.SetFeatureActive(secondSource, true)
        && featureService.SetFeatureActive(firstSource, false)
        && featureService.GetIsFeatureActive()
        && featureService.SetFeatureActive(secondSource, false)
        && !featureService.GetIsFeatureActive()
        && featureService.GetVolumeQuality().quality
            == VolumeQuality::Custom
        && featureService.GetVolumeQuality().maxDimension == 1000;
    failureCount += GetCaseResult(
        hasFeatureAggregate,
        "Feature sources aggregate without overwriting Custom 1000") ? 0 : 1;

    auto dimensionImage = vtkSmartPointer<vtkImageData>::New();
    dimensionImage->SetDimensions(1200, 1, 1);
    dimensionImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    VolumeStrategy dimensionStrategy;
    dimensionStrategy.SetInputData(dimensionImage);
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
    RenderParams dimensionParams;
    dimensionParams.volumeQuality = custom1000;
    dimensionStrategy.SetVisualState(
        dimensionParams, UpdateFlags::Quality);
    const int customDimension =
        getMaxDimension(dimensionMapper);
    dimensionParams.isFeatureActive = true;
    dimensionStrategy.SetVisualState(
        dimensionParams, UpdateFlags::Quality);
    const int featureDimension =
        getMaxDimension(dimensionMapper);
    dimensionParams.isFeatureActive = false;
    dimensionStrategy.SetVisualState(
        dimensionParams, UpdateFlags::Quality);
    failureCount += GetCaseResult(
        customDimension == 1000
            && featureDimension == 766
            && getMaxDimension(dimensionMapper) == 1000,
        "Custom 1000 uses Quality 766 in Feature and restores 1000") ? 0 : 1;

    const auto getFeatureCycleRestored =
        [&dimensionStrategy, dimensionMapper, &getMaxDimension](
            const VolumeQualityParams& configured,
            const int restoredDimension,
            const double restoredSampleDistance,
            const bool isJitterExpected) {
        RenderParams cycleParams;
        cycleParams.volumeQuality = configured;
        dimensionStrategy.SetVisualState(
            cycleParams, UpdateFlags::Quality);
        cycleParams.isFeatureActive = true;
        dimensionStrategy.SetVisualState(
            cycleParams, UpdateFlags::Quality);
        const bool isFeatureQuality =
            getMaxDimension(dimensionMapper) == 766
            && dimensionMapper
            && dimensionMapper->GetAutoAdjustSampleDistances() == 0
            && std::abs(
                dimensionMapper->GetImageSampleDistance() - 1.0)
                < 1e-12
            && dimensionMapper->GetUseJittering() != 0;
        cycleParams.isFeatureActive = false;
        dimensionStrategy.SetVisualState(
            cycleParams, UpdateFlags::Quality);
        return isFeatureQuality
            && getMaxDimension(dimensionMapper)
                == restoredDimension
            && dimensionMapper->GetAutoAdjustSampleDistances() == 0
            && std::abs(
                dimensionMapper->GetImageSampleDistance() - 1.0)
                < 1e-12
            && (restoredSampleDistance <= 0.0
                || std::abs(
                    dimensionMapper->GetSampleDistance()
                        - restoredSampleDistance) < 1e-12)
            && (dimensionMapper->GetUseJittering() != 0)
                == isJitterExpected;
    };
    failureCount += GetCaseResult(
        getFeatureCycleRestored(
            { VolumeQuality::Quality, 766, 1.0, true },
            766, 0.0, true)
        && getFeatureCycleRestored(
            { VolumeQuality::Custom, 512, 0.25, false },
            512, 0.25, false),
        "Feature mapper restores Quality and Custom") ? 0 : 1;

    auto cacheMask = vtkSmartPointer<vtkImageData>::New();
    cacheMask->CopyStructure(dimensionImage);
    cacheMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::fill_n(
        static_cast<unsigned char*>(cacheMask->GetScalarPointer()),
        dimensionImage->GetNumberOfPoints(),
        static_cast<unsigned char>(255));
    VolumeStrategy cacheStrategy;
    cacheStrategy.SetInputData(dimensionImage);
    cacheStrategy.SetInputMask(cacheMask);
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
    const auto qualityMapperTime =
        cacheMapper ? cacheMapper->GetMTime() : 0;
    const bool isQualityStable =
        cacheMapper
        && qualityInput
        && qualityMask
        && cacheMapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(
            cacheMapper->GetImageSampleDistance() - 1.0) < 1e-12
        && std::abs(
            cacheMapper->GetMinimumImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(
            cacheMapper->GetMaximumImageSampleDistance() - 1.0)
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
        && getMaxDimension(cacheMapper) == 766
        && cacheMapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(
            cacheMapper->GetImageSampleDistance() - 1.0) < 1e-12
        && std::abs(
            cacheMapper->GetSampleDistance()
                - qualitySampleDistance) < 1e-12
        && cacheMapper->GetUseJittering() != 0
        && cacheMapper->GetMTime() == qualityMapperTime;
    failureCount += GetCaseResult(
        isQualityStable && isFeatureCacheReused,
        "Feature reuses the cached Quality producer and mapper") ? 0 : 1;

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
    failureCount += GetCaseResult(
        isFailedInputPreserved
            && retryMapper->GetInputConnection(0, 0)
                != oldRetryInput.GetPointer()
            && getMaxDimension(retryMapper) == 8,
        "Failed producer input preserves the old cache and allows retry") ? 0 : 1;

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
    const std::vector<TFNode> isoNodes{
        { 0.00, 0.0, 0.75, 0.75, 0.75 },
        { 0.50, 0.0, 0.75, 0.75, 0.75 },
        { 0.85, 0.8, 0.75, 0.75, 0.75 },
        { 1.00, 1.0, 0.75, 0.75, 0.75 }
    };

    auto* colorMapper = volume
        ? vtkGPUVolumeRayCastMapper::SafeDownCast(
            volume->GetMapper())
        : nullptr;
    vtkAlgorithmOutput* colorInput = colorMapper
        ? colorMapper->GetInputConnection(0, 0) : nullptr;
    RenderParams colorParams;
    colorParams.tfNodes = isoNodes;
    volumeStrategy.SetVisualState(
        colorParams, UpdateFlags::TF);
    auto* colorFunction = volume && volume->GetProperty()
        ? volume->GetProperty()->GetRGBTransferFunction()
        : nullptr;
    bool hasVolumeRgb =
        colorFunction
        && colorFunction->GetSize()
            == static_cast<int>(isoNodes.size());
    for (int index = 0;
        hasVolumeRgb && index < colorFunction->GetSize();
        ++index) {
        double node[6] = {};
        colorFunction->GetNodeValue(index, node);
        hasVolumeRgb = getColorMatches(node + 1);
    }
    failureCount += GetCaseResult(
        hasVolumeRgb
            && colorMapper
            && colorInput
            && colorMapper->GetInputConnection(0, 0)
                == colorInput,
        "Volume TF accepts the Iso base RGB without rebuilding input") ? 0 : 1;

    const std::vector<TFNode> customNodes{
        { 0.0, 0.0, 1.0, 0.0, 0.0 },
        { 1.0, 1.0, 0.0, 0.0, 1.0 }
    };
    colorParams.tfNodes = customNodes;
    volumeStrategy.SetVisualState(
        colorParams, UpdateFlags::TF);
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
            && colorMapper->GetInputConnection(0, 0)
                == colorInput,
        "Explicit Volume TF keeps custom RGB and input identity") ? 0 : 1;

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
    isoImage->SetDimensions(1200, 2, 2);
    isoImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    IsoSurfaceStrategy isoQualityStrategy;
    isoQualityStrategy.SetInputData(isoImage);
    auto* isoQualityActor = vtkActor::SafeDownCast(
        isoQualityStrategy.GetMainProp());
    auto* isoMapper = isoQualityActor
        ? vtkPolyDataMapper::SafeDownCast(
            isoQualityActor->GetMapper())
        : nullptr;
    vtkSmartPointer<vtkAlgorithmOutput> isoInput =
        isoMapper ? isoMapper->GetInputConnection(0, 0) : nullptr;
    auto* isoFilter = isoInput
        ? vtkFlyingEdges3D::SafeDownCast(
            isoInput->GetProducer())
        : nullptr;
    auto* isoResample = isoFilter
        && isoFilter->GetInputConnection(0, 0)
        ? vtkImageResample::SafeDownCast(
            isoFilter->GetInputConnection(0, 0)->GetProducer())
        : nullptr;
    if (isoResample) {
        isoResample->Update();
    }
    int isoDimensions[3] = {};
    if (isoResample) {
        isoResample->GetOutput()->GetDimensions(isoDimensions);
    }
    failureCount += GetCaseResult(
        isoResample
            && std::max({
                isoDimensions[0],
                isoDimensions[1],
                isoDimensions[2] }) == 766
            && isoFilter
            && isoMapper->GetInputConnection(0, 0)
                == isoInput.GetPointer(),
        "Iso uses one fixed Quality 766 producer") ? 0 : 1;

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
    params.volumeQuality = {
        VolumeQuality::Custom, 512, 0.25, true
    };
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
                mapper->GetMaximumImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(mapper->GetSampleDistance() - 0.25)
                < 1e-12
            && mapper->GetUseJittering() != 0
            && gradient && gradient->GetSize() == 3,
        "Custom quality and gradient opacity reach VTK properties") ? 0 : 1;

    params.isFeatureActive = true;
    volumeStrategy.SetVisualState(
        params, UpdateFlags::Quality);
    vtkAlgorithmOutput* featureQualityInput =
        mapper ? mapper->GetInputConnection(0, 0) : nullptr;
    const bool isFeatureQuality =
        mapper
        && featureQualityInput
        && featureQualityInput != customInput
        && mapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(mapper->GetImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(mapper->GetSampleDistance() - 0.5)
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
        && std::abs(mapper->GetSampleDistance() - 0.25)
            < 1e-12
        && mapper->GetUseJittering() != 0;
    failureCount += GetCaseResult(
        isFeatureQuality
            && isCustomRestored,
        "Feature uses Quality and restores Custom config") ? 0 : 1;

    params.gradientOpacity.clear();
    volumeStrategy.SetVisualState(
        params, UpdateFlags::GradientOpacity);
    gradient = volume->GetProperty()->GetGradientOpacity();
    failureCount += GetCaseResult(
        gradient && gradient->GetSize() == 2,
        "Empty gradient restores VTK default function") ? 0 : 1;

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
    failureCount += GetCaseResult(
        denoisedImage
            && denoisedGeometry[0] == denoiseDims[0]
            && denoisedGeometry[1] == denoiseDims[1]
            && denoisedGeometry[2] == denoiseDims[2]
            && hasSameBounds
            && denoiseVolume->GetMapper() == mapperIdentity
            && denoiseMapper->GetInputConnection(0, 0)
                != imageBeforeDenoise.GetPointer()
            && maskBeforeDenoise
            && denoiseMapper->GetMaskInput()
                == maskBeforeDenoise.GetPointer()
            && denoisedStats.leftVariance
                <= inputStats.leftVariance * 0.8
            && denoisedStats.rightVariance
                <= inputStats.rightVariance * 0.8
            && denoisedContrast >= inputContrast * 0.9
            && noisyImage->GetScalarRange()[0] == inputRange[0]
            && noisyImage->GetScalarRange()[1] == inputRange[1],
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
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession session(std::move(config));

    int failureCount = 0;
    HostViewSetRequest value;
    value.targetView.viewId = "missing-view";
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(value)),
        "View missing target rejection") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.mode = HostRenderMode::IsoSurface;
    value.opacity = 0.7;
    value.spacing = std::array<double, 3>{ 1.0, 0.0, 1.0 };
    failureCount += GetCaseResult(
        !session.SendRequest(std::move(value)),
        "View late invalid spacing atomic rejection") ? 0 : 1;

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.transferNodes = std::vector<HostTransferNode>{};
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

    value = HostViewSetRequest{};
    value.targetView.viewId = "view";
    value.iso = 0.42;
    value.cursor = HostCursorParams{ { 1.0, 2.0, 3.0 }, -1 };
    value.visibility = HostVisibilityParams{
        false, true, false
    };
    value.isAxesVisible = true;
    failureCount += GetCaseResult(
        session.SendRequest(std::move(value)),
        "Qt Host can set iso, cursor and runtime visibility") ? 0 : 1;

    HostViewResetRequest reset;
    reset.targetView.viewId = "view";
    failureCount += GetCaseResult(
        session.SendRequest(std::move(reset)),
        "Qt Host can reset the target camera") ? 0 : 1;

    return failureCount
        + GetHistogramFailCount()
        + GetResampleFailCount()
        + GetRenderContractFailCount();
}
