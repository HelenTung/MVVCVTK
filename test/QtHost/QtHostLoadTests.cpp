#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <utility>
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
    load.filePath = u8"不存在/体数据 é.raw";
    failureCount += GetCaseResult(
        !session.SendData({ HostDataAction::LoadFile, load }),
        "Load UTF-8 missing path rejection") ? 0 : 1;

    HostRenderViewConfig view;
    view.id = "utf8-load";
    view.role = HostRenderViewRole::Primary3D;
    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession unicodeSession(std::move(config));
    failureCount += GetCaseResult(
        unicodeSession.BuildSession()
            && unicodeSession.SendData({ HostDataAction::LoadFile, load }),
        "Load UTF-8 request facade acceptance") ? 0 : 1;

    HostReloadRequest reload;
    reload.voxels.assign(7, 1.0f);
    reload.geometry = load.geometry;
    failureCount += GetCaseResult(
        !session.SendData({ HostDataAction::ReloadBuffer, std::move(reload) }),
        "Reload voxel-count rejection") ? 0 : 1;
    return failureCount;
}
