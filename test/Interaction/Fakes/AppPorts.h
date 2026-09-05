#pragma once

#include "App/Services/AppPorts.h"
#include "Interaction/InteractionPorts.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class DataPortStub;
class SessionPortStub;
class UpdatePortStub;
class ViewPortStub;

// 三个窄 fake 共享测试状态，但 concrete identity 始终彼此独立。
class AppPortState final {
public:
    int GetViewSetCount() const
    {
        return m_viewSetCount + m_vizModeSetCount;
    }
    int GetMaterialSetCount() const { return m_materialSetCount; }
    int GetOpacitySetCount() const { return m_opacitySetCount; }
    int GetSpacingSetCount() const { return m_spacingSetCount; }
    int GetQualitySetCount() const { return m_qualitySetCount; }
    int GetGradientSetCount() const { return m_gradientSetCount; }
    int GetDenoiseSetCount() const { return m_denoiseSetCount; }
    int GetCursorSetCount() const { return m_cursorSetCount; }
    int GetVisibilitySetCount() const
    {
        return m_visibilitySetCount;
    }
    int GetDirtySetCount() const { return m_dirtySetCount; }
    int GetLoadCount() const { return m_loadCount; }
    int GetReloadCount() const { return m_reloadCount; }
    int GetExportCount() const { return m_exportCount; }
    int GetSliceCount() const { return m_sliceCount; }
    const std::string& GetLoadPath() const { return m_loadPath; }
    const VolumeLayout& GetLoadLayout() const { return *m_loadLayout; }
    const VolumeBuffer& GetReloadBuffer() const
    {
        return *m_reloadBuffer;
    }
    const std::string& GetExportDir() const { return m_exportDir; }
    const std::string& GetExportExtension() const
    {
        return m_exportExtension;
    }
    const std::string& GetSlicePath() const { return m_slicePath; }
    const std::optional<double>& GetSliceAngleDeg() const
    {
        return m_sliceAngleDeg;
    }
    const MaterialParams& GetMaterial() const { return m_material; }
    const VolumeTransferFunction& GetVolumeTransferFunction() const
    {
        return m_volumeTransferFunction;
    }
    double GetIsoThreshold() const { return m_isoThreshold; }
    VolumeQuality GetVolumeQuality() const
    {
        return m_volumeQuality;
    }
    const std::vector<GradientOpacityNode>& GetGradientOpacity() const
    {
        return m_gradientOpacity;
    }
    const std::array<double, 3>& GetCursorWorld() const
    {
        return m_cursorWorld;
    }
    int GetCursorAxis() const { return m_cursorAxis; }
    std::uint32_t GetVisibilityMask() const
    {
        return m_visibilityMask;
    }
    VizMode GetVizMode() const { return m_vizMode; }
    WindowLevelMode GetWindowLevelMode() const
    {
        return m_windowLevelMode;
    }
    std::uint64_t GetRevision() const { return m_revision; }
    bool GetIsAvailable() const { return m_isViewAvailable; }
    bool GetIsDirty() const { return m_isDirty; }
    bool StopView()
    {
        m_isViewAvailable = false;
        return true;
    }

    void SetVizMode(const VizMode mode) { m_vizMode = mode; }
    void SetSpacingAccepted(const bool isAccepted)
    {
        m_isSpacingAccepted = isAccepted;
    }
    void SetQualityAccepted(const bool isAccepted)
    {
        m_isQualityAccepted = isAccepted;
    }
    void SetGradientAccepted(const bool isAccepted)
    {
        m_isGradientAccepted = isAccepted;
    }
    void SetDenoiseAccepted(const bool isAccepted)
    {
        m_isDenoiseAccepted = isAccepted;
    }
    void SetRestoreAccepted(const bool isAccepted)
    {
        m_isRestoreAccepted = isAccepted;
    }
    void SetDirtyAccepted(const bool isAccepted)
    {
        m_isDirtyAccepted = isAccepted;
    }

private:
    friend class DataPortStub;
    friend class SessionPortStub;
    friend class UpdatePortStub;
    friend class ViewPortStub;

    VizMode m_vizMode = VizMode::Volume;
    int m_vizModeSetCount = 0;
    int m_viewSetCount = 0;
    int m_materialSetCount = 0;
    int m_opacitySetCount = 0;
    int m_spacingSetCount = 0;
    int m_qualitySetCount = 0;
    int m_gradientSetCount = 0;
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
    bool m_isSpacingAccepted = true;
    bool m_isQualityAccepted = true;
    bool m_isGradientAccepted = true;
    bool m_isDenoiseAccepted = true;
    bool m_isRestoreAccepted = true;
    bool m_isDirtyAccepted = true;
    bool m_isViewAvailable = true;
    bool m_isDenoiseOn = false;
    bool m_isDirty = false;
    int m_cursorAxis = -1;
    std::uint32_t m_visibilityMask = 0;
    double m_isoThreshold = 0.0;
    MaterialParams m_material;
    VolumeQuality m_volumeQuality = VolumeQuality::Auto;
    std::vector<GradientOpacityNode> m_gradientOpacity;
    VolumeTransferFunction m_volumeTransferFunction;
    BackgroundColor m_background;
    std::array<double, 3> m_spacing{ 1.0, 1.0, 1.0 };
    WindowLevelParams m_windowLevel;
    WindowLevelMode m_windowLevelMode = WindowLevelMode::Auto;
    std::array<double, 3> m_cursorWorld{};
    std::optional<VolumeLayout> m_loadLayout;
    std::optional<VolumeBuffer> m_reloadBuffer;
    std::uint64_t m_revision = 0;
};

