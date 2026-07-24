#pragma once

#include "AppTypes.h"
#include "VolumeTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>

class VizService {
public:
    bool LoadFileAsync(
        std::string path,
        VolumeLayout layout,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_loadPath = std::move(path);
        m_loadLayout = std::move(layout);
        ++m_loadCount;
        if (onComplete) onComplete(true);
        return true;
    }

    bool ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool isSuccess)> onComplete)
    {
        if (!m_isReloadAccepted) return false;
        m_reloadBuffer = std::move(buffer);
        m_reloadComplete = std::move(onComplete);
        ++m_reloadCount;
        return true;
    }

    void SetReloadAccepted(bool isAccepted) { m_isReloadAccepted = isAccepted; }

    bool SendReloadComplete(bool isSuccess)
    {
        if (!m_reloadComplete) return false;
        auto onComplete = std::move(m_reloadComplete);
        onComplete(isSuccess);
        return true;
    }

    bool SendReloadUpdate() { return true; }

    void ExportDataAsync(const std::string& path,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_exportPath = path;
        ++m_exportCount;
        if (onComplete) onComplete(true);
    }

    void ExportSlicesAsync(const std::string& path, std::optional<double> angleDeg,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_slicePath = path;
        m_sliceAngleDeg = angleDeg;
        ++m_sliceCount;
        if (onComplete) onComplete(true);
    }

    void SetVizMode(VizMode mode) { m_vizMode = mode; ++m_vizModeSetCount; }
    VizMode GetVizMode() const { return m_vizMode; }
    int GetVizModeSetCount() const { return m_vizModeSetCount; }
    int GetViewSetCount() const { return m_viewSetCount + m_vizModeSetCount; }
    int GetLoadCount() const { return m_loadCount; }
    int GetReloadCount() const { return m_reloadCount; }
    int GetExportCount() const { return m_exportCount; }
    int GetSliceCount() const { return m_sliceCount; }
    const std::string& GetLoadPath() const { return m_loadPath; }
    const VolumeLayout& GetLoadLayout() const { return *m_loadLayout; }
    const VolumeBuffer& GetReloadBuffer() const { return *m_reloadBuffer; }
    const std::string& GetExportPath() const { return m_exportPath; }
    const std::string& GetSlicePath() const { return m_slicePath; }
    const std::optional<double>& GetSliceAngleDeg() const { return m_sliceAngleDeg; }

    template <typename... Args> void SetMaterial(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetOpacity(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetTransferFunction(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetIsoThreshold(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetBackground(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetSpacing(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetWindowLevel(Args&&...) { ++m_viewSetCount; }

private:
    VizMode m_vizMode = VizMode::Volume;
    int m_vizModeSetCount = 0;
    int m_viewSetCount = 0;
    int m_loadCount = 0;
    int m_reloadCount = 0;
    int m_exportCount = 0;
    int m_sliceCount = 0;
    std::string m_loadPath;
    std::string m_exportPath;
    std::string m_slicePath;
    std::optional<double> m_sliceAngleDeg;
    bool m_isReloadAccepted = true;
    std::optional<VolumeLayout> m_loadLayout;
    std::optional<VolumeBuffer> m_reloadBuffer;
    std::function<void(bool isSuccess)> m_reloadComplete;
};
