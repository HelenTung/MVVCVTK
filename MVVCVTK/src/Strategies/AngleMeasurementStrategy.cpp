#include "AngleMeasurementStrategy.h"
#include "MeasurementComputeService.h"
#include <vtkCellArray.h>
#include <vtkProperty.h>
#include <vtkTextProperty.h>

AngleMeasurementStrategy::AngleMeasurementStrategy(uint64_t id)
{
    m_result.id = id;
    m_result.type = MeasurementType::Angle;
    m_result.status = MeasurementStatus::InProgress;
    m_result.unit = "deg";

    m_firstLineSource = vtkSmartPointer<vtkLineSource>::New();
    m_secondLineSource = vtkSmartPointer<vtkLineSource>::New();
    m_firstLineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_secondLineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_firstLineMapper->SetInputConnection(m_firstLineSource->GetOutputPort());
    m_secondLineMapper->SetInputConnection(m_secondLineSource->GetOutputPort());

    m_firstLineActor = vtkSmartPointer<vtkActor>::New();
    m_secondLineActor = vtkSmartPointer<vtkActor>::New();
    m_firstLineActor->SetMapper(m_firstLineMapper);
    m_secondLineActor->SetMapper(m_secondLineMapper);
    m_firstLineActor->PickableOff();
    m_secondLineActor->PickableOff();
    SetManagedProp(m_firstLineActor);
    SetManagedProp(m_secondLineActor);

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

void AngleMeasurementStrategy::SetSessionStateSynced(const MeasurementSessionState& state)
{
    m_result = state.result;
    m_worldPoints = state.result.worldPoints;
    m_modelPoints = state.result.modelPoints;
    m_previewWorldPoint = state.previewWorldPoint;
    m_previewModelPoint = state.previewModelPoint;
    m_hasPreviewPoint = state.hasPreviewPoint;
    SetStyleUpdated();
    SetVisualUpdated();
}

MeasurementStatus AngleMeasurementStrategy::SetPointAdded(const double worldPos[3],
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

    if (m_worldPoints.size() >= 3) {
        m_result.value = MeasurementComputeService::GetAngle(m_worldPoints[0], m_worldPoints[1], m_worldPoints[2]);
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

void AngleMeasurementStrategy::SetPreviewPointUpdated(const double worldPos[3],
    const double modelPos[3])
{
    if (GetFinished() || m_worldPoints.empty() || !worldPos || !modelPos) {
        return;
    }

    m_previewWorldPoint = { worldPos[0], worldPos[1], worldPos[2] };
    m_previewModelPoint = { modelPos[0], modelPos[1], modelPos[2] };
    m_hasPreviewPoint = true;

    if (m_worldPoints.size() >= 2) {
        m_result.value = MeasurementComputeService::GetAngle(
            m_worldPoints[0],
            m_worldPoints[1],
            m_previewWorldPoint);
    }
    else {
        m_result.value = MeasurementComputeService::GetLength(
            m_worldPoints[0],
            m_previewWorldPoint);
    }
    SetPreviewVisualUpdated();
}

void AngleMeasurementStrategy::SetPreviewCleared()
{
    if (GetFinished() || m_worldPoints.empty()) {
        return;
    }

    m_hasPreviewPoint = false;
    m_result.value = 0.0;
    SetPreviewVisualUpdated();
}

void AngleMeasurementStrategy::SetLatest(bool isLatest)
{
    m_result.isHistorical = !isLatest;
    SetStyleUpdated();
}

void AngleMeasurementStrategy::SetVisible(bool show)
{
    m_result.visible = show;
    SetVisualUpdated();
}

void AngleMeasurementStrategy::SetVisualState(const RenderParams& params, UpdateFlags flags)
{
    if (HasFlag(flags, UpdateFlags::Transform)) {
        SetWorldPointsSynced(params.modelMatrix);
        SetPointSetUpdated();
        SetPreviewVisualUpdated();
    }
}

void AngleMeasurementStrategy::SetVisualUpdated()
{
    SetPointSetUpdated();
    SetPreviewVisualUpdated();
}

void AngleMeasurementStrategy::SetPointSetUpdated()
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

void AngleMeasurementStrategy::SetPreviewVisualUpdated()
{
    if (m_worldPoints.empty()) {
        m_firstLineActor->SetVisibility(0);
        m_secondLineActor->SetVisibility(0);
        m_textActor->SetVisibility(0);
        return;
    }

    if (!m_hasPreviewPoint && !GetFinished()) {
        m_firstLineActor->SetVisibility(0);
        m_secondLineActor->SetVisibility(0);
        m_textActor->SetVisibility(0);
        return;
    }

    if (m_worldPoints.size() == 1) {
        m_firstLineSource->SetPoint1(m_worldPoints[0].data());
        m_firstLineSource->SetPoint2(m_previewWorldPoint.data());
        m_firstLineSource->Update();
        m_firstLineActor->SetVisibility(m_result.visible ? 1 : 0);
        m_secondLineActor->SetVisibility(0);
        m_textActor->SetInput(MeasurementComputeService::GetValueText(m_result.value, "mm").c_str());
        m_textActor->SetPosition(m_previewWorldPoint[0], m_previewWorldPoint[1], m_previewWorldPoint[2]);
        m_textActor->SetVisibility(m_result.visible ? 1 : 0);
        return;
    }

    m_firstLineSource->SetPoint1(m_worldPoints[1].data());
    m_firstLineSource->SetPoint2(m_worldPoints[0].data());
    m_firstLineSource->Update();
    m_firstLineActor->SetVisibility(m_result.visible ? 1 : 0);

    const auto& secondEnd = GetFinished() ? m_worldPoints[2] : m_previewWorldPoint;
    m_secondLineSource->SetPoint1(m_worldPoints[1].data());
    m_secondLineSource->SetPoint2(secondEnd.data());
    m_secondLineSource->Update();

    const auto textPos = MeasurementComputeService::GetAngleTextPosition(
        m_worldPoints[0], m_worldPoints[1], secondEnd);
    m_textActor->SetInput(MeasurementComputeService::GetValueText(m_result.value, m_result.unit).c_str());
    m_textActor->SetPosition(textPos[0], textPos[1], textPos[2]);
    m_secondLineActor->SetVisibility(m_result.visible ? 1 : 0);
    m_textActor->SetVisibility(m_result.visible ? 1 : 0);
}

void AngleMeasurementStrategy::SetStyleUpdated()
{
    const double opacity = m_result.isHistorical ? 0.35 : 1.0;
    const double lineWidth = m_result.isHistorical ? 2.0 : 3.0;

    m_firstLineActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
    m_firstLineActor->GetProperty()->SetOpacity(opacity);
    m_firstLineActor->GetProperty()->SetLineWidth(lineWidth);
    m_secondLineActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
    m_secondLineActor->GetProperty()->SetOpacity(opacity);
    m_secondLineActor->GetProperty()->SetLineWidth(lineWidth);
    m_pointActor->GetProperty()->SetColor(1.0, 1.0, 0.0);
    m_pointActor->GetProperty()->SetOpacity(opacity);
    m_textActor->GetTextProperty()->SetColor(1.0, 1.0, 0.0);
    m_textActor->GetTextProperty()->SetOpacity(opacity);
}

void AngleMeasurementStrategy::SetWorldPointsSynced(const std::array<double, 16>& modelMatrix)
{
    for (size_t i = 0; i < m_modelPoints.size() && i < m_worldPoints.size(); ++i) {
        m_worldPoints[i] = MeasurementComputeService::GetWorldPoint(modelMatrix, m_modelPoints[i]);
    }
    if (m_hasPreviewPoint) {
        m_previewWorldPoint = MeasurementComputeService::GetWorldPoint(modelMatrix, m_previewModelPoint);
    }
    m_result.worldPoints = m_worldPoints;
}
