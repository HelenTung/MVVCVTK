#include "Host/VtkAppHostSession.h"
#include "Host/HostFeature.h"
#if defined(MVVCVTK_TEST_GAP)
#include "Host/GapHostFeature.h"
#endif
#if defined(MVVCVTK_TEST_CROP)
#include "Host/CropHostFeature.h"
#endif
#if defined(MVVCVTK_TEST_PART)
#include "Host/PartSegmentationHostFeature.h"
#endif
#if defined(MVVCVTK_TEST_SURFACE)
#include "Host/SurfaceDeterminationHostFeature.h"
#endif

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QMetaObject>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyle.h>
#include <vtkNew.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace {
bool Wait(const std::function<bool()>& isDone, const int timeoutMs = 15000)
{
    if (isDone()) return true;
    QEventLoop loop;
    QTimer check;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&check, &QTimer::timeout, &loop, [&] {
        if (isDone()) loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    check.start(5); // 只观察断言，不泵 SDK 更新或绘制。
    deadline.start(timeoutMs);
    loop.exec();
    return isDone();
}

struct ScheduleState final {
    QObject* receiver = nullptr;
    std::weak_ptr<VtkAppHostSession> session;
    std::atomic<int> notifications{0};
    int updates = 0;
    bool isRunning = true;
    bool hasUnexpectedRender = false;
    std::array<int, 2> frames{};
    HostUpdateResult lastUpdate;
    QTimer* renderTimer = nullptr;
    bool isAutomaticDrawing = false;
    std::chrono::steady_clock::time_point nextFrame{};
    std::vector<std::chrono::steady_clock::time_point> renderStarts;
    std::vector<std::string> pendingDrawIds;
};

class WorkFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "test.work"; }
    bool AttachHost(const HostFeatureContext& context) override
    {
        host = context.host;
        return static_cast<bool>(host);
    }
    bool DetachHost() override
    {
        if (onDetach) onDetach();
        host.reset();
        return true;
    }
    bool OnHostTick() override
    {
        if (hasResult.exchange(false)) ++completions;
        return true;
    }
    std::shared_ptr<FeatureHostControl> host;
    std::function<void()> onDetach;
    std::atomic<bool> hasResult{false};
    int completions = 0;
};

