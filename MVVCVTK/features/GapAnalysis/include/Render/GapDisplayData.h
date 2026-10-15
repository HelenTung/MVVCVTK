#pragma once

#include "GapAnalysisTypes.h"
#include "Host/RoiReadTypes.h"
#include "Render/Support/AnalysisColorStyle.h"
#include "Render/GapLabelLookupTable.h"
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
#include <set>

// 私有显示产物不发布到 DataGraph，正式标签、网格与供应商统计完全独立。
struct GapDisplayData final {
    vtkSmartPointer<vtkPolyData> mesh;
    vtkSmartPointer<vtkLookupTable> labels;
    vtkSmartPointer<vtkLookupTable> volumes;
    bool hasRegions = false;
    std::string resultLabel;
    // 稀疏 ID 与其正式体积只供重新着色，切换不重新提取网格或修改标签。
    std::vector<std::pair<std::int32_t, double>> regionVolumes;
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
        result->labels = vtkSmartPointer<GapLabelLookupTable>::New();
        result->labels->SetNumberOfTableValues(static_cast<vtkIdType>(source.regionVolumes.size() + 1));
        result->labels->IndexedLookupOn();
        vtkNew<vtkIntArray> annotationIds;
        vtkNew<vtkStringArray> annotationNames;
        annotationIds->InsertNextValue(0);
        annotationNames->InsertNextValue("background");
        result->labels->SetTableValue(0, 0, 0, 0, 0);
        result->labels->SetNanColor(0.5, 0.5, 0.5, 1);
        for (std::size_t index = 0; index < source.regionVolumes.size(); ++index) {
            const auto& region = source.regionVolumes[index];
            const auto rgba=AnalysisColorStyle::GetMappedColor(result->mappingRange,ramp,region.second);
            annotationIds->InsertNextValue(region.first);
            annotationNames->InsertNextValue(std::to_string(region.first));
            result->labels->SetTableValue(static_cast<vtkIdType>(index + 1),rgba.data());
        }
        result->labels->SetAnnotations(annotationIds, annotationNames);
        result->labels->Build();
        return result;
    }

    static std::shared_ptr<const GapDisplayData> Build(vtkImageData* image,
        const std::vector<VoidRegion>& regions, const std::atomic<bool>& isStopping,
        const RoiReadSnapshot& evaluationRoi={},const std::vector<VoidRegion>* parentRegions=nullptr,
        std::string* failureReason=nullptr)
    {
        if (failureReason) failureReason->clear();
        const auto fail = [&](const std::string& reason) -> std::shared_ptr<const GapDisplayData> {
            if (failureReason) *failureReason = reason;
            return {};
        };
        if (!image || image->GetScalarType() != VTK_INT
            || image->GetNumberOfScalarComponents() != 1) return {};
        constexpr std::size_t roiByteLimit = 64U * 1024U * 1024U;
        constexpr std::size_t meshByteLimit = 256U * 1024U * 1024U;
        auto result = std::make_shared<GapDisplayData>();
        std::array<double, 2> range{std::numeric_limits<double>::max(),
            std::numeric_limits<double>::lowest()};
        std::set<std::int32_t> ids;
        for (const auto& region : regions) {
            if (region.id <= 0 || !ids.insert(region.id).second
                || !std::isfinite(region.volumeMM3) || region.volumeMM3 < 0) return {};
            range[0] = std::min(range[0], double(region.volumeMM3));
            range[1] = std::max(range[1], double(region.volumeMM3));
        }
        result->hasRegions = !regions.empty();
        if (regions.empty()) range = {0, 0};
        result->resultRange=range;result->mappingRange=range;
        result->volumes = AnalysisColorStyle::BuildRamp(range, GetColorRamp(result->params));
        result->labels = vtkSmartPointer<GapLabelLookupTable>::New();
        result->labels->SetNumberOfTableValues(static_cast<vtkIdType>(regions.size() + 1));
        result->labels->IndexedLookupOn();
        vtkNew<vtkIntArray> annotationIds;
        vtkNew<vtkStringArray> annotationNames;
        annotationIds->InsertNextValue(0);
        annotationNames->InsertNextValue("background");
        result->labels->SetTableValue(0, 0, 0, 0, 0);
        result->labels->SetNanColor(0.5, 0.5, 0.5, 1);
        // 逐孔隙临时网格立即写入共享数组，不保留成千上万个 polydata。
        // 预算按合并后的点、连通性和标签字节计量，最后再核对实际分配。
        auto meshPoints = vtkSmartPointer<vtkPoints>::New();
        meshPoints->SetDataType(evaluationRoi ? VTK_DOUBLE : VTK_FLOAT);
        auto meshCells = vtkSmartPointer<vtkCellArray>::New();
        auto meshIds = vtkSmartPointer<vtkIntArray>::New();
        meshIds->SetName("gap.display.id");
        const auto meshFits = [&](vtkIdType points, vtkIdType cells) {
            constexpr auto cellBytes = 4 * sizeof(vtkIdType) + sizeof(int);
            const auto pointBytes = 3 * (evaluationRoi ? sizeof(double) : sizeof(float));
            if (points < 0 || cells < 0 || std::uint64_t(points) > meshByteLimit / pointBytes)
                return false;
            const auto remaining = meshByteLimit - std::size_t(points) * pointBytes;
            return remaining >= sizeof(vtkIdType)
                && std::uint64_t(cells) <= (remaining - sizeof(vtkIdType)) / cellBytes;
        };
        int sourceExtent[6]; image->GetExtent(sourceExtent);
        for (std::size_t index = 0; index < regions.size(); ++index) {
            if (isStopping.load()) return {};
            const auto& region = regions[index];
            result->regionVolumes.emplace_back(region.id, region.volumeMM3);
            double rgb[3]; result->volumes->GetColor(region.volumeMM3, rgb);
            annotationIds->InsertNextValue(region.id);
            annotationNames->InsertNextValue(std::to_string(region.id));
            result->labels->SetTableValue(static_cast<vtkIdType>(index + 1), rgb[0], rgb[1], rgb[2], 1);
            if (region.voxelCount == 0) continue;
            // 从完整父标签提面；评价区域只选择原表面的面片，不产生截断盖面。
            const VoidRegion* parent=&region;
            if(parentRegions){const auto found=std::find_if(parentRegions->begin(),parentRegions->end(),[&](const auto& p){return p.id==region.id;});if(found==parentRegions->end())return {};parent=&*found;}
            int extent[6]; std::size_t count = 1;
            for (int axis = 0; axis < 3; ++axis) {
                if (parent->bbox[2 * axis] < sourceExtent[2 * axis]
                    || parent->bbox[2 * axis + 1] > sourceExtent[2 * axis + 1]
                    || parent->bbox[2 * axis] > parent->bbox[2 * axis + 1]
                    || parent->bbox[2 * axis] == std::numeric_limits<int>::min()
                    || parent->bbox[2 * axis + 1] == std::numeric_limits<int>::max()) return {};
                extent[2 * axis] = parent->bbox[2 * axis] - 1;
                extent[2 * axis + 1] = parent->bbox[2 * axis + 1] + 1;
                const auto size = std::size_t(std::int64_t(extent[2 * axis + 1])
                    - extent[2 * axis] + 1);
                if (size > roiByteLimit / count) return fail("Region " + std::to_string(region.id) + " mask exceeds 64 MiB display budget.");
                count *= size;
            }
            auto mask = vtkSmartPointer<vtkImageData>::New();
            mask->CopyStructure(image); mask->SetExtent(extent);
            mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
            auto* values = static_cast<unsigned char*>(mask->GetScalarPointer());
            std::fill_n(values, count, static_cast<unsigned char>(0));
            for (int z = parent->bbox[4]; z <= parent->bbox[5]; ++z) {
                if (isStopping.load()) return {};
                for (int y = parent->bbox[2]; y <= parent->bbox[3]; ++y) {
                    auto* source = static_cast<const int*>(image->GetScalarPointer(parent->bbox[0], y, z));
                    auto* target = static_cast<unsigned char*>(mask->GetScalarPointer(parent->bbox[0], y, z));
                    for (int x = parent->bbox[0]; x <= parent->bbox[1]; ++x)
                        *target++ = *source++ == region.id ? 1 : 0;
                }
            }
            auto filter = vtkSmartPointer<vtkFlyingEdges3D>::New();
            vtkNew<vtkCallbackCommand> cancel;
            cancel->SetClientData(const_cast<std::atomic<bool>*>(&isStopping));
            cancel->SetCallback([](vtkObject* object,unsigned long,void* data,void*) {
                if(static_cast<const std::atomic<bool>*>(data)->load())
                    static_cast<vtkAlgorithm*>(object)->AbortExecuteOn();
            });
            filter->AddObserver(vtkCommand::ProgressEvent,cancel);
            filter->SetInputData(mask); filter->SetValue(0, 0.5);
            filter->ComputeNormalsOff(); filter->ComputeScalarsOff(); filter->Update();
            if (isStopping.load()) return {};
            auto poly = vtkSmartPointer<vtkPolyData>::New();
            poly->ShallowCopy(filter->GetOutput());
            if(evaluationRoi){
                auto points=vtkSmartPointer<vtkPoints>::New();points->SetDataTypeToDouble();
                auto cells=vtkSmartPointer<vtkCellArray>::New();
                vtkIdType count=0;const vtkIdType* ids=nullptr;auto* triangles=poly->GetPolys();triangles->InitTraversal();
                while(triangles->GetNextCell(count,ids)){
                    if(count!=3)return {};
                    std::array<std::array<double,3>,3> triangle{};for(int k=0;k<3;++k)poly->GetPoint(ids[k],triangle[k].data());
                    const auto selected=evaluationRoi->GetTriangleSelection(triangle,0,[&]{return isStopping.load();});
                    if(selected.error!=RoiError::None)return {};
                    for(const auto& piece:selected.triangles){
                        if (!meshFits(meshPoints->GetNumberOfPoints() + points->GetNumberOfPoints() + 3,
                            meshCells->GetNumberOfCells() + cells->GetNumberOfCells() + 1))
                            return fail("ROI display mesh exceeds 256 MiB budget.");
                        vtkIdType corners[3];for(int k=0;k<3;++k)corners[k]=points->InsertNextPoint(piece[k].data());cells->InsertNextCell(3,corners);
                    }
                }
                poly=vtkSmartPointer<vtkPolyData>::New();poly->SetPoints(points);poly->SetPolys(cells);
            }
            const auto pointOffset = meshPoints->GetNumberOfPoints();
            if (!meshFits(pointOffset + poly->GetNumberOfPoints(),
                    meshCells->GetNumberOfCells() + poly->GetNumberOfCells()))
                return fail("Merged display mesh exceeds 256 MiB budget at region " + std::to_string(region.id) + ".");
            for (vtkIdType point = 0; point < poly->GetNumberOfPoints(); ++point) {
                double position[3]; poly->GetPoint(point, position);
                meshPoints->InsertNextPoint(position);
            }
            vtkIdType cellSize = 0; const vtkIdType* corners = nullptr;
            auto* triangles = poly->GetPolys(); triangles->InitTraversal();
            while (triangles->GetNextCell(cellSize, corners)) {
                if (isStopping.load()) return {};
                if (cellSize != 3) return fail("Display surface contains a non-triangle cell.");
                const vtkIdType triangle[3]{corners[0] + pointOffset, corners[1] + pointOffset, corners[2] + pointOffset};
                meshCells->InsertNextCell(3, triangle);
                meshIds->InsertNextValue(region.id);
            }
        }
        result->labels->SetAnnotations(annotationIds, annotationNames);
        result->labels->Build();
        meshPoints->Squeeze(); meshCells->Squeeze(); meshIds->Squeeze();
        result->mesh = vtkSmartPointer<vtkPolyData>::New();
        result->mesh->SetPoints(meshPoints);
        result->mesh->SetPolys(meshCells);
        result->mesh->GetCellData()->SetScalars(meshIds);
        if (std::size_t(result->mesh->GetActualMemorySize()) * 1024 > meshByteLimit)
            return fail("Merged display mesh allocation exceeds 256 MiB budget.");
        return isStopping.load() ? nullptr : result;
    }
};
