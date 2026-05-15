#pragma once

#include "AppTypes.h"

class IAppStateSyncTarget {
public:
    virtual ~IAppStateSyncTarget() = default;
    virtual void SetStrategyCacheClearRequested() = 0;
    virtual void SetCursorCentered() = 0;
    virtual void SetPendingFlagsMerged(UpdateFlags flags) = 0;
    virtual void SetDataRefreshRequested() = 0;
    virtual void SetLoadFailedRequested() = 0;
    virtual void SetSyncRequested() = 0;
};

class AppStateSyncStrategy {
public:
    void SetFlagsHandled(UpdateFlags flags, IAppStateSyncTarget& target) const {
        if (HasFlag(flags, UpdateFlags::DataReady)) {
            target.SetStrategyCacheClearRequested();
            target.SetCursorCentered();
            target.SetPendingFlagsMerged(UpdateFlags::All);
            target.SetDataRefreshRequested();
            return;
        }

        if (HasFlag(flags, UpdateFlags::Spacing)) {
            target.SetStrategyCacheClearRequested();
            target.SetPendingFlagsMerged(UpdateFlags::All);
            target.SetDataRefreshRequested();
            return;
        }

        if (HasFlag(flags, UpdateFlags::LoadFailed)) {
            target.SetLoadFailedRequested();
            return;
        }

        target.SetPendingFlagsMerged(flags);
        target.SetSyncRequested();
    }
};