#include "ThicknessOverlay.h"
#include "Render/Contracts/SlicePlaneState.h"
#include "Render/Support/AnalysisColorStyle.h"
#include "Render/Support/SliceContourPlane.h"
#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCellPicker.h>
#include <vtkCutter.h>
#include <vtkIdTypeArray.h>
#include <vtkLookupTable.h>
#include <vtkLegendBoxActor.h>
#include <vtkPlane.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkScalarBarActor.h>
#include <vtkTextProperty.h>
#include <vtkUnsignedCharArray.h>
#include <algorithm>
#include <cmath>

namespace {
void SetThicknessLegend(AnalysisLegend& legend, const char* title)
{
    AnalysisColorStyle::SetLegend(legend, title);
    // 完整结果标识需要足够的标题宽度，色带仍保持窄条，避免文字向左裁出视口。
    legend.SetWidth(.38);
    legend.SetMaximumWidthInPixels(220);
    legend.SetBarRatio(.06);
}

AnalysisColorStyle::RampMode GetRampMode(ThicknessColorMode mode)
{
    switch (mode) {
    case ThicknessColorMode::Constant: return AnalysisColorStyle::RampMode::Constant;
    case ThicknessColorMode::Gradient: return AnalysisColorStyle::RampMode::Gradient;
    case ThicknessColorMode::Rainbow: return AnalysisColorStyle::RampMode::Rainbow;
    case ThicknessColorMode::InverseRainbow: return AnalysisColorStyle::RampMode::InverseRainbow;
    case ThicknessColorMode::HueLoop: return AnalysisColorStyle::RampMode::HueLoop;
    default: return static_cast<AnalysisColorStyle::RampMode>(-1);
    }
}
AnalysisColorStyle::RampParams GetColorRamp(const ThicknessDisplay& display)
{
    const auto& band=display.colorBand;
    AnalysisColorStyle::RampParams params;params.mode=GetRampMode(band.mode);
    params.constantColor = band.constantColor; params.lowColor = band.lowColor;
    params.highColor = band.highColor; params.belowColor = band.belowColor;
    params.aboveColor = band.aboveColor;
    params.isReversed=display.colorBand.isReversed;
    params.opacityRange=display.opacityRange;
    params.blend=display.style==ThicknessDisplayStyle::Overlay || display.style==ThicknessDisplayStyle::Constant
        ? AnalysisColorStyle::BlendMode::Constant : display.style==ThicknessDisplayStyle::Inclined
        ? AnalysisColorStyle::BlendMode::Inclined : display.style==ThicknessDisplayStyle::InverseInclined
        ? AnalysisColorStyle::BlendMode::InverseInclined : static_cast<AnalysisColorStyle::BlendMode>(-1);
    for(const auto& segment:band.segments)
        params.segments.push_back({segment.lower,segment.upper,GetRampMode(segment.mode),segment.lowColor,segment.highColor});
    return params;
}
}

bool ThicknessOverlay::GetColorValid(const ThicknessDisplay& display) noexcept
{
    if(!AnalysisColorStyle::GetColorValid(display.toleranceBelowColor) || !AnalysisColorStyle::GetColorValid(display.toleranceWithinColor)
        || !AnalysisColorStyle::GetColorValid(display.toleranceAboveColor) || display.colorBand.segments.size()>1024 || !AnalysisColorStyle::GetLegendLayoutValid(display))return false;
    try { return AnalysisColorStyle::GetRampValid(GetColorRamp(display))
        && (display.rangeMode == ThicknessRangeMode::Manual
            || display.rangeMode == ThicknessRangeMode::Result
            || display.rangeMode == ThicknessRangeMode::Histogram);
    } catch (...) { return false; }
}

