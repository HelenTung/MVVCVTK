#pragma once

#include "AppTypes.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    bool ReloadFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dimensions,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete)
    {
        if (!m_isReloadAccepted) {
            // production 在 task/admission 拒绝后也可能把 false callback 留给 Timer 消费。
            m_reloadComplete = std::move(onComplete);
            return false;
        }
        const auto voxelCount = static_cast<std::size_t>(dimensions[0])
            * static_cast<std::size_t>(dimensions[1])
            * static_cast<std::size_t>(dimensions[2]);
        m_reloadVoxels.assign(data, data + voxelCount);
        m_reloadDimensions = dimensions;
        m_reloadSpacing = spacing;
        m_reloadOrigin = origin;
        m_reloadComplete = std::move(onComplete);
        ++m_reloadCount;
        return true;
    }

    void SetReloadAccepted(bool isAccepted)
    {
        m_isReloadAccepted = isAccepted;
    }

    bool SendReloadComplete(bool isSuccess)
    {
        if (!m_reloadComplete) {
            return false;
        }
        auto onComplete = std::move(m_reloadComplete);
        onComplete(isSuccess);
        return true;
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

    int GetReloadCount() const
    {
        return m_reloadCount;
    }

    const std::vector<float>& GetReloadVoxels() const
    {
        return m_reloadVoxels;
    }

    const std::array<int, 3>& GetReloadDimensions() const
    {
        return m_reloadDimensions;
    }

    const std::array<float, 3>& GetReloadSpacing() const
    {
        return m_reloadSpacing;
    }

    const std::array<float, 3>& GetReloadOrigin() const
    {
        return m_reloadOrigin;
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
    int m_reloadCount = 0;
    int m_exportCount = 0;
    int m_sliceCount = 0;
    std::string m_loadPath;
    std::string m_exportPath;
    std::string m_slicePath;
    bool m_isReloadAccepted = true;
    std::vector<float> m_reloadVoxels;
    std::array<int, 3> m_reloadDimensions{ 0, 0, 0 };
    std::array<float, 3> m_reloadSpacing{ 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_reloadOrigin{ 0.0f, 0.0f, 0.0f };
    std::function<void(bool isSuccess)> m_reloadComplete;
};
