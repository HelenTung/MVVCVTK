#include "MeasurementInteractionHost.h"
#include "AngleMeasurementStrategy.h"
#include "LengthMeasurementStrategy.h"
#include <vtkRenderWindow.h>
#include <unordered_set>

MeasurementInteractionHost::MeasurementInteractionHost(std::shared_ptr<IMeasurementService> service)
    : m_measurementService(std::move(service))
{
}

void MeasurementInteractionHost::SetContext(vtkRenderer* renderer,
    vtkPropPicker* picker,
    AbstractInteractiveService* interactiveService)
{
    m_renderer = renderer;
    m_picker = picker;
    m_interactiveService = interactiveService;

    if (!m_measurementService) {
        return;
    }

    m_measurementService->SetMeasurementObserver(shared_from_this(), [weakSelf = weak_from_this()]() {
        auto self = weakSelf.lock();
        if (!self) return;
        self->SetStateSynced();
        });
    SetStateSynced();
}

bool MeasurementInteractionHost::GetClickHandled(int x, int y)
{
    if (!m_measurementService) {
        return false;
    }

    double worldPos[3] = { 0.0, 0.0, 0.0 };
    double modelPos[3] = { 0.0, 0.0, 0.0 };
    if (!GetPickedPositions(x, y, worldPos, modelPos)) {
        return true;
    }

    m_lastPreviewX = x;
    m_lastPreviewY = y;
    return m_measurementService->SetMeasurementPointAdded(worldPos, modelPos);
}

bool MeasurementInteractionHost::GetMouseMoveHandled(int x, int y)
{
    if (!m_measurementService) {
        return false;
    }
    if (m_lastPreviewX == x && m_lastPreviewY == y) {
        return true;
    }

    m_lastPreviewX = x;
    m_lastPreviewY = y;
    double worldPos[3] = { 0.0, 0.0, 0.0 };
    double modelPos[3] = { 0.0, 0.0, 0.0 };
    if (!GetPickedPositions(x, y, worldPos, modelPos)) {
        return true;
    }

    return m_measurementService->SetMeasurementPreviewPointUpdated(worldPos, modelPos);
}

void MeasurementInteractionHost::SetStateSynced()
{
    if (!m_measurementService) {
        return;
    }

    const auto sessions = m_measurementService->GetSessionStates();
    SetOverlaysSynced(sessions);

    const bool isInteracting = std::any_of(sessions.begin(), sessions.end(),
        [](const MeasurementSessionState& session) {
            return session.result.status == MeasurementStatus::InProgress;
        });
    if (m_interactiveService) {
        m_interactiveService->SetInteracting(isInteracting);
    }
    SetRenderRequested(isInteracting);
}

std::shared_ptr<IMeasurementStrategy> MeasurementInteractionHost::GetStrategyCreated(MeasurementType type, uint64_t id) const
{
    if (type == MeasurementType::Length) {
        return std::make_shared<LengthMeasurementStrategy>(id);
    }
    if (type == MeasurementType::Angle) {
        return std::make_shared<AngleMeasurementStrategy>(id);
    }
    return nullptr;
}

void MeasurementInteractionHost::SetOverlaysSynced(const std::vector<MeasurementSessionState>& sessions)
{
    if (!m_interactiveService) {
        return;
    }

    std::unordered_set<uint64_t> keepIds;
    for (const auto& session : sessions) {
        keepIds.insert(session.result.id);
        auto it = m_strategyById.find(session.result.id);
        if (it == m_strategyById.end()) {
            auto strategy = GetStrategyCreated(session.result.type, session.result.id);
            if (!strategy) {
                continue;
            }
            auto overlay = std::dynamic_pointer_cast<AbstractVisualStrategy>(strategy);
            if (overlay) {
                m_interactiveService->SetOverlayStrategyAdded(overlay);
            }
            it = m_strategyById.emplace(session.result.id, strategy).first;
        }
        it->second->SetSessionStateSynced(session);
    }

    for (auto it = m_strategyById.begin(); it != m_strategyById.end();) {
        if (keepIds.find(it->first) != keepIds.end()) {
            ++it;
            continue;
        }
        auto overlay = std::dynamic_pointer_cast<AbstractVisualStrategy>(it->second);
        if (overlay) {
            m_interactiveService->SetOverlayStrategyRemoved(overlay);
        }
        it = m_strategyById.erase(it);
    }
}

void MeasurementInteractionHost::SetRenderRequested(bool immediate) const
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

bool MeasurementInteractionHost::GetPickedPositions(int x, int y, double worldPos[3], double modelPos[3]) const
{
    if (!m_picker || !m_renderer || !m_interactiveService || !worldPos || !modelPos) {
        return false;
    }
    if (!m_picker->Pick(x, y, 0, m_renderer)) {
        return false;
    }

    vtkProp* pickedProp = m_picker->GetViewProp();
    double* pickedWorld = m_picker->GetPickPosition();
    if (!pickedProp || !pickedWorld) {
        return false;
    }

    worldPos[0] = pickedWorld[0];
    worldPos[1] = pickedWorld[1];
    worldPos[2] = pickedWorld[2];
    m_interactiveService->GetModelPositionFromWorld(worldPos, modelPos);
    return true;
}
