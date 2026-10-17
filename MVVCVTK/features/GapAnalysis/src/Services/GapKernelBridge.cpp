#include "GapKernelBridge.h"
#include "GapKernelLabelView.h"

#include "DefXAnalysisService.h"

#include <vtkDataArray.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <unordered_map>
#include <vector>

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
        || request->backgroundMean > request->materialMean
        || request->numberRuns <= 0
        || (request->validityMask ? request->validityCount != request->voxelCount : request->validityCount != 0)) {
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
    region.hasWholeObjectGeometry = 1;
    return region;
}

struct GapLabelData final {
    ~GapLabelData() noexcept
    {
        if(!hasReleasedLabels)analysis.ReleaseLabelImage();
    }

    bool hasReleasedLabels=false;
    std::vector<float> maskedVoxels;
    DefXAnalysisService analysis;
    std::vector<std::int32_t> derivedLabels;
    vtkSmartPointer<vtkImageData> labelImage;
    GapKernelLabelView labelView;
};

using GapLabelOwner = std::shared_ptr<GapLabelData>;

bool GetCancelled(const GapKernelRequest& request) noexcept
{
    return request.getCancelled && request.getCancelled(request.cancelContext) != 0;
}

struct GrayMoments final {
    std::uint64_t count = 0;
    double mean = 0, squareDeviation = 0;
    void Add(float value) noexcept {
        ++count; const double delta = value - mean;
        mean += delta / static_cast<double>(count);
        squareDeviation += delta * (value - mean);
    }
    float GetStd() const noexcept {
        return count ? static_cast<float>(std::sqrt(std::max(0.0, squareDeviation / count))) : 0.f;
    }
};
struct MaskStatistics final {
    GrayMoments material, background;
    float maximum = std::numeric_limits<float>::lowest();
    double voxelVolume = 0;
};

bool BuildMaskStatistics(const GapKernelRequest& request, MaskStatistics& statistics)
{
    const auto* d = request.direction;
    const double determinant = d[0]*(d[4]*d[8]-d[5]*d[7])
        - d[1]*(d[3]*d[8]-d[5]*d[6]) + d[2]*(d[3]*d[7]-d[4]*d[6]);
    statistics.voxelVolume = request.spacing[0]*request.spacing[1]*request.spacing[2]*std::abs(determinant);
    if (!std::isfinite(statistics.voxelVolume) || statistics.voxelVolume <= 0) return false;
    for (std::size_t i=0; i<request.voxelCount; ++i) {
        if (!(i%4096) && GetCancelled(request)) return false;
        if (!request.validityMask[i]) continue;
        const float value=request.voxelData[i];
        if (!std::isfinite(value)) return false;
        statistics.maximum=std::max(statistics.maximum,value);
        // DefX 的候选灰度上界包含 iso；填充值必须严格大于 iso。
        (value > request.isoThreshold ? statistics.material : statistics.background).Add(value);
    }
    return statistics.material.count + statistics.background.count != 0;
}

void SetMaskHeader(const GapKernelRequest& request, const MaskStatistics& statistics,
    GapKernelHeader& header)
{
    std::copy_n(request.dims,3,header.dims);
    header.materialMean=static_cast<float>(statistics.material.mean);
    header.materialStd=statistics.material.GetStd();
    header.airMean=static_cast<float>(statistics.background.mean);
    header.airStd=statistics.background.GetStd();
    header.materialVolumeMM3=static_cast<float>(statistics.material.count*statistics.voxelVolume);
    // totalVoxelCount 是材料与所有可见孔的并集；服务据此扣除可见孔得到材料量。
    header.totalVoxelCount=static_cast<std::int64_t>(statistics.material.count);
    header.defectVolumeMM3=0;header.defectVolumeRatio=0;
    header.objectMean=header.materialMean;header.objectStd=header.materialStd;
    header.algorithmMode=DefXAlgorithm_BestOperator;
}

