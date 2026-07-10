#pragma once

#include "AppTypes.h"

class VizService {
public:
    template <typename... Args>
    void LoadFileAsync(Args&&...)
    {
    }

    template <typename... Args>
    void ExportDataAsync(Args&&...)
    {
    }

    template <typename... Args>
    void ExportSlicesAsync(Args&&...)
    {
    }

    void SetVizMode(VizMode mode)
    {
        m_vizMode = mode;
        ++m_vizModeSetCount;
    }

    VizMode GetVizMode() const
    {
        return m_vizMode;
    }

    int GetVizModeSetCount() const
    {
        return m_vizModeSetCount;
    }

    template <typename... Args>
    void SetMaterial(Args&&...)
    {
    }

    template <typename... Args>
    void SetOpacity(Args&&...)
    {
    }

    template <typename... Args>
    void SetTransferFunction(Args&&...)
    {
    }

    template <typename... Args>
    void SetIsoThreshold(Args&&...)
    {
    }

    template <typename... Args>
    void SetBackground(Args&&...)
    {
    }

    template <typename... Args>
    void SetSpacing(Args&&...)
    {
    }

    template <typename... Args>
    void SetWindowLevel(Args&&...)
    {
    }

private:
    VizMode m_vizMode = VizMode::Volume;
    int m_vizModeSetCount = 0;
};
