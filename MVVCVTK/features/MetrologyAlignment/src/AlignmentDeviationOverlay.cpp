#include "AlignmentDeviationOverlay.h"
#include "AlignmentDeviationColors.h"
#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDoubleArray.h>
#include <vtkLookupTable.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkScalarBarActor.h>
#include <vtkUnsignedCharArray.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>



bool AlignmentDeviationOverlay::GetDisplayValid(const AlignmentDeviationDisplay &d,
                                                const AlignmentDeviationSnapshot &r) {
    return AlignmentDeviationColors::GetDisplayValid(d,r);
}
std::size_t AlignmentDeviationOverlay::GetRequiredBytes(std::size_t count) {
    constexpr std::size_t fixedBytes=65536;
    if (count>(std::numeric_limits<std::size_t>::max()-fixedBytes)/3)
        throw std::length_error("Deviation display size overflows.");
    return fixedBytes+3*count;
}
std::shared_ptr<const AlignmentDeviationRenderData> AlignmentDeviationOverlay::Build(
    VtkSurfaceMeshSnapshot mesh, const AlignmentDeviationSnapshot &result,
    const AlignmentDeviationDisplay &display,const AlignmentDeviationRenderData* previous,std::optional<vtkSmartPointer<vtkPolyData>> previousMarker) {
    if(!result.forward || !result.parameters)throw std::invalid_argument("Missing deviation result fields.");
    const auto actualRange=previous ? previous->actualRange:AlignmentDeviationColors::GetEffectiveRange(*result.forward);
    const auto resolved=AlignmentDeviationColors::GetResolvedDisplay(display,result,&actualRange);
    if (!mesh || !mesh->data || mesh->data->self != result.sourceMesh || !mesh->mesh ||
        !resolved ||
        (mesh->mesh->GetNumberOfPoints() != static_cast<vtkIdType>(result.forward->originalVertexCount)
         || result.forward->samples.size()<result.forward->originalVertexCount))
        throw std::invalid_argument("Invalid deviation display input.");
    std::vector<std::size_t> sampleIndices;
    vtkSmartPointer<vtkPolyData> selected;
    if(!previous && result.parameters->evaluationRoi){
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> triangles;
        for(std::size_t i=result.forward->originalVertexCount;i<result.forward->samples.size();++i){const auto& sample=result.forward->samples[i];if(!sample.sourceTriangle)continue;
            vtkIdType ids[3];for(int j=0;j<3;++j){ids[j]=points->InsertNextPoint((*sample.sourceTriangle)[j].data());sampleIndices.push_back(i);}triangles->InsertNextCell(3,ids);}
        selected=vtkSmartPointer<vtkPolyData>::New();selected->SetPoints(points);selected->SetPolys(triangles);
    }
    const auto indices=previous ? previous->sampleIndices:result.parameters->evaluationRoi.has_value()
        ? std::make_shared<const std::vector<std::size_t>>(std::move(sampleIndices)):nullptr;
    const auto count=indices ? indices->size():result.forward->originalVertexCount;
    const auto requiredBytes=GetRequiredBytes(count);
    auto output=std::make_shared<AlignmentDeviationRenderData>();
    output->source=std::move(mesh);output->field=result.forward;output->workingBytes=requiredBytes;
    output->sampleIndices=indices;output->display=*resolved;
    output->actualRange=actualRange;output->hasValues=actualRange.has_value();
    auto colors=vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("metrology.deviation.color"); colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(static_cast<vtkIdType>(count));
    auto *buffer=colors->GetPointer(0);
    const AlignmentDeviationColors::Ramp ramp(*resolved,result.parameters->tolerance);
    bool hasInvalid=false;
    for (std::size_t i=0;i<count;++i) {
        const auto &sample=output->field->samples[indices ? (*indices)[i]:i];
        hasInvalid=hasInvalid || sample.state!=AlignmentDeviationState::Valid || !std::isfinite(sample.value);
        const auto color=sample.state == AlignmentDeviationState::Valid
            ? ramp.ColorAt(sample.value) : AlignmentDeviationColors::invalidColor;
        std::copy(color.begin(),color.end(),buffer+3*i);
    }
    output->colored=vtkSmartPointer<vtkPolyData>::New();
    output->colored->ShallowCopy(previous ? previous->colored:selected ? selected:output->source->mesh);
    output->colored->GetPointData()->SetScalars(colors);
    output->surface=vtkSmartPointer<vtkPolyData>::New();output->surface->ShallowCopy(output->colored);
    output->usesCellColors=display.useToleranceColors || !display.segments.empty() || hasInvalid;
    if(output->usesCellColors) {
        // 仅取既有输出样本的类别，不新增测量值：面片使用其最小有效样本的颜色，未测面片灰。
        // 独立散点显示每个输出样本的精确类别，避免 RGB 插值制造不存在的分类。
        auto cells=vtkSmartPointer<vtkUnsignedCharArray>::New();cells->SetNumberOfComponents(3);cells->SetName("metrology.deviation.cell.color");
        cells->SetNumberOfTuples(output->surface->GetNumberOfCells());
        auto* topology=output->surface->GetPolys();topology->InitTraversal();vtkIdType pointCount=0,index=0;const vtkIdType* ids=nullptr;
        while(topology->GetNextCell(pointCount,ids)) {
            if(pointCount!=3)throw std::invalid_argument("Deviation overlay requires triangular cells.");
            bool valid=true;double minimum=std::numeric_limits<double>::infinity();
            for(vtkIdType j=0;j<pointCount;++j) {
                if(ids[j]<0 || static_cast<std::size_t>(ids[j])>=count)throw std::invalid_argument("Invalid deviation triangle index.");
                const auto point=static_cast<std::size_t>(ids[j]);const auto& sample=result.forward->samples[indices ? (*indices)[point]:point];
                valid=valid && sample.state==AlignmentDeviationState::Valid && std::isfinite(sample.value);minimum=std::min(minimum,sample.value);
            }
            const auto color=valid ? ramp.ColorAt(minimum):AlignmentDeviationColors::invalidColor;cells->SetTypedTuple(index++,color.data());
        }
        if(index!=output->surface->GetNumberOfCells())throw std::invalid_argument("Deviation overlay contains non-triangular primitives.");
        output->surface->GetCellData()->SetScalars(cells);
    } else {
        // 连续显示只在渲染端对输出标量插值；不回写测量场，不对 RGB 解释物理值。
        auto values=vtkSmartPointer<vtkDoubleArray>::New();values->SetName("metrology.deviation.value");values->SetNumberOfValues(static_cast<vtkIdType>(count));
        for(std::size_t i=0;i<count;++i)values->SetValue(static_cast<vtkIdType>(i),result.forward->samples[indices ? (*indices)[i]:i].value);
        output->surface->GetPointData()->SetScalars(values);
    }
    output->samplePoints=vtkSmartPointer<vtkPolyData>::New();
    if(previous && previous->samplePoints)output->samplePoints->ShallowCopy(previous->samplePoints);
    else {
        output->samplePoints->SetPoints(output->colored->GetPoints());auto vertices=vtkSmartPointer<vtkCellArray>::New();
        for(vtkIdType i=0;i<static_cast<vtkIdType>(count);++i)vertices->InsertNextCell(1,&i);
        output->samplePoints->SetVerts(vertices);
    }
    output->samplePoints->GetPointData()->SetScalars(colors);
    output->lookup=ramp.GetLookup();output->ramp=ramp.GetParameters();
    const char *unit=result.unit == AlignmentUnit::Millimeter ? "mm" : result.unit == AlignmentUnit::Meter ? "m" : "model unit";
    output->title=std::string(u8"计量 · ")+AnalysisColorStyle::GetResultLabel(result.result)+"\n"+ (result.parameters->metric == AlignmentDeviationMetric::UnsignedDistance ? u8"距离":u8"法向偏差")+" ("+unit+")";
    output->marker=previousMarker ? *previousMarker:BuildMarker(output->source,result,display.selectedVertex);
    // 诊断记录显示缓存实际保留的数组容量；共享的原始网格与测量场不重复计入。
    output->workingBytes=65536;
    const auto addBytes=[&](std::size_t bytes) {
        if(bytes>std::numeric_limits<std::size_t>::max()-output->workingBytes)
            throw std::length_error("Deviation display size overflows.");
        output->workingBytes+=bytes;
    };
    const auto addMemory=[&](auto* array) { if(array)addBytes(static_cast<std::size_t>(array->GetActualMemorySize())*1024); };
    addMemory(colors.GetPointer());
    addMemory(output->usesCellColors ? output->surface->GetCellData()->GetScalars():output->surface->GetPointData()->GetScalars());
    addMemory(output->samplePoints->GetVerts());
    if(indices) {
        addBytes(indices->capacity()*sizeof(std::size_t));
        addMemory(output->colored->GetPoints());addMemory(output->colored->GetPolys());
    }
    addMemory(output->marker.GetPointer());
    return output;
}
vtkSmartPointer<vtkPolyData> AlignmentDeviationOverlay::BuildMarker(VtkSurfaceMeshSnapshot mesh,
    const AlignmentDeviationSnapshot& result,std::optional<std::uint64_t> vertex) {
    if(!vertex)return {};
    auto points=vtkSmartPointer<vtkPoints>::New();points->SetDataTypeToDouble();
    double point[3];const auto& sample=result.forward->samples[*vertex];
    if(sample.sourcePosition)std::copy(sample.sourcePosition->begin(),sample.sourcePosition->end(),point);
    else mesh->mesh->GetPoint(static_cast<vtkIdType>(*vertex),point);
    points->InsertNextPoint(point);auto vertices=vtkSmartPointer<vtkCellArray>::New();const vtkIdType id=0;vertices->InsertNextCell(1,&id);
    auto marker=vtkSmartPointer<vtkPolyData>::New();marker->SetPoints(points);marker->SetVerts(vertices);return marker;
}
AlignmentDeviationOverlay::AlignmentDeviationOverlay(std::shared_ptr<const AlignmentDeviationRenderData> data) {
    m_mapper=vtkSmartPointer<vtkPolyDataMapper>::New();m_mapper->SetScalarModeToUsePointData();
    m_mapper->SetColorModeToDirectScalars();m_mapper->ScalarVisibilityOn();m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor=vtkSmartPointer<vtkActor>::New();m_actor->SetMapper(m_mapper);m_actor->SetPickable(false);
    m_actor->GetProperty()->SetLighting(false);AttachProp(m_actor);
    m_sampleMapper=vtkSmartPointer<vtkPolyDataMapper>::New();m_sampleMapper->SetColorModeToDirectScalars();m_sampleMapper->SetScalarModeToUsePointData();
    m_sampleMapper->SetResolveCoincidentTopologyToPolygonOffset();m_sampleMapper->SetRelativeCoincidentTopologyPointOffsetParameter(-4);
    m_sampleActor=vtkSmartPointer<vtkActor>::New();m_sampleActor->SetMapper(m_sampleMapper);m_sampleActor->PickableOff();
    m_sampleActor->GetProperty()->LightingOff();m_sampleActor->GetProperty()->SetPointSize(3);AttachProp(m_sampleActor);
    m_legend=vtkSmartPointer<AnalysisLegend>::New();AttachProp(m_legend);
    m_markerMapper=vtkSmartPointer<vtkPolyDataMapper>::New();m_markerActor=vtkSmartPointer<vtkActor>::New();
    m_markerActor->SetMapper(m_markerMapper);m_markerActor->SetPickable(false);m_markerActor->GetProperty()->SetColor(1,1,0);
    m_markerActor->GetProperty()->SetPointSize(12);m_markerActor->GetProperty()->RenderPointsAsSpheresOn();
    m_markerActor->GetProperty()->SetLighting(false);AttachProp(m_markerActor);
    SetProduct(std::move(data));
}
void AlignmentDeviationOverlay::SetProduct(std::shared_ptr<const AlignmentDeviationRenderData> data) {
    if(!data || !data->colored || !data->lookup)throw std::invalid_argument("Missing deviation render data.");
    m_data=std::move(data);m_mapper->SetInputData(m_data->surface);m_mapper->SetLookupTable(m_data->lookup);m_mapper->UseLookupTableScalarRangeOn();
    if(m_data->usesCellColors){m_mapper->SetScalarModeToUseCellData();m_mapper->SetColorModeToDirectScalars();m_mapper->InterpolateScalarsBeforeMappingOff();}
    else {m_mapper->SetScalarModeToUsePointData();m_mapper->SetColorModeToMapScalars();m_mapper->InterpolateScalarsBeforeMappingOn();}
    m_sampleMapper->SetInputData(m_data->samplePoints);m_legend->SetLookupTable(m_data->lookup);
    AnalysisColorStyle::SetLegend(*m_legend,m_data->title.c_str());
    m_legend->SetPosition(m_data->display.legendPosition[0],m_data->display.legendPosition[1]);m_legend->SetMapping(m_data->display.range,m_data->ramp,m_data->hasValues);
    SetSelection(m_data->marker);SetPresentation(m_data->display);
}
void AlignmentDeviationOverlay::SetSelection(vtkSmartPointer<vtkPolyData> marker) {
    m_hasMarker=marker!=nullptr;m_markerMapper->SetInputData(marker ? marker:vtkSmartPointer<vtkPolyData>::New());m_markerActor->SetVisibility(m_hasMarker && m_actor->GetVisibility());
}
void AlignmentDeviationOverlay::SetPresentation(const AlignmentDeviationDisplay& display) {
    m_sampleActor->GetProperty()->SetOpacity(display.opacity);m_sampleActor->SetVisibility(display.isVisible && m_data->usesCellColors);
    m_actor->GetProperty()->SetOpacity(display.opacity);m_actor->SetVisibility(display.isVisible);
    m_legend->SetPosition(display.legendPosition[0],display.legendPosition[1]);
    m_legend->SetVisibility(display.isVisible && display.hasLegend);
    m_markerActor->SetVisibility(display.isVisible && m_hasMarker);
}
void AlignmentDeviationOverlay::SetInputData(vtkSmartPointer<vtkDataObject>) {
    // 输入由已校验的不可变 render product 绑定，禁止外部替换成另一网格而沿用颜色。
}
void AlignmentDeviationOverlay::SetOverlayState(const FeatureOverlayState &state) {
    Set3DPropsTransform(state.modelToWorld);
}
