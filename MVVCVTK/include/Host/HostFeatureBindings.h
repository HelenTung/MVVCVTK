#pragma once

#include "Host/HostCoreServices.h"
#include "Host/HostRenderViewSet.h"
#include "Host/HostSessionTypes.h"

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
// 继承 enable_shared_from_this 是为了把 VTK observer 的回调生命周期收敛到 session 持有的 shared_ptr；
// observer 只保存 weak_ptr，窗口事件晚到时不会反向延长或访问已销毁的 HostFeatureBindings。
class HostFeatureBindings : public std::enable_shared_from_this<HostFeatureBindings> {
public:
    HostFeatureBindings();
    ~HostFeatureBindings();

    // 注册 core 能力和 submit 回调；这里不绑定 reference/preview 窗口，因为目标必须来自后续激活请求。
    void AttachFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);

    // 显式激活裁切链路，只解析参考/预览窗口并刷新输入图像，不负责具体按键语义。
    bool StartCrop(const HostCropRequest& request);
    // 显式进入孔隙显示链路；只在请求提供目标窗口后才启动一次算法请求。
    bool StartGapView(const HostGapRequest& request);
    // standalone 输入使用的“进入或切换显示”命令；具体状态由 GapAnalysis feature 自己判断。
    bool SwitchGapView(const HostGapRequest& request);
    // 临时隐藏/显示已进入模式的孔隙 overlay；不清除算法结果，也不退出显示模式。
    bool SwitchGapLayer();
    // 彻底退出孔隙显示模式；清除 overlay 缓存和 pending 请求。
    bool ExitGapView();
    bool GetGapView() const;

    bool SwitchCropBox(const HostCropRequest& request);
    bool SwitchCropPlane(const HostCropRequest& request);
    bool SwitchCropView(
        const HostCropRequest& request,
        HostCropPreviewMode previewMode);
    bool SendCrop(const HostCropRequest& request);
    bool ExitCrop();
    bool ExitFeature();
    bool GetCropActive() const;
    void ClearCropInput() const;
    std::function<bool()> BuildCropInput();

    // 通用 TimerEvent pump 是 host/session 主线程收敛点；具体 feature 只在 tick 中消费自己的 pending 状态。
    void AttachHostTimer(const HostTimerEventPumpConfig& eventPumpConfig);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