class DataPortStub final : public AppDataPort {
public:
    explicit DataPortStub(std::shared_ptr<AppPortState> state)
        : m_state(std::move(state))
    {
    }

    TaskAdmissionResult LoadFileAsync(
        std::string path,
        VolumeLayout layout,
        std::function<void(bool)> onComplete) override
    {
        if (!m_state) return TaskAdmissionResult::Unavailable;
        m_state->m_loadPath = std::move(path);
        m_state->m_loadLayout = std::move(layout);
        ++m_state->m_loadCount;
        if (onComplete) onComplete(true);
        return TaskAdmissionResult::Accepted;
    }

    TaskAdmissionResult ReloadFromBufferAsync(
        VolumeBuffer buffer,
        std::function<void(bool)> onComplete) override
    {
        if (!m_state) return TaskAdmissionResult::Unavailable;
        m_state->m_reloadBuffer = std::move(buffer);
        ++m_state->m_reloadCount;
        if (onComplete) onComplete(true);
        return TaskAdmissionResult::Accepted;
    }

    TaskAdmissionResult ExportDataAsync(
        std::string outputDir,
        std::string extension,
        std::function<void(bool)> onComplete) override
    {
        if (!m_state) return TaskAdmissionResult::Unavailable;
        m_state->m_exportDir = std::move(outputDir);
        m_state->m_exportExtension = std::move(extension);
        ++m_state->m_exportCount;
        if (onComplete) onComplete(true);
        return TaskAdmissionResult::Accepted;
    }

    TaskAdmissionResult ExportSlicesAsync(
        std::string path,
        std::optional<double> angleDeg,
        std::function<void(bool)> onComplete) override
    {
        if (!m_state) return TaskAdmissionResult::Unavailable;
        m_state->m_slicePath = std::move(path);
        m_state->m_sliceAngleDeg = angleDeg;
        ++m_state->m_sliceCount;
        if (onComplete) onComplete(true);
        return TaskAdmissionResult::Accepted;
    }

private:
    std::shared_ptr<AppPortState> m_state;
};

class ViewPortStub final : public AppViewPort {
public:
    explicit ViewPortStub(std::shared_ptr<AppPortState> state)
        : m_state(std::move(state))
    {
    }

    bool SetViewConfig(const PreInitConfig&) override
    {
        return m_state != nullptr;
    }

    bool SendViewUpdate(const AppViewUpdate& update) override
    {
        if (!m_state) return false;
        const AppViewState oldState = GetViewState();
        const auto reject = [this, &oldState]() {
            SetState(oldState);
            return false;
        };

        if (update.volumeQuality) {
            ++m_state->m_qualitySetCount;
            if (!m_state->m_isQualityAccepted) return reject();
            m_state->m_volumeQuality = *update.volumeQuality;
            ++m_state->m_viewSetCount;
        }
        if (update.gradientOpacity) {
            ++m_state->m_gradientSetCount;
            if (!m_state->m_isGradientAccepted) return reject();
            m_state->m_gradientOpacity = *update.gradientOpacity;
            ++m_state->m_viewSetCount;
        }
        if (update.isDenoiseOn) {
            ++m_state->m_denoiseSetCount;
            if (!m_state->m_isDenoiseAccepted) return reject();
            m_state->m_isDenoiseOn = *update.isDenoiseOn;
            ++m_state->m_viewSetCount;
        }

        if (update.mode) {
            m_state->m_vizMode = *update.mode;
            ++m_state->m_vizModeSetCount;
        }
        if (update.material) {
            m_state->m_material = *update.material;
            ++m_state->m_materialSetCount;
            ++m_state->m_viewSetCount;
        }
        if (update.opacity) {
            m_state->m_material.opacity = *update.opacity;
            ++m_state->m_opacitySetCount;
            ++m_state->m_viewSetCount;
        }
        if (update.volumeTransferFunction) {
            m_state->m_volumeTransferFunction = *update.volumeTransferFunction;
            ++m_state->m_viewSetCount;
        }
        if (update.isoThreshold) {
            m_state->m_isoThreshold = *update.isoThreshold;
            ++m_state->m_viewSetCount;
        }
        if (update.background) {
            m_state->m_background = *update.background;
            ++m_state->m_viewSetCount;
        }
        if (update.windowLevel) {
            m_state->m_windowLevel = *update.windowLevel;
            if (!update.windowLevelMode) {
                m_state->m_windowLevelMode = WindowLevelMode::Manual;
            }
            ++m_state->m_viewSetCount;
        }
        if (update.windowLevelMode) {
            m_state->m_windowLevelMode = *update.windowLevelMode;
            if (*update.windowLevelMode == WindowLevelMode::Auto
                && !update.windowLevel) {
                m_state->m_windowLevel = { 255.0, 127.5 };
            }
            ++m_state->m_viewSetCount;
        }
        if (update.visibility) {
            const auto setVisibility = [this](
                const std::optional<bool>& isVisible,
                const std::uint32_t flag) {
                if (!isVisible) return;
                if (*isVisible) m_state->m_visibilityMask |= flag;
                else m_state->m_visibilityMask &= ~flag;
                ++m_state->m_visibilitySetCount;
                ++m_state->m_viewSetCount;
            };
            setVisibility(
                update.visibility->isPlanes3DVisible,
                VisFlags::Planes3D);
            setVisibility(
                update.visibility->isCrosshairVisible,
                VisFlags::Crosshair);
            setVisibility(
                update.visibility->isRulerVisible,
                VisFlags::Ruler);
        }
        // AppViewPort 自身的内部 dirty 不属于 Router 显式 RenderUpdatePort
        // 调用次数；测试只统计跨端口补帧（方向轴、相机复位）。
        m_state->m_isDirty = true;
        ++m_state->m_revision;
        return true;
    }

