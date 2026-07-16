#pragma once

#include "Host/HostCoreServices.h"
#include "Host/HostCommandTypes.h"
#include "Host/HostRenderViewSet.h"

#include <functional>
#include <memory>

class StdRenderContext;
// HostFeatureBindings 是 host/session 到 feature 的绑定边界，由 VtkAppHostSession 持有。
// 它上接 HostRenderViewSet 的窗口查询，下接裁切 bridge / 孔隙算法服务，职责只限“把显式宿主命令绑定到 feature 能力”。
// 为什么不放进 feature 插件：窗口 role、standalone hotkey、VTK observer 生命周期都是宿主事实，插件应保持可复用算法/交互能力。
//
// 行为边界：
// 1. 初始化只注册能力和回调，不默认进入 feature 模式。
// 2. 激活请求必须带目标窗口语义，由上位机或 standalone hotkey 显式触发。
// 3. 具体功能键只存在于 standalone host 输入层，不进入插件代码。
class HostFeatureBindings final {
public:
    HostFeatureBindings();
    ~HostFeatureBindings();

    // 注册 core 能力和 submit 回调；这里不绑定 reference/preview 窗口，因为目标必须来自后续激活请求。
    void AttachFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);

    // 统一接收领域命令；具体 Crop/Gap action 在 Pimpl 内分发并共享同一 session 生命周期。
    bool SendCommand(HostFeatureCommand command);
    bool GetGapView() const;
    bool GetCropActive() const;
    void ClearCropInput() const;
    bool SendCropInput();

    // 通用 TimerEvent pump 是 host/session 主线程收敛点；具体 feature 只在 tick 中消费自己的 pending 状态。
    void AttachHostTimer(const HostTimerEventPumpConfig& eventPumpConfig);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
