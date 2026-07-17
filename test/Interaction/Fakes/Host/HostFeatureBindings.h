#pragma once

#include "Host/Types/HostCommandTypes.h"

#include <functional>
#include <utility>

class HostFeatureBindings {
public:
    bool StartCrop(const HostCropTargetRequest& request)
    {
        m_cropTarget = request;
        ++m_cropStartCount;
        return SetCropActive();
    }

    bool SwitchCropBox(const HostCropTargetRequest& request)
    {
        m_cropTarget = request;
        ++m_cropBoxCount;
        return SetCropActive();
    }

    bool SwitchCropPlane(const HostCropTargetRequest& request)
    {
        m_cropTarget = request;
        ++m_cropPlaneCount;
        return SetCropActive();
    }

    bool SwitchCropView(const HostCropPreviewRequest& request)
    {
        m_cropPreview = request;
        ++m_cropViewCount;
        return SetCropActive();
    }

    bool SendCrop(const HostCropTargetRequest& request,
        HostCompleteCallback onComplete)
    {
        m_cropTarget = request;
        ++m_cropSendCount;
        if (onComplete) onComplete(m_isCommandSuccess);
        return m_isCommandSuccess;
    }

    bool ExitCrop()
    {
        ++m_cropExitCount;
        if (!m_isCommandSuccess || !m_isCropActive) return false;
        m_isCropActive = false;
        return true;
    }

    bool StartGap(const HostGapStartRequest& request,
        HostCompleteCallback onComplete)
    {
        m_gapStart = request;
        ++m_gapStartCount;
        if (m_isCommandSuccess) m_isGapView = true;
        if (onComplete) onComplete(m_isCommandSuccess);
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
        if (!m_isCommandSuccess || !m_isGapView) return false;
        m_isGapView = false;
        return true;
    }

    bool GetGapView() const { return m_isGapView; }
    bool GetCropActive() const { return m_isCropActive; }
    void SetCropActive(bool isActive) { m_isCropActive = isActive; }
    void SetGapView(bool isActive) { m_isGapView = isActive; }
    void SetCommandResult(bool isSuccess) { m_isCommandSuccess = isSuccess; }
    void ClearCropInput() const { ++m_cropClearCount; }
    bool SendCropInput() { ++m_cropInputCount; return true; }

    const HostCropTargetRequest& GetCropTarget() const { return m_cropTarget; }
    const HostCropPreviewRequest& GetCropPreview() const { return m_cropPreview; }
    const HostGapStartRequest& GetGapStart() const { return m_gapStart; }
    int GetCropStartCount() const { return m_cropStartCount; }
    int GetCropBoxCount() const { return m_cropBoxCount; }
    int GetCropPlaneCount() const { return m_cropPlaneCount; }
    int GetCropViewCount() const { return m_cropViewCount; }
    int GetCropSendCount() const { return m_cropSendCount; }
    int GetCropExitCount() const { return m_cropExitCount; }
    int GetGapStartCount() const { return m_gapStartCount; }
    int GetGapLayerCount() const { return m_gapLayerCount; }
    int GetGapExitCount() const { return m_gapExitCount; }
    int GetCropInputCount() const { return m_cropInputCount; }
    int GetCropClearCount() const { return m_cropClearCount; }

private:
    bool SetCropActive()
    {
        if (m_isCommandSuccess) m_isCropActive = true;
        return m_isCommandSuccess;
    }

    bool m_isCropActive = false;
    bool m_isGapView = false;
    bool m_isCommandSuccess = true;
    HostCropTargetRequest m_cropTarget;
    HostCropPreviewRequest m_cropPreview;
    HostGapStartRequest m_gapStart;
    int m_cropStartCount = 0;
    int m_cropBoxCount = 0;
    int m_cropPlaneCount = 0;
    int m_cropViewCount = 0;
    int m_cropSendCount = 0;
    int m_cropExitCount = 0;
    int m_gapStartCount = 0;
    int m_gapLayerCount = 0;
    int m_gapExitCount = 0;
    int m_cropInputCount = 0;
    mutable int m_cropClearCount = 0;
};