bool Check(bool value, const char* message)
{
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

bool TestScheduling()
{
    QObject receiver;
    QTimer renderTimer(&receiver);
    renderTimer.setSingleShot(true);
    renderTimer.setTimerType(Qt::PreciseTimer);
    QVTKOpenGLNativeWidget widgetA;
    QVTKOpenGLNativeWidget widgetB;
    vtkNew<vtkGenericOpenGLRenderWindow> windowA;
    vtkNew<vtkGenericOpenGLRenderWindow> windowB;
    widgetA.setRenderWindow(windowA);
    widgetB.setRenderWindow(windowB);
    widgetA.resize(240, 180);
    widgetB.resize(240, 180);
    widgetA.show();
    widgetB.show();
    if (!Wait([&] { return windowA->GetReadyForRendering()
        && windowB->GetReadyForRendering(); })) return false;
    windowA->SetWindowName("external-title");
    const std::array<int, 2> sizeA{windowA->GetSize()[0], windowA->GetSize()[1]};

    auto state = std::make_shared<ScheduleState>();
    state->receiver = &receiver;
    state->renderTimer = &renderTimer;
    const std::weak_ptr<ScheduleState> weakState = state;
    std::array<vtkSmartPointer<vtkCallbackCommand>, 2> observers;
    for (std::size_t index = 0; index < observers.size(); ++index) {
        observers[index] = vtkSmartPointer<vtkCallbackCommand>::New();
        observers[index]->SetClientData(&state->frames[index]);
        observers[index]->SetCallback([](vtkObject*, unsigned long, void* data, void*) {
            ++*static_cast<int*>(data);
        });
    }
    const auto tagA = windowA->AddObserver(vtkCommand::EndEvent, observers[0]);
    const auto tagB = windowB->AddObserver(vtkCommand::EndEvent, observers[1]);
    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    for (int index = 0; index < 2; ++index) {
        HostRenderViewConfig view;
        view.id = index == 0 ? "a" : "b";
        view.role = index == 0 ? HostRenderViewRole::Primary3D : HostRenderViewRole::Auxiliary;
        view.renderWindow = index == 0 ? windowA.GetPointer() : windowB.GetPointer();
        view.window.title = "SDK-title-must-not-apply";
        view.window.viewInit.hasIso = true;
        view.window.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
        view.window.viewInit.isoThreshold = 50.0;
        config.renderViews.push_back(std::move(view));
    }
    config.sendOwnerTask = [&receiver](std::function<void()> task) {
        return QMetaObject::invokeMethod(&receiver, std::move(task), Qt::QueuedConnection);
    };
    config.onWorkAvailable = [weakState] {
        const auto current = weakState.lock();
        if (!current) return;
        ++current->notifications;
        QMetaObject::invokeMethod(current->receiver, [weakState] {
            const auto current = weakState.lock();
            if (!current || !current->isRunning) return;
            const auto session = current->session.lock();
            if (!session) return;
            const auto before = current->frames;
            current->lastUpdate = session->SendUpdates();
            ++current->updates;
            current->hasUnexpectedRender |= current->frames != before;
            if (current->isAutomaticDrawing) {
                current->pendingDrawIds = current->lastUpdate.renderViewIds;
                if (!current->pendingDrawIds.empty() && !current->renderTimer->isActive())
                    current->renderTimer->start(0);
            }
        }, Qt::QueuedConnection);
    };
    auto session = std::make_shared<VtkAppHostSession>(std::move(config));
    state->session = session;
    QObject::connect(&renderTimer, &QTimer::timeout, &receiver, [&] {
        if (!state->isRunning || !state->isAutomaticDrawing) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < state->nextFrame) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                state->nextFrame - now).count();
            renderTimer.start(static_cast<int>(remaining + 1));
            return;
        }
        HostRenderRequest request;
        for (const auto& id : state->pendingDrawIds) {
            if ((id == "a" && widgetA.isVisible()) || (id == "b" && widgetB.isVisible()))
                request.viewIds.push_back(id);
        }
        if (request.viewIds.empty()) return;
        state->nextFrame = now + std::chrono::milliseconds(20);
        const auto result = session->SendRender(request);
        if (result.status == HostRenderStatus::Rendered)
            state->renderStarts.push_back(now);
        // Deferred 留待窗口恢复/新工作通知；不以立即重试制造忙循环。
        state->pendingDrawIds.clear();
    });
    bool valid = Check(session->BuildSession(), "BuildSession");
    auto* endpointA = session->GetRenderViewEndpoint("a");
    auto* endpointB = session->GetRenderViewEndpoint("b");
    valid &= Check(endpointA && endpointB, "endpoints");
    if (!valid) return false;
    valid &= Check(!endpointA->interactor->GetEnableRender(), "style render disabled");
    valid &= Check(windowA->GetSize()[0] == sizeA[0]
        && windowA->GetSize()[1] == sizeA[1]
        && std::string(windowA->GetWindowName()) == "external-title", "external layout retained");
    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    valid &= Check(!session->AttachTimer(timer) && !session->Start(), "mixed drive rejected");

    int reloads = 0;
    auto sendReload = [&] {
        HostReloadRequest request;
        request.geometry.dimensions = {5, 5, 5};
        request.geometry.spacing = {1.0f, 1.0f, 1.0f};
        request.geometry.origin = {0.0f, 0.0f, 0.0f};
        request.metadata.identity.datasetId = "host-scheduling";
        request.metadata.source.uri = "memory://host-scheduling";
        request.voxels.assign(125, 100.0f);
        for (int z = 1; z <= 3; ++z)
            for (int y = 1; y <= 3; ++y)
                for (int x = 1; x <= 3; ++x)
                    request.voxels[x + 5 * (y + 5 * z)] = 0.0f;
        return session->SendRequestResult(std::move(request), [&](HostResult result) {
            if (result.isSucceeded) ++reloads;
        });
    };
    valid &= Check(sendReload() && Wait([&] { return reloads == 1; }),
        "load completes without render");
    valid &= Check(!state->hasUnexpectedRender, "updates never render");
    const auto beforeA = state->frames[0];
    const auto renderA = session->SendRender({{"a"}, 42.0});
    valid &= Check(renderA.status == HostRenderStatus::Rendered
        && state->frames[0] == beforeA + 1, "selected view renders");
    auto scenes = session->GetSceneViewStates();
    valid &= Check(scenes.size() == 2 && scenes[1].renderedEpoch < scenes[1].sceneEpoch,
        "unselected view remains pending");
    const auto firstEpoch = scenes.empty() ? 0 : scenes[0].sceneEpoch;
    widgetB.hide();
    windowB->SetReadyForRendering(false);
    valid &= Check(sendReload() && Wait([&] { return reloads == 2; }),
        "hidden view does not block next commit");
    scenes = session->GetSceneViewStates();
    valid &= Check(scenes.size() == 2 && scenes[1].sceneEpoch > firstEpoch
        && scenes[1].renderedEpoch < scenes[1].sceneEpoch, "pending inherits latest epoch");
    valid &= Check(session->SendRender({{"b"}, 8.0}).status == HostRenderStatus::Deferred,
        "not ready does not complete render");
    const int beforeReadyUpdate = state->updates;
    widgetB.show();
    windowB->SetReadyForRendering(true);
    valid &= Check(Wait([&] { return state->updates > beforeReadyUpdate; }),
        "window recovery wakes update");
    const auto restored = session->SendRender({{"b"}, 8.0});
    valid &= Check(restored.status == HostRenderStatus::Rendered, "ready draws latest");
    valid &= Check(session->SendRender({{}, 8.0}).status == HostRenderStatus::Unchanged,
        "empty selection never renders");
    const auto beforeInvalid = state->frames;
    valid &= Check(session->SendRender({{"a", "missing"}, 8.0}).status == HostRenderStatus::Failed
        && session->SendRender({{"a", "a"}, 8.0}).status == HostRenderStatus::Failed
        && session->SendRender({{"a"}, std::numeric_limits<double>::quiet_NaN()}).status
            == HostRenderStatus::Failed
        && state->frames == beforeInvalid, "invalid batch rejected before render");

    const int beforeInput = state->frames[0];
    const int beforeUpdates = state->updates;
    endpointA->interactor->Render();
    valid &= Check(state->frames[0] == beforeInput, "interactor cannot bypass schedule");
    valid &= Check(Wait([&] { return state->updates > beforeUpdates; }), "input wakes update");
    valid &= Check(session->SendRender({{"a"}, 37.0}).status == HostRenderStatus::Rendered
        && windowA->GetDesiredUpdateRate() == 37.0, "host render budget");

    widgetA.hide();
    widgetB.hide();
    windowA->SetReadyForRendering(false);
    windowB->SetReadyForRendering(false);
    const auto hiddenFrames = state->frames;
    int imageReads = 0;
    valid &= Check(session->StartImageRead({}, [&](ImageReadResult result) {
        if (result.error == ImageReadError::None) ++imageReads;
    }) == ImageReadAdmission::Accepted && Wait([&] { return imageReads == 1; }),
        "image read wakes with all windows hidden");
    QTemporaryDir exportDirectory;
    HostDataExportRequest exportRequest;
    exportRequest.sourceView.viewId = "a";
    exportRequest.outputPath = exportDirectory.path().toUtf8().toStdString();
    exportRequest.format = HostDataExportFormat::Raw;
    int exports = 0;
    valid &= Check(exportDirectory.isValid()
        && session->SendRequestResult(std::move(exportRequest), [&](HostResult result) {
            if (result.isSucceeded) ++exports;
        }) && Wait([&] { return exports == 1; })
        && !QDir(exportDirectory.path()).entryList(QDir::Files).isEmpty()
        && state->frames == hiddenFrames,
        "export wakes and writes data with all windows hidden and no drawing");
    const int beforeShowUpdate = state->updates;
    widgetA.show();
    widgetB.show();
    windowA->SetReadyForRendering(true);
    windowB->SetReadyForRendering(true);
    valid &= Check(Wait([&] { return state->updates > beforeShowUpdate; }),
        "all windows restored wake update");
    const int beforeResizeUpdate = state->updates;
    widgetA.resize(260, 190);
    valid &= Check(Wait([&] { return state->updates > beforeResizeUpdate; }),
        "Qt resize wakes update");

