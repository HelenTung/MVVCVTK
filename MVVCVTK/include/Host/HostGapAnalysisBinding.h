#pragma once

#include "Host/HostCoreServices.h"
#include "Host/HostSessionTypes.h"

#include <memory>

class HostRenderViewSet;

// HostGapAnalysisBinding 是 host/session 层的孔隙分析适配器。
// 职责边界：
// 1. 接收上位机显式的显示目标、算法参数和事件泵窗口配置。
// 2. 把这些 host DTO 转成 GapAnalysisService 参数和 overlay 投递目标。
// 3. 不暴露 Qt、固定按键、窗口数量或 GapAnalysis 插件内部状态给调用方。
class HostGapAnalysisBinding final {
public:
    HostGapAnalysisBinding();
    ~HostGapAnalysisBinding();

    HostGapAnalysisBinding(const HostGapAnalysisBinding&) = delete;
    HostGapAnalysisBinding& operator=(const HostGapAnalysisBinding&) = delete;
    HostGapAnalysisBinding(HostGapAnalysisBinding&&) noexcept;
    HostGapAnalysisBinding& operator=(HostGapAnalysisBinding&&) noexcept;

    // 注册一次 session 的窗口无关 core 与窗口集合查询入口；这里不进入分析模式，也不预绑 overlay 目标。
    void Register(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);

    // 显式进入孔隙显示链路；请求必须给出 overlay 目标和算法参数。
    bool ActivateDisplay(const HostGapAnalysisActivationRequest& request);
    // 临时显隐 overlay，不退出显示模式，也不丢弃本轮分析缓存。
    bool ToggleOverlayVisibility();
    // 彻底退出显示模式，清除 pending 请求、缓存结果和已挂载 overlay。
    bool ExitDisplay();
    bool GetDisplayActive() const;

    // 绑定主线程 TimerEvent 轮询点；窗口由 HostGapAnalysisEventPumpConfig 显式指定。
    void AttachTimer(const HostGapAnalysisEventPumpConfig& eventPumpConfig);

private:
    // PIMPL 隐藏 VTK command、overlay strategy 和 GapAnalysisService 细节，避免 HostFeatureBindings 重新变胖。
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
