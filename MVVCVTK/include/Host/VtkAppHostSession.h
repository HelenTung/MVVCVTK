#pragma once

#include "Host/HostSessionTypes.h"

#include <functional>
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
    // 构造 session 时整份移入 Impl，BuildSession 只消费该快照；后续显式命令不会回写 Config。
    // 它只描述外部事实和输入协议，不创建对象，因此可安全地在 BuildSession 前完整校验。
    struct Config {
        // 初始化标志位
        bool isInitialRenderEnabled = true;
        // standalone hotkey 映射；关闭 standalone 输入时这些值不会被消费。
        HostHotkeyBindings hotkeys;
        // 可选启动加载命令；路径、spacing、origin 必须由宿主显式填入。
        InitialVolumeLoadConfig initialVolume;
        // 窗口拓扑必须由宿主显式传入；空列表表示当前 session 没有窗口，不再由构造函数暗补调试视图。
        std::vector<HostRenderViewConfig> renderViews;
        // RenderContext 热键仅属于独立 VTK 调试宿主；Qt / 上位机默认不接管窗口事件。
        HostContextInput renderContextInput;
        // 导出命令参数属于 host I/O 输入，不放进窗口热键范围配置。
        HostDataExportConfig dataExport;
        // standalone hotkey 绑定属于当前 VTK 调试 host；Qt / 上位机可关闭它，只通过显式命令驱动 feature。
        HostCommandInputConfig commandInput;
        // host/session 主线程 tick 事件泵配置；standalone/Qt 都必须显式选择承载 TimerEvent 的窗口。
        HostTimerEventPumpConfig timerEventPump;
    };

    explicit VtkAppHostSession(Config config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    void BuildSession();
    void Start();

    // 显式体数据加载命令入口；session 只构造 request，不直接调用底层 service。
    bool LoadVolume(
        const InitialVolumeLoadConfig& request,
        std::function<void(bool isSuccess)> onComplete = nullptr);

    // 以下是上位机 / Qt host 可以调用的稳定 feature 命令入口。
    // session 只做命令转发和目标窗口解析，不把具体按键或固定五窗口假设写进插件。
    bool StartCrop(const HostCropViewRequest& request);
    bool SwitchCropBox(const HostCropViewRequest& request);
    bool SwitchCropPlane(const HostCropViewRequest& request);
    bool SwitchCropView(
        const HostCropViewRequest& request,
        HostCropPreviewMode previewMode);
    bool SendCrop(const HostCropViewRequest& request);
    bool ExitCrop();
    bool StartGapView(const HostGapViewRequest& request);
    bool SwitchGapLayer();
    bool ExitGapView();
    // 运行期视图配置命令入口；session 只记录请求并统一交给 router 分发。
    bool SetViewConfig(const HostViewConfig& config);
    // 显式导出命令入口；session 只转交上位机配置，不解析具体导出 service。
    bool ExportData(
        const HostDataExportConfig& dataExportConfig,
        std::function<void(bool isSuccess)> onComplete = nullptr);

    // endpoint 指针只在当前 session 生命周期内有效；外部宿主按 id/role 绑定 widget，不接管所有权。
    // 读取 endpoint 会懒 BuildSession，避免 Qt host 构造 session 后立即取窗口句柄时拿到空缓存。
    const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints();
    const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& id);
    const HostRenderViewEndpoint* GetPrimaryEndpoint();

private:
    // PIMPL 隐藏 HostCoreServices、HostRenderViewSet 等组装细节，减少上位机包含本头文件时的编译依赖。
    // 这里不是为了抽象 feature，而是为了让 public header 只暴露稳定 host 命令和 endpoint 类型。
    // session 独占 Impl 生命周期；移动 session 时一起移动，避免 VTK observer 回调看到半拆状态。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