bool BuildMaskedRegions(const GapKernelRequest& request, const MaskStatistics& statistics,
    GapLabelData& data, std::vector<GapKernelRegion>& regions, GapKernelHeader& header)
{
    const auto* original=data.labelView.GetData();
    if (!original || data.labelView.GetCount()!=request.voxelCount) return false;
    struct Source final { const GapKernelRegion* region; std::uint64_t labeled = 0; };
    std::unordered_map<std::int32_t,Source> source;
    for (const auto& region:regions)
        if(region.id<=0 || region.voxelCount<0 || !source.emplace(region.id,Source{&region,0}).second)return false;
    // 先验证供应商标签/目录的一致性，不能靠清理掩膜隐藏损坏的原始批次。
    for (std::size_t i=0;i<request.voxelCount;++i) {
        if (!(i%4096) && GetCancelled(request)) return false;
        if(original[i]<0)return false;
        if(original[i]>0) {
            const auto found=source.find(original[i]);if(found==source.end())return false;
            ++found->second.labeled;
        }
    }
    for(const auto& entry:source)
        if(entry.second.labeled!=static_cast<std::uint64_t>(entry.second.region->voxelCount))return false;
    data.derivedLabels.assign(static_cast<std::size_t>(request.voxelCount),0);
    std::vector<GapKernelRegion> selected;
    std::vector<std::size_t> pending;
    const auto dx=static_cast<std::size_t>(request.dims[0]);
    const auto dy=static_cast<std::size_t>(request.dims[1]);
    const auto plane=dx*dy;
    GrayMoments object=statistics.material;
    SetMaskHeader(request,statistics,header);
    std::uint64_t formalVoxels=0;
    // 1. 无效域归零。2. 有效候选按已实测的六邻接重标。3. 任一面邻接无效域则整个组件不确定。
    for(std::size_t seed=0;seed<request.voxelCount;++seed) {
        if (!(seed%4096) && GetCancelled(request))return false;
        if(!request.validityMask[seed] || original[seed]<=0 || data.derivedLabels[seed])continue;
        if(selected.size()>=static_cast<std::size_t>(INT32_MAX))return false;
        const auto id=static_cast<std::int32_t>(selected.size()+1);
        const auto sourceId=original[seed];bool singleSource=true,boundary=false;
        std::int32_t bounds[6]={INT32_MAX,INT32_MIN,INT32_MAX,INT32_MIN,INT32_MAX,INT32_MIN};
        GrayMoments gray;double sums[3]={};float minimum=std::numeric_limits<float>::max(),maximum=std::numeric_limits<float>::lowest();
        pending.clear();pending.push_back(seed);data.derivedLabels[seed]=id;
        while(!pending.empty()) {
            if (!(gray.count%4096) && GetCancelled(request))return false;
            const auto index=pending.back();pending.pop_back();
            const std::size_t coordinates[3]={index%dx,(index/dx)%dy,index/plane};
            const float value=request.voxelData[index];gray.Add(value);minimum=std::min(minimum,value);maximum=std::max(maximum,value);
            singleSource=singleSource && original[index]==sourceId;
            for(int axis=0;axis<3;++axis) {
                const int voxel=request.extent[axis*2]+static_cast<int>(coordinates[axis]);
                sums[axis]+=voxel;bounds[axis*2]=std::min(bounds[axis*2],voxel);bounds[axis*2+1]=std::max(bounds[axis*2+1],voxel);
            }
            const auto neighbor=[&](std::size_t next) {
                if(!request.validityMask[next]){boundary=true;return;}
                if(original[next]>0 && !data.derivedLabels[next]){data.derivedLabels[next]=id;pending.push_back(next);}
            };
            if(coordinates[0])neighbor(index-1);if(coordinates[0]+1<dx)neighbor(index+1);
            if(coordinates[1])neighbor(index-dx);if(coordinates[1]+1<dy)neighbor(index+dx);
            if(coordinates[2])neighbor(index-plane);if(coordinates[2]+1<static_cast<std::size_t>(request.dims[2]))neighbor(index+plane);
        }
        const auto* parent=source.at(sourceId).region;
        const bool unchanged=singleSource && gray.count==static_cast<std::uint64_t>(parent->voxelCount);
        GapKernelRegion region=unchanged ? *parent : GapKernelRegion{};
        region.id=id;region.voxelCount=static_cast<std::int64_t>(gray.count);
        region.volumeMM3=static_cast<float>(gray.count*statistics.voxelVolume);
        region.boundaryStatus=boundary ? 1U : 0U;
        region.hasWholeObjectGeometry=unchanged && !boundary ? 1U : 0U;
        std::copy_n(bounds,6,region.bbox);
        const std::size_t seedCoordinates[3]={seed%dx,(seed/dx)%dy,seed/plane};
        for(int axis=0;axis<3;++axis)region.seedVoxel[axis]=request.extent[axis*2]+static_cast<int>(seedCoordinates[axis]);
        region.minGray=minimum;region.maxGray=maximum;region.meanGray=static_cast<float>(gray.mean);region.stdDevGray=gray.GetStd();
        region.grayDeviation=region.meanGray-static_cast<float>(statistics.background.mean);
        if(!unchanged) {
            // 截断/合并后只承诺重算体积与包围盒；不把旧父区域形状量或概率用于新片段。
            for(int row=0;row<3;++row) {
                region.centerMM[row]=request.origin[row];
                for(int col=0;col<3;++col)region.centerMM[row]+=request.direction[row*3+col]*request.spacing[col]*sums[col]/gray.count;
            }
        }
        header.totalVoxelCount+=region.voxelCount;
        if(!boundary) {
            formalVoxels+=gray.count;
            // 用同一有效体素集合计算对象灰度量，不包含人工材料填值。
            const double delta=gray.mean-object.mean;const auto combined=object.count+gray.count;
            if(combined) {
                object.squareDeviation+=gray.squareDeviation+delta*delta*object.count*gray.count/combined;
                object.mean+=delta*gray.count/combined;object.count=combined;
            }
        }
        selected.push_back(region);
    }
    regions=std::move(selected);header.regionCount=static_cast<std::int32_t>(regions.size());
    header.defectVolumeMM3=static_cast<float>(formalVoxels*statistics.voxelVolume);
    header.defectVolumeRatio=header.materialVolumeMM3>0 ? header.defectVolumeMM3/header.materialVolumeMM3:0;
    header.objectMean=static_cast<float>(object.mean);header.objectStd=object.GetStd();
    return !GetCancelled(request);
}


