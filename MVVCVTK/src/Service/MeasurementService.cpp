#include "MeasurementService.h"
#include "MeasurementCsvExporter.h"
#include <algorithm>

void MeasurementService::SetToolMode(ToolMode mode)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_toolMode == mode) {
            return;
        }

        const bool shouldClearPending = !GetIsMeasureMode(mode)
            || !GetIsMeasureMode(m_toolMode)
            || mode != m_toolMode;
        if (shouldClearPending) {
            const int activeIndex = GetActiveSessionIndex();
            if (activeIndex >= 0) {
                m_sessions.erase(m_sessions.begin() + activeIndex);
            }
        }

        m_toolMode = mode;
        changed = true;
    }
    if (changed) {
        SetObserversNotified();
    }
}

bool MeasurementService::SetMeasurementPointAdded(const double worldPos[3], const double modelPos[3])
{
    MeasurementResult completedResult;
    bool hasCompletedResult = false;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!GetIsMeasureMode(m_toolMode) || !worldPos || !modelPos) {
            return false;
        }

        int activeIndex = GetActiveSessionIndex();
        if (activeIndex < 0) {
            m_sessions.push_back(GetSessionCreated());
            activeIndex = static_cast<int>(m_sessions.size()) - 1;
            SetHistoryStyleUpdated(m_sessions[activeIndex].result.type);
        }

        auto& session = m_sessions[activeIndex];
        session.result.worldPoints.push_back({ worldPos[0], worldPos[1], worldPos[2] });
        session.result.modelPoints.push_back({ modelPos[0], modelPos[1], modelPos[2] });
        session.previewWorldPoint = session.result.worldPoints.back();
        session.previewModelPoint = session.result.modelPoints.back();
        session.hasPreviewPoint = true;
        session.result.status = MeasurementStatus::InProgress;

        const size_t pointCount = session.result.worldPoints.size();
        if (session.result.type == MeasurementType::Length && pointCount >= 2) {
            session.result.value = MeasurementComputeService::GetLength(
                session.result.worldPoints[0], session.result.worldPoints[1]);
            session.result.status = MeasurementStatus::Succeeded;
            session.hasPreviewPoint = false;
            completedResult = session.result;
            hasCompletedResult = true;
        }
        else if (session.result.type == MeasurementType::Angle && pointCount >= 3) {
            session.result.value = MeasurementComputeService::GetAngle(
                session.result.worldPoints[0],
                session.result.worldPoints[1],
                session.result.worldPoints[2]);
            session.result.status = MeasurementStatus::Succeeded;
            session.hasPreviewPoint = false;
            completedResult = session.result;
            hasCompletedResult = true;
        }

        changed = true;
    }

    if (changed) {
        SetObserversNotified();
    }
    if (hasCompletedResult && m_resultCallback) {
        m_resultCallback(completedResult);
    }
    return true;
}

bool MeasurementService::SetMeasurementPreviewPointUpdated(const double worldPos[3], const double modelPos[3])
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const int activeIndex = GetActiveSessionIndex();
        if (activeIndex < 0 || !worldPos || !modelPos) {
            return false;
        }

        auto& session = m_sessions[activeIndex];
        session.previewWorldPoint = { worldPos[0], worldPos[1], worldPos[2] };
        session.previewModelPoint = { modelPos[0], modelPos[1], modelPos[2] };
        session.hasPreviewPoint = true;

        if (session.result.type == MeasurementType::Length && !session.result.worldPoints.empty()) {
            session.result.value = MeasurementComputeService::GetLength(
                session.result.worldPoints[0],
                session.previewWorldPoint);
        }
        else if (session.result.type == MeasurementType::Angle) {
            if (session.result.worldPoints.size() >= 2) {
                session.result.value = MeasurementComputeService::GetAngle(
                    session.result.worldPoints[0],
                    session.result.worldPoints[1],
                    session.previewWorldPoint);
            }
            else if (session.result.worldPoints.size() == 1) {
                session.result.value = MeasurementComputeService::GetLength(
                    session.result.worldPoints[0],
                    session.previewWorldPoint);
            }
        }
        changed = true;
    }

    if (changed) {
        SetObserversNotified();
    }
    return true;
}

