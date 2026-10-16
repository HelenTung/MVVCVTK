#pragma once

#include "GapAnalysisTypes.h"
#include "Algorithms/GapSurfaceBuilder.h"
#include "Host/RoiReadTypes.h"
#include "Render/Support/AnalysisColorStyle.h"
#include "Render/GapLabelLookupTable.h"
#include "MappedArrayStorage.h"
#include "FeatureSupport/SpatialMeshSource.h"
#include <vtkCellData.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkFlyingEdges3D.h>
#include <vtkIntArray.h>
#include <vtkStringArray.h>
#include <vtkPointData.h>
#include <vtkVariant.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkNew.h>
#include <atomic>
#include <cmath>
#include <limits>

// 私有显示产物不发布到 DataGraph，正式标签、网格与供应商统计完全独立。
struct GapDisplayData final {
    vtkSmartPointer<vtkPolyData> mesh;
    vtkSmartPointer<vtkLookupTable> labels;
    vtkSmartPointer<vtkLookupTable> volumes;
    std::shared_ptr<const FeatureInternal::SpatialMeshSource> spatial;
    bool hasRegions = false;
    std::string resultLabel;
    // 稀疏 ID 与其正式体积只供重新着色，切换不重新提取网格或修改标签。
    std::shared_ptr<const GapRegionVolumes> regionVolumes;
    std::array<double, 2> resultRange{0, 0};
    std::array<double,2> mappingRange{0,0};
    GapDisplayParams params;

    static AnalysisColorStyle::RampMode GetRampMode(GapColorMode mode)
    {
        switch (mode) {
        case GapColorMode::Constant: return AnalysisColorStyle::RampMode::Constant;
        case GapColorMode::Gradient: return AnalysisColorStyle::RampMode::Gradient;
        case GapColorMode::Rainbow: return AnalysisColorStyle::RampMode::Rainbow;
        case GapColorMode::InverseRainbow: return AnalysisColorStyle::RampMode::InverseRainbow;
        case GapColorMode::HueLoop: return AnalysisColorStyle::RampMode::HueLoop;
        default: return static_cast<AnalysisColorStyle::RampMode>(-1);
        }
    }
    static AnalysisColorStyle::RampParams GetColorRamp(const GapDisplayParams& params)
    {
        AnalysisColorStyle::RampParams ramp;ramp.mode=GetRampMode(params.mode);
        ramp.isReversed=params.isReversed;
        ramp.constantColor = params.constantColor; ramp.lowColor = params.lowColor;
        ramp.highColor = params.highColor; ramp.belowColor = params.belowColor;
        ramp.aboveColor = params.aboveColor;
        ramp.opacityRange=params.opacityRange;
        ramp.blend=params.style==GapDisplayStyle::Constant ? AnalysisColorStyle::BlendMode::Constant
            : params.style==GapDisplayStyle::Inclined ? AnalysisColorStyle::BlendMode::Inclined
            : params.style==GapDisplayStyle::InverseInclined ? AnalysisColorStyle::BlendMode::InverseInclined
            : static_cast<AnalysisColorStyle::BlendMode>(-1);
        for(const auto& segment:params.segments)
            ramp.segments.push_back({segment.lower,segment.upper,GetRampMode(segment.mode),segment.lowColor,segment.highColor});
        return ramp;
    }

    static bool GetParamsValid(const GapDisplayParams& params) noexcept
    {
        if(!AnalysisColorStyle::GetLegendLayoutValid(params) || params.segments.size()>1024 || !std::isfinite(params.opacity) || params.opacity<0 || params.opacity>1)return false;
        try { return AnalysisColorStyle::GetRampValid(GetColorRamp(params))
            && (params.rangeMode == GapRangeMode::Result
                || (params.rangeMode == GapRangeMode::SelectedInterval
                    && std::isfinite(params.range[0]) && std::isfinite(params.range[1])
                    && params.range[0] >= 0 && params.range[0] < params.range[1]));
        } catch (...) { return false; }
    }