ThicknessDisplayData ThicknessOverlay::BuildData(const ThicknessData::Record &record,
                                                 const SurfaceMeshPayload &mesh,
                                                 const ThicknessDisplay &display,const ThicknessDisplayData* previous)
{
    auto ramp=GetColorRamp(display);
    AnalysisColorStyle::SetTailColors(ramp);
    if(display.mode==ThicknessDisplayMode::Tolerance) {
        ramp.tolerance=std::array<double,2>{record.archive.evaluation.lower,record.archive.evaluation.upper};
        ramp.toleranceBelowColor=display.toleranceBelowColor;ramp.toleranceAboveColor=display.toleranceAboveColor;
        ramp.toleranceColor=display.toleranceWithinColor;
        ramp.segments.clear();
    }
    if(previous && previous->range==display.range && AnalysisColorStyle::GetRampEqual(previous->ramp,ramp))return *previous;
    auto lookup=AnalysisColorStyle::BuildRamp(display.range,ramp);
    const bool hasAlpha=display.style==ThicknessDisplayStyle::Inclined || display.style==ThicknessDisplayStyle::InverseInclined;
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    auto cells = vtkSmartPointer<vtkCellArray>::New();
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("thickness.display");
    colors->SetNumberOfComponents(hasAlpha ? 4 : 3);
    auto ids = vtkSmartPointer<vtkIdTypeArray>::New();
    ids->SetName("thickness.sample");
    const auto &samples = *record.field.samples;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto &sample = samples[i];
        if (sample.validity == ThicknessValidity::OutsideEvaluation)
            continue;
        if(!previous) {
            vtkIdType triangle[3]{};
            for(std::size_t k=0;k<3;++k)triangle[k]=points->InsertNextPoint(ThicknessAlgorithm::GetSampleCorner(mesh,sample,k).data());
            cells->InsertNextCell(3,triangle);ids->InsertNextValue(static_cast<vtkIdType>(i));
        }
        unsigned char rgb[4]{128, 128, 128,255};
        if (sample.validity == ThicknessValidity::Valid)
        {
            const auto mapped=AnalysisColorStyle::GetMappedColor(display.range,ramp,sample.thickness);
            for(int k=0;k<3;++k)rgb[k]=static_cast<unsigned char>(std::lround(255*mapped[k]));
            if(hasAlpha)rgb[3]=static_cast<unsigned char>(std::lround(255*mapped[3]));
        }
        colors->InsertNextTypedTuple(rgb);
    }
    auto output = vtkSmartPointer<vtkPolyData>::New();
    if(previous)output->ShallowCopy(previous->mesh);
    else { output->SetPoints(points);output->SetPolys(cells);output->GetCellData()->AddArray(ids); }
    // Cell RGB 不在有效样本与无效零占位之间插值。
    output->GetCellData()->SetScalars(colors);
    return {output,lookup,ramp,display.range,record.statistics.validCount>0};
}
ThicknessOverlay::ThicknessOverlay(ThicknessDisplayData data, const ThicknessDisplay &display,
                                   ThicknessUnit unit, HostRenderViewRole role)
{
    m_unit=unit;m_resultLabel=data.resultLabel;
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_selectionActor = vtkSmartPointer<vtkActor>::New();
    m_selectionMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_selectionMapper->SetInputData(vtkSmartPointer<vtkPolyData>::New());
    m_legend = vtkSmartPointer<AnalysisLegend>::New();
    m_isSlice = role == HostRenderViewRole::TopDownSlice ||
                role == HostRenderViewRole::FrontBackSlice ||
                role == HostRenderViewRole::LeftRightSlice;
    if (m_isSlice)
    {
        const auto orientation = role == HostRenderViewRole::TopDownSlice ? Orientation::Top_down
                                 : role == HostRenderViewRole::FrontBackSlice
                                     ? Orientation::Front_back
                                     : Orientation::Left_right;
        m_normal = SlicePlaneState::Build(orientation, {}).worldNormal;
        m_plane = vtkSmartPointer<vtkPlane>::New();
        m_plane->SetNormal(m_normal.data());
        m_cutter = vtkSmartPointer<vtkCutter>::New();
        m_cutter->SetCutFunction(m_plane);
        m_cutter->SetInputData(data.mesh);
        m_cutter->GenerateTrianglesOff();
        m_mapper->SetInputConnection(m_cutter->GetOutputPort());
    }
    else
        m_mapper->SetInputData(data.mesh);
    m_mapper->ScalarVisibilityOn();
    m_mapper->SetScalarModeToUseCellData();
    m_mapper->SetColorModeToDirectScalars();
    m_mapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->LightingOff();
    m_actor->GetProperty()->SetOpacity(display.opacity);
    m_actor->GetProperty()->SetLineWidth(2);
    m_actor->SetVisibility(display.isVisible);
    if (m_isSlice) {
        auto contourMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        contourMapper->SetInputConnection(m_cutter->GetOutputPort());
        contourMapper->ScalarVisibilityOff();
        m_contourActor = vtkSmartPointer<vtkActor>::New();
        m_contourActor->SetMapper(contourMapper);
        m_contourActor->GetProperty()->SetColor(1, 1, 1);
        m_contourActor->GetProperty()->LightingOff();
        m_contourActor->GetProperty()->SetLineWidth(4);
        m_contourActor->PickableOff();
        m_contourActor->SetVisibility(display.isVisible);
        AttachProp(m_contourActor);
    }
    m_selectionActor->SetMapper(m_selectionMapper);
    m_selectionActor->PickableOff();
    m_selectionActor->GetProperty()->SetColor(1, 1, 1);
    m_selectionActor->GetProperty()->SetLineWidth(3);
    m_selectionActor->GetProperty()->SetPointSize(8);
    m_selectionActor->GetProperty()->RenderPointsAsSpheresOn();
    m_selectionMapper->SetResolveCoincidentTopologyToPolygonOffset();
    m_selectionMapper->SetRelativeCoincidentTopologyPointOffsetParameter(-4);
    m_selectionActor->GetProperty()->LightingOff();
    m_selectionActor->VisibilityOff();
    m_legend->SetLookupTable(data.lookup);
    const std::string title =
        std::string(display.mode == ThicknessDisplayMode::Tolerance ? u8"壁厚公差" : u8"壁厚") +
        " · "+data.resultLabel+(unit == ThicknessUnit::Millimeter ? " [mm]" : " [m]");
    SetThicknessLegend(*m_legend, title.c_str());
    m_legend->SetPosition(display.legendPosition[0],display.legendPosition[1]);
    m_legend->SetPosition(display.legendPosition[0],display.legendPosition[1]);
    m_legend->SetVisibility(display.isVisible && display.hasLegend);
    m_legend->SetMapping(data.range,data.ramp,data.hasValues);
    AttachProp(m_actor);AttachProp(m_selectionActor);AttachProp(m_legend);
}
void ThicknessOverlay::SetDisplayData(const ThicknessDisplayData& data,const ThicknessDisplay& display)
{
    SetInputData(data.mesh);m_actor->GetProperty()->SetOpacity(display.opacity);m_actor->SetVisibility(display.isVisible);
    if(m_contourActor)m_contourActor->SetVisibility(display.isVisible);
    SetThicknessLegend(*m_legend,(std::string(display.mode==ThicknessDisplayMode::Tolerance ? u8"壁厚公差":u8"壁厚")
        +" · "+m_resultLabel+(m_unit==ThicknessUnit::Millimeter ? " [mm]":" [m]")).c_str());
    m_legend->SetLookupTable(data.lookup);m_legend->SetMapping(data.range,data.ramp,data.hasValues);
    m_legend->SetPosition(display.legendPosition[0],display.legendPosition[1]);
    m_legend->SetVisibility(display.isVisible && display.hasLegend);

}
void ThicknessOverlay::SetInputData(vtkSmartPointer<vtkDataObject> data)
{
    auto *mesh = vtkPolyData::SafeDownCast(data);
    if (!mesh)
        return;
    auto* previous = m_cutter ? vtkPolyData::SafeDownCast(m_cutter->GetInput()) : m_mapper->GetInput();
    if(!previous || mesh->GetPoints()!=previous->GetPoints() || mesh->GetPolys()!=previous->GetPolys())m_selectionActor->VisibilityOff();
    if (m_cutter)
        m_cutter->SetInputData(mesh);
    else
        m_mapper->SetInputData(mesh);
}
void ThicknessOverlay::SetOverlayState(const FeatureOverlayState &state)
{
    if (m_plane)
    {
        (void)SliceContourPlane::SetPlane(*m_plane,state.cursor,m_normal,state.modelToWorld);
    }
    Set3DPropsTransform(state.modelToWorld);
}
void ThicknessOverlay::SetSelection(const ThicknessSample *sample)
{
    if (!sample || sample->validity != ThicknessValidity::Valid)
    {
        m_selectionActor->VisibilityOff();
        return;
    }
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    points->InsertNextPoint(sample->source.data());
    // 插值值没有唯一对应的双端；只标记查询位置。
    auto vertices = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType id = 0;
    vertices->InsertNextCell(1, &id);
    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetVerts(vertices);
    m_selectionMapper->SetInputData(data);
    m_selectionActor->SetVisibility(m_actor->GetVisibility());
}
std::optional<std::size_t> ThicknessOverlay::GetPickedSample(int x, int y, vtkRenderer *renderer)
{
    if (!renderer || !m_actor->GetVisibility())
        return {};
    auto picker = vtkSmartPointer<vtkCellPicker>::New();
    // 切线是一维屏幕目标；容许像素取整误差，不扩大三维面拾取范围。
    picker->SetTolerance(m_isSlice ? 0.005 : 0.0005);
    picker->PickFromListOn();
    picker->AddPickList(m_actor);
    if (!picker->Pick(x, y, 0, renderer) || picker->GetCellId() < 0)
        return {};
    auto *data = vtkPolyData::SafeDownCast(picker->GetDataSet());
    auto *ids =
        data ? vtkIdTypeArray::SafeDownCast(data->GetCellData()->GetArray("thickness.sample"))
             : nullptr;
    if (!ids || picker->GetCellId() >= ids->GetNumberOfTuples())
        return {};
    const auto value = ids->GetValue(picker->GetCellId());
    return value >= 0 ? std::optional<std::size_t>(static_cast<std::size_t>(value)) : std::nullopt;
}