#if defined(MVVCVTK_TEST_GAP)
    GapHostConfig gapConfig;
    gapConfig.defaultStart.targetViews.viewIds = {"a"};
    gapConfig.defaultStart.surface.isoMode = GapIsoMode::AbsoluteValue;
    gapConfig.defaultStart.surface.absoluteIsoValue = 50.0;
    gapConfig.defaultStart.surface.backgroundMean = 0.0f;
    gapConfig.defaultStart.surface.materialMean = 100.0f;
    gapConfig.inputViews.viewIds = {"a"};
    gapConfig.keys.switchOverlay.keyCode = 'j';
    gapConfig.keys.exit.keySym = "Escape";
    auto gap = std::make_shared<GapHostFeature>(gapConfig);
    int gapCompletions = 0;
    GapHostRequest gapRequest;
    gapRequest.action = GapHostAction::Start;
    gapRequest.start = gapConfig.defaultStart;
    valid &= Check(session->AttachFeature(gap)
        && gap->SendRequest(std::move(gapRequest), [&](GapHostResult result) {
            if (result.status == GapResultStatus::Succeeded
                || result.status == GapResultStatus::SucceededWithDisplayFailure)
                ++gapCompletions;
            else std::cerr << "Gap result: " << result.message << '\n';
        }) && Wait([&] { return gapCompletions == 1; }),
        "Gap worker completes without rendering");
    valid &= Check(session->DetachFeature(*gap), "Gap detach");
