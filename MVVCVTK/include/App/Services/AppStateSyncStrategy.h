#pragma once

#include "AppTypes.h"

class IAppStateSyncTarget {
public:
    virtual ~IAppStateSyncTarget() = default;
    virtual void RequestStrategyCacheClear() = 0;
    virtual void CenterCursor() = 0;
    virtual void MergePendingFlags(UpdateFlags flags) = 0;
    virtual void RequestDataRefresh() = 0;
    virtual void RequestLoadFailed() = 0;
    virtual void RequestSync() = 0;
};

class AppStateSyncStrategy {
public:
    void SendFlags(UpdateFlags flags, IAppStateSyncTarget& target) const {
        // DataReady 代表底层数据对象已经替换，旧 Strategy 持有的输入与派生缓存都可能失效，
        // 所以这里走“清缓存 -> 居中光标 -> 全量标志 -> 请求重建”的重路径，而不是普通增量刷新。
        if (GetFlagOn(flags, UpdateFlags::DataReady)) {
            target.RequestStrategyCacheClear();
            target.CenterCursor();
            target.MergePendingFlags(UpdateFlags::All);
            target.RequestDataRefresh();
            return;
        }

        // Spacing 改变会影响切片、坐标换算和体绘制采样，语义上等价于渲染管线基础输入发生变化，
        // 因此同样强制走一次全量重建，避免局部同步留下旧几何或旧采样参数。
        if (GetFlagOn(flags, UpdateFlags::Spacing)) {
            target.RequestStrategyCacheClear();
            target.MergePendingFlags(UpdateFlags::All);
            target.RequestDataRefresh();
            return;
        }

        // 失败链路只负责收敛到主线程清理现场，不再继续叠加普通增量标志。
        if (GetFlagOn(flags, UpdateFlags::LoadFailed)) {
            target.RequestLoadFailed();
            return;
        }

        // 其余事件都属于“状态还在、表现需刷新”的轻路径：只合并标志并请求下一帧同步。
        target.MergePendingFlags(flags);
        target.RequestSync();
    }
};
