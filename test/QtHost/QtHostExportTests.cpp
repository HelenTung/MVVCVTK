#include "QtHostMethodCases.h"

#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"

#include <vtkCommand.h>
#include <vtkRenderWindowInteractor.h>

#include <chrono>
#include <filesystem>
#include <thread>
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
    bool isReloadComplete = false;
    bool isReloadSucceeded = false;
    bool isExportComplete = false;
    bool isExportSucceeded = false;
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView.viewId = "utf8-export";
    const bool isBuilt = unicodeSession.BuildSession();
    const bool isReloadSent = isBuilt
        && unicodeSession.SendRequest(
            std::move(reload),
            [&isReloadComplete, &isReloadSucceeded](const bool value) {
                isReloadSucceeded = value;
                isReloadComplete = true;
            });
    const bool isTimerSet = isReloadSent
        && unicodeSession.AttachTimer(timer);
    const bool isStarted = isTimerSet && unicodeSession.Start();
    const auto* endpoint = unicodeSession.GetPrimaryEndpoint();
    const auto sendTimer = [endpoint]() {
        if (!endpoint || !endpoint->interactor) return false;
        int timerId = endpoint->interactor->GetTimerEventId();
        if (timerId == 0) {
            for (int candidate = 1; candidate <= 64; ++candidate) {
                if (endpoint->interactor->GetTimerDuration(candidate) != 0) {
                    timerId = candidate;
                    break;
                }
            }
        }
        if (timerId == 0) return false;
        endpoint->interactor->InvokeEvent(
            vtkCommand::TimerEvent, &timerId);
        return true;
    };
    constexpr int pollCount = 1000;
    for (int poll = 0; isStarted && !isReloadComplete
        && poll < pollCount; ++poll) {
        if (!sendTimer()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool isExportSent = isReloadSucceeded
        && !createError
        && unicodeSession.SendRequest(
            std::move(exportRequest),
            [&isExportComplete, &isExportSucceeded](const bool value) {
                isExportSucceeded = value;
                isExportComplete = true;
            });
    for (int poll = 0; isExportSent && !isExportComplete
        && poll < pollCount; ++poll) {
        if (!sendTimer()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return GetCaseResult(
        isBuilt
            && isReloadSent
            && isStarted
            && isReloadComplete
            && isReloadSucceeded
            && isExportSent
            && isExportComplete
            && isExportSucceeded
            && unicodeSession.Stop(),
        "Export UTF-8 request facade acceptance") ? 0 : 1;
}
