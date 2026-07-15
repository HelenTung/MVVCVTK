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
    ViewConfig,
    // 追加在末尾，避免改变已有命令枚举值。
    Reload
};

// HostCommandRouter 是 host 命令分发器，不是 feature，也不是业务 service。
// HostCommandRouterRequest 是 session->router 的跨对象命令包，不是 host 对外请求模型。
// 对外业务请求仍由 VtkAppHostSession 暴露；router 只消费 session 已经归一化后的命令。
struct HostCommandRouterRequest {
    // 命令判别字段；None 必定拒绝，其余 payload 只按下面标明的 command 分支解释。
    HostCommandKind command = HostCommandKind::None;

    // 仅 Load 有效；包含路径、是否启动加载及可选物理 geometry。
    InitialVolumeLoadConfig initialVolume;
    // 仅 Load 有效；分发时移动到异步加载链，空函数表示调用方不接收完成通知。
    std::function<void(bool isSuccess)> loadComplete;

    // CropStart/CropBox/CropPlane/CropPreview/CropApply 有效；CropExit 不读取该字段。
    HostCropViewRequest cropViewRequest;
    // 仅 CropPreview 有效；默认 KeepInside 只避免值未初始化，不会让其他命令隐式启动预览。
    HostCropPreviewMode cropPreviewMode = HostCropPreviewMode::KeepInside;

    // GapStart/GapSwitch 有效；GapOverlay/GapExit 不读取目标或算法字段。
    HostGapViewRequest gapViewRequest;

    // Export 时是本次导出请求，Hotkeys 时是后续热键导出的基础配置；其他命令不读取。
    HostDataExportConfig dataExportConfig;
    // 仅 Export 有效；分发时移动到选中的异步导出链，Hotkeys 不消费该 callback。
    std::function<void(bool isSuccess)> dataExportComplete;

    // 仅 ViewConfig 有效；optional 子字段按存在性做局部写入。
    HostViewConfig viewConfig;

    // 以下三个字段仅 Hotkeys 有效，分别描述 context 输入范围、feature 输入范围和键位映射。
    HostContextInput renderContextInput;
    HostCommandInputConfig commandInput;
    HostHotkeyBindings hotkeys;

    // 仅 Reload 有效；payload 自有体素，分发进入 service 后可立即释放。
    HostVolumeBufferRequest volumeBuffer;
    // 仅 Reload 有效；Host 前置拒绝不调用，底层 task/admission 拒绝仍可能异步回传 false。
    std::function<void(bool isSuccess)> reloadComplete;
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
    // router 独占实现和 observer 注册状态；析构 Impl 时统一解除 host 输入绑定。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