    static std::shared_ptr<const GapDisplayData> CreateColors(
        const GapDisplayData& source, const GapDisplayParams& params)
    {
        if (!GetParamsValid(params)) return {};
        auto result = std::make_shared<GapDisplayData>(source);
        result->params = params;AnalysisColorStyle::SetTailColors(result->params);
        result->mappingRange = params.rangeMode == GapRangeMode::Result ? source.resultRange : params.range;
        const auto ramp=GetColorRamp(params);
        if(result->mappingRange==source.mappingRange && AnalysisColorStyle::GetRampEqual(ramp,GetColorRamp(source.params)))return result;
        result->volumes = AnalysisColorStyle::BuildRamp(result->mappingRange,ramp);
        auto labels=vtkSmartPointer<GapLabelLookupTable>::New();
        labels->SetRegions(source.regionVolumes,result->mappingRange,ramp);
        result->labels=labels;
        return result;
    }

    static std::shared_ptr<const GapDisplayData> Build(vtkImageData* image,
        const DataArray<VoidRegion>& regions, const std::atomic<bool>& isStopping,
        const RoiReadSnapshot& evaluationRoi={},const DataArray<VoidRegion>* parentRegions=nullptr,
        std::string* failureReason=nullptr,vtkPolyData* preparedMesh=nullptr,
        std::shared_ptr<const SurfaceMeshPayload> preparedPayload={})
    {
        if (failureReason) failureReason->clear();
        const auto fail = [&](const std::string& reason) -> std::shared_ptr<const GapDisplayData> {
            if (failureReason) *failureReason = reason;
            return {};
        };
        if (!image || image->GetScalarType() != VTK_INT
            || image->GetNumberOfScalarComponents() != 1) return {};
        auto result = std::make_shared<GapDisplayData>();
        std::array<double, 2> range{std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest()};
        ResultArrayScope directoryStorage;
        auto directory=std::make_shared<GapRegionVolumes>();
        directory->reserve(regions.size());
        for (const auto& region : regions) {
            if(isStopping.load())return {};
            if (region.id <= 0
                || !std::isfinite(region.volumeMM3) || region.volumeMM3 < 0) return {};
            if(region.voxelCount<=0)continue;
            directory->emplace_back(region.id,region.volumeMM3);
            range[0] = std::min(range[0], double(region.volumeMM3));
            range[1] = std::max(range[1], double(region.volumeMM3));
        }
        try {
            std::sort(directory->begin(),directory->end(),[&](const auto& a,const auto& b){
                if(isStopping.load())throw std::runtime_error("Gap display directory cancelled.");return a.first<b.first;});
        }catch(const std::runtime_error&){if(isStopping.load())return {};throw;}
        if(std::adjacent_find(directory->begin(),directory->end(),[](const auto& a,const auto& b){return a.first==b.first;})!=directory->end())return {};
        directoryStorage.Finish();
        result->regionVolumes=std::move(directory);
        result->hasRegions = !result->regionVolumes->empty();
        if (!result->hasRegions) range = {0, 0};
        result->resultRange=range;result->mappingRange=range;
        result->volumes = AnalysisColorStyle::BuildRamp(range, GetColorRamp(result->params));
        auto labels=vtkSmartPointer<GapLabelLookupTable>::New();
        labels->SetRegions(result->regionVolumes,range,GetColorRamp(result->params),result->volumes);
        result->labels=labels;
        auto geometry=preparedMesh?vtkSmartPointer<vtkPolyData>(preparedMesh):GapSurfaceBuilder::Build(image,regions,isStopping,evaluationRoi,parentRegions);
        if(!geometry)return fail("Gap surface construction failed or was cancelled.");
        result->mesh=vtkSmartPointer<vtkPolyData>::New();result->mesh->ShallowCopy(geometry);
        auto* regionIds=vtkDataArray::SafeDownCast(geometry->GetCellData()->GetArray("gap.region.id"));
        if(!regionIds)return fail("Gap surface region identifiers are missing.");
        // 显示只创建数组外壳，共享正式几何与编号缓冲，不修改正式候选的属性名。
        auto displayIds=MeshVtkStorage::GetIntegerIds(regionIds,[&]{return isStopping.load();});
        if(!displayIds)return fail("Gap surface region identifiers are invalid or cancelled.");displayIds->SetName("gap.display.id");
        result->mesh->GetCellData()->RemoveArray("gap.region.id");result->mesh->GetCellData()->SetScalars(displayIds);
        if(preparedPayload)result->spatial=FeatureInternal::SpatialMeshSource::Build(std::move(preparedPayload),[&]{return isStopping.load();});
        return isStopping.load() ? nullptr : result;
    }
};
