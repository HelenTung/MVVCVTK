#include "AlignmentDeviationOverlay.h"
#include "AlignmentDeviationColors.h"
#include <vtkActor.h>
#include <vtkCellArray.h>
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
    const AlignmentDeviationDisplay &display) {
    const auto resolved=AlignmentDeviationColors::GetResolvedDisplay(display,result);
    if (!mesh || !mesh->data || mesh->data->self != result.sourceMesh || !mesh->mesh ||
        !resolved ||
        mesh->mesh->GetNumberOfPoints() != static_cast<vtkIdType>(result.forward->originalVertexCount))
        throw std::invalid_argument("Invalid deviation display input.");
    std::vector<std::size_t> sampleIndices;
    vtkSmartPointer<vtkPolyData> selected;
    if(result.parameters->evaluationRoi){
        vtkNew<vtkPoints> points;points->SetDataTypeToDouble();vtkNew<vtkCellArray> triangles;
        for(std::size_t i=result.forward->originalVertexCount;i<result.forward->samples.size();++i){const auto& sample=result.forward->samples[i];if(!sample.sourceTriangle)continue;
            vtkIdType ids[3];for(int j=0;j<3;++j){ids[j]=points->InsertNextPoint((*sample.sourceTriangle)[j].data());sampleIndices.push_back(i);}triangles->InsertNextCell(3,ids);}
        selected=vtkSmartPointer<vtkPolyData>::New();selected->SetPoints(points);selected->SetPolys(triangles);
    }else for(std::size_t i=0;i<result.forward->samples.size();++i)sampleIndices.push_back(i);
    const auto count=sampleIndices.size();
    const auto requiredBytes=GetRequiredBytes(count);
    auto output=std::make_shared<AlignmentDeviationRenderData>();
    output->source=std::move(mesh); output->field=result.forward; output->workingBytes=requiredBytes;
    auto colors=vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("metrology.deviation.color"); colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(static_cast<vtkIdType>(count));
    auto *buffer=colors->GetPointer(0);
    const AlignmentDeviationColors::Ramp ramp(*resolved,result.parameters->tolerance);
    for (std::size_t i=0;i<count;++i) {
        const auto &sample=output->field->samples[sampleIndices[i]];
        const auto color=sample.state == AlignmentDeviationState::Valid
            ? ramp.ColorAt(sample.value) : AlignmentDeviationColors::invalidColor;
        std::copy(color.begin(),color.end(),buffer+3*i);
    }
    output->colored=vtkSmartPointer<vtkPolyData>::New();
    output->colored->ShallowCopy(selected ? selected:output->source->mesh);
    output->colored->GetPointData()->SetScalars(colors);
    output->lookup=ramp.GetLookup();
    const char *unit=result.unit == AlignmentUnit::Millimeter ? "mm" : result.unit == AlignmentUnit::Meter ? "m" : "model unit";
    output->title=std::string(result.parameters->metric == AlignmentDeviationMetric::UnsignedDistance ? "Distance" : "Normal projection")+" ("+unit+")";
    if (display.selectedVertex) {
        auto points=vtkSmartPointer<vtkPoints>::New(); points->SetDataTypeToDouble();
        double point[3];const auto& sample=result.forward->samples[*display.selectedVertex];
        if(sample.sourcePosition)std::copy(sample.sourcePosition->begin(),sample.sourcePosition->end(),point);else output->source->mesh->GetPoint(static_cast<vtkIdType>(*display.selectedVertex),point);
        points->InsertNextPoint(point);
        auto vertices=vtkSmartPointer<vtkCellArray>::New(); const vtkIdType id=0; vertices->InsertNextCell(1,&id);
        output->marker=vtkSmartPointer<vtkPolyData>::New(); output->marker->SetPoints(points); output->marker->SetVerts(vertices);
    }
    return output;
}
AlignmentDeviationOverlay::AlignmentDeviationOverlay(std::shared_ptr<const AlignmentDeviationRenderData> data)
    : m_data(std::move(data)) {
    if (!m_data || !m_data->colored || !m_data->lookup) throw std::invalid_argument("Missing deviation render data.");
    auto mapper=vtkSmartPointer<vtkPolyDataMapper>::New(); mapper->SetInputData(m_data->colored);
    mapper->SetScalarModeToUsePointData(); mapper->SetColorModeToDirectScalars(); mapper->ScalarVisibilityOn();
    mapper->SetResolveCoincidentTopologyToPolygonOffset();
    auto actor=vtkSmartPointer<vtkActor>::New(); actor->SetMapper(mapper); actor->SetPickable(false);
    actor->GetProperty()->SetLighting(false); AttachProp(actor);
    auto legend=vtkSmartPointer<vtkScalarBarActor>::New(); legend->SetLookupTable(m_data->lookup);
    AnalysisColorStyle::SetLegend(*legend,m_data->title.c_str());
    // 长计量/单位标题按色条中心排版，左侧保留文字余量。
    legend->SetPosition(.15,.2);
    legend->SetNanAnnotation("invalid / unmatched"); legend->DrawNanAnnotationOn();
    AttachProp(legend);
    if (m_data->marker) {
        auto markerMapper=vtkSmartPointer<vtkPolyDataMapper>::New(); markerMapper->SetInputData(m_data->marker);
        auto marker=vtkSmartPointer<vtkActor>::New(); marker->SetMapper(markerMapper); marker->SetPickable(false);
        marker->GetProperty()->SetColor(1,1,0); marker->GetProperty()->SetPointSize(12);
        marker->GetProperty()->RenderPointsAsSpheresOn(); marker->GetProperty()->SetLighting(false); AttachProp(marker);
    }
}
void AlignmentDeviationOverlay::SetInputData(vtkSmartPointer<vtkDataObject>) {
    // 输入由已校验的不可变 render product 绑定，禁止外部替换成另一网格而沿用颜色。
}
void AlignmentDeviationOverlay::SetOverlayState(const FeatureOverlayState &state) {
    Set3DPropsTransform(state.modelToWorld);
}
