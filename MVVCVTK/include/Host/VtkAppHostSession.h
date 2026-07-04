#pragma once

#include "Host/HostSessionTypes.h"

#include <memory>
#include <string>
#include <vector>

// VtkAppHostSession 是上位机 / standalone main 面向项目的 host 门面。
// 职责边界：
// 1. 组装 core services、render views 和 feature bindings，控制一次 session 生命周期。
// 2. 对外暴露稳定命令入口，让 Qt / 上位机不用知道 feature 内部类。
// 3. 不保存固定窗口数，也不把具体热键写进插件；这些事实都由 Config 明确传入。
class VtkAppHostSession final {
public:
    // Config 是宿主输入的聚合 DTO：main 现在模拟上位机填它，未来 Qt / 上位机也填同一结构。
    // 它只描述外部事实和输入协议，不创建对象，因此可安全地在 Initialize 前完整校验。
    struct Config {
        // standalone hotkey 映射；关闭 standalone 输入时这些值不会被消费。
        HostHotkeyBindings hotkeys;
        // 可选启动加载命令；路径、spacing、origin 必须由宿主显式填入。
        InitialVolumeLoadConfig initialVolume;
        // 空列表表示使用独立 VTK host 的默认调试视图；非空则完全由宿主决定窗口拓扑。
        std::vector<HostRenderViewConfig> renderViews;
        // RenderContext 热键仅属于独立 VTK 调试宿主；Qt / 上位机默认不接管窗口事件。
        HostRenderContextInputConfig renderContextInput;
        // standalone hotkey 绑定属于当前 VTK 调试 host；Qt / 上位机可关闭它，只通过显式命令驱动 feature。
        HostCommandInputConfig commandInput;
    };

    VtkAppHostSession();
    explicit VtkAppHostSession(Config config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    void Initialize();
    void Start();

    // 以下是上位机 / Qt host 可以调用的稳定 feature 命令入口。
    // session 只做命令转发和目标窗口解析，不把具体按键或固定五窗口假设写进插件。
    bool ActivateOrthogonalCrop(const HostOrthogonalCropActivationRequest& request);
    bool ToggleOrthogonalCropBox(const HostOrthogonalCropActivationRequest& request);
    bool ToggleOrthogonalCropPlane(const HostOrthogonalCropActivationRequest& request);
    bool ToggleOrthogonalCropPreview(
        const HostOrthogonalCropActivationRequest& request,
        HostCropPreviewMode previewMode);
    bool ApplyOrthogonalCropSubmit(const HostOrthogonalCropActivationRequest& request);
    bool ExitOrthogonalCrop();
    bool ActivateGapAnalysisDisplay(const HostGapAnalysisActivationRequest& request);
    bool ToggleGapAnalysisOverlayVisibility();
    bool ExitGapAnalysisDisplay();

    // endpoint 指针只在当前 session 生命周期内有效；外部宿主按 id/role 绑定 widget，不接管所有权。
    const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints() const;
    const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& id) const;
    const HostRenderViewEndpoint* GetPrimaryRenderViewEndpoint() const;

    // 默认五视图只是保持现有独立 VTK 调试体验，不代表项目架构要求固定窗口数。
    static std::vector<HostRenderViewConfig> BuildDefaultRenderViewConfigs();

private:
    // PIMPL 隐藏 HostCoreServices、HostRenderViewSet 等组装细节，减少上位机包含本头文件时的编译依赖。
    // 这里不是为了抽象 feature，而是为了让 public header 只暴露稳定 host 命令和 endpoint 类型。
    struct Impl;
    // session 独占 Impl 生命周期；移动 session 时一起移动，避免 VTK observer 回调看到半拆状态。
    std::unique_ptr<Impl> m_impl;
};
