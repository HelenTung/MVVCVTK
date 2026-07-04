#pragma once

#include "Host/HostCoreServices.h"
#include "Host/HostRenderViewSet.h"
#include "Host/HostSessionTypes.h"

#include <functional>
#include <memory>

class IGapAnalysisDisplayController;

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
    HostFeatureBindings() = default;

    // 注册 core 能力和 submit 回调；这里不绑定 reference/preview 窗口，因为目标必须来自后续激活请求。
    void RegisterFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);

    // 显式激活裁切链路，只解析参考/预览窗口并刷新输入图像，不负责具体按键语义。
    bool ActivateOrthogonalCrop(const HostOrthogonalCropActivationRequest& request);
    // 显式进入孔隙显示链路；只在请求提供目标窗口后才启动一次算法请求。
    bool ActivateGapAnalysisDisplay(const HostGapAnalysisActivationRequest& request);
    // 临时隐藏/显示已进入模式的孔隙 overlay；不清除算法结果，也不退出显示模式。
    bool ToggleGapAnalysisOverlayVisibility();
    // 彻底退出孔隙显示模式；清除 overlay 缓存和 pending 请求。
    bool ExitGapAnalysisDisplay();
    bool GetGapAnalysisDisplayActive() const;

    bool ToggleInteractiveCrop(const HostOrthogonalCropActivationRequest& request);
    bool ToggleInteractivePlanarCrop(const HostOrthogonalCropActivationRequest& request);
    bool ToggleCropPreview(
        const HostOrthogonalCropActivationRequest& request,
        HostCropPreviewMode previewMode);
    bool ApplyCropSubmit(const HostOrthogonalCropActivationRequest& request);
    bool ExitInteractiveCrop();
    bool GetInteractiveCropActive() const;
    std::function<bool()> BuildOrthogonalCropInputRefreshHandler() const;

    // 仅 standalone 调试入口使用。真实上位机应直接调用 public feature 命令，不安装固定按键。
    void AttachStandaloneHotkeys(
        const HostCommandInputConfig& inputConfig,
        const HostHotkeyBindings& hotkeys);
    // Timer 只是把异步孔隙算法结果提交回显示控制器；没有显式显示请求时它保持空转。
    void AttachGapAnalysisTimer();

private:
    // 从共享 DataManager 把当前 vtkImageData 交给裁切 bridge；加载完成和显式激活都会复用同一刷新点。
    bool RefreshOrthogonalCropInputImage();
    // 按 request 解析 reference view 和 preview views，并把这些 host 窗口注入裁切 bridge。
    bool ConfigureOrthogonalCrop(const HostOrthogonalCropActivationRequest& request);

    // 会话核心服务的拷贝，shared_ptr 成员共享所有权；HostFeatureBindings 不单独创建数据或算法服务。
    HostCoreServices m_core;
    // 非拥有指针，指向 VtkAppHostSession::Impl 中的窗口集合；session 保证其生命周期覆盖本对象。
    const HostRenderViewSet* m_renderViews = nullptr;
    // 孔隙显示控制器只管理 overlay 显隐和结果缓存；用接口保存是为了把 timer observer 和 public 命令都收敛到同一状态机。
    std::shared_ptr<IGapAnalysisDisplayController> m_gapAnalysisOverlayController;
};
