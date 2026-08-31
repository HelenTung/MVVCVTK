#include "GapKernelBridge.h"

#include "DefXAnalysisService.h"

#include <vtkDataArray.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkSetGet.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>
#include <utility>

namespace {

bool GetRequestValid(const GapKernelRequest* request)
{
    if (!request
        || request->abiVersion != GapKernelAbiVersion
        || request->structSize != sizeof(GapKernelRequest)
        || !request->voxelData
        || !std::isfinite(request->backgroundMean)
        || !std::isfinite(request->materialMean)
        || !std::isfinite(request->isoThreshold)
        || !std::isfinite(request->minVolumeMM3)
        || request->backgroundMean > request->materialMean
        || request->minVolumeMM3 < 0.0f
        || request->numberRuns <= 0
        || request->isFilterEnabled > 1U) {
        return false;
    }

    std::uint64_t voxelCount = 1;
    for (int axis = 0; axis < 3; ++axis) {
        const auto extentMin = static_cast<std::int64_t>(
            request->extent[axis * 2]);
        const auto extentMax = static_cast<std::int64_t>(
            request->extent[axis * 2 + 1]);
        if (request->dims[axis] <= 0
            || extentMax < extentMin
            || extentMax - extentMin + 1
                != static_cast<std::int64_t>(request->dims[axis])
            || !std::isfinite(request->spacing[axis])
            || request->spacing[axis] <= 0.0
            || !std::isfinite(request->origin[axis])) {
            return false;
        }
        const auto dimension = static_cast<std::uint64_t>(
            request->dims[axis]);
        if (voxelCount
            > (std::numeric_limits<std::uint64_t>::max)()
                / dimension) {
            return false;
        }
        voxelCount *= dimension;
    }
    for (double value : request->direction) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return voxelCount == request->voxelCount
        && voxelCount <= static_cast<std::uint64_t>(
            (std::numeric_limits<vtkIdType>::max)());
}

vtkSmartPointer<vtkImageData> BuildInputImage(
    const GapKernelRequest& request)
{
    auto inputImage = vtkSmartPointer<vtkImageData>::New();
    inputImage->SetExtent(
        request.extent[0], request.extent[1],
        request.extent[2], request.extent[3],
        request.extent[4], request.extent[5]);
    inputImage->SetSpacing(request.spacing);
    inputImage->SetOrigin(request.origin);

    auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            direction->SetElement(
                row,
                column,
                request.direction[row * 3 + column]);
        }
    }
    inputImage->SetDirectionMatrix(direction);

    auto scalars = vtkSmartPointer<vtkFloatArray>::New();
    scalars->SetNumberOfComponents(1);
    // save=1 表示 VTK 不释放调用方持有的缓冲区；同步分析返回前 owner 始终有效。
    scalars->SetArray(
        const_cast<float*>(request.voxelData),
        static_cast<vtkIdType>(request.voxelCount),
        1);
    inputImage->GetPointData()->SetScalars(scalars);
    return inputImage->GetNumberOfPoints()
            == static_cast<vtkIdType>(request.voxelCount)
        ? inputImage : nullptr;
}

bool GetGeometryValid(
    vtkImageData* inputImage,
    vtkImageData* labelImage)
{
    if (!inputImage || !labelImage) {
        return false;
    }

    int inputDims[3] = {};
    int labelDims[3] = {};
    int inputExtent[6] = {};
    int labelExtent[6] = {};
    double inputOrigin[3] = {};
    double labelOrigin[3] = {};
    double inputSpacing[3] = {};
    double labelSpacing[3] = {};
    inputImage->GetDimensions(inputDims);
    labelImage->GetDimensions(labelDims);
    inputImage->GetExtent(inputExtent);
    labelImage->GetExtent(labelExtent);
    inputImage->GetOrigin(inputOrigin);
    labelImage->GetOrigin(labelOrigin);
    inputImage->GetSpacing(inputSpacing);
    labelImage->GetSpacing(labelSpacing);
    for (int axis = 0; axis < 3; ++axis) {
        if (labelDims[axis] != inputDims[axis]
            || inputOrigin[axis] != labelOrigin[axis]
            || inputSpacing[axis] != labelSpacing[axis]) {
            return false;
        }
    }
    for (int index = 0; index < 6; ++index) {
        if (inputExtent[index] != labelExtent[index]) {
            return false;
        }
    }

    const auto* inputDirection = inputImage->GetDirectionMatrix();
    const auto* labelDirection = labelImage->GetDirectionMatrix();
    if ((inputDirection == nullptr) != (labelDirection == nullptr)) {
        return false;
    }
    if (!inputDirection) {
        return true;
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (inputDirection->GetElement(row, column)
                != labelDirection->GetElement(row, column)) {
                return false;
            }
        }
    }
    return true;
}