#endif
#if defined(MVVCVTK_TEST_PART)
    auto part = std::make_shared<PartSegmentationHostFeature>();
    PartSegmentationRequest partRequest;
    partRequest.action = PartSegmentationAction::Start;
    PartSegmentationStartParams partStart;
    partStart.targetViews.viewIds = {"a"};
    partStart.threshold = 50.0;
    partRequest.start = partStart;
    int partCompletions = 0;
    valid &= Check(session->AttachFeature(part)
        && part->SendRequest(std::move(partRequest), [&](PartSegmentationResult result) {
            if (result.status == PartResultStatus::Succeeded) ++partCompletions;
        }).status == PartAdmissionStatus::Accepted
        && Wait([&] { return partCompletions == 1; }),
        "Part worker completes without rendering");
    valid &= Check(session->DetachFeature(*part), "Part detach");
#endif
#if defined(MVVCVTK_TEST_SURFACE)
    auto surface = std::make_shared<SurfaceDeterminationHostFeature>();
    SurfaceDeterminationRequest surfaceRequest;
    surfaceRequest.action = SurfaceDeterminationAction::Start;
    SurfaceDeterminationStartParams surfaceStart;
    surfaceStart.targetViews.viewIds = {"a"};
    surfaceStart.method = SurfaceDeterminationMethod::GlobalIsoPreview;
    surfaceStart.initialIsoValue = 50.0;
    surfaceRequest.start = surfaceStart;
    int surfaceCompletions = 0;
    valid &= Check(session->AttachFeature(surface)
        && surface->SendRequest(std::move(surfaceRequest), [&](SurfaceDeterminationResult result) {
            if (result.status == SurfaceResultStatus::Succeeded) ++surfaceCompletions;
            else std::cerr << "Surface result: " << result.message << '\n';
        }).status == SurfaceAdmissionStatus::Accepted
        && Wait([&] { return surfaceCompletions == 1; }),
        "Surface worker completes without rendering");
    valid &= Check(session->DetachFeature(*surface), "Surface detach");
