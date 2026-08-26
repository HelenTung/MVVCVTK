#pragma once

#include "Host/HostCoreServices.h"
#include "Host/Types/HostFeatureViewTypes.h"
#include "Host/Types/HostSessionTypes.h"

#include <array>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AbstractViewContext;
class AppTaskExecutor;
class AppDataPort;
class AppSessionPort;
class AppViewPort;
class FeatureViewService;
class OverlayService;
class RenderUpdatePort;

struct HostDataRoute final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    HostRenderMode mode = HostRenderMode::Volume;
    std::weak_ptr<AppDataPort> data;
};

struct HostViewRoute final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    std::weak_ptr<AppViewPort> view;
    std::weak_ptr<RenderUpdatePort> update;
    std::weak_ptr<AbstractViewContext> context;
    // 补偿失败时只停用目标 view；回调内部通过弱目录校验生命周期与 owner thread。
    std::function<bool()> stopView;
};

struct HostInputRoute final {
    std::string id;
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    std::weak_ptr<AbstractViewContext> context;
};

// Router 只依赖这一份类型化目录；实现统一承担 owner-thread 与生命周期 gate。
class IHostViewDirectory {
public:
    virtual ~IHostViewDirectory() = default;

    // target 未指定 id/role 时选择 primary data route。
    virtual std::optional<HostDataRoute> GetDataRoute(
        const HostViewTarget& target) const = 0;
    virtual std::optional<HostViewRoute> GetViewRoute(
        const HostViewTarget& target) const = 0;
    virtual std::weak_ptr<AppSessionPort> GetSessionPort() const = 0;
    virtual std::vector<HostInputRoute> GetInputRoutes(
        const HostViewTargets& targets) const = 0;
    virtual bool StopView(std::string_view viewId) = 0;
};

// Session 内的多 View runtime owner/coordinator：维护 topology、context 生命周期、
// 能力目录与跨 View 数据提交；单 View pipeline 仍由 AppRuntime 负责。
// 层级关系：
// 1. VtkAppHostSession 调用 Build 创建/接管窗口。
// 2. Feature 只通过 id/role 查询目标窗口，不知道默认五窗口布局。
// 3. 外部 Qt / 上位机只拿 endpoint，不直接碰内部端口。
// 输入事件不在这里安装；独立 VTK 调试热键由 HostHotkeyRouter 安装 input handler 并翻译成 typed command。
class HostViewRuntimeRegistry {
public:
    HostViewRuntimeRegistry();
    ~HostViewRuntimeRegistry();

    HostViewRuntimeRegistry(const HostViewRuntimeRegistry&) = delete;
    HostViewRuntimeRegistry& operator=(const HostViewRuntimeRegistry&) = delete;
    HostViewRuntimeRegistry(HostViewRuntimeRegistry&&) noexcept;
    HostViewRuntimeRegistry& operator=(HostViewRuntimeRegistry&&) noexcept;

    // 根据 host 配置构建一次窗口集合；configs 数量就是窗口数量，id 必须由宿主作为稳定外部事实提供。
    bool Build(
        const HostCoreServices& core,
        const std::vector<HostRenderViewConfig>& configs);

    // 返回目标视图的独立业务状态快照；调用方不获得 runtime/service 所有权。
    std::optional<HostRenderViewState> GetViewState(
        const HostViewTarget& target) const;
    // 按拓扑顺序返回全部视图状态快照，容器和节点均为值复制。
    std::vector<HostRenderViewState> GetViewStates() const;
    std::weak_ptr<IHostViewDirectory> GetViewDirectory() const;
    std::vector<HostFeatureView> GetFeatureViews(
        const HostViewTargets& targets) const;
    std::shared_ptr<FeatureViewService> GetFeaturePort(
        const std::string& viewId) const;
    std::shared_ptr<OverlayService> GetOverlayPort(
        const std::string& viewId) const;
    std::optional<HostInputView> GetInputView(
        const HostViewTarget& target) const;
    // 非空 ids 绑定 Feature 的精确活动视图，空 ids 解除绑定。
    bool SetFeatureViews(
        const std::string& featureId,
        const std::vector<std::string>& viewIds);
    // 返回注册表最后确认成功的活动 View IDs，供跨层 detach 事务失败时恢复。
    std::vector<std::string> GetFeatureViewIds(
        const std::string& featureId) const;
    bool SetViewStatus(
        const std::vector<std::string>& viewIds,
        const std::string& status) const;
    // Host 内部唯一窗口换绑入口；Qt 公共 Session/DTO 头不因此改变。
    bool SetViewWindow(
        const std::string& viewId,
        vtkSmartPointer<vtkRenderWindow> renderWindow);
    // Session/测试只提交动作，不取得 context 或 port 所有权。
    bool SetTimerHandler(
        const HostViewTarget& target,
        std::function<void()> handler) const;
    bool ClearTimerHandler(const HostViewTarget& target) const;
    bool SetModelMatrix(
        const HostViewTarget& target,
        const std::array<double, 16>& modelToWorld) const;
    bool SendViewUpdates(const HostViewTarget& target) const;
    bool StartStandaloneView() const;
    // 仅供 Session 提交有界后台读取；调用方不能直接管理 worker 生命周期。
    std::shared_ptr<AppTaskExecutor> GetTaskExecutor() const;
    // Feature 已清理全部借用对象后，由 owner thread 失效本轮 view lease。
    bool StopLease();

    bool SetInitialVisibility() const;
    bool SendRenderAll() const;
    bool SetInteractorsReady() const;
    std::vector<HostRenderViewEndpoint> BuildEndpoints() const;

    bool GetRoleIs3DView(HostRenderViewRole role) const;
    bool GetRoleIsSliceView(HostRenderViewRole role) const;

private:
    // view set 独占多窗口 runtime 集合；移动对象时一起转移，外部 endpoint 不获得该所有权。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
