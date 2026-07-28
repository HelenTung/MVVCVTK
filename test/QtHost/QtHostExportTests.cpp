#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"

#include <filesystem>
#include <system_error>
#include <utility>

int GetExportFailCount()
{
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
    return GetCaseResult(
        unicodeSession.BuildSession()
            && unicodeSession.SendData({ HostDataAction::ReloadBuffer, std::move(reload) })
            && !createError
            && unicodeSession.SendData({
                HostDataAction::ExportData,
                HostDataExportRequest{
                    unicodeDir.u8string(),
                    HostDataExportFormat::Raw,
                    {} } }),
        "Export UTF-8 request facade acceptance") ? 0 : 1;
}