void* MVVCVTK_GAP_KERNEL_CALL CloneLabelOwner(
    const void* owner) noexcept
{
    if (!owner) {
        return nullptr;
    }
    try {
        const auto& source = *static_cast<const GapLabelOwner*>(owner);
        if (!source) {
            return nullptr;
        }
        auto clone = std::make_unique<GapLabelOwner>(source);
        return clone.release();
    }
    catch (...) {
        return nullptr;
    }
}

void MVVCVTK_GAP_KERNEL_CALL ReleaseLabelOwner(
    void* owner) noexcept
{
    std::unique_ptr<GapLabelOwner> release(
        static_cast<GapLabelOwner*>(owner));
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
        if(GetCancelled(*request))return 0;
        auto labelData = std::make_shared<GapLabelData>();
        auto effectiveRequest=*request;
        MaskStatistics maskStatistics;
        if(request->validityMask) {
            if(!BuildMaskStatistics(*request,maskStatistics))return 0;
            if(maskStatistics.material.count && maskStatistics.background.count) {
                // 最大有效材料值严格高于 iso，不进入自动背景阈值的统计集合。
                labelData->maskedVoxels.resize(static_cast<std::size_t>(request->voxelCount));
                for(std::size_t i=0;i<request->voxelCount;++i) {
                    if(!(i%4096) && GetCancelled(*request))return 0;
                    labelData->maskedVoxels[i]=request->validityMask[i] ? request->voxelData[i]:maskStatistics.maximum;
                }
                effectiveRequest.voxelData=labelData->maskedVoxels.data();
            }
        }
        auto inputImage = BuildInputImage(effectiveRequest);
        if (!inputImage) {
            return 0;
        }

        DefXAnalysisRequest analysisRequest{};
        analysisRequest.algoMode = DefXAlgorithm_BestOperator;
        analysisRequest.material.analysisMode = DefXAnalysis_Pore;
        analysisRequest.material.surface.backgroundMean =
            effectiveRequest.backgroundMean;
        analysisRequest.material.surface.materialMean =
            effectiveRequest.materialMean;
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
        // DefX 只输出原始检测批次；Gap 在自己的结果阶段统一执行尺寸筛选。
        analysisRequest.filter.enabled = false;

        auto& analysis = labelData->analysis;
        std::cerr
            << "[GapAnalysis][DefX] started"
            << " | dims=" << request->dims[0]
            << 'x' << request->dims[1]
            << 'x' << request->dims[2]
            << " | background=" << request->backgroundMean
            << " | material=" << request->materialMean
            << " | iso=" << request->isoThreshold
            << '\n' << std::flush;
        const auto analysisStart = std::chrono::steady_clock::now();
        const bool emptyMasked=request->validityMask && (!maskStatistics.material.count || !maskStatistics.background.count);
        const bool isAnalyzed = emptyMasked || (analysis.SetInputImage(inputImage)
            && analysis.RunVglCoreAnalysis(analysisRequest, nullptr));
        const std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - analysisStart;
        std::cerr
            << "[GapAnalysis][DefX] "
            << (isAnalyzed ? "completed" : "failed")
            << " | elapsed=" << elapsed.count() << "s\n"
            << std::flush;
        if (!isAnalyzed || GetCancelled(*request)) {
            return 0;
        }

        if(emptyMasked) {
            labelData->labelImage=vtkSmartPointer<vtkImageData>::New();
            labelData->labelImage->CopyStructure(inputImage);labelData->labelImage->AllocateScalars(VTK_INT,1);
            std::fill_n(static_cast<int*>(labelData->labelImage->GetScalarPointer()),static_cast<std::size_t>(request->voxelCount),0);
        } else labelData->labelImage = analysis.getLabelImage();
        if (!labelData->labelImage) {
            // 供应商 import library 未导出 BuildLabelImage；factor=1 只复制并保留原标签值。
            labelData->labelImage =
                analysis.BuildDownsampledLabelImage(1);
        }
        if (!labelData->labelImage
            || !GetGeometryValid(
                inputImage,
                labelData->labelImage)) {
            return 0;
        }

        const auto labelCount = static_cast<std::size_t>(
            request->voxelCount);
        auto* scalars = labelData->labelImage->GetPointData()
            ? labelData->labelImage->GetPointData()->GetScalars()
            : nullptr;
        if (!BuildGapKernelLabelView(
                scalars,
                labelCount,
                labelData->labelView)) {
            return 0;
        }
        std::cerr
            << "[GapAnalysis][DefX] labels ready"
            << " | scalar=" << scalars->GetDataTypeAsString()
            << " | transfer="
            << (labelData->labelView.GetIsBorrowed()
                ? "borrowed" : "converted")
            << '\n' << std::flush;

        std::vector<GapKernelRegion> regions;
        GapKernelHeader header{};
        if(!emptyMasked) {
            const auto& sourceRegions=analysis.GetDefectRegions();regions.reserve(sourceRegions.size());
            for(const auto& sourceRegion:sourceRegions)regions.push_back(BuildRegion(sourceRegion));
            header=BuildHeader(analysis.GetOutputHeader());
        }
        if(request->validityMask) {
            if(emptyMasked)header.computedThreshold=request->isoThreshold;
            if(!BuildMaskedRegions(*request,maskStatistics,*labelData,regions,header))return 0;
        }
        if(request->validityMask) {
            // 派生标签已独立持有；发布前释放供应商原标签，避免它在读者生命周期内叠加驻留。
            labelData->labelView=GapKernelLabelView{};
            labelData->labelImage=nullptr;
            labelData->hasReleasedLabels=true;
            analysis.ReleaseLabelImage();
        }
        const auto* outputLabels=request->validityMask ? labelData->derivedLabels.data():labelData->labelView.GetData();
        const GapKernelResultView result{
            GapKernelAbiVersion,
            static_cast<std::uint32_t>(sizeof(GapKernelResultView)),
            static_cast<std::uint32_t>(sizeof(GapKernelHeader)),
            static_cast<std::uint32_t>(sizeof(GapKernelRegion)),
            &header,
            regions.empty() ? nullptr : regions.data(),
            static_cast<std::uint64_t>(regions.size()),
            outputLabels,
            request->voxelCount,
            &labelData,
            &CloneLabelOwner,
            &ReleaseLabelOwner
        };
        if(GetCancelled(*request))return 0;
        const std::int32_t isConsumed = sink(&result, context);
        return isConsumed != 0 ? 1 : 0;
    }
    catch (const std::exception& error) {
        std::cerr << "[GapAnalysis][DefX] Result bridge failed: " << error.what() << '\n';
        return 0;
    }
    catch (...) {
        std::cerr << "[GapAnalysis][DefX] Result bridge failed: unknown exception.\n";
        return 0;
    }
}
