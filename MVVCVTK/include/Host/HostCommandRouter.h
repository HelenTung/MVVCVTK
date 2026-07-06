#pragma once

#include "Host/HostSessionTypes.h"

#include <functional>
#include <memory>

class HostFeatureBindings;
class HostRenderViewSet;
struct HostCoreServices;

enum class HostCommandKind {
    None,
    Load,
    CropStart,
    CropBox,
    CropPlane,
    CropPreview,
    CropApply,
    CropExit,
    FeatureExit,
    GapStart,
    GapToggle,
    GapOverlay,
    GapExit,
    Export,
    Hotkeys
};

// HostCommandRouter 是 host 命令分发器，不是 feature，也不是业务 service。
// 外部只提交一个命令类型和它需要的配置；具体导出、feature、standalone 输入安装都在私有函数里分发。
struct HostCommandRouterRequest {
    HostCommandKind command = HostCommandKind::None;

    InitialVolumeLoadConfig initialVolume;
    std::function<void(bool success)> loadComplete;

    HostOrthogonalCropActivationRequest orthogonalCropRequest;
    HostCropPreviewMode cropPreviewMode = HostCropPreviewMode::KeepInside;

    HostGapAnalysisActivationRequest gapAnalysisRequest;

    HostDataExportConfig dataExportConfig;
    std::function<void(bool success)> dataExportComplete;

    HostRenderContextInputConfig renderContextInput;
    HostCommandInputConfig commandInput;
    HostHotkeyBindings hotkeys;
};

class HostCommandRouter final : public std::enable_shared_from_this<HostCommandRouter> {
public:
    HostCommandRouter(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews,
        std::shared_ptr<HostFeatureBindings> featureBindings);

    bool DispatchHostCommand(HostCommandRouterRequest request) const;

private:
    bool DispatchInitialVolumeLoad(
        const InitialVolumeLoadConfig& initialVolume,
        std::function<void(bool success)> loadComplete) const;
    bool DispatchFeatureCommand(const HostCommandRouterRequest& request) const;
    bool DispatchDataExport(
        const HostDataExportConfig& dataExportConfig,
        std::function<void(bool success)> onComplete) const;
    bool AttachStandaloneHotkeys(const HostCommandRouterRequest& request) const;

    const HostCoreServices* m_core = nullptr;
    const HostRenderViewSet* m_renderViews = nullptr;
    std::weak_ptr<HostFeatureBindings> m_featureBindings;
};
