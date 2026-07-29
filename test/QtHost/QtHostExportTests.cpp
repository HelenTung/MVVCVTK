#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"

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
    HostDataExportRequest exportRequest;
    exportRequest.outputPath = unicodeDir.u8string();
    exportRequest.format = HostDataExportFormat::Raw;
    return GetCaseResult(
        unicodeSession.BuildSession()
            && unicodeSession.SendRequest(std::move(reload))
            && !createError
            && unicodeSession.SendRequest(
                std::move(exportRequest)),
        "Export UTF-8 request facade acceptance") ? 0 : 1;
}
