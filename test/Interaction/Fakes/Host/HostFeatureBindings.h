#pragma once

#include "Host/HostSessionTypes.h"

#include <functional>

class HostFeatureBindings {
public:
    bool StartCrop(const HostCropViewRequest&)
    {
        ++m_cropStartCount;
        m_isCropActive = true;
        return true;
    }

    bool SwitchCropBox(const HostCropViewRequest&)
    {
        ++m_cropBoxCount;
        m_isCropActive = true;
        return true;
    }

    bool SwitchCropPlane(const HostCropViewRequest&)
    {
        ++m_cropPlaneCount;
        m_isCropActive = true;
        return true;
    }

    bool SwitchCropView(const HostCropViewRequest&, HostCropPreviewMode)
    {
        ++m_cropViewCount;
        m_isCropActive = true;
        return true;
    }

    bool SendCrop(const HostCropViewRequest&)
    {
        ++m_cropSendCount;
        return true;
    }

    bool ExitCrop()
    {
        ++m_cropExitCount;
        if (!m_isCropActive) {
            return false;
        }
        m_isCropActive = false;
        return true;
    }

    bool StartGapView(const HostGapViewRequest&)
    {
        ++m_gapStartCount;
        m_isGapView = true;
        return true;
    }

    bool SwitchGapView(const HostGapViewRequest&)
    {
        ++m_gapViewCount;
        m_isGapView = true;
        return true;
    }

    bool SwitchGapLayer()
    {
        ++m_gapLayerCount;
        return m_isGapView;
    }

    bool ExitGapView()
    {
        ++m_gapExitCount;
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

    bool SetGapView(bool isActive)
    {
        m_isGapView = isActive;
        return true;
    }

    int GetCropSendCount() const
    {
        return m_cropSendCount;
    }

    int GetFeatureExitCount() const
    {
        return m_featureExitCount;
    }

    int GetCropStartCount() const { return m_cropStartCount; }
    int GetCropBoxCount() const { return m_cropBoxCount; }
    int GetCropPlaneCount() const { return m_cropPlaneCount; }
    int GetCropViewCount() const { return m_cropViewCount; }
    int GetCropExitCount() const { return m_cropExitCount; }
    int GetGapStartCount() const { return m_gapStartCount; }
    int GetGapViewCount() const { return m_gapViewCount; }
    int GetGapLayerCount() const { return m_gapLayerCount; }
    int GetGapExitCount() const { return m_gapExitCount; }
    int GetCropInputCount() const { return m_cropInputCount; }

    void ClearCropInput() const
    {
    }

    bool SendCropInput()
    {
        ++m_cropInputCount;
        return true;
    }

private:
    bool m_isCropActive = false;
    bool m_isGapView = false;
    int m_cropSendCount = 0;
    int m_featureExitCount = 0;
    int m_cropStartCount = 0;
    int m_cropBoxCount = 0;
    int m_cropPlaneCount = 0;
    int m_cropViewCount = 0;
    int m_cropExitCount = 0;
    int m_gapStartCount = 0;
    int m_gapViewCount = 0;
    int m_gapLayerCount = 0;
    int m_gapExitCount = 0;
    int m_cropInputCount = 0;
};
