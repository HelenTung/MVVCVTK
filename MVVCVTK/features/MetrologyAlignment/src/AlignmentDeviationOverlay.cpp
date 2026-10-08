#include "AlignmentDeviationOverlay.h"
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

namespace {
using Color = std::array<unsigned char,3>;
Color Mix(Color a, Color b, double fraction) {
    Color result{}; fraction = std::clamp(fraction,0.0,1.0);
    for (std::size_t i=0;i<3;++i) result[i]=static_cast<unsigned char>(std::lround(a[i]+(b[i]-a[i])*fraction));
    return result;
}
Color ColorAt(double value, const AlignmentDeviationDisplay &display,
              const std::optional<std::array<double,2>> &tolerance) {
    const double low=display.range[0], high=display.range[1];
    if (display.useToleranceColors && tolerance) {
        if (value >= (*tolerance)[0] && value <= (*tolerance)[1]) return {60,180,95};
        if (value < (*tolerance)[0]) return Mix({45,90,215},{150,205,255},
            (*tolerance)[0] > low ? (value-low)/((*tolerance)[0]-low) : 0);
        return Mix({255,200,130},{210,45,35},high > (*tolerance)[1] ? (value-(*tolerance)[1])/(high-(*tolerance)[1]) : 1);
    }
    const double center=std::clamp(0.0,low,high);
    if (value < center) return Mix({45,90,215},{235,235,235},center > low ? (value-low)/(center-low) : 0);
    return Mix({235,235,235},{210,45,35},high > center ? (value-center)/(high-center) : 1);
}
void CheckDeadline(std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Deviation display deadline reached.");
}
} // namespace

bool AlignmentDeviationOverlay::GetDisplayValid(const AlignmentDeviationDisplay &d,
                                                const AlignmentDeviationSnapshot &r) {
    if (!r.forward || !r.parameters || !GetDataRevisionRefValid(d.result) || d.result != r.result ||
        !std::isfinite(d.range[0]) || !std::isfinite(d.range[1]) || d.range[0] >= d.range[1] ||
        !std::isfinite(d.range[1]-d.range[0]) ||
        (r.parameters->metric == AlignmentDeviationMetric::UnsignedDistance && d.range[0] < 0) ||
        (d.selectedVertex && *d.selectedVertex >= r.forward->samples.size())) return false;
    return !d.useToleranceColors || !r.parameters->tolerance ||
        (d.range[0] <= (*r.parameters->tolerance)[0] && d.range[1] >= (*r.parameters->tolerance)[1]);
}
std::size_t AlignmentDeviationOverlay::GetRequiredBytes(std::size_t count) {
    constexpr std::size_t fixedBytes=65536;
    if (count>(std::numeric_limits<std::size_t>::max()-fixedBytes)/3)
        throw std::length_error("Deviation display size overflows.");
    return fixedBytes+3*count;
}
std::shared_ptr<const AlignmentDeviationRenderData> AlignmentDeviationOverlay::Build(
    VtkSurfaceMeshSnapshot mesh, const AlignmentDeviationSnapshot &result,
    const AlignmentDeviationDisplay &display, std::size_t availableBytes,
    std::chrono::steady_clock::time_point deadline) {
    CheckDeadline(deadline);
    if (!mesh || !mesh->data || mesh->data->self != result.sourceMesh || !mesh->mesh ||
        !GetDisplayValid(display,result) ||
        mesh->mesh->GetNumberOfPoints() != static_cast<vtkIdType>(result.forward->samples.size()))
        throw std::invalid_argument("Invalid deviation display input.");
    const auto count=result.forward->samples.size();
    const auto requiredBytes=GetRequiredBytes(count);
    if (requiredBytes>availableBytes)
        throw std::length_error("Deviation color arrays exceed display working budget.");
    auto output=std::make_shared<AlignmentDeviationRenderData>();
    output->source=std::move(mesh); output->field=result.forward; output->workingBytes=requiredBytes;
    auto colors=vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("metrology.deviation.color"); colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(static_cast<vtkIdType>(count));
    auto *buffer=colors->GetPointer(0);
    for (std::size_t i=0;i<count;++i) {
        if (i%256==0) CheckDeadline(deadline);
        const auto &sample=output->field->samples[i];
        const auto color=sample.state == AlignmentDeviationState::Valid
            ? ColorAt(sample.value,display,result.parameters->tolerance) : Color{128,128,128};
        std::copy(color.begin(),color.end(),buffer+3*i);
    }
    output->colored=vtkSmartPointer<vtkPolyData>::New();
    output->colored->ShallowCopy(output->source->mesh);
    output->colored->GetPointData()->SetScalars(colors);
    output->lookup=vtkSmartPointer<vtkLookupTable>::New();
    output->lookup->SetRange(display.range.data()); output->lookup->SetNumberOfTableValues(257);
    output->lookup->SetNanColor(.5,.5,.5,1); output->lookup->Build();
    for (int i=0;i<=256;++i) {
        const auto color=ColorAt(display.range[0]+(display.range[1]-display.range[0])*i/256.0,display,result.parameters->tolerance);
        output->lookup->SetTableValue(i,color[0]/255.0,color[1]/255.0,color[2]/255.0,1);
    }
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
    legend->SetTitle(m_data->title.c_str()); legend->SetNumberOfLabels(5); legend->SetMaximumNumberOfColors(257);
    legend->SetNanAnnotation("invalid / unmatched"); legend->DrawNanAnnotationOn();
    legend->SetPosition(.82,.1); legend->SetWidth(.16); legend->SetHeight(.8); AttachProp(legend);
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
