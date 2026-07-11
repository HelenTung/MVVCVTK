#pragma once

#include "Host/HostSessionTypes.h"

#include <functional>

class HostFeatureBindings {
public:
    bool StartCrop(const HostCropViewRequest&)
    {
        m_isCropActive = true;
        return true;
    }

    bool SwitchCropBox(const HostCropViewRequest& request)
    {
        return StartCrop(request);
    }

    bool SwitchCropPlane(const HostCropViewRequest& request)
    {
        return StartCrop(request);
    }

    bool SwitchCropView(const HostCropViewRequest& request, HostCropPreviewMode)
    {
        return StartCrop(request);
    }

    bool SendCrop(const HostCropViewRequest&)
    {
        ++m_cropSendCount;
        return true;
    }

    bool ExitCrop()
    {
        if (!m_isCropActive) {
            return false;
        }
        m_isCropActive = false;
        return true;
    }

    bool StartGapView(const HostGapViewRequest&)
    {
        m_isGapView = true;
        return true;
    }

    bool SwitchGapView(const HostGapViewRequest& request)
    {
        return m_isGapView ? true : StartGapView(request);
    }

    bool SwitchGapLayer()
    {
        return m_isGapView;
    }

    bool ExitGapView()
    {
        if (!m_isGapView) {
            return false;
        }
        m_isGapView = false;
        return true;
    }

    bool ExitFeature()
    {
        ++m_featureExitCount;
        return ExitCrop() || ExitGapView();
    }

    bool GetCropActive() const
    {
        return m_isCropActive;
    }

    bool GetGapView() const
    {
        return m_isGapView;
    }

    void SetCropActive(bool isActive)
    {
        m_isCropActive = isActive;
    }

    int GetCropSendCount() const
    {
        return m_cropSendCount;
    }

    int GetFeatureExitCount() const
    {
        return m_featureExitCount;
    }

    void ClearCropInput() const
    {
    }

    bool SendCropInput()
    {
        return true;
    }

private:
    bool m_isCropActive = false;
    bool m_isGapView = false;
    int m_cropSendCount = 0;
    int m_featureExitCount = 0;
};