#endif
#if defined(MVVCVTK_TEST_CROP)
    CropHostTarget cropTarget;
    cropTarget.inputBinding = std::string(primaryVolumeBinding);
    cropTarget.referenceView.viewId = "a";
    cropTarget.targetViews.viewIds = {"a"};
    auto crop = std::make_shared<CropHostFeature>();
    CropHostRequest box;
    box.action = CropHostAction::Box;
    box.target = cropTarget;
    valid &= Check(session->AttachFeature(crop)
        && crop->SendRequest(std::move(box)), "Crop box");
    CropHostRequest mode;
    mode.action = CropHostAction::Mode;
    mode.target = cropTarget;
    mode.removalMode = CropRemovalMode::KeepInside;
    valid &= Check(crop->SendRequest(std::move(mode)), "Crop mode");
    // 先建立真实 widget 操作；本测试只通过公开输入制造候选，后台等待不画帧。
    vtkSmartPointer<vtkInteractorObserver> previousStyle = endpointA->interactor->GetInteractorStyle();
    endpointA->interactor->SetInteractorStyle(nullptr);
    const double bounds[6] = {0.0, 4.0, 0.0, 4.0, 0.0, 4.0};
    const std::array<std::array<double, 3>, 10> points{{
        {4.0, 2.0, 2.0}, {0.0, 2.0, 2.0}, {2.0, 4.0, 2.0},
        {2.0, 0.0, 2.0}, {2.0, 2.0, 4.0}, {2.0, 2.0, 0.0},
        {4.0, 4.0, 4.0}, {0.0, 0.0, 0.0}, {4.0, 0.0, 4.0}, {0.0, 4.0, 0.0}
    }};
    for (const auto& point : points) {
        endpointA->renderer->ResetCamera(bounds);
        const auto prior = state->updates;
        endpointA->interactor->Render();
        (void)Wait([&] { return state->updates > prior; });
        (void)session->SendRender({{"a"}, 0.001});
        endpointA->renderer->SetWorldPoint(point[0], point[1], point[2], 1.0);
        endpointA->renderer->WorldToDisplay();
        const auto* display = endpointA->renderer->GetDisplayPoint();
        const auto x = static_cast<int>(display[0]);
        const auto y = static_cast<int>(display[1]);
        endpointA->interactor->SetEventPosition(x, y);
        endpointA->interactor->InvokeEvent(vtkCommand::LeftButtonPressEvent);
        endpointA->interactor->SetEventPosition(x + 8, y);
        endpointA->interactor->InvokeEvent(vtkCommand::MouseMoveEvent);
        endpointA->interactor->InvokeEvent(vtkCommand::LeftButtonReleaseEvent);
        if (Wait([&] { return crop->GetState().history.nodeCount > 0; }, 150)) break;
    }
    endpointA->interactor->SetInteractorStyle(previousStyle);
    valid &= Check(crop->GetState().history.nodeCount > 0, "Crop input commits shader");
    CropHostRequest materialize;
    materialize.action = CropHostAction::BuildResult;
    materialize.target = cropTarget;
    int cropCompletions = 0;
    valid &= Check(crop->SendRequest(std::move(materialize), [&](CropBuildResult result) {
        if (result.isSucceeded) ++cropCompletions;
    }) && Wait([&] { return cropCompletions == 1; }),
        "Crop worker completes without rendering");
    valid &= Check(session->DetachFeature(*crop), "Crop detach");
