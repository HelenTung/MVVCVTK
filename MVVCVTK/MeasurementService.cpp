#include "MeasurementService.h"
#include "MeasurementCsvExporter.h"
#include <vtkProp.h>

namespace {

bool GetIsMeasureMode(ToolMode mode)
{
    return mode == ToolMode::DistanceMeasure || mode == ToolMode::AngleMeasure;
}

std::shared_ptr<AbstractVisualStrategy> GetOverlayStrategy(const std::shared_ptr<IMeasurementStrategy>& strategy)
{
    return std::dynamic_pointer_cast<AbstractVisualStrategy>(strategy);
}

} // namespace

void MeasurementService::SetToolMode(ToolMode mode)
{
    if (m_toolMode == mode) {
        return;
    }

    const bool shouldClearPending = !GetIsMeasureMode(mode)
        || !GetIsMeasureMode(m_toolMode)
        || mode != m_toolMode;
    if (shouldClearPending) {
        SetPendingStrategyCleared();
    }

    m_toolMode = mode;
    m_lastPreviewX = -1;
    m_lastPreviewY = -1;
}

void MeasurementService::SetResultCallback(std::function<void(const MeasurementResult&)> callback)
{
    m_resultCallback = std::move(callback);
}

std::vector<MeasurementResult> MeasurementService::GetResults() const
{
    std::vector<MeasurementResult> results;
    results.reserve(m_finishedStrategies.size());
    for (const auto& strategy : m_finishedStrategies) {
        if (strategy) {
            results.push_back(strategy->GetResult());
        }
    }
    return results;
}

bool MeasurementService::SetResultVisible(uint64_t id, bool show)
{
    for (const auto& strategy : m_finishedStrategies) {
        if (!strategy || strategy->GetId() != id) {
            continue;
        }
        strategy->SetVisible(show);
        SetRenderRequested();
        return true;
    }
    return false;
}

bool MeasurementService::SetResultsFileSaved(const std::string& path) const
{
    return MeasurementCsvExporter::SetResultsFileSaved(path, GetResults());
}

void MeasurementService::SetResultsCleared()
{
    SetPendingStrategyCleared();
    if (m_interactiveService) {
        for (auto& strategy : m_finishedStrategies) {
            auto overlay = GetOverlayStrategy(strategy);
            if (overlay) {
                m_interactiveService->SetOverlayStrategyRemoved(overlay);
            }
        }
    }
    m_finishedStrategies.clear();
    SetRenderRequested();
}

void MeasurementService::SetRenderContext(vtkRenderer* renderer, vtkPropPicker* picker)
{
    m_renderer = renderer;
    m_picker = picker;
}

void MeasurementService::SetInteractiveService(AbstractInteractiveService* service)
{
    m_interactiveService = service;
}

bool MeasurementService::GetClickHandled(int x, int y)
{
    if (!GetIsMeasureMode(m_toolMode)) {
        return false;
    }
    if (!m_picker || !m_renderer || !m_interactiveService) {
        return true;
    }
    if (!m_picker->Pick(x, y, 0, m_renderer)) {
        return true;
    }

    vtkProp* pickedProp = m_picker->GetViewProp();
    double* worldPos = m_picker->GetPickPosition();
    if (!pickedProp || !worldPos) {
        return true;
    }

    auto strategy = m_activeStrategy;
    if (!strategy) {
        strategy = GetStrategyCreated();
        if (!strategy) {
            return true;
        }
        auto overlay = GetOverlayStrategy(strategy);
        if (overlay) {
            m_interactiveService->SetOverlayStrategyAdded(overlay);
        }
        m_interactiveService->SetInteracting(true);
        m_activeStrategy = strategy;
    }

    double modelPos[3] = { 0.0, 0.0, 0.0 }; // 当前有效拾取点对应的模型坐标
    m_interactiveService->GetModelPositionFromWorld(worldPos, modelPos);
    const MeasurementStatus status = strategy->SetPointAdded(worldPos, modelPos);
    if (status == MeasurementStatus::Succeeded) {
        SetHistoryStyleUpdated(strategy->GetMeasurementType());
        strategy->SetLatest(true);
        m_finishedStrategies.push_back(strategy);
        m_activeStrategy.reset();
        if (m_resultCallback) {
            m_resultCallback(strategy->GetResult());
        }
        if (m_interactiveService) {
            m_interactiveService->SetInteracting(false);
        }
    }
    SetRenderRequested(true);
    return true;
}

bool MeasurementService::GetMouseMoveHandled(int x, int y)
{
    if (!GetIsMeasureMode(m_toolMode)) {
        return false;
    }
    if (!m_picker || !m_renderer || !m_interactiveService || !m_activeStrategy) {
        return true;
    }
    if (m_lastPreviewX == x && m_lastPreviewY == y) {
        return true;
    }
    m_lastPreviewX = x;
    m_lastPreviewY = y;
    if (!m_picker->Pick(x, y, 0, m_renderer)) {
        return true;
    }

    vtkProp* pickedProp = m_picker->GetViewProp();
    double* worldPos = m_picker->GetPickPosition();
    if (!pickedProp || !worldPos) {
        return true;
    }

    double modelPos[3] = { 0.0, 0.0, 0.0 }; // 当前鼠标命中点对应的模型坐标预览端点
    m_interactiveService->GetModelPositionFromWorld(worldPos, modelPos);
    m_activeStrategy->SetPreviewPointUpdated(worldPos, modelPos);
    SetRenderRequested(true);
    return true;
}

std::shared_ptr<IMeasurementStrategy> MeasurementService::GetStrategyCreated()
{
    if (m_toolMode == ToolMode::DistanceMeasure) {
        return std::make_shared<LengthMeasurementStrategy>(m_nextId++);
    }
    if (m_toolMode == ToolMode::AngleMeasure) {
        return std::make_shared<AngleMeasurementStrategy>(m_nextId++);
    }
    return nullptr;
}

void MeasurementService::SetPendingStrategyCleared()
{
    if (!m_activeStrategy) {
        return;
    }
    m_activeStrategy->SetPreviewCleared();
    if (m_interactiveService) {
        m_interactiveService->SetInteracting(false);
    }
    if (m_interactiveService) {
        auto overlay = GetOverlayStrategy(m_activeStrategy);
        if (overlay) {
            m_interactiveService->SetOverlayStrategyRemoved(overlay);
        }
    }
    m_activeStrategy.reset();
    m_lastPreviewX = -1;
    m_lastPreviewY = -1;
    SetRenderRequested();
}

void MeasurementService::SetHistoryStyleUpdated(MeasurementType type)
{
    for (auto& strategy : m_finishedStrategies) {
        if (!strategy || strategy->GetMeasurementType() != type) {
            continue;
        }
        strategy->SetLatest(false);
    }
}

void MeasurementService::SetRenderRequested(bool immediate) const
{
    if (m_interactiveService) {
        m_interactiveService->SetDirtyMarked();
    }
    if (!immediate || !m_renderer) {
        return;
    }

    vtkRenderWindow* renderWindow = m_renderer->GetRenderWindow();
    if (renderWindow
        && renderWindow->GetMapped()
        && renderWindow->GetGenericWindowId())
    {
        renderWindow->Render();
    }
}
