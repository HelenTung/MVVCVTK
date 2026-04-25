#include "LengthMeasurementStrategy.h"
#include "MeasurementComputeService.h"
#include <vtkCellArray.h>
#include <vtkProperty.h>
#include <vtkTextProperty.h>

LengthMeasurementStrategy::LengthMeasurementStrategy(uint64_t id)
{
    m_result.id = id;
    m_result.type = MeasurementType::Length;
    m_result.status = MeasurementStatus::InProgress;
    m_result.unit = "mm";

    m_lineSource = vtkSmartPointer<vtkLineSource>::New();
    m_lineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_lineMapper->SetInputConnection(m_lineSource->GetOutputPort());

    m_lineActor = vtkSmartPointer<vtkActor>::New();
    m_lineActor->SetMapper(m_lineMapper);
    m_lineActor->PickableOff();
    SetManagedProp(m_lineActor);

    m_pointPositions = vtkSmartPointer<vtkPoints>::New();
    m_pointPolyData = vtkSmartPointer<vtkPolyData>::New();
    m_pointPolyData->SetPoints(m_pointPositions);
    m_pointMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_pointMapper->SetInputData(m_pointPolyData);

    m_pointActor = vtkSmartPointer<vtkActor>::New();
    m_pointActor->SetMapper(m_pointMapper);
    m_pointActor->PickableOff();
    m_pointActor->GetProperty()->RenderPointsAsSpheresOn();
    m_pointActor->GetProperty()->SetPointSize(12.0f);
    SetManagedProp(m_pointActor);

    m_textActor = vtkSmartPointer<vtkBillboardTextActor3D>::New();
    m_textActor->PickableOff();
    m_textActor->GetTextProperty()->SetFontSize(18);
    m_textActor->GetTextProperty()->BoldOn();
    SetManagedProp(m_textActor);

    SetStyleUpdated();
    SetVisible(true);
}

MeasurementStatus LengthMeasurementStrategy::SetPointAdded(const double worldPos[3],
    const double modelPos[3])
{
    if (GetFinished() || !worldPos || !modelPos) {
        return m_result.status;
    }

    m_worldPoints.push_back({ worldPos[0], worldPos[1], worldPos[2] });
    m_modelPoints.push_back({ modelPos[0], modelPos[1], modelPos[2] });
    m_previewWorldPoint = m_worldPoints.back();
    m_previewModelPoint = m_modelPoints.back();
    m_hasPreviewPoint = true;
    m_result.worldPoints = m_worldPoints;
    m_result.modelPoints = m_modelPoints;

    if (m_worldPoints.size() >= 2) {
        m_result.value = MeasurementComputeService::GetLength(m_worldPoints[0], m_worldPoints[1]);
        m_result.status = MeasurementStatus::Succeeded;
        m_hasPreviewPoint = false;
        SetPointSetUpdated();
        SetPreviewVisualUpdated();
        return m_result.status;
    }

    m_result.status = MeasurementStatus::InProgress;
    SetPointSetUpdated();
    SetPreviewVisualUpdated();
    return m_result.status;
}

void LengthMeasurementStrategy::SetPreviewPointUpdated(const double worldPos[3],
    const double modelPos[3])
{
    if (GetFinished() || m_worldPoints.empty() || !worldPos || !modelPos) {
        return;
    }

    m_previewWorldPoint = { worldPos[0], worldPos[1], worldPos[2] };
    m_previewModelPoint = { modelPos[0], modelPos[1], modelPos[2] };
    m_hasPreviewPoint = true;
    m_result.value = MeasurementComputeService::GetLength(m_worldPoints[0], m_previewWorldPoint);
    SetPreviewVisualUpdated();
}

void LengthMeasurementStrategy::SetPreviewCleared()
{
    if (GetFinished() || m_worldPoints.empty()) {
        return;
    }

    m_hasPreviewPoint = false;
    m_result.value = 0.0;
    SetPreviewVisualUpdated();
}

