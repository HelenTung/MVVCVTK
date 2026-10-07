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
constexpr std::array<unsigned char, 3> belowColor{208, 88, 89};
constexpr std::array<unsigned char, 3> withinColor{97, 179, 113};
constexpr std::array<unsigned char, 3> aboveColor{83, 114, 188};

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
    if(display.colorBand.segments.size()>1024)return false;
    try { return AnalysisColorStyle::GetRampValid(GetColorRamp(display))
        && (display.rangeMode == ThicknessRangeMode::Manual
            || display.rangeMode == ThicknessRangeMode::Result
            || display.rangeMode == ThicknessRangeMode::Histogram);
    } catch (...) { return false; }
}

ThicknessDisplayData ThicknessOverlay::BuildData(const ThicknessData::Record &record,
                                                 const SurfaceMeshPayload &mesh,
                                                 const ThicknessDisplay &display)
{
    const auto ramp=GetColorRamp(display);
    auto lookup = AnalysisColorStyle::BuildRamp(display.range,ramp);
    const bool hasAlpha=display.style==ThicknessDisplayStyle::Inclined || display.style==ThicknessDisplayStyle::InverseInclined;
    for (int i = 0; i < 256; ++i)
    {
        const double f = double(i) / 255;
        const double value = display.range[0] + f * (display.range[1] - display.range[0]);
        if (display.mode == ThicknessDisplayMode::Tolerance)
        {
            const auto& color = value < record.archive.evaluation.lower ? belowColor
                : value > record.archive.evaluation.upper ? aboveColor : withinColor;
            lookup->SetTableValue(i, color[0] / 255.0, color[1] / 255.0, color[2] / 255.0, lookup->GetOpacity(value));
        }
    }
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
        vtkIdType triangle[3]{};
        for (std::size_t k = 0; k < 3; ++k)
        {
            const auto p = ThicknessAlgorithm::GetSampleCorner(mesh, sample, k);
            triangle[k] = points->InsertNextPoint(p.data());
        }
        cells->InsertNextCell(3, triangle);
        ids->InsertNextValue(static_cast<vtkIdType>(i));
        unsigned char rgb[4]{128, 128, 128,255};
        if (sample.validity == ThicknessValidity::Valid)
        {
            const auto mapped=AnalysisColorStyle::GetMappedColor(*lookup,ramp,sample.thickness);
            if (display.mode == ThicknessDisplayMode::Tolerance)
            {
                const auto& color = sample.thickness < record.archive.evaluation.lower ? belowColor
                    : sample.thickness > record.archive.evaluation.upper ? aboveColor : withinColor;
                std::copy(color.begin(), color.end(), rgb);
            }
            else
            {
                for (int k = 0; k < 3; ++k)
                    rgb[k] = static_cast<unsigned char>(std::lround(255 * mapped[k]));
            }
            if(hasAlpha)rgb[3]=static_cast<unsigned char>(std::lround(255*mapped[3]));
        }
        colors->InsertNextTypedTuple(rgb);
    }
    auto output = vtkSmartPointer<vtkPolyData>::New();
    output->SetPoints(points);
    output->SetPolys(cells);
    // Cell RGB 不在有效样本与无效零占位之间插值。
    output->GetCellData()->SetScalars(colors);
    output->GetCellData()->AddArray(ids);
    return {output, lookup};
}
ThicknessOverlay::ThicknessOverlay(ThicknessDisplayData data, const ThicknessDisplay &display,
                                   ThicknessUnit unit, HostRenderViewRole role)
{
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_selectionActor = vtkSmartPointer<vtkActor>::New();
    m_selectionMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_legend = vtkSmartPointer<vtkScalarBarActor>::New();
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
        (unit == ThicknessUnit::Millimeter ? " [mm]" : " [m]");
    AnalysisColorStyle::SetLegend(*m_legend, title.c_str());
    m_legend->SetVisibility(display.isVisible && display.hasLegend);
    m_invalidLegend = vtkSmartPointer<vtkLegendBoxActor>::New();
    auto symbolPoints = vtkSmartPointer<vtkPoints>::New();
    symbolPoints->InsertNextPoint(0, 0, 0);
    symbolPoints->InsertNextPoint(1, 0, 0);
    symbolPoints->InsertNextPoint(0.5, 1, 0);
    auto symbolCells = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType symbolIds[3]{0, 1, 2};
    symbolCells->InsertNextCell(3, symbolIds);
    auto symbol = vtkSmartPointer<vtkPolyData>::New();
    symbol->SetPoints(symbolPoints);
    symbol->SetPolys(symbolCells);
    auto symbolColor = vtkSmartPointer<vtkUnsignedCharArray>::New();
    symbolColor->SetNumberOfComponents(3);
    const unsigned char grey[3]{128, 128, 128};
    symbolColor->InsertNextTypedTuple(grey);
    symbol->GetCellData()->SetScalars(symbolColor);
    double textColor[3]{1, 1, 1};
    m_invalidLegend->SetNumberOfEntries(1);
    m_invalidLegend->SetEntry(0, symbol, u8"无效/未测", textColor);
    m_invalidLegend->ScalarVisibilityOn();
    // 与左下方向轴和右下标尺分开，限制自动排版后的文字高度。
    m_invalidLegend->SetPosition(0.25, 0.06);
    m_invalidLegend->SetPosition2(0.26, 0.04);
    m_invalidLegend->SetPadding(0);
    m_invalidLegend->GetEntryTextProperty()->SetFontSize(12);
    RenderTextStyle::SetFont(*m_invalidLegend->GetEntryTextProperty());
    m_invalidLegend->GetEntryTextProperty()->ItalicOff();
    m_invalidLegend->GetEntryTextProperty()->BoldOff();
    m_invalidLegend->BorderOff();
    m_invalidLegend->SetVisibility(display.isVisible && display.hasLegend);
    AttachProp(m_actor);
    AttachProp(m_selectionActor);
    AttachProp(m_legend);
    AttachProp(m_invalidLegend);
}
void ThicknessOverlay::SetInputData(vtkSmartPointer<vtkDataObject> data)
{
    auto *mesh = vtkPolyData::SafeDownCast(data);
    if (!mesh)
        return;
    const auto* previous = m_cutter ? vtkPolyData::SafeDownCast(m_cutter->GetInput()) : m_mapper->GetInput();
    if (mesh != previous) m_selectionActor->VisibilityOff();
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
