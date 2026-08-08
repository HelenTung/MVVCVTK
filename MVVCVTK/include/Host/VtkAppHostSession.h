#pragma once

#include "Host/Types/HostInputTypes.h"
#include "Host/Types/HostRequest.h"
#include "Host/Types/HostSessionTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vtkSmartPointer.h>

class HostFeature;

// 一次 VTK 宿主会话的顶层 owner：惰性组装共享数据/状态服务、窗口拓扑、feature 注册、
// 命令路由和 standalone 输入适配。外部只通过 HostRequest/Endpoint 交互，不直接依赖 AppService。
// Endpoint 中的 VTK 裸指针由本会话内部窗口持有；会话移动、重建或析构后不得继续缓存使用。
class VtkAppHostSession final {
public:
    explicit VtkAppHostSession(HostSessionConfig config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    // 幂等构建；首次成功后复用既有服务和窗口，空 renderViews 或任一构建步骤失败返回 false。
    bool BuildSession();
    // 把主线程 TimerEvent pump 绑定到指定视图；重复调用会替换旧 timer handler。
    bool AttachTimer(const HostTimerConfig& config);
    // 替换 standalone 按键 observer；主体热键直接进入统一请求路由。
    bool AttachHotkeys(const HostHotkeyConfig& config);
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    bool DetachFeature(const HostFeature& feature);
    // 仅用于 standalone VTK 事件循环；Qt host 已有外部事件循环时不调用。
    bool Start();

    bool SendRequest(
        HostRequest&& request,
        HostCompleteCallback onComplete = nullptr);

    // 返回会话内部 endpoint 集合的只读引用；引用和元素地址只在本会话拓扑不变且存活期间有效。
    const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints();
    const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& viewId);
    const HostRenderViewEndpoint* GetPrimaryEndpoint();
    // 返回指定视图的独立状态快照；查询失败返回空，不改变既有 SendRequest 流程。
    std::optional<HostRenderViewState> GetRenderViewState(
        const HostViewTarget& target);
    // 按会话拓扑顺序返回所有视图的独立状态快照。
    std::vector<HostRenderViewState> GetRenderViewStates();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