void LengthMeasurementStrategy::SetLatest(bool isLatest)
{
    m_result.isHistorical = !isLatest;
    SetStyleUpdated();
}

void LengthMeasurementStrategy::SetVisible(bool show)
{
    m_result.visible = show;
    SetVisualUpdated();
}

void LengthMeasurementStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (HasFlag(flags, UpdateFlags::Transform)) {
        SetWorldPointsSynced(params.modelMatrix);
        SetPointSetUpdated();
        SetPreviewVisualUpdated();
    }
}

void LengthMeasurementStrategy::SetVisualUpdated()
{
    SetPointSetUpdated();
    SetPreviewVisualUpdated();
}

void LengthMeasurementStrategy::SetPointSetUpdated()
{
    m_pointPositions->Reset();
    auto pointCells = vtkSmartPointer<vtkCellArray>::New();
    for (const auto& worldPoint : m_worldPoints) {
        const vtkIdType pointId = m_pointPositions->InsertNextPoint(worldPoint.data());
        pointCells->InsertNextCell(1);
        pointCells->InsertCellPoint(pointId);
    }
    m_pointPolyData->SetVerts(pointCells);
    m_pointPolyData->Modified();

    if (m_worldPoints.empty()) {
        m_pointActor->SetVisibility(0);
        return;
    }

    m_pointActor->SetVisibility(m_result.visible ? 1 : 0);
}

void LengthMeasurementStrategy::SetPreviewVisualUpdated()
{
    if (m_worldPoints.empty()) {
        m_lineActor->SetVisibility(0);
        m_textActor->SetVisibility(0);
        return;
    }

    if (!m_hasPreviewPoint && !GetFinished()) {
        m_lineActor->SetVisibility(0);
        m_textActor->SetVisibility(0);
        return;
    }

    const auto& lineEnd = GetFinished() ? m_worldPoints[1] : m_previewWorldPoint;
    m_lineSource->SetPoint1(m_worldPoints[0].data());
    m_lineSource->SetPoint2(lineEnd.data());
    m_lineSource->Update();

    const auto textPos = MeasurementComputeService::GetMidPoint(m_worldPoints[0], lineEnd);
    m_textActor->SetInput(MeasurementComputeService::GetValueText(m_result.value, m_result.unit).c_str());
    m_textActor->SetPosition(textPos[0], textPos[1], textPos[2]);
    m_lineActor->SetVisibility(m_result.visible ? 1 : 0);
    m_textActor->SetVisibility(m_result.visible ? 1 : 0);
}

void LengthMeasurementStrategy::SetStyleUpdated()
{
    const double opacity = m_result.isHistorical ? 0.35 : 1.0;
    const double lineWidth = m_result.isHistorical ? 2.0 : 3.0;

    m_lineActor->GetProperty()->SetColor(1.0, 0.0, 0.0);
    m_lineActor->GetProperty()->SetOpacity(opacity);
    m_lineActor->GetProperty()->SetLineWidth(lineWidth);
    m_pointActor->GetProperty()->SetColor(1.0, 0.0, 0.0);
    m_pointActor->GetProperty()->SetOpacity(opacity);
    m_textActor->GetTextProperty()->SetColor(1.0, 0.0, 0.0);
    m_textActor->GetTextProperty()->SetOpacity(opacity);
}

void LengthMeasurementStrategy::SetWorldPointsSynced(const std::array<double, 16>& modelMatrix)
{
    for (size_t i = 0; i < m_modelPoints.size() && i < m_worldPoints.size(); ++i) {
        m_worldPoints[i] = MeasurementComputeService::GetWorldPoint(modelMatrix, m_modelPoints[i]);
    }
    if (m_hasPreviewPoint) {
        m_previewWorldPoint = MeasurementComputeService::GetWorldPoint(modelMatrix, m_previewModelPoint);
    }
    m_result.worldPoints = m_worldPoints;
}