#endif

    auto feature = std::make_shared<WorkFeature>();
    valid &= Check(session->AttachFeature(feature), "work feature attach");
    auto oldHost = feature->host;
    std::thread worker([feature, oldHost] {
        feature->hasResult.store(true);
        (void)oldHost->SendWorkAvailable();
    });
    worker.join();
    valid &= Check(Wait([&] { return feature->completions == 1; }), "worker wakes feature tick");
    valid &= Check(session->DetachFeature(*feature) && !oldHost->SendWorkAvailable(),
        "detached feature wake rejected");
    HostUpdateResult wrongThread;
    std::thread wrong([&] { wrongThread = session->SendUpdates(); });
    wrong.join();
    valid &= Check(wrongThread.status == HostUpdateStatus::Failed, "wrong thread rejected");
    state->isAutomaticDrawing = true;
    QTimer inputTimer(&receiver);
    QObject::connect(&inputTimer, &QTimer::timeout, &receiver, [&] {
        endpointA->interactor->Render();
    });
    inputTimer.start(1);
    valid &= Check(Wait([&] { return state->renderStarts.size() >= 4; }),
        "Qt single-shot scheduler draws continuous input");
    inputTimer.stop();
    bool hasSettled = false;
    QTimer::singleShot(100, &receiver, [&] { hasSettled = true; });
    (void)Wait([&] { return hasSettled; });
    for (std::size_t index = 1; index < state->renderStarts.size(); ++index)
        valid &= Check(state->renderStarts[index] - state->renderStarts[index - 1]
            >= std::chrono::milliseconds(20), "Qt enforces start-to-start frame interval");
    const auto idleFrames = state->frames;
    const auto idleUpdates = state->updates;
    hasSettled = false;
    QTimer::singleShot(100, &receiver, [&] { hasSettled = true; });
    (void)Wait([&] { return hasSettled; });
    valid &= Check(state->frames == idleFrames && state->updates == idleUpdates,
        "idle does not self-trigger updates or render");
    state->isAutomaticDrawing = false;
    struct StopProbe final {
        bool isAccepted = false;
        bool isNestedRejected = false;
        bool isStopReentryDeferred = false;
        int completions = 0;
    };
    const auto stopProbe = std::make_shared<StopProbe>();
    auto stopFeature = std::make_shared<WorkFeature>();
    valid &= Check(session->AttachFeature(stopFeature), "stop feature attach");
    const auto stopHost = stopFeature->host;
    stopFeature->onDetach = [stopHost, stopProbe, weakSession = std::weak_ptr<VtkAppHostSession>(session)] {
        stopProbe->isAccepted = stopHost->SendOwnerComplete([stopHost, stopProbe, weakSession] {
            ++stopProbe->completions;
            stopProbe->isNestedRejected = !stopHost->SendOwnerComplete([stopProbe] {
                ++stopProbe->completions;
            });
            if (const auto current = weakSession.lock())
                stopProbe->isStopReentryDeferred = !current->Stop();
        });
    };
    const int beforeFinalUpdate = state->updates;
    endpointA->interactor->Render();
    valid &= Check(Wait([&] { return state->updates > beforeFinalUpdate; }), "final update");
    bool hasDeferredReentry = false;
    std::function<void()> onRender = [&] {
        hasDeferredReentry = session->SendUpdates().status == HostUpdateStatus::Deferred
            && session->SendRender({{"a"}, 1.0}).status == HostRenderStatus::Deferred
            && !session->Stop();
    };
    vtkNew<vtkCallbackCommand> stopObserver;
    stopObserver->SetClientData(&onRender);
    stopObserver->SetCallback([](vtkObject*, unsigned long, void* data, void*) {
        (*static_cast<std::function<void()>*>(data))();
    });
    const auto stopTag = windowA->AddObserver(vtkCommand::EndEvent, stopObserver);
    const int beforeStopB = state->frames[1];
    const auto stoppedRender = session->SendRender({{"a", "b"}, 10.0});
    windowA->RemoveObserver(stopTag);
    state->isRunning = false;
    valid &= Check(hasDeferredReentry && stoppedRender.status == HostRenderStatus::Stopped
        && session->GetIsStopped() && state->frames[1] == beforeStopB,
        "render reentry deferred; Stop prevents remaining views");
    valid &= Check(stopProbe->isAccepted && stopProbe->isNestedRejected
        && stopProbe->isStopReentryDeferred
        && stopProbe->completions == 1,
        "Stop drains admitted completion once and rejects new completion admission");
    valid &= Check(session->SendUpdates().status == HostUpdateStatus::Stopped
        && !oldHost->SendWorkAvailable(), "stopped work rejected");
    windowA->RemoveObserver(tagA);
    windowB->RemoveObserver(tagB);
    return valid;
}
} // namespace

int main(int argc, char* argv[])
{
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    const bool valid = TestScheduling();
    std::cout << (valid ? "PASS" : "FAIL") << ": Qt HostDriven scheduling\n";
    return valid ? 0 : 1;
}
