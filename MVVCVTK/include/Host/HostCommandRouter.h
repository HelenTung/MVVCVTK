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
    GapSwitch,
    GapOverlay,
    GapExit,
    Export,
    Hotkeys,
    ViewConfig
};

// HostCommandRouter 是 host 命令分发器，不是 feature，也不是业务 service。
// HostCommandRouterRequest 是 session->router 的跨对象命令包，不是 host 对外请求模型。
// 对外业务请求仍由 VtkAppHostSession 暴露；router 只消费 session 已经归一化后的命令。
struct HostCommandRouterRequest {
    HostCommandKind command = HostCommandKind::None;

    InitialVolumeLoadConfig initialVolume;
    std::function<void(bool isSuccess)> loadComplete;

    HostCropViewRequest cropViewRequest;
    HostCropPreviewMode cropPreviewMode = HostCropPreviewMode::KeepInside;

    HostGapViewRequest gapViewRequest;

    HostDataExportConfig dataExportConfig;
    std::function<void(bool isSuccess)> dataExportComplete;

    HostViewConfig viewConfig;

    HostContextInput renderContextInput;
    HostCommandInputConfig commandInput;
    HostHotkeyBindings hotkeys;
};

class HostCommandRouter final : public std::enable_shared_from_this<HostCommandRouter> {
public:
    HostCommandRouter(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews,
        std::shared_ptr<HostFeatureBindings> featureBindings);
    ~HostCommandRouter();

    bool DispatchCommand(HostCommandRouterRequest request) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