void MeasurementService::SetMeasurementPreviewCleared()
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const int activeIndex = GetActiveSessionIndex();
        if (activeIndex < 0) {
            return;
        }
        m_sessions.erase(m_sessions.begin() + activeIndex);
        changed = true;
    }

    if (changed) {
        SetObserversNotified();
    }
}

void MeasurementService::SetResultCallback(std::function<void(const MeasurementResult&)> callback)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_resultCallback = std::move(callback);
}

std::vector<MeasurementResult> MeasurementService::GetResults() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<MeasurementResult> results;
    results.reserve(m_sessions.size());
    for (const auto& session : m_sessions) {
        if (session.result.status == MeasurementStatus::Succeeded) {
            results.push_back(session.result);
        }
    }
    return results;
}

std::vector<MeasurementSessionState> MeasurementService::GetSessionStates() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_sessions;
}

bool MeasurementService::SetResultVisible(uint64_t id, bool show)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& session : m_sessions) {
            if (session.result.id != id || session.result.status != MeasurementStatus::Succeeded) {
                continue;
            }
            if (session.result.visible != show) {
                session.result.visible = show;
                changed = true;
            }
            break;
        }
    }

    if (changed) {
        SetObserversNotified();
    }
    return changed;
}

bool MeasurementService::SetResultsFileSaved(const std::string& path) const
{
    return MeasurementCsvExporter::SetResultsFileSaved(path, GetResults());
}

void MeasurementService::SetResultsCleared()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_sessions.clear();
    }
    SetObserversNotified();
}

void MeasurementService::SetMeasurementObserver(std::shared_ptr<void> owner,
    std::function<void()> callback)
{
    if (!owner || !callback) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    m_observers.erase(
        std::remove_if(m_observers.begin(), m_observers.end(),
            [&owner](const MeasurementObserverEntry& entry) {
                auto current = entry.owner.lock();
                return !current || current.get() == owner.get();
            }),
        m_observers.end());
    m_observers.push_back({ std::move(owner), std::move(callback) });
}

int MeasurementService::GetActiveSessionIndex() const
{
    for (int i = 0; i < static_cast<int>(m_sessions.size()); ++i) {
        if (m_sessions[i].result.status == MeasurementStatus::InProgress) {
            return i;
        }
    }
    return -1;
}

MeasurementSessionState MeasurementService::GetSessionCreated()
{
    MeasurementSessionState session;
    session.result.id = m_nextId++;
    session.result.type = (m_toolMode == ToolMode::AngleMeasure)
        ? MeasurementType::Angle
        : MeasurementType::Length;
    session.result.status = MeasurementStatus::InProgress;
    session.result.unit = (session.result.type == MeasurementType::Angle) ? "deg" : "mm";
    session.result.visible = true;
    session.result.isHistorical = false;
    return session;
}

void MeasurementService::SetHistoryStyleUpdated(MeasurementType type)
{
    for (auto& session : m_sessions) {
        if (session.result.type == type && session.result.status == MeasurementStatus::Succeeded) {
            session.result.isHistorical = true;
        }
    }
}

void MeasurementService::SetObserversNotified()
{
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_observers.erase(
            std::remove_if(m_observers.begin(), m_observers.end(),
                [](const MeasurementObserverEntry& entry) {
                    return entry.owner.expired();
                }),
            m_observers.end());
        callbacks.reserve(m_observers.size());
        for (const auto& entry : m_observers) {
            callbacks.push_back(entry.callback);
        }
    }

    for (const auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

bool MeasurementService::GetIsMeasureMode(ToolMode mode)
{
    return mode == ToolMode::DistanceMeasure || mode == ToolMode::AngleMeasure;
}
