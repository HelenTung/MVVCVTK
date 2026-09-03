#include "Host/GapHostFeature.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/VtkAppHostSession.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <QtGlobal>

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkRendererCollection.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr char viewId[] = "qt-gap-primary";

HostSessionConfig BuildSessionConfig(
    const vtkSmartPointer<vtkGenericOpenGLRenderWindow>& renderWindow)
{
    HostRenderViewConfig view;
    view.id = viewId;
    view.role = HostRenderViewRole::Primary3D;
    view.window.title = "MVVCVTK Qt Gap Smoke";
    view.window.width = 640;
    view.window.height = 480;
    view.window.viewInit.viewMode =
        HostRenderMode::CompositeIsoSurface;
    view.renderWindow = renderWindow;

    HostSessionConfig config;
    config.renderViews.push_back(std::move(view));
    config.sendOwnerTask = [](std::function<void()> task) {
        auto* appInstance = QCoreApplication::instance();
        if (!appInstance || !task) {
            return false;
        }
        QTimer::singleShot(
            0,
            appInstance,
            [ownerTask = std::move(task)]() mutable {
                ownerTask();
            });
        return true;
    };
    return config;
}

GapHostStartParams BuildGapStart()
{
    GapHostStartParams start;
    start.targetViews.viewIds = { viewId };
    start.surface.isoMode = GapIsoMode::AbsoluteValue;
    start.surface.absoluteIsoValue = 50.0;
    start.surface.backgroundMean = 0.0f;
    start.surface.materialMean = 100.0f;
    start.voidParams.isFilterEnabled = false;
    start.voidParams.minVolumeMM3 = 0.0;
    return start;
}

GapHostConfig BuildGapConfig(const GapHostStartParams& start)
{
    GapHostConfig config;
    config.defaultStart = start;
    config.inputViews.viewIds = { viewId };
    config.keys.switchOverlay.keyCode = 'j';
    config.keys.exit.keySym = "Escape";
    return config;
}

}

