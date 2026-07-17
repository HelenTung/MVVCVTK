#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <vector>

int GetLoadFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    int failureCount = 0;
    HostLoadRequest load;
    load.geometry = { { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    failureCount += GetCaseResult(
        !session.SendData({ HostDataAction::LoadFile, load }),
        "Load missing path rejection") ? 0 : 1;

    HostReloadRequest reload;
    reload.voxels.assign(7, 1.0f);
    reload.geometry = load.geometry;
    failureCount += GetCaseResult(
        !session.SendData({ HostDataAction::ReloadBuffer, std::move(reload) }),
        "Reload voxel-count rejection") ? 0 : 1;
    return failureCount;
}
