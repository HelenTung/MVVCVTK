#include "QtHostMethodCases.h"

#include "AppService.h"
#include "DataConverters.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"
#include "ImageProcessor.h"
#include "CompositeStrategy.h"
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
        && spacingKeyInput != firstKeyInput
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
    const bool isDataKeyUpdated =
        keyMapper
        && dataKeyInput
        && keyMapper->GetInputConnection(0, 0)
            != dataKeyInput.GetPointer();

    vtkSmartPointer<vtkAlgorithmOutput> extentKeyInput =
        keyMapper ? keyMapper->GetInputConnection(0, 0) : nullptr;
    keyImage->SetDimensions(16, 4, 2);
    keyImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    keyImage->Modified();
    keyStrategy.SetInputData(keyImage);
    const bool isExtentKeyUpdated =
        keyMapper
        && extentKeyInput
        && keyMapper->GetInputConnection(0, 0)
            != extentKeyInput.GetPointer()
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
    keyMask->SetSpacing(2.5, 3.0, 4.0);
    keyMask->Modified();
    keyStrategy.SetInputMask(keyMask);
    auto* spacingKeyMask =
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
            && spacingKeyMask
            && spacingKeyMask != firstKeyMask.GetPointer()
            && std::abs(spacingKeyMask->GetSpacing()[0] - 2.5)
                < 1e-12
            && failedClearMask == spacingKeyMask,
        "Same-pointer mutations invalidate caches and failed mask clear rolls back") ? 0 : 1;

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
    RenderParams colorParams;
    colorParams.tfNodes = isoNodes;
    colorParams.scalarRange[0] = 10.0;
    colorParams.scalarRange[1] = 30.0;
    colorParams.material.opacity = 0.4;
    volumeStrategy.SetVisualState(
        colorParams, UpdateFlags::TF);
    auto* colorFunction = volume && volume->GetProperty()
        ? volume->GetProperty()->GetRGBTransferFunction()
        : nullptr;
    bool hasVolumeRgb =
        colorFunction
        && colorFunction->GetSize()
            == static_cast<int>(isoNodes.size());
    auto* opacityFunction = volume && volume->GetProperty()
        ? volume->GetProperty()->GetScalarOpacity()
        : nullptr;
    hasVolumeRgb = hasVolumeRgb
        && opacityFunction
        && opacityFunction->GetSize()
            == static_cast<int>(isoNodes.size());
    for (int index = 0;
        hasVolumeRgb && index < colorFunction->GetSize();
        ++index) {
        double colorNode[6] = {};
        double opacityNode[4] = {};
        colorFunction->GetNodeValue(index, colorNode);
        opacityFunction->GetNodeValue(index, opacityNode);
        const auto& sourceNode =
            isoNodes[static_cast<std::size_t>(index)];
        const double scalarValue =
            colorParams.scalarRange[0]
            + sourceNode.position
                * (colorParams.scalarRange[1]
                    - colorParams.scalarRange[0]);
        hasVolumeRgb =
            getColorMatches(colorNode + 1)
            && std::abs(colorNode[0] - scalarValue) < 1e-12
            && std::abs(opacityNode[0] - scalarValue) < 1e-12
            && std::abs(
                opacityNode[1]
                    - sourceNode.opacity
                        * colorParams.material.opacity) < 1e-12;
    }
    failureCount += GetCaseResult(
        hasVolumeRgb
            && colorMapper
            && colorInput
            && colorMapper->GetInputConnection(0, 0)
                == colorInput,
        "Volume TF maps base RGB, scalar position, and opacity without rebuilding input") ? 0 : 1;

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
                == colorInput,
        "Explicit Volume TF keeps custom RGB, quality, and input identity") ? 0 : 1;

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
                mapper->GetMaximumImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(mapper->GetSampleDistance() - 0.25)
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
        "Custom quality and SCALAR gradient opacity reach VTK properties") ? 0 : 1;

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
    previewParams.volumeQuality = {
        VolumeQuality::Custom, 32, 0.25, false
    };
    previewParams.isDenoiseOn = true;
    previewParams.scalarRange[0] = 0.0;
    previewParams.scalarRange[1] = 255.0;
    previewParams.tfNodes = {
        { 0.0, 0.0, 0.1, 0.2, 0.3 },
        { 1.0, 0.9, 0.8, 0.7, 0.6 }
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
            | UpdateFlags::TF
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
    const vtkMTimeType previewPropertyTime =
        previewProperty ? previewProperty->GetMTime() : 0;
    const vtkIdType previewMemoryBytes = previewMapper
        ? previewMapper->GetMaxMemoryInBytes() : 0;
    const double previewMemoryFraction = previewMapper
        ? previewMapper->GetMaxMemoryFraction() : 0.0;
    previewWindow->SetDesiredUpdateRate(GetRenderRate(true));
    previewWindow->Render();
    const bool isInteractiveMapperQuality =
        previewMapper
        && std::abs(previewMapper->GetImageSampleDistance() - 2.0)
            < 1e-12
        && previewMapper->GetAutoAdjustSampleDistances() == 0
        && std::abs(
            previewMapper->GetMinimumImageSampleDistance()
                - stillMinImage) < 1e-12
        && std::abs(
            previewMapper->GetMaximumImageSampleDistance()
                - stillMaxImage) < 1e-12
        && previewMapper->GetSampleDistance()
            >= 2.0 * stillRay
        && previewMapper->GetUseJittering() == stillJitter
        && previewMapper->GetInputConnection(0, 0) == previewInput
        && previewMapper->GetMaskInput() == previewMaskInput
        && previewProperty
        && previewProperty->GetRGBTransferFunction()
            == previewColor
        && previewProperty->GetScalarOpacity()
            == previewOpacity
        && previewProperty->GetGradientOpacity()
            == previewGradient
        && previewProperty->GetMTime() == previewPropertyTime
        && previewMapper->GetMaxMemoryInBytes()
            == previewMemoryBytes
        && std::abs(
            previewMapper->GetMaxMemoryFraction()
                - previewMemoryFraction) < 1e-12;
    previewWindow->SetDesiredUpdateRate(GetRenderRate(false));
    previewWindow->Render();
    const bool isStillMapperQuality =
        previewMapper
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
        && previewProperty->GetMTime() == previewPropertyTime
        && previewMapper->GetMaxMemoryInBytes()
            == previewMemoryBytes
        && std::abs(
            previewMapper->GetMaxMemoryFraction()
                - previewMemoryFraction) < 1e-12;

    previewWindow->SetDesiredUpdateRate(GetRenderRate(true));
    previewWindow->Render();
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
    invalidPreviewParams.volumeQuality.maxDimension = 0;
    invalidPreviewParams.volumeQuality.sampleDistance = -1.0;
    invalidPreviewParams.tfNodes = {
        { 0.0, 0.0, 1.0, 0.0, 0.0 },
        { 1.0, 1.0, 0.0, 1.0, 0.0 }
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
            | UpdateFlags::TF
            | UpdateFlags::GradientOpacity
            | UpdateFlags::Material);
    const bool isInvalidPreviewRolledBack =
        previewMapper
        && previewMapper->GetInputConnection(0, 0)
            == previewCustomInput
        && std::abs(
            previewMapper->GetImageSampleDistance() - 2.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - 0.5)
            < 1e-12
        && previewMapper->GetUseJittering() == 0
        && previewProperty
        && previewProperty->GetRGBTransferFunction()
            == rollbackColor
        && previewProperty->GetScalarOpacity()
            == rollbackOpacity
        && previewProperty->GetGradientOpacity()
            == rollbackGradient
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
        && featureInput != previewCustomInput
        && std::abs(
            previewMapper->GetImageSampleDistance() - 2.0)
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
            previewMapper->GetSampleDistance() - 0.5)
            < 1e-12
        && previewMapper->GetUseJittering() == 0;

    previewParams.tfNodes = {
        { 0.0, 0.0, 0.75, 0.75, 0.75 },
        { 1.0, 1.0, 0.75, 0.75, 0.75 }
    };
    previewParams.gradientOpacity = {
        { 0.0, 0.0 }, { 255.0, 0.9 }
    };
    previewParams.material = {
        0.25, 0.65, 0.10, 8.0, 0.8, false
    };
    previewStrategy.SetVisualState(
        previewParams,
        UpdateFlags::TF
            | UpdateFlags::GradientOpacity
            | UpdateFlags::Material);
    auto* updatedColor = previewProperty
        ? previewProperty->GetRGBTransferFunction() : nullptr;
    auto* updatedOpacity = previewProperty
        ? previewProperty->GetScalarOpacity() : nullptr;
    auto* updatedGradient = previewProperty
        ? previewProperty->GetGradientOpacity() : nullptr;
    previewWindow->SetDesiredUpdateRate(GetRenderRate(false));
    previewWindow->Render();
    const bool isVisualStateRestored =
        previewMapper
        && std::abs(
            previewMapper->GetImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - 0.25)
            < 1e-12
        && previewMapper->GetUseJittering() == 0
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

    previewParams.volumeQuality = {
        VolumeQuality::Quality, 766, 1.0, true
    };
    previewWindow->SetDesiredUpdateRate(GetRenderRate(false));
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
    previewWindow->SetDesiredUpdateRate(GetRenderRate(true));
    previewWindow->Render();
    const bool isQualityPreview =
        previewMapper
        && qualityPreviewInput.GetPointer()
        && previewMapper->GetInputConnection(0, 0)
            == qualityPreviewInput.GetPointer()
        && std::abs(
            previewMapper->GetImageSampleDistance() - 2.0)
            < 1e-12
        && previewMapper->GetSampleDistance()
            >= 2.0 * qualityStillRay;
    previewWindow->SetDesiredUpdateRate(GetRenderRate(false));
    previewWindow->Render();
    const bool isQualityRestored =
        previewMapper
        && previewMapper->GetInputConnection(0, 0)
            == qualityPreviewInput.GetPointer()
        && std::abs(
            previewMapper->GetImageSampleDistance() - 1.0)
            < 1e-12
        && std::abs(
            previewMapper->GetSampleDistance() - qualityStillRay)
            < 1e-12;

    previewParams.volumeQuality = {
        VolumeQuality::Custom, 32, 0.25, false
    };
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
        previewWindow->SetDesiredUpdateRate(GetRenderRate(true));
        previewWindow->Render();
        const bool isPreviewStable =
            previewMapper
            && previewProperty
            && std::abs(
                previewMapper->GetImageSampleDistance() - 2.0)
                < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance() - 0.5)
                < 1e-12
            && previewProperty->GetShade()
                == static_cast<int>(material.isShadeOn)
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
        previewWindow->SetDesiredUpdateRate(GetRenderRate(false));
        previewWindow->Render();
        const bool isStillStable =
            previewMapper
            && std::abs(
                previewMapper->GetImageSampleDistance() - 1.0)
                < 1e-12
            && std::abs(
                previewMapper->GetSampleDistance() - 0.25)
                < 1e-12;
        areMaterialsStable =
            isPreviewStable && isStillStable
            && areMaterialsStable;
    }

    CompositeStrategy compositeStrategy(
        VizMode::CompositeVolume);
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
        compositeMapper
        && std::abs(
            compositeMapper->GetImageSampleDistance()
                - 2.0) < 1e-12
        && std::abs(
            compositeMapper->GetSampleDistance()
                - 0.5) < 1e-12
        && areReferencePropsStable;
    compositeWindow->SetDesiredUpdateRate(
        GetRenderRate(false));
    compositeWindow->Render();
    const bool isCompositeRestored =
        compositeMapper
        && std::abs(
            compositeMapper->GetImageSampleDistance()
                - 1.0) < 1e-12
        && std::abs(
            compositeMapper->GetSampleDistance()
                - 0.25) < 1e-12;
    compositeStrategy.DetachRenderer(
        compositeRenderer);
    if (!(isInteractiveMapperQuality
        && isStillMapperQuality
        && isInvalidPreviewRolledBack
        && isFeaturePreview
        && isFeaturePreviewRestored
        && isVisualStateRestored
        && isQualityPreview
        && isQualityRestored
        && areMaterialsStable
        && isCompositePreview
        && isCompositeRestored)) {
        std::cerr
            << "DIAG_VOLUME_PREVIEW:"
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
            << " rollback_property_time="
            << (previewProperty
                && previewProperty->GetMTime()
                    == previewPropertyTime)
            << " feature=" << isFeaturePreview
        << " feature_restore="
        << isFeaturePreviewRestored
        << " visual_restore="
        << isVisualStateRestored
        << " quality=" << isQualityPreview
        << " quality_restore=" << isQualityRestored
        << " materials=" << areMaterialsStable
        << " composite=" << isCompositePreview
            << " composite_restore="
            << isCompositeRestored
            << '\n';
    }
    failureCount += GetCaseResult(
        isInteractiveMapperQuality
            && isStillMapperQuality
            && isInvalidPreviewRolledBack
            && isFeaturePreview
            && isFeaturePreviewRestored
            && isVisualStateRestored
            && isQualityPreview
            && isQualityRestored
            && areMaterialsStable
            && isCompositePreview
            && isCompositeRestored,
        "Volume mapper switches Quality/Custom/material sampling and restores baseline") ? 0 : 1;

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
    zeroRangeParams.tfNodes = customNodes;
    volumeStrategy.SetVisualState(
        zeroRangeParams, UpdateFlags::TF);
    colorFunction = volume->GetProperty()->GetRGBTransferFunction();
    opacityFunction = volume->GetProperty()->GetScalarOpacity();
    failureCount += GetCaseResult(
        colorFunction
            && opacityFunction
            && colorFunction->GetSize() == 1
            && opacityFunction->GetSize() == 1
            && std::abs(colorFunction->GetRange()[0] - 12.0)
                < 1e-12
            && std::abs(colorFunction->GetRange()[1] - 12.0)
                < 1e-12,
        "Zero-width scalar range collapses TF nodes without invalid values") ? 0 : 1;

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
