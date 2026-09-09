#include "ThicknessOverlay.h"
#include "Render/Contracts/SlicePlaneState.h"
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

ThicknessDisplayData ThicknessOverlay::BuildData(const ThicknessData::Record &record,
                                                 const SurfaceMeshPayload &mesh,
                                                 const ThicknessDisplay &display)
{
    auto lookup = vtkSmartPointer<vtkLookupTable>::New();
    lookup->SetNumberOfTableValues(256);
    lookup->SetRange(display.range[0], display.range[1]);
    lookup->Build();
    for (int i = 0; i < 256; ++i)
    {
        const double f = double(i) / 255;
        const double value = display.range[0] + f * (display.range[1] - display.range[0]);
        if (display.mode == ThicknessDisplayMode::Tolerance)
        {
            const bool low = value<record.archive.evaluation.lower, high = value> record.archive
                                 .evaluation.upper;
            lookup->SetTableValue(i,
                                  low    ? 0.9
                                  : high ? 0.1
                                         : 0.2,
                                  low    ? 0.1
                                  : high ? 0.3
                                         : 0.8,
                                  high ? 0.9 : 0.1, 1);
        }
        else
            lookup->SetTableValue(i, std::min(1.0, 2 * f), 1 - std::abs(2 * f - 1),
                                  std::min(1.0, 2 * (1 - f)), 1);
    }
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    auto cells = vtkSmartPointer<vtkCellArray>::New();
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("thickness.display");
    colors->SetNumberOfComponents(3);
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
        unsigned char rgb[3]{128, 128, 128};
        if (sample.validity == ThicknessValidity::Valid)
        {
            if (display.mode == ThicknessDisplayMode::Tolerance)
            {
                const bool low =
                    sample
                        .thickness<record.archive.evaluation.lower, high = sample.thickness>
                            record.archive.evaluation.upper;
                rgb[0] = low ? 230 : high ? 26 : 51;
                rgb[1] = low ? 26 : high ? 77 : 204;
                rgb[2] = high ? 230 : 26;
            }
            else
            {
                double color[3]{};
                lookup->GetColor(sample.thickness, color);
                for (int k = 0; k < 3; ++k)
                    rgb[k] = static_cast<unsigned char>(std::lround(255 * color[k]));
            }
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
    m_lineActor = vtkSmartPointer<vtkActor>::New();
    m_lineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
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
    m_lineActor->SetMapper(m_lineMapper);
    m_lineActor->PickableOff();
    m_lineActor->GetProperty()->SetColor(1, 1, 1);
    m_lineActor->GetProperty()->SetLineWidth(3);
    m_lineActor->GetProperty()->LightingOff();
    m_lineActor->VisibilityOff();
    m_legend->SetLookupTable(data.lookup);
    m_legend->SetNumberOfLabels(5);
    const std::string title =
        std::string(display.mode == ThicknessDisplayMode::Tolerance ? "Ray tol." : "Ray") +
        (unit == ThicknessUnit::Millimeter ? "\n[mm]" : "\n[m]");
    m_legend->SetTitle(title.c_str());
    m_legend->SetWidth(0.18);
    m_legend->SetHeight(0.65);
    m_legend->SetPosition(0.81, 0.15);
    // 在宽三维工作区中使用稳定字号，不让色标文字随整个 viewport 放大。
    m_legend->SetMaximumWidthInPixels(120);
    m_legend->SetMaximumHeightInPixels(320);
    m_legend->SetUnconstrainedFontSize(true);
    for (auto* text : {m_legend->GetTitleTextProperty(), m_legend->GetLabelTextProperty(), m_legend->GetAnnotationTextProperty()}) {
        text->SetFontSize(13); text->BoldOff(); text->ItalicOff(); text->ShadowOff();
    }
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
    m_invalidLegend->SetEntry(0, symbol, "Invalid / unmeasured", textColor);
    m_invalidLegend->ScalarVisibilityOn();
    m_invalidLegend->SetPosition(0.64, 0.02);
    m_invalidLegend->SetPosition2(0.34, 0.035);
    m_invalidLegend->GetEntryTextProperty()->SetFontSize(12);
    m_invalidLegend->GetEntryTextProperty()->ItalicOff();
    m_invalidLegend->GetEntryTextProperty()->BoldOff();
    m_invalidLegend->BorderOff();
    m_invalidLegend->SetVisibility(display.isVisible && display.hasLegend);
    AttachProp(m_actor);
    AttachProp(m_lineActor);
    AttachProp(m_legend);
    AttachProp(m_invalidLegend);
}
void ThicknessOverlay::SetInputData(vtkSmartPointer<vtkDataObject> data)
{
    auto *mesh = vtkPolyData::SafeDownCast(data);
    if (!mesh)
        return;
    if (m_cutter)
        m_cutter->SetInputData(mesh);
    else
        m_mapper->SetInputData(mesh);
}
void ThicknessOverlay::SetOverlayState(const FeatureOverlayState &state)
{
    if (m_plane)
    {
        m_plane->SetOrigin(state.cursor.data());
        m_plane->SetNormal(m_normal.data());
    }
    Set3DPropsTransform(state.modelToWorld);
}
void ThicknessOverlay::SetSelection(const ThicknessSample *sample)
{
    if (!sample || sample->validity != ThicknessValidity::Valid)
    {
        m_lineActor->VisibilityOff();
        return;
    }
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    points->InsertNextPoint(sample->source.data());
    points->InsertNextPoint(sample->opposite.data());
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType ids[2]{0, 1};
    lines->InsertNextCell(2, ids);
    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetLines(lines);
    m_lineMapper->SetInputData(data);
    m_lineActor->SetVisibility(m_actor->GetVisibility());
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
