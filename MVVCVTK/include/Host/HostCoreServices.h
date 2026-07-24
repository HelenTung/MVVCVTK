#pragma once

#include "AppInterfaces.h"

#include <functional>
#include <memory>

class RawVolumeDataManager;
class SharedInteractionState;
class SharedStateBroadcaster;

// core services 是 host/session 层的“窗口无关”组合结果，由 VtkAppHostSession 创建，
// 再交给 HostRenderViewSet 和通用 HostFeatureContext 使用。
// 为什么独立成结构体：窗口数量、输入协议和 feature 激活会随上位机变化，但数据源与共享状态
// 的生命周期应只跟一次 session 绑定，避免每个窗口各自创建一套 DataManager。
struct HostCoreServices {
    // 会话唯一体数据管理器；加载与 feature 原子发布都通过它访问同一份图像批次。
    std::shared_ptr<RawVolumeDataManager> sharedDataMgr;
    // 状态事件源，向各视图广播加载、重载和交互状态变化。
    std::shared_ptr<SharedStateBroadcaster> sharedStateBroadcaster;
    // 会话共享状态真源；render view 和 feature 只读/更新这里，不各自缓存加载状态。
    std::shared_ptr<SharedInteractionState> sharedState;

    // 返回只弱观察数据真源的冻结快照读取能力；core 销毁后返回空快照。
    std::function<ImageSnapshot()> GetImageReader() const;
    // 返回 expected snapshot CAS 发布能力；只弱观察数据真源与共享交互状态。
    std::function<bool(
        ImageState,
        const ImageSnapshot&,
        ImageSnapshot&)> GetImageWriter() const;
};
