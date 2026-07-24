#pragma once

#include "Host/Types/HostCommandTypes.h"

#include <utility>

class HostFeatureBindings {
public:
    bool StartGap(
        const HostGapStartRequest& request,
        HostCompleteCallback onComplete)
    {
        m_gapStart = request;
        ++m_gapStartCount;
        if (m_isCommandSuccess) {
            m_isGapView = true;
        }
        if (onComplete) {
            onComplete(m_isCommandSuccess);
        }
        return m_isCommandSuccess;
    }

    bool SwitchGapLayer()
    {
        ++m_gapLayerCount;
        return m_isCommandSuccess && m_isGapView;
    }

    bool ExitGap()
    {
        ++m_gapExitCount;
        if (!m_isCommandSuccess || !m_isGapView) {
            return false;
        }
        m_isGapView = false;
        return true;
    }

    bool GetGapView() const { return m_isGapView; }
    void SetGapView(bool isActive) { m_isGapView = isActive; }
    void SetCommandResult(bool isSuccess) { m_isCommandSuccess = isSuccess; }

    const HostGapStartRequest& GetGapStart() const { return m_gapStart; }
    int GetGapStartCount() const { return m_gapStartCount; }
    int GetGapLayerCount() const { return m_gapLayerCount; }
    int GetGapExitCount() const { return m_gapExitCount; }

private:
    bool m_isGapView = false;
    bool m_isCommandSuccess = true;
    HostGapStartRequest m_gapStart;
    int m_gapStartCount = 0;
    int m_gapLayerCount = 0;
    int m_gapExitCount = 0;
};
