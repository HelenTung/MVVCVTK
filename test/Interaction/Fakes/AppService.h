#pragma once

#include "AppTypes.h"

#include <array>
#include <functional>
#include <optional>
#include <string>

class VizService {
public:
    void LoadFileAsync(
        const std::string& path,
        const std::array<float, 3>&,
        const std::array<float, 3>&,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_loadPath = path;
        ++m_loadCount;
        if (onComplete) {
            onComplete(true);
        }
    }

    void ExportDataAsync(
        const std::string& path,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_exportPath = path;
        ++m_exportCount;
        if (onComplete) {
            onComplete(true);
        }
    }

    void ExportSlicesAsync(
        const std::string& path,
        std::optional<double>,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_slicePath = path;
        ++m_sliceCount;
        if (onComplete) {
            onComplete(true);
        }
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

    int GetLoadCount() const
    {
        return m_loadCount;
    }

    int GetExportCount() const
    {
        return m_exportCount;
    }

    int GetSliceCount() const
    {
        return m_sliceCount;
    }

    const std::string& GetLoadPath() const
    {
        return m_loadPath;
    }

    const std::string& GetExportPath() const
    {
        return m_exportPath;
    }

    const std::string& GetSlicePath() const
    {
        return m_slicePath;
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
    int m_loadCount = 0;
    int m_exportCount = 0;
    int m_sliceCount = 0;
    std::string m_loadPath;
    std::string m_exportPath;
    std::string m_slicePath;
};
