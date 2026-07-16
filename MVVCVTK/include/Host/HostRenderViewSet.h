#pragma once

#include "Host/HostCoreServices.h"
#include "Host/HostSessionTypes.h"

#include <memory>
#include <string>
#include <vector>

class InteractiveService;
class VizService;
class StdRenderContext;
// runtime 是 HostRenderViewSet 内部的单视图运行态，组合“宿主配置 + 单窗口渲染 context + 业务 service”。
// 为什么不是 endpoint：runtime 允许 host/session 内部继续绑定 feature，而 endpoint 只给外部宿主观察窗口句柄。
struct HostRenderViewRuntime {
    // 上位机或 standalone main 下发的窗口声明，是 id/role/默认预览策略的真源。
    HostRenderViewConfig config;
    // 每个窗口自己的 VizService；共享数据来自 HostCoreServices，不在这里另起 DataManager。
    std::shared_ptr<VizService> service;
    // 单窗口渲染组装仍由 StdRenderContext 负责，HostRenderViewSet 不接管其内部 camera/interactor 细节。
    std::shared_ptr<StdRenderContext> context;
};

// HostRenderViewSet 只回答“有哪些窗口、它们是什么 role、怎样找到目标窗口”。
// 层级关系：
// 1. VtkAppHostSession 调用 Build 创建/接管窗口。
// 2. HostFeatureBindings 只通过 id/role 查询目标窗口，不知道默认五窗口布局。
// 3. 外部 Qt / 上位机只拿 endpoint，不直接碰 service。
// 这样它和 StdRenderContext 不重叠：StdRenderContext 负责单窗口渲染对象，HostRenderViewSet 负责多窗口集合语义。
// 输入事件不在这里安装；独立 VTK 调试热键由 HostHotkeyRouter 安装 input handler 并翻译成 typed command。
class HostRenderViewSet {
public:
    HostRenderViewSet();
    ~HostRenderViewSet();

    HostRenderViewSet(const HostRenderViewSet&) = delete;
    HostRenderViewSet& operator=(const HostRenderViewSet&) = delete;
    HostRenderViewSet(HostRenderViewSet&&) noexcept;
    HostRenderViewSet& operator=(HostRenderViewSet&&) noexcept;

    // 根据 host 配置构建一次窗口集合；configs 数量就是窗口数量，id 必须由宿主作为稳定外部事实提供。
    void Build(
        const HostCoreServices& core,
        const std::vector<HostRenderViewConfig>& configs);

    const std::vector<HostRenderViewRuntime>& GetViews() const;
    // id 查找用于上位机已绑定具体窗口的场景；找不到返回 nullptr，让调用方显式失败。
    const HostRenderViewRuntime* GetViewById(const std::string& id) const;
    // role 查找用于“第一个 Primary3D / 切片视图”这类语义选择，不承诺固定窗口序号。
    const HostRenderViewRuntime* GetFirstViewByRole(HostRenderViewRole role) const;
    // 单视图 selector 统一使用“id 优先，否则按 role fallback”，避免各调用点手写同一判断。
    const HostRenderViewRuntime* GetViewBySelector(
        const std::string& id,
        bool isRoleUsed,
        HostRenderViewRole role) const;
    // 初始加载和裁切默认参考需要一个可解释的主视图；没有 Primary3D 时按 3D role 再退到首视图。
    const HostRenderViewRuntime* GetPrimaryView() const;
    // standalone VTK 只能有一个阻塞事件循环承载点；Qt host 不调用 Start，因此不受该选择约束。
    const HostRenderViewRuntime* GetStandaloneStartView() const;

    // ids 和 roles 是显式作用域并集；二者都为空时返回空，避免漏配请求被解释为全窗口。
    std::vector<const HostRenderViewRuntime*> GetViewsByIdsAndRoles(
        const std::vector<std::string>& ids,
        const std::vector<HostRenderViewRole>& roles) const;
    // 只有请求允许使用配置默认值时才调用，避免裁切 preview 在未声明目标时接管新窗口。
    std::vector<const HostRenderViewRuntime*> GetCropPreviewViews() const;
    // 孔隙 overlay 默认角色也是显式 fallback；它描述可显示 overlay 的 role，不描述当前窗口数量。
    std::vector<const HostRenderViewRuntime*> GetGapOverlayViews() const;

    // 裁切 bridge 只需要 InteractiveService 列表；在这里降级接口，避免 feature 看到完整 runtime。
    std::vector<std::shared_ptr<InteractiveService>> BuildServices(
        const std::vector<const HostRenderViewRuntime*>& views) const;

    void SetInitialVisibility() const;
    void SendRenderAll() const;
    void SetInteractorsReady() const;
    std::vector<HostRenderViewEndpoint> BuildEndpoints() const;

    bool GetRoleIs3DView(HostRenderViewRole role) const;
    bool GetRoleIsSliceView(HostRenderViewRole role) const;
    bool GetRoleIsGapOverlayRole(HostRenderViewRole role) const;

private:
    // view set 独占多窗口 runtime 集合；移动对象时一起转移，外部 endpoint 不获得该所有权。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