GapKernelHeader BuildHeader(
    const DefXAnalysisOutputHeader& source)
{
    GapKernelHeader header{};
    header.regionCount = source.regionCount;
    std::copy_n(source.dims, 3, header.dims);
    header.totalVoxelCount = source.totalVoxelCount;
    header.materialMean = source.materialMean;
    header.materialStd = source.materialStd;
    header.airMean = source.airMean;
    header.airStd = source.airStd;
    header.computedThreshold = source.computedThreshold;
    header.algorithmMode = source.algorithmMode;
    header.materialVolumeMM3 = source.materialVolumeMM3;
    header.defectVolumeMM3 = source.accumulatedDefectVolumeMM3;
    header.defectVolumeRatio = source.defectVolumeRatio;
    header.objectMean = source.objectMean;
    header.objectStd = source.objectStd;
    return header;
}

GapKernelRegion BuildRegion(const DefXDefectRegion& source)
{
    GapKernelRegion region{};
    region.id = source.id;
    region.voxelCount = source.voxelCount;
    region.volumeMM3 = source.volumeMM3;
    region.equivalentDiameterMM = source.equivalentDiameterMM;
    region.radiusMM = source.radiusMM;
    region.diameterMM = source.diameterMM;
    std::copy_n(source.centerMM, 3, region.centerMM);
    std::copy_n(source.centroidMM, 3, region.centroidMM);
    std::copy_n(source.bbox, 6, region.bbox);
    std::copy_n(source.seedVoxel, 3, region.seedVoxel);
    region.minGray = source.minGray;
    region.maxGray = source.maxGray;
    region.meanGray = source.meanGray;
    region.stdDevGray = source.stdDevGray;
    region.grayDeviation = source.grayDeviation;
    region.gapMM = source.gapMM;
    region.compactness = source.compactness;
    region.surfaceAreaMM2 = source.surfaceAreaMM2;
    region.sphericity = source.sphericity;
    region.pcaDeviation1 = source.pcaDeviation1;
    region.pcaDeviation2 = source.pcaDeviation2;
    region.pcaDeviation3 = source.pcaDeviation3;
    region.pcaMaxDeviationRatio = source.pcaMaxDeviationRatio;
    region.pcaMinDeviationRatio = source.pcaMinDeviationRatio;
    region.projectedAreaXMM2 = source.projectedAreaXMM2;
    region.projectedAreaYMM2 = source.projectedAreaYMM2;
    region.projectedAreaZMM2 = source.projectedAreaZMM2;
    region.projectedSizeX = source.projectedSizeX;
    region.projectedSizeY = source.projectedSizeY;
    region.projectedSizeZ = source.projectedSizeZ;
    region.defectProbability = source.defectProbability;
    return region;
}

template <typename TValue>
bool BuildLabels(
    const TValue* source,
    const std::size_t labelCount,
    std::vector<std::int32_t>& labels)
{
    if (!source) {
        return false;
    }
    labels.resize(labelCount);
    constexpr long double maxLabel =
        static_cast<long double>(
            (std::numeric_limits<std::int32_t>::max)());
    for (std::size_t index = 0; index < labelCount; ++index) {
        const long double value = static_cast<long double>(source[index]);
        if (!std::isfinite(value)
            || value < 0.0L
            || value > maxLabel
            || std::trunc(value) != value) {
            labels.clear();
            return false;
        }
        labels[index] = static_cast<std::int32_t>(value);
    }
    return true;
}

bool BuildLabels(
    vtkDataArray* scalars,
    const std::size_t labelCount,
    std::vector<std::int32_t>& labels)
{
    labels.clear();
    if (!scalars
        || scalars->GetNumberOfComponents() != 1
        || scalars->GetNumberOfTuples()
            != static_cast<vtkIdType>(labelCount)) {
        return false;
    }

    bool isBuilt = false;
    switch (scalars->GetDataType()) {
        vtkTemplateMacro(
            isBuilt = BuildLabels(
                static_cast<const VTK_TT*>(scalars->GetVoidPointer(0)),
                labelCount,
                labels));
    default:
        break;
    }
    return isBuilt;
}

}

