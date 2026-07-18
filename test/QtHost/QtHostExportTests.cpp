#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <filesystem>
#include <system_error>
#include <utility>

int GetExportFailCount()
{
    VtkAppHostSession session(HostSessionConfig{});
    int failureCount = 0;
    failureCount += GetCaseResult(
        !session.SendData({ HostDataAction::ExportVolume,
            HostVolumeExportRequest{} }),
        "Export empty path rejection") ? 0 : 1;
    failureCount += GetCaseResult(
        !session.SendData({ HostDataAction::ExportVolume,
            HostVolumeExportRequest{ u8"不存在/导出 é.raw" } }),
        "Export UTF-8 unavailable session rejection") ? 0 : 1;

    HostRenderViewConfig view;
    view.id = "utf8-export";
    view.role = HostRenderViewRole::Primary3D;
    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    VtkAppHostSession unicodeSession(std::move(config));
    HostReloadRequest reload;
    reload.voxels.assign(8, 1.0f);
    reload.geometry = { { 2, 2, 2 }, { 1.0f, 1.0f, 1.0f }, {} };
    const auto unicodeDir = std::filesystem::temp_directory_path()
        / std::filesystem::u8path(u8"MVVCVTK_导出_é");
    std::error_code createError;
    std::filesystem::create_directories(unicodeDir, createError);
    failureCount += GetCaseResult(
        unicodeSession.BuildSession()
            && unicodeSession.SendData({ HostDataAction::ReloadBuffer, std::move(reload) })
            && !createError
            && unicodeSession.SendData({ HostDataAction::ExportVolume,
                HostVolumeExportRequest{ (unicodeDir
                    / std::filesystem::u8path(u8"导出 é.raw")).u8string() } }),
        "Export UTF-8 request facade acceptance") ? 0 : 1;
    return failureCount;
}
