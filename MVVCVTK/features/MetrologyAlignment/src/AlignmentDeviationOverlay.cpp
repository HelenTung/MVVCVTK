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
    if (!mesh || !mesh->data || mesh->data->self != result.sourceMesh || !mesh->mesh ||
        !GetDisplayValid(display,result) ||
        mesh->mesh->GetNumberOfPoints() != static_cast<vtkIdType>(result.forward->samples.size()))
        throw std::invalid_argument("Invalid deviation display input.");
    const auto count=result.forward->samples.size();
    const auto requiredBytes=GetRequiredBytes(count);
    auto output=std::make_shared<AlignmentDeviationRenderData>();
    output->source=std::move(mesh); output->field=result.forward; output->workingBytes=requiredBytes;
    auto colors=vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("metrology.deviation.color"); colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(static_cast<vtkIdType>(count));
    auto *buffer=colors->GetPointer(0);
    const AlignmentDeviationColors::Ramp ramp(display,result.parameters->tolerance);
    for (std::size_t i=0;i<count;++i) {
        const auto &sample=output->field->samples[i];
        const auto color=sample.state == AlignmentDeviationState::Valid
            ? ramp.ColorAt(sample.value) : AlignmentDeviationColors::invalidColor;
        std::copy(color.begin(),color.end(),buffer+3*i);
    }
    output->colored=vtkSmartPointer<vtkPolyData>::New();
    output->colored->ShallowCopy(output->source->mesh);
    output->colored->GetPointData()->SetScalars(colors);
    output->lookup=ramp.GetLookup();
    const char *unit=result.unit == AlignmentUnit::Millimeter ? "mm" : result.unit == AlignmentUnit::Meter ? "m" : "model unit";
    output->title=std::string(result.parameters->metric == AlignmentDeviationMetric::UnsignedDistance ? "Distance" : "Normal projection")+" ("+unit+")";
    if (display.selectedVertex) {
        auto points=vtkSmartPointer<vtkPoints>::New(); points->SetDataTypeToDouble();
        double point[3]; output->source->mesh->GetPoint(static_cast<vtkIdType>(*display.selectedVertex),point);
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