extern "C" std::int32_t MVVCVTK_GAP_KERNEL_CALL BuildGapResult(
    const GapKernelRequest* request,
    const GapKernelResultSink sink,
    void* context) noexcept
{
    if (!GetRequestValid(request) || !sink) {
        return 0;
    }

    try {
        auto inputImage = BuildInputImage(*request);
        if (!inputImage) {
            return 0;
        }

        DefXAnalysisRequest analysisRequest{};
        analysisRequest.algoMode = DefXAlgorithm_BestOperator;
        analysisRequest.material.analysisMode = DefXAnalysis_Pore;
        analysisRequest.material.surface.backgroundMean =
            request->backgroundMean;
        analysisRequest.material.surface.materialMean =
            request->materialMean;
        analysisRequest.material.surface.isoThreshold =
            request->isoThreshold;
        analysisRequest.material.thresholdMode = ThresholdMode_Deviation;
        analysisRequest.material.autoThresholdFactor = 0.0f;
        analysisRequest.material.extremeValue = 0.0f;
        analysisRequest.params.noiseReduction = NoiseReduction_None;
        analysisRequest.params.criterion = Criterion_Standard;
        analysisRequest.params.areaMode = Area_InsideAll;
        analysisRequest.params.useSurfaceSealing = false;
        analysisRequest.params.surfaceSealingVoxel = 0;
        analysisRequest.params.seedStrategy = DefXSeed_LocalMinima;
        analysisRequest.params.numberRuns = request->numberRuns;
        analysisRequest.filter = FilterCriteria{};
        analysisRequest.filter.enabled = request->isFilterEnabled != 0U;
        analysisRequest.filter.minVolume = request->minVolumeMM3;

        DefXAnalysisService analysis;
        std::cerr
            << "[GapAnalysis][DefX] started"
            << " | dims=" << request->dims[0]
            << 'x' << request->dims[1]
            << 'x' << request->dims[2]
            << " | background=" << request->backgroundMean
            << " | material=" << request->materialMean
            << " | iso=" << request->isoThreshold
            << " | filter=" << request->isFilterEnabled
            << '\n' << std::flush;
        const auto analysisStart = std::chrono::steady_clock::now();
        const bool isAnalyzed = analysis.SetInputImage(inputImage)
            && analysis.RunVglCoreAnalysis(analysisRequest);
        const std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - analysisStart;
        std::cerr
            << "[GapAnalysis][DefX] "
            << (isAnalyzed ? "completed" : "failed")
            << " | elapsed=" << elapsed.count() << "s\n"
            << std::flush;
        if (!isAnalyzed) {
            return 0;
        }

        auto labelImage = analysis.getLabelImage();
        if (!labelImage) {
            // 供应商 import library 未导出 BuildLabelImage；factor=1 只复制并保留原标签值。
            labelImage = analysis.BuildDownsampledLabelImage(1);
        }
        if (!labelImage || !GetGeometryValid(inputImage, labelImage)) {
            analysis.ReleaseLabelImage();
            return 0;
        }

        const auto labelCount = static_cast<std::size_t>(
            request->voxelCount);
        auto* scalars = labelImage->GetPointData()
            ? labelImage->GetPointData()->GetScalars() : nullptr;
        std::vector<std::int32_t> labels;
        if (!BuildLabels(scalars, labelCount, labels)) {
            analysis.ReleaseLabelImage();
            return 0;
        }

        const auto& sourceRegions = analysis.GetDefectRegions();
        std::vector<GapKernelRegion> regions;
        regions.reserve(sourceRegions.size());
        for (const auto& sourceRegion : sourceRegions) {
            regions.push_back(BuildRegion(sourceRegion));
        }
        const GapKernelHeader header = BuildHeader(
            analysis.GetOutputHeader());
        const GapKernelResultView result{
            GapKernelAbiVersion,
            static_cast<std::uint32_t>(sizeof(GapKernelResultView)),
            static_cast<std::uint32_t>(sizeof(GapKernelHeader)),
            static_cast<std::uint32_t>(sizeof(GapKernelRegion)),
            &header,
            regions.empty() ? nullptr : regions.data(),
            static_cast<std::uint64_t>(regions.size()),
            labels.empty() ? nullptr : labels.data(),
            static_cast<std::uint64_t>(labels.size())
        };
        const std::int32_t isConsumed = sink(&result, context);
        analysis.ReleaseLabelImage();
        return isConsumed != 0 ? 1 : 0;
    }
    catch (...) {
        return 0;
    }
}
