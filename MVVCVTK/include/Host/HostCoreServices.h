#pragma once

#include <memory>

class GapAnalysisService;
class OrthogonalCropInteractionBridgeService;
class RawVolumeDataManager;
class SharedInteractionState;
class SharedStateBroadcaster;
class VolumeAnalysisService;

// core services 是 host/session 层的“窗口无关”组合结果，由 VtkAppHostSession 创建，
// 再交给 HostRenderViewSet 和 HostFeatureBindings 共享使用。
// 为什么独立成结构体：窗口数量、输入协议和 feature 激活会随上位机变化，但数据源、共享状态和算法服务
// 的生命周期应只跟一次 session 绑定，避免每个窗口各自创建一套 DataManager 或算法服务。
struct HostCoreServices {
    // 会话唯一体数据管理器；加载、裁切 submit 和孔隙分析都通过它读取同一份 vtkImageData。
    std::shared_ptr<RawVolumeDataManager> sharedDataMgr;
    // 状态事件源，向各视图广播加载、重载和交互状态变化。
    std::shared_ptr<SharedStateBroadcaster> sharedStateBroadcaster;
    // 会话共享状态真源；render view 和 feature 只读/更新这里，不各自缓存加载状态。
    std::shared_ptr<SharedInteractionState> sharedState;
    // 图像分析服务，例如直方图；它依赖 sharedDataMgr，但不持有窗口。
    std::shared_ptr<VolumeAnalysisService> imageAnalysis;
    // 孔隙算法服务，只负责计算结果；显示目标和 overlay 生命周期由 HostFeatureBindings 决定。
    std::shared_ptr<GapAnalysisService> gapAnalysis;
    // 裁切交互桥，连接裁切 feature 与 host 窗口；具体 reference/preview 窗口在激活请求到来后再绑定。
    std::shared_ptr<OrthogonalCropInteractionBridgeService> orthogonalCropBridge;
};

// 构建一次 session 的窗口无关服务集合。调用方再把它传入窗口层和 feature 绑定层，
// 因此这里不读取 Config::renderViews，也不安装任何热键。
HostCoreServices BuildHostCoreServices();
