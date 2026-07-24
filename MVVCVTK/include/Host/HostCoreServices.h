#pragma once

#include <memory>
class GapAnalysisService;
class RawVolumeDataManager;
class SharedInteractionState;
class SharedStateBroadcaster;

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
    // 孔隙 feature 服务负责计算、显示模式状态和 overlay 生命周期；host 只注入目标和主线程 tick。
    std::shared_ptr<GapAnalysisService> gapAnalysis;
};