int main(int argc, char* argv[])
{
    static_assert(
        QT_VERSION == QT_VERSION_CHECK(5, 14, 2),
        "QtHostGapSmoke requires Qt 5.14.2 headers");

    QSurfaceFormat::setDefaultFormat(
        QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);

    QVTKOpenGLNativeWidget widget;
    widget.setWindowTitle(QStringLiteral("MVVCVTK Qt Gap Smoke"));
    vtkNew<vtkGenericOpenGLRenderWindow> renderWindow;
    struct VtkErrorState final {
        std::size_t count = 0;
        std::string text;
    };
    VtkErrorState vtkError;
    vtkNew<vtkCallbackCommand> errorCallback;
    errorCallback->SetClientData(&vtkError);
    errorCallback->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void* callData) {
            auto* errorState = static_cast<VtkErrorState*>(clientData);
            if (!errorState) {
                return;
            }
            ++errorState->count;
            const auto* message = static_cast<const char*>(callData);
            if (message) {
                errorState->text = message;
            }
        });
    renderWindow->AddObserver(vtkCommand::ErrorEvent, errorCallback);
    widget.setRenderWindow(renderWindow);
    widget.resize(640, 480);
    widget.show();

    const int rendererBaseline =
        renderWindow->GetRenderers()->GetNumberOfItems();
    const auto ownerThread = std::this_thread::get_id();
    const GapHostStartParams gapStart = BuildGapStart();
    std::unique_ptr<VtkAppHostSession> session;
    std::shared_ptr<GapHostFeature> gap;
    QTimer exitPoll(&widget);
    exitPoll.setInterval(5);
    QTimer callbackGrace(&widget);
    callbackGrace.setSingleShot(true);
    callbackGrace.setInterval(50);
    QTimer stopPoll(&widget);
    stopPoll.setInterval(5);
    int callbackCount = 0;
    bool isCallbackOnOwner = false;
    bool isFinishing = false;
    bool finishSucceeded = false;
    std::string finishReason;
    int processResult = 1;

    std::function<void()> tryStop;
    tryStop = [&]() {
        if (!isFinishing) {
            return;
        }

        bool isStopped = !session;
        if (session) {
            (void)session->Stop();
            isStopped = session->GetIsStopped();
        }
        if (!isStopped) {
            if (!stopPoll.isActive()) {
                stopPoll.start();
            }
            return;
        }

        stopPoll.stop();
        gap.reset();
        session.reset();
        const bool isDetached =
            renderWindow->GetRenderers()->GetNumberOfItems()
                == rendererBaseline;
        const bool isValid = finishSucceeded
            && isDetached
            && callbackCount == 1
            && isCallbackOnOwner
            && vtkError.count == 0;
        processResult = isValid ? 0 : 1;
        if (isValid) {
            std::cout
                << "PASS: Qt event loop drove Gap Reload/Start/Overlay/Exit\n";
        }
        else {
            std::cerr
                << "FAIL: Qt Gap smoke"
                << " reason=" << finishReason
                << " stopped=" << isStopped
                << " detached=" << isDetached
                << " callbacks=" << callbackCount
                << " callbackOwner=" << isCallbackOnOwner
                << " vtkErrors=" << vtkError.count
                << " vtkMessage=" << vtkError.text
                << '\n';
        }
        widget.setRenderWindow(
            static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
        QApplication::exit(processResult);
    };
    QObject::connect(
        &stopPoll,
        &QTimer::timeout,
        &widget,
        [&]() { tryStop(); });

    std::function<void(bool, const char*)> sendExit;
    sendExit = [&](const bool isSucceeded, const char* reason) {
        if (isFinishing) {
            return;
        }
        isFinishing = true;
        finishSucceeded = isSucceeded;
        finishReason = reason ? reason : "unknown";
        exitPoll.stop();
        callbackGrace.stop();
        tryStop();
    };

    QObject::connect(
        &callbackGrace,
        &QTimer::timeout,
        &widget,
        [&]() {
            sendExit(
                callbackCount == 1,
                callbackCount == 1 ? "ok" : "Gap callback count");
        });

    QObject::connect(
        &exitPoll,
        &QTimer::timeout,
        &widget,
        [&]() {
            if (!gap) {
                sendExit(false, "missing Gap owner during exit");
                return;
            }
            const GapHostState state = gap->GetState();
            if (state.analysisState == GapAnalysisState::Idle
                && !state.isViewActive
                && !state.isExitPending) {
                exitPoll.stop();
                callbackGrace.start();
            }
        });

    QTimer::singleShot(0, &widget, [&]() {
        const bool isBound = widget.renderWindow() == renderWindow;
        const bool isQtVersion =
            QLatin1String(qVersion()) == QLatin1String("5.14.2");
        const bool isWindowsPlugin =
            QGuiApplication::platformName() == QStringLiteral("windows");
        if (!isBound || !isQtVersion || !isWindowsPlugin) {
            sendExit(false, "Qt/VTK binding prerequisite");
            return;
        }

        session = std::make_unique<VtkAppHostSession>(
            BuildSessionConfig(renderWindow));
        if (!session->BuildSession()) {
            sendExit(false, "BuildSession");
            return;
        }
        gap = std::make_shared<GapHostFeature>(
            BuildGapConfig(gapStart));
        if (!session->AttachFeature(gap)) {
            sendExit(false, "AttachFeature");
            return;
        }

        HostTimerConfig timer;
        timer.isTimerEnabled = true;
        timer.targetView.viewId = viewId;
        if (!session->AttachTimer(timer)) {
            sendExit(false, "AttachTimer");
            return;
        }

        HostReloadRequest reload;
        constexpr int sideLength = 5;
        constexpr std::size_t voxelCount =
            sideLength * sideLength * sideLength;
        reload.geometry.dimensions = {
            sideLength, sideLength, sideLength
        };
        reload.geometry.spacing = { 1.0f, 1.0f, 1.0f };
        reload.geometry.origin = { 0.0f, 0.0f, 0.0f };
        reload.voxels.assign(voxelCount, 100.0f);
        reload.voxels[2 + sideLength * (2 + sideLength * 2)] = 0.0f;

        const bool isReloadAccepted = session->SendRequestResult(
            std::move(reload),
            [&](HostResult result) {
                const bool isLoaded = result.isSucceeded
                    && result.errorCode == HostErrorCode::None;
                QTimer::singleShot(0, &widget, [&, isLoaded]() {
                    if (!isLoaded || !gap) {
                        sendExit(false, "Reload");
                        return;
                    }

                    GapHostRequest request;
                    request.action = GapHostAction::Start;
                    request.start = gapStart;
                    const bool isGapAccepted = gap->SendRequest(
                        std::move(request),
                        [&](const bool isDisplayed) {
                            ++callbackCount;
                            if (callbackCount != 1) {
                                sendExit(false, "duplicate Gap callback");
                                return;
                            }
                            isCallbackOnOwner =
                                std::this_thread::get_id() == ownerThread;
                            const GapHostState state = gap
                                ? gap->GetState() : GapHostState{};
                            if (!isDisplayed
                                || state.analysisState
                                    != GapAnalysisState::Succeeded
                                || !state.isViewActive
                                || state.statistics.voidVoxelCount == 0) {
                                sendExit(false, "Gap result");
                                return;
                            }

                            GapHostRequest hide;
                            hide.action = GapHostAction::Overlay;
                            GapHostRequest show;
                            show.action = GapHostAction::Overlay;
                            GapHostRequest exit;
                            exit.action = GapHostAction::Exit;
                            if (!gap->SendRequest(std::move(hide))
                                || !gap->SendRequest(std::move(show))
                                || !gap->SendRequest(std::move(exit))) {
                                sendExit(false, "Gap Overlay/Exit");
                                return;
                            }
                            exitPoll.start();
                        });
                    if (!isGapAccepted) {
                        sendExit(false, "Gap Start admission");
                    }
                });
            });
        if (!isReloadAccepted) {
            sendExit(false, "Reload admission");
        }
    });

    QTimer::singleShot(
        30000,
        &widget,
        [&]() { sendExit(false, "timeout"); });

    const int appResult = app.exec();
    if (session || gap) {
        std::cerr << "FAIL: Qt Gap smoke exited before Stop completed\n";
        return 1;
    }
    return appResult;
}