    bool SetViewState(
        const AppViewState& state,
        const std::uint64_t expectedRevision) override
    {
        if (!m_state || !m_state->m_isRestoreAccepted
            || m_state->m_revision != expectedRevision) {
            return false;
        }
        SetState(state);
        ++m_state->m_revision;
        return true;
    }

    AppViewState GetViewState() const override
    {
        AppViewState state;
        if (!m_state) return state;
        state.mode = m_state->m_vizMode;
        state.material = m_state->m_material;
        state.volumeTransferFunction = m_state->m_volumeTransferFunction;
        state.isoThreshold = m_state->m_isoThreshold;
        state.background = m_state->m_background;
        state.spacing = m_state->m_spacing;
        state.windowLevel = m_state->m_windowLevel;
        state.windowLevelMode = m_state->m_windowLevelMode;
        state.volumeQuality = m_state->m_volumeQuality;
        state.gradientOpacity = m_state->m_gradientOpacity;
        state.isDenoiseOn = m_state->m_isDenoiseOn;
        state.cursorWorld = m_state->m_cursorWorld;
        state.cursorAxis = static_cast<AppCursorAxis>(
            m_state->m_cursorAxis);
        state.visibilityMask = m_state->m_visibilityMask;
        state.revision = m_state->m_revision;
        return state;
    }

private:
    void SetState(const AppViewState& state)
    {
        m_state->m_vizMode = state.mode;
        m_state->m_material = state.material;
        m_state->m_volumeTransferFunction = state.volumeTransferFunction;
        m_state->m_isoThreshold = state.isoThreshold;
        m_state->m_background = state.background;
        m_state->m_spacing = state.spacing;
        m_state->m_windowLevel = state.windowLevel;
        m_state->m_windowLevelMode = state.windowLevelMode;
        m_state->m_volumeQuality = state.volumeQuality;
        m_state->m_gradientOpacity = state.gradientOpacity;
        m_state->m_isDenoiseOn = state.isDenoiseOn;
        m_state->m_cursorWorld = state.cursorWorld;
        m_state->m_cursorAxis = static_cast<int>(state.cursorAxis);
        m_state->m_visibilityMask = state.visibilityMask;
    }

    std::shared_ptr<AppPortState> m_state;
};

class SessionPortStub final : public AppSessionPort {
public:
    explicit SessionPortStub(std::shared_ptr<AppPortState> state)
        : m_state(std::move(state))
    {
    }

    bool SendSessionUpdate(
        const AppSessionUpdate& update) override
    {
        if (!m_state || (!update.spacing && !update.cursorWorld)) {
            return false;
        }
        if (update.spacing) {
            ++m_state->m_spacingSetCount;
            if (!m_state->m_isSpacingAccepted) return false;
            m_state->m_spacing = *update.spacing;
        }
        if (update.cursorWorld) {
            m_state->m_cursorWorld = *update.cursorWorld;
            m_state->m_cursorAxis = static_cast<int>(update.cursorAxis);
            ++m_state->m_cursorSetCount;
        }
        return true;
    }

private:
    std::shared_ptr<AppPortState> m_state;
};

class UpdatePortStub final : public RenderUpdatePort {
public:
    explicit UpdatePortStub(std::shared_ptr<AppPortState> state)
        : m_state(std::move(state))
    {
    }

    bool SendUpdates() override { return m_state != nullptr; }

    bool SendPendingUpdates() override { return m_state != nullptr; }

    void SendCompletions() override {}

    bool SetRenderNeeded() override
    {
        if (!m_state || !m_state->m_isDirtyAccepted) return false;
        m_state->m_isDirty = true;
        ++m_state->m_dirtySetCount;
        return true;
    }

    bool ResetRenderNeeded() override
    {
        if (!m_state) return false;
        const bool wasDirty = m_state->m_isDirty;
        m_state->m_isDirty = false;
        return wasDirty;
    }

private:
    std::shared_ptr<AppPortState> m_state;
};
