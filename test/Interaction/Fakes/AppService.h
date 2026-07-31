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

    void ExportDataAsync(
        std::string outputDir,
        std::string extension,
        std::function<void(bool isSuccess)> onComplete)
    {
        m_exportDir = std::move(outputDir);
        m_exportExtension = std::move(extension);
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
    int GetMaterialSetCount() const { return m_materialSetCount; }
    int GetOpacitySetCount() const { return m_opacitySetCount; }
    int GetSpacingSetCount() const { return m_spacingSetCount; }
    int GetQualitySetCount() const { return m_qualitySetCount; }
    int GetGradientSetCount() const { return m_gradientSetCount; }
    int GetTransferPresetSetCount() const { return m_transferPresetSetCount; }
    int GetDenoiseSetCount() const { return m_denoiseSetCount; }
    int GetCursorSetCount() const { return m_cursorSetCount; }
    int GetVisibilitySetCount() const { return m_visibilitySetCount; }
    int GetDirtySetCount() const { return m_dirtySetCount; }
    const MaterialParams& GetMaterial() const { return m_material; }
    const VolumeQualityParams& GetVolumeQuality() const { return m_volumeQuality; }
    const std::vector<GradientOpacityNode>& GetGradientOpacity() const {
        return m_gradientOpacity;
    }
    const std::array<double, 3>& GetCursorWorld() const { return m_cursorWorld; }
    int GetCursorAxis() const { return m_cursorAxis; }
    uint32_t GetVisibilityMask() const { return m_visibilityMask; }
    bool GetDenoiseOn() const { return m_isDenoiseOn; }
    int GetLoadCount() const { return m_loadCount; }
    int GetReloadCount() const { return m_reloadCount; }
    int GetExportCount() const { return m_exportCount; }
    int GetSliceCount() const { return m_sliceCount; }
    const std::string& GetLoadPath() const { return m_loadPath; }
    const VolumeLayout& GetLoadLayout() const { return *m_loadLayout; }
    const VolumeBuffer& GetReloadBuffer() const { return *m_reloadBuffer; }
    const std::string& GetExportDir() const { return m_exportDir; }
    const std::string& GetExportExtension() const { return m_exportExtension; }
    const std::string& GetSlicePath() const { return m_slicePath; }
    const std::optional<double>& GetSliceAngleDeg() const { return m_sliceAngleDeg; }
    void SetSpacingAccepted(bool isAccepted) { m_isSpacingAccepted = isAccepted; }
    void SetQualityAccepted(bool isAccepted) { m_isQualityAccepted = isAccepted; }
    void SetGradientAccepted(bool isAccepted) { m_isGradientAccepted = isAccepted; }
    void SetPresetAccepted(bool isAccepted) { m_isPresetAccepted = isAccepted; }
    void SetDenoiseAccepted(bool isAccepted) { m_isDenoiseAccepted = isAccepted; }

    void SetMaterial(const MaterialParams& material) {
        m_material = material;
        ++m_materialSetCount;
        ++m_viewSetCount;
    }
    template <typename... Args> void SetOpacity(Args&&...) {
        ++m_opacitySetCount;
        ++m_viewSetCount;
    }
    template <typename... Args> void SetTransferFunction(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetIsoThreshold(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> void SetBackground(Args&&...) { ++m_viewSetCount; }
    template <typename... Args> bool SetSpacing(Args&&...) {
        ++m_spacingSetCount;
        if (!m_isSpacingAccepted) return false;
        ++m_viewSetCount;
        return true;
    }
    template <typename... Args> void SetWindowLevel(Args&&...) { ++m_viewSetCount; }
    bool SetVolumeQuality(const VolumeQualityParams& quality) {
        ++m_qualitySetCount;
        if (!m_isQualityAccepted) return false;
        m_volumeQuality = quality;
        ++m_viewSetCount;
        return true;
    }
    bool SetGradientOpacity(const std::vector<GradientOpacityNode>& nodes) {
        ++m_gradientSetCount;
        if (!m_isGradientAccepted) return false;
        m_gradientOpacity = nodes;
        ++m_viewSetCount;
        return true;
    }
    bool SetTransferPreset(TransferPreset) {
        ++m_transferPresetSetCount;
        if (!m_isPresetAccepted) return false;
        ++m_viewSetCount;
        return true;
    }
    bool SetDenoiseOn(bool isDenoiseOn) {
        ++m_denoiseSetCount;
        if (!m_isDenoiseAccepted) return false;
        m_isDenoiseOn = isDenoiseOn;
        ++m_viewSetCount;
        return true;
    }
    void SetCursorWorldPosition(double worldPos[3], int axis) {
        m_cursorWorld = { worldPos[0], worldPos[1], worldPos[2] };
        m_cursorAxis = axis;
        ++m_cursorSetCount;
        ++m_viewSetCount;
    }
    void SetElementVisible(uint32_t flagBit, bool isVisible) {
        if (isVisible) {
            m_visibilityMask |= flagBit;
        }
        else {
            m_visibilityMask &= ~flagBit;
        }
        ++m_visibilitySetCount;
        ++m_viewSetCount;
    }
    void SetDirty() { ++m_dirtySetCount; }

private:
    VizMode m_vizMode = VizMode::Volume;
    int m_vizModeSetCount = 0;
    int m_viewSetCount = 0;
    int m_materialSetCount = 0;
    int m_opacitySetCount = 0;
    int m_spacingSetCount = 0;
    int m_qualitySetCount = 0;
    int m_gradientSetCount = 0;
    int m_transferPresetSetCount = 0;
    int m_denoiseSetCount = 0;
    int m_cursorSetCount = 0;
    int m_visibilitySetCount = 0;
    int m_dirtySetCount = 0;
    int m_loadCount = 0;
    int m_reloadCount = 0;
    int m_exportCount = 0;
    int m_sliceCount = 0;
    std::string m_loadPath;
    std::string m_exportDir;
    std::string m_exportExtension;
    std::string m_slicePath;
    std::optional<double> m_sliceAngleDeg;
    bool m_isReloadAccepted = true;
    bool m_isSpacingAccepted = true;
    bool m_isQualityAccepted = true;
    bool m_isGradientAccepted = true;
    bool m_isPresetAccepted = true;
    bool m_isDenoiseAccepted = true;
    bool m_isDenoiseOn = false;
    int m_cursorAxis = -1;
    uint32_t m_visibilityMask = 0;
    MaterialParams m_material;
    VolumeQualityParams m_volumeQuality;
    std::vector<GradientOpacityNode> m_gradientOpacity;
    std::array<double, 3> m_cursorWorld{};
    std::optional<VolumeLayout> m_loadLayout;
    std::optional<VolumeBuffer> m_reloadBuffer;
    std::function<void(bool isSuccess)> m_reloadComplete;
};
