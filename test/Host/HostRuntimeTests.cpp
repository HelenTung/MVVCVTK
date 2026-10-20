// 测试用途：验证宿主运行时的视图上下文、帧租约、渲染更新与完成通知。
#include "Host/Internal/HostFrameRuntime.h"
#include "App/Services/PrimaryDataActivation.h"
#include "App/Services/FeatureViewService.h"
#include "App/Services/AppServiceFactory.h"
#include "App/AppState.h"
#include "App/AppStateEvents.h"
#include "Data/DataManager.h"
#include "Host/HostCommandRouter.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/VtkAppHostSession.h"
#include "Interaction/AbstractViewContext.h"
#include "Render/Support/BaseVisualStrategy.h"
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int GetAutoDisplayDefaultsFailCount();

namespace {
class ViewProbe final : public AppViewPort {
public:
    bool SetViewConfig(const PreInitConfig&) override { return true; }
    bool SendViewUpdate(const AppViewUpdate&) override { return true; }
    bool SetViewState(const AppViewState&, std::uint64_t) override { return true; }
    AppViewState GetViewState() const override { ++viewReads; return state; }
    RulerState GetRulerState() const override { return state.rulerState; }
    mutable int viewReads = 0;
    AppViewState state;
};
class UpdateProbe final : public RenderUpdatePort {
public:
    bool SendUpdates() override { ++applyCount; return isApplyAccepted; }
    bool SendPendingUpdates() override { ++pendingCount; return isApplyAccepted; }
    void SendCompletions() override { ++completeCount; if (onComplete) onComplete(); }
    bool SetRenderNeeded() override { isDirty = true; ++notifications; return true; }
    bool GetRenderNeeded() const override { return isDirty; }
    bool ResetRenderNeeded() override {
        if (isClaimThrowing) throw std::runtime_error("claim rejected");
        const bool previous = isDirty;
        isDirty = false;
        return previous;
    }
    bool isDirty = true;
    bool isApplyAccepted = true;
    bool isClaimThrowing = false;
    int applyCount = 0;
    int pendingCount = 0;
    int completeCount = 0;
    int notifications = 0;
    std::function<void()> onComplete;
};
class ContextProbe final : public AbstractViewContext {
public:
    bool SendRender() override { ++renderCount; if (onRender) onRender(); return isRenderAccepted; }
    std::optional<ViewCameraState> GetCameraState() const override {
        return hasCamera ? std::optional<ViewCameraState>{camera} : std::nullopt;
    }
    bool SetCameraStyle(VizMode) override { return true; }
    bool SetInteractorReady() override { return true; }
    bool GetIsInputIdle() const override { return true; }
    bool SetInputEnabled(bool) override { return true; }
    bool Start() override { return true; }
    bool StopInput() override { return true; }
    bool SetOrientationAxesVisible(bool) override { return true; }
    bool GetOrientationAxesVisible() const override { return areAxesVisible; }
    bool SetToolMode(ToolMode) override { return true; }
    ToolMode GetToolMode() const override { return ToolMode::Navigation; }
    bool SetInputHandler(InteractionRouteCallback, std::vector<InteractionEventKind>) override { return true; }
    bool ClearInputHandler() override { return true; }
    InteractionResult SendInput(const InteractionEvent&) override { return {}; }
    InteractionResult CancelInput(const InteractionCaptureKey&) override { return {}; }
    bool SetTimerHandler(std::function<void()>) override { return true; }
    bool ClearTimerHandler() override { return true; }
    vtkRenderWindowInteractor* GetInteractor() const override { return nullptr; }
    int renderCount = 0;
    bool isRenderAccepted = true;
    bool hasCamera = true;
    bool areAxesVisible = false;
    ViewCameraState camera;
    std::function<void()> onRender;
};
struct FrameFixture final {
    std::vector<HostRenderViewRuntime> views{2};
    std::shared_ptr<FeatureViewLease> lease =
        std::make_shared<FeatureViewLease>(std::this_thread::get_id());
    std::shared_ptr<UpdateProbe> updates[2] = {
        std::make_shared<UpdateProbe>(), std::make_shared<UpdateProbe>() };
    std::shared_ptr<ContextProbe> contexts[2] = {
        std::make_shared<ContextProbe>(), std::make_shared<ContextProbe>() };
    int featureReadCount = 0;
    HostFrameRuntime frames{views, lease, [this](const HostRenderViewRuntime&) {
        ++featureReadCount;
        return std::vector<std::string>{}; }};
    FrameFixture() {
        for (int i = 0; i < 2; ++i) {
            views[i].config.id = std::to_string(i);
            views[i].app.view = std::make_shared<ViewProbe>();
            views[i].interaction.update = updates[i];
            views[i].context = contexts[i];
            views[i].isAvailable = true;
        }
        frames.BuildSceneStates();
        (void)frames.SetFrameGeneration(17);
    }
};
bool GetRulerCopyValid()
{
    FrameFixture fixture;
    auto view = std::dynamic_pointer_cast<ViewProbe>(fixture.views[0].app.view);
    if (!view || !fixture.frames.CollectFrameUpdates()
        || fixture.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    fixture.frames.SetFrameCommit(1);
    const int readsBefore = view->viewReads;
    // 模拟实际 draw 才产生标尺结果，旧 scene 的缓存仍是 NoData。
    view->state.rulerState.status = RulerStatus::Visible;
    view->state.rulerState.lengthMm = 2.0;
    view->state.rulerState.lengthPixels = 100.0;
    if (!fixture.frames.SendFrameRender(1)) return false;
    const auto scenes = fixture.frames.GetSceneStates();
    return view->viewReads == readsBefore && scenes.size() == 2 && scenes[0].presentation
        && scenes[0].presentation->rulerState.status == HostRulerStatus::Visible
        && scenes[0].presentation->rulerState.lengthMm == 2.0;
}

bool GetFrameFailuresValid()
{
    FrameFixture f;
    bool isWrongThreadAccepted = true;
    std::thread worker([&]() { isWrongThreadAccepted = f.frames.CollectFrameUpdates(); });
    worker.join();
    if (isWrongThreadAccepted || f.frames.GetIsBusy()) return false;
    f.updates[1]->isApplyAccepted = false;
    if (f.frames.CollectFrameUpdates() || !f.updates[0]->isDirty
        || f.contexts[0]->renderCount != 0) return false;
    f.updates[1]->isApplyAccepted = true;
    if (!f.frames.CollectFrameUpdates() || !f.frames.ApplyFrameUpdates()) return false;
    f.updates[1]->isClaimThrowing = true;
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Failed
        || !f.views[0].GetRenderNeeded() || !f.views[1].GetRenderNeeded()) return false;
    f.updates[1]->isClaimThrowing = false;
    f.contexts[1]->hasCamera = false;
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Failed
        || !f.views[0].GetRenderNeeded() || !f.views[1].GetRenderNeeded()) return false;
    f.contexts[1]->hasCamera = true;
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.ClearFrameStage();
    return f.views[0].GetRenderNeeded() && f.views[1].GetRenderNeeded()
        && f.frames.GetSceneStates()[0].sceneEpoch == 0;
}
bool GetFailedFrameWakeValid()
{
    FrameFixture f;
    f.frames.SetDriveMode(HostDriveMode::HostDriven);
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(1);
    f.contexts[1]->hasCamera = false;
    (void)f.updates[0]->SetRenderNeeded();
    for (int retry = 0; retry < 100; ++retry) {
        if (f.frames.BuildFrameStage(2) != HostFrameStageStatus::Failed) return false;
        f.frames.ClearFrameStage();
        if (!f.views[0].GetRenderNeeded() || f.updates[0]->notifications != 1
            || f.updates[1]->notifications != 0) return false;
    }
    const auto deferred = f.frames.SendFrameRender({{"0"}, .001}, [] { return true; });
    if (deferred.status != HostRenderStatus::Deferred || f.contexts[0]->renderCount != 0) return false;
    // 模拟异步产品完成的新事件；恢复领取保留需求并提交，无需周期重试。
    f.contexts[1]->hasCamera = true;
    (void)f.updates[1]->SetRenderNeeded();
    if (f.updates[1]->notifications != 1
        || f.frames.BuildFrameStage(2) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(2);
    return f.frames.GetSceneStates()[0].sceneEpoch == 2
        && !f.views[0].GetRenderNeeded() && !f.views[1].GetRenderNeeded();
}
enum class TransitionExit { Complete, Failed, Cancelled, Stop, Detach };
bool GetInputTransitionBarrierValid(const bool changesData,
    const TransitionExit exit = TransitionExit::Complete)
{
    const bool detachesPending = exit == TransitionExit::Detach;
    const auto check = [](bool condition, const char* reason) {
        if (!condition) std::cerr << "Input transition barrier: " << reason << '\n';
        return condition;
    };
    auto data = std::make_shared<RawVolumeDataManager>();
    ImageMetadata metadata;
    metadata.identity.datasetId = "input-transition-barrier";
    metadata.source.kind = ImageSourceKind::Memory;
    metadata.source.uri = "memory://input-transition-barrier";
    const auto layout = VolumeLayout::Create({2,2,2}, {1.0f,1.0f,1.0f}, {},
        std::array<double,9>{1,0,0,0,1,0,0,0,1}, metadata);
    const auto buffer = layout ? VolumeBuffer::Create({0,1,2,3,4,5,6,7}, *layout)
        : std::optional<VolumeBuffer>{};
    VtkImageGridSnapshot original;
    if (!buffer || !data->SetFromBuffer(*buffer)
        || !data->SetLoadCommit(data->GetLoadStage(), original) || !original) return false;

    FrameFixture f;
    f.frames.SetDriveMode(HostDriveMode::HostDriven);
    f.frames.SetDataRead(data);
    const auto setInput = [&](const int index, const DataRevisionRef& input,
                              const DataBindingRevision binding) {
        auto view = std::dynamic_pointer_cast<ViewProbe>(f.views[index].app.view);
        view->state.dataRevision = input;
        view->state.bindingRevision = binding;
    };
    for (int index = 0; index < 2; ++index) {
        f.views[index].config.synchronizationGroup = "primary";
        setInput(index, original->data->self, original->binding->revision);
    }
    std::vector<HostFrameIntent> appliedIntents;
    HostFrameCoordinator::Callbacks callbacks;
    callbacks.collectUpdates = [&] { return f.frames.CollectFrameUpdates(); };
    callbacks.sendFeatureTicks = [] {};
    callbacks.setIntents = [&](const auto& intents) {
        appliedIntents = intents;
        return f.frames.SetFrameIntents(intents);
    };
    callbacks.applyFeatureUpdates = [&] { return f.frames.ApplyFrameUpdates(); };
    callbacks.buildStage = [&](auto epoch) { return f.frames.BuildFrameStage(epoch); };
    callbacks.setCommit = [&](auto epoch) { f.frames.SetFrameCommit(epoch); };
    callbacks.sendRender = [&](auto epoch) { return f.frames.SendFrameRender(epoch); };
    callbacks.getRenderPending = [&] { return f.frames.GetFrameRenderPending(); };
    callbacks.sendCompletions = [] {};
    callbacks.clearStage = [&] { f.frames.ClearFrameStage(); };
    HostFrameCoordinator coordinator(17, std::move(callbacks));
    if (coordinator.SendUpdates() != HostFrameCoordinator::FlushStatus::Completed
        || coordinator.GetCommittedEpoch() != 1) return false;
    const auto previous = f.frames.GetSceneStates();
    if (previous.size() != 2 || !previous[0].presentation || !previous[1].presentation) return false;

    // 图绑定已经成功，显示候选完成顺序仍可不同；场景必须保留完整旧帧。
    const DataRevisionRef target = changesData
        ? DataRevisionRef{data->CreateDataEntityId(), 1} : original->data->self;
    DataTransaction transaction;
    if (changesData) transaction.outputs.push_back({target.entityId, 0,
        original->data->type, {}, original->data->payload, {}});
    transaction.bindings.push_back({std::string(primaryVolumeBinding),
        original->binding->revision, true, original->data->self, target});
    if (data->SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded) return false;
    const auto primary = data->GetPrimaryImage();
    if (!primary || primary->data->self != target) return false;
    const auto binding = primary->binding->revision;
    setInput(1, target, binding);
    auto preparingView = std::static_pointer_cast<ViewProbe>(f.views[0].app.view);
    preparingView->state.isPresentationPending = true;
    (void)f.updates[1]->SetRenderNeeded();
    FeatureSceneDelta delta;
    delta.viewIds = {"0", "1"};
    delta.requestId = 77;
    const auto attachment = std::make_shared<std::atomic<bool>>(true);
    if (!coordinator.Enqueue("transition-test", delta, 1, attachment)) return false;
    int completions = 0;
    bool completed = false;
    if (!coordinator.SendDisplayComplete([&]() -> std::optional<bool> {
        if (!attachment->load()) return false;
        if (!preparingView->state.isPresentationPending
            && preparingView->state.hasPresentationFailed) return false;
        const auto scenes = f.frames.GetSceneStates();
        if (scenes.size() != 2) return false;
        for (const auto& scene : scenes) {
            if (!scene.presentation || scene.presentation->dataRevision != target
                || scene.presentation->bindingRevision != binding) return std::nullopt;
        }
        return true;
    }, [&](bool succeeded) { ++completions; completed = succeeded; })) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Deferred,
                "valid inputs awaiting group convergence must defer before scene commit")) return false;
        const auto scenes = f.frames.GetSceneStates();
        if (!check(coordinator.GetCommittedEpoch() == 1 && completions == 0
            && scenes.size() == 2
            && f.views[0].pendingRenderEpoch == 1 && f.views[1].pendingRenderEpoch == 1
            && f.views[0].GetRenderNeeded() && f.views[1].GetRenderNeeded()
            && appliedIntents.size() == 1 && appliedIntents[0].delta.requestId == 77,
            "deferral lost dirty, pending render, intent or completion boundary")) return false;
        for (const auto& scene : scenes) {
            if (!check(scene.sceneEpoch == 1 && scene.presentation
                && scene.presentation->dataRevision == original->data->self
                && scene.presentation->bindingRevision == original->binding->revision,
                "deferred stage exposed a partial scene")) return false;
        }
        const auto render = f.frames.SendFrameRender({{"0", "1"}, .001}, [] { return true; });
        if (!check(render.status == HostRenderStatus::Deferred
            && f.contexts[0]->renderCount == 0 && f.contexts[1]->renderCount == 0,
            "mixed state reached a draw")) return false;
    }
    if (!check(f.updates[0]->notifications == 0 && f.updates[1]->notifications == 1,
        "rollback generated work without a new event")) return false;

    if (exit == TransitionExit::Failed || exit == TransitionExit::Cancelled) {
        preparingView->state.isPresentationPending = false;
        preparingView->state.hasPresentationFailed = true;
        return check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed
            && completions == 1 && !completed && coordinator.GetCommittedEpoch() == 1
            && coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed
            && completions == 1 && data->GetPrimaryImage()->data->self == target,
            "terminal candidate retained deferred completion or reverted committed data");
    }

    // 真正失败或已取消的候选没有等待理由；不能用另一个 pending 位掩盖失败。
    preparingView->state.hasPresentationFailed = true;
    if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed,
        "failed or cancelled candidate was hidden by deferral")) return false;
    preparingView->state.hasPresentationFailed = false;
    preparingView->state.isPresentationPending = false;
    if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed,
        "stable conflicting inputs were accepted as a transition")) return false;
    preparingView->state.isPresentationPending = true;
    // 暂缓的组先于后续 View 被采样，也不能遮蔽缺相机和非法图引用。
    f.contexts[1]->hasCamera = false;
    if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed,
        "preparing input hid a missing camera")) return false;
    f.contexts[1]->hasCamera = true;
    setInput(1, {data->CreateDataEntityId(), 1}, binding);
    if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed,
        "preparing input hid an invalid graph reference")) return false;
    setInput(1, target, binding);
    if (changesData) {
        for (auto& view : f.views) view.config.syncPolicy = HostViewSyncPolicy::ExplicitInputs;
        if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Failed,
            "preparing input hid incompatible explicit sources")) return false;
        for (auto& view : f.views) view.config.syncPolicy = HostViewSyncPolicy::SamePrimary;
    }
    if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Deferred,
        "negative probes lost the retryable transition")) return false;
    if (exit == TransitionExit::Stop) {
        coordinator.Stop();
        return check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Stopped
            && completions == 1 && !completed && coordinator.GetCommittedEpoch() == 1
            && f.contexts[0]->renderCount == 0 && f.contexts[1]->renderCount == 0
            && data->GetPrimaryImage()->data->self == target,
            "Stop left a deferred completion or reverted committed business data");
    }
    if (detachesPending) {
        attachment->store(false);
        if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Deferred
            && completions == 1 && !completed,
            "detached owner retained a deferred display completion")) return false;
    }

    // 用明确的新完成事件推进另一 View，不靠 sleep 或持续置脏促成恢复。
    setInput(0, target, binding);
    preparingView->state.isPresentationPending = false;
    (void)f.updates[0]->SetRenderNeeded();
    if (!check(coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Completed
        && coordinator.GetCommittedEpoch() == 2 && completions == 1 && completed == !detachesPending
        && f.frames.GetSceneStates().size() == 2
        && !f.views[0].GetRenderNeeded() && !f.views[1].GetRenderNeeded(),
        "new work did not commit the complete group exactly once")) return false;
    for (const auto& scene : f.frames.GetSceneStates()) {
        if (!check(scene.sceneEpoch == 2 && scene.presentation
            && scene.presentation->dataRevision == target
            && scene.presentation->bindingRevision == binding,
            "recovered scene has the wrong input")) return false;
    }
    return check(f.frames.SendFrameRender(2)
        && !f.frames.GetFrameRenderPending()
        && f.contexts[0]->renderCount == 1 && f.contexts[1]->renderCount == 1
        && coordinator.SendUpdates() == HostFrameCoordinator::FlushStatus::Completed
        && coordinator.GetCommittedEpoch() == 2 && completions == 1
        && previous[0].presentation->dataRevision == original->data->self
        && previous[0].presentation->bindingRevision == original->binding->revision,
        "recovery changed the old snapshot or repeated commit/completion");
}
bool GetRenderRetryValid()
{
    FrameFixture f;
    if (!f.frames.CollectFrameUpdates() || f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(1);
    f.contexts[1]->isRenderAccepted = false;
    if (f.frames.SendFrameRender(1) || !f.frames.GetFrameRenderPending()) return false;
    f.updates[0]->SetRenderNeeded();
    if (!f.frames.CollectFrameUpdates() || !f.frames.ApplyFrameUpdates()
        || f.frames.BuildFrameStage(2) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(2);
    if (f.frames.SendFrameRender(2) || f.contexts[0]->renderCount != 2
        || f.views[0].pendingRenderEpoch != 0 || f.views[1].pendingRenderEpoch != 2) return false;
    f.contexts[1]->isRenderAccepted = true;
    return f.frames.SendFrameRender(2) && f.contexts[0]->renderCount == 2
        && f.views[0].renderedEpoch == 2 && f.views[1].renderedEpoch == 2;
}
bool GetStoppedStageValid()
{
    FrameFixture f;
    if (!f.frames.CollectFrameUpdates()
        || f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.views[0].isAvailable = false;
    f.frames.SetViewUnavailable(0);
    f.frames.SetFrameCommit(1);
    if (!f.frames.SendFrameRender(1)) return false;
    const auto states = f.frames.GetSceneStates();
    const bool isStopped = !states[0].isAvailable && !states[0].presentation
        && !states[0].camera && f.contexts[0]->renderCount == 0
        && f.contexts[1]->renderCount == 1;
    (void)f.lease->StopLease();
    return isStopped && !f.frames.CollectFrameUpdates();
}
bool GetProjectionReuseValid()
{
    FrameFixture f;
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(1);
    if (!f.frames.SendFrameRender(1)) return false;
    f.featureReadCount = 0;
    f.updates[0]->SetRenderNeeded();
    // 同 revision 下，camera、axes、cursor、scalar/binding 等不能被错误缓存。
    f.contexts[1]->camera.position = {4, 5, 6};
    f.contexts[1]->areAxesVisible = true;
    auto view = std::static_pointer_cast<ViewProbe>(f.views[1].app.view);
    view->state.cursorWorld = {7, 8, 9};
    view->state.isInteracting = true;
    view->state.scalarRange = {-3, 17};
    view->state.volumeTransferFunction.colorNodes = {{0, 0, 0, 0}, {1, 1, 1, 1}};
    if (f.frames.BuildFrameStage(2) != HostFrameStageStatus::Ready || f.featureReadCount != 2) return false;
    f.frames.SetFrameCommit(2);
    const auto state = f.frames.GetSceneState(1);
    if (!state || !state->camera || !state->presentation || state->sceneEpoch != 2
        || state->camera->position != f.contexts[1]->camera.position
        || !state->presentation->isAxesVisible || !state->presentation->isInteracting
        || state->presentation->cursorWorld != view->state.cursorWorld
        || state->presentation->scalarRange != view->state.scalarRange
        || state->presentation->volumeTransferFunction.colorNodes.size() != 2) return false;
    if (!f.frames.SendFrameRender(2)) return false;
    f.updates[0]->SetRenderNeeded();
    f.contexts[1]->hasCamera = false;
    return f.frames.BuildFrameStage(3) == HostFrameStageStatus::Failed
        && f.views[0].GetRenderNeeded() && f.frames.GetSceneState(1)->sceneEpoch == 2;
}

bool GetPendingRollbackValid()
{
    FrameFixture f;
    f.frames.SetDriveMode(HostDriveMode::HostDriven);
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(1);
    f.updates[0]->SetRenderNeeded();
    f.contexts[1]->hasCamera = false;
    if (f.frames.BuildFrameStage(2) != HostFrameStageStatus::Failed
        || !f.views[0].GetRenderNeeded() || f.views[1].GetRenderNeeded()
        || f.views[0].pendingRenderEpoch != 1 || f.views[1].pendingRenderEpoch != 1) return false;
    f.contexts[1]->hasCamera = true;
    if (f.frames.BuildFrameStage(2) != HostFrameStageStatus::Ready) return false;
    f.frames.ClearFrameStage();
    if (!f.views[0].GetRenderNeeded() || f.views[1].GetRenderNeeded()
        || f.views[0].pendingRenderEpoch != 1 || f.views[1].pendingRenderEpoch != 1) return false;
    if (f.frames.BuildFrameStage(2) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(2);
    return f.views[0].pendingRenderEpoch == 2 && f.views[1].pendingRenderEpoch == 2;
}

class CandidateStrategyProbe final : public BaseVisualStrategy {
public:
    RenderProductStatus status = RenderProductStatus::Idle;
    int attachments = 0;
    bool isDetachRejected = false;
    void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
    bool SetProductCommit() override { return true; }
    RenderTransitionState GetTransitionState() const override {
        RenderTransitionState result;
        result.status = status;
        return result;
    }
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {
        ++attachments;
        BaseVisualStrategy::AttachRenderer(std::move(renderer));
    }
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override {
        if (isDetachRejected) throw std::runtime_error("candidate cleanup rejected");
        BaseVisualStrategy::DetachRenderer(std::move(renderer));
    }
};

class CandidateViewDirectory final : public IHostViewDirectory {
public:
    explicit CandidateViewDirectory(std::vector<HostRenderViewRuntime>& views) : m_views(views) {}
    std::optional<HostDataRoute> GetDataRoute(const HostViewTarget&) const override { return {}; }
    std::optional<HostViewRoute> GetViewRoute(const HostViewTarget& target) const override {
        for (const auto& view : m_views) {
            if ((!target.viewId.empty() && target.viewId != view.config.id)
                || (target.isViewRoleUsed && target.viewRole != view.config.role)) continue;
            return HostViewRoute{view.config.id, view.config.role, view.app.view,
                view.interaction.update, view.context, {}};
        }
        return {};
    }
    std::weak_ptr<AppSessionPort> GetSessionPort() const override { return m_views[0].app.session; }
    std::vector<HostInputRoute> GetInputRoutes(const HostViewTargets&) const override { return {}; }
    bool StopView(std::string_view) override { return false; }
private:
    std::vector<HostRenderViewRuntime>& m_views;
};

struct RealCandidateFixture final {
    std::shared_ptr<RawVolumeDataManager> data = std::make_shared<RawVolumeDataManager>();
    std::shared_ptr<SharedStateBroadcaster> events = std::make_shared<SharedStateBroadcaster>();
    std::shared_ptr<SharedInteractionState> state = std::make_shared<SharedInteractionState>(events);
    std::vector<HostRenderViewRuntime> views{3};
    std::shared_ptr<FeatureViewLease> lease = std::make_shared<FeatureViewLease>(std::this_thread::get_id());
    std::array<AppFactoryResult, 3> ports;
    std::array<std::shared_ptr<ContextProbe>, 3> contexts;
    std::array<std::vector<std::shared_ptr<CandidateStrategyProbe>>, 3> strategies;
    std::array<RenderProductStatus, 3> nextStatus{RenderProductStatus::Idle,
        RenderProductStatus::Idle, RenderProductStatus::Idle};
    HostFrameRuntime frames{views, lease, [](const auto&) { return std::vector<std::string>{}; }};
    std::unique_ptr<HostFrameCoordinator> coordinator;
    std::shared_ptr<CandidateViewDirectory> directory;
    std::unique_ptr<HostCommandRouter> router;
    VtkImageGridSnapshot original;
    bool Build() {
        ImageMetadata metadata;
        metadata.identity.datasetId = "real-app-candidate-failure";
        metadata.source.kind = ImageSourceKind::Memory;
        metadata.source.uri = "memory://real-app-candidate-failure";
        const auto layout = VolumeLayout::Create({2,2,2}, {1,1,1}, {},
            std::array<double,9>{1,0,0,0,1,0,0,0,1}, metadata);
        const auto buffer = layout ? VolumeBuffer::Create({0,1,2,3,4,5,6,7}, *layout)
            : std::optional<VolumeBuffer>{};
        if (!buffer || !data->SetFromBuffer(*buffer)
            || !data->SetLoadCommit(data->GetLoadStage(), original)) return false;
        for (std::size_t index = 0; index < ports.size(); ++index) {
            AppServiceArgs args;
            args.dataManager = data;
            args.interactionState = state;
            args.eventSource = events;
            args.strategyCreate = [this, index](VizMode) {
                auto strategy = std::make_shared<CandidateStrategyProbe>();
                strategy->status = nextStatus[index];
                strategies[index].push_back(strategy);
                return strategy;
            };
            ports[index] = CreateAppPorts(std::move(args));
            auto window = vtkSmartPointer<vtkRenderWindow>::New();
            auto renderer = vtkSmartPointer<vtkRenderer>::New();
            if (!ports[index].renderBind->SetRenderTarget(window, renderer)) return false;
            AppViewUpdate initial;
            initial.mode = VizMode::SliceTop_down;
            initial.isoThreshold = 0.5;
            if (!ports[index].app.view->SendViewUpdate(initial)) return false;
            auto& view = views[index];
            view.config.id = std::to_string(index);
            view.config.role = index == 0 ? HostRenderViewRole::Primary3D : HostRenderViewRole::Auxiliary;
            view.app = ports[index].app;
            view.interaction = ports[index].interaction;
            view.context = contexts[index] = std::make_shared<ContextProbe>();
            view.isAvailable = true;
        }
        frames.BuildSceneStates();
        frames.SetDriveMode(HostDriveMode::HostDriven);
        frames.SetDataRead(data);
        if (!frames.SetFrameGeneration(29)) return false;
        HostFrameCoordinator::Callbacks callbacks;
        callbacks.collectUpdates = [&] { return frames.CollectFrameUpdates(); };
        callbacks.sendFeatureTicks = [] {};
        callbacks.setIntents = [&](const auto& intents) { return frames.SetFrameIntents(intents); };
        callbacks.applyFeatureUpdates = [&] { return frames.ApplyFrameUpdates(); };
        callbacks.buildStage = [&](auto epoch) { return frames.BuildFrameStage(epoch); };
        callbacks.setCommit = [&](auto epoch) { frames.SetFrameCommit(epoch); };
        callbacks.sendRender = [&](auto epoch) { return frames.SendFrameRender(epoch); };
        callbacks.getRenderPending = [&] { return frames.GetFrameRenderPending(); };
        callbacks.sendCompletions = [&] { frames.SendFrameCompletions(); };
        callbacks.clearStage = [&] { frames.ClearFrameStage(); };
        coordinator = std::make_unique<HostFrameCoordinator>(29, std::move(callbacks));
        directory = std::make_shared<CandidateViewDirectory>(views);
        router = std::make_unique<HostCommandRouter>(directory);
        return coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Completed
            && coordinator->GetCommittedEpoch() == 1;
    }
    VtkImageGridSnapshot Select(const bool createsData, const DataRevisionRef& target = {}) {
        const auto primary = data->GetPrimaryImage();
        const DataRevisionRef next = createsData ? DataRevisionRef{data->CreateDataEntityId(), 1} : target;
        DataTransaction transaction;
        if (createsData) transaction.outputs.push_back({next.entityId, 0, primary->data->type,
            {}, primary->data->payload, {}});
        transaction.bindings.push_back({std::string(primaryVolumeBinding), primary->binding->revision,
            true, primary->data->self, next});
        PrimaryDataActivation activation(*data, *state);
        return activation.SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded
            ? data->GetPrimaryImage() : VtkImageGridSnapshot{};
    }
    bool Stop() {
        if (coordinator) coordinator->Stop();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool stopped = true;
        for (auto& port : ports) if (port.taskControl) stopped = port.taskControl->SetTaskStopping() && stopped;
        for (auto& port : ports) if (port.taskControl) stopped = port.taskControl->StopTasks(deadline) && stopped;
        return stopped;
    }
    ~RealCandidateFixture() { if (!Stop()) std::cerr << "Real candidate fixture did not stop its workers\n"; }
};

bool GetRealCandidateFailureValid(const RenderProductStatus terminal, const bool hasReadyPeer,
    const bool rejectsCleanup = false)
{
    int completions = 0, recoveredCompletions = 0, blockedCompletions = 0;
    bool completed = true, recovered = false;
    RealCandidateFixture f;
    const auto check = [](bool value, const char* reason) {
        if (!value) std::cerr << "Real App candidate: " << reason << '\n';
        return value;
    };
    if (!check(f.Build(), "fixture construction")) return false;
    const auto oldStrategy = f.strategies[0].back();
    f.nextStatus = {RenderProductStatus::Preparing, RenderProductStatus::Preparing,
        hasReadyPeer ? RenderProductStatus::Idle : RenderProductStatus::Preparing};
    const auto selected = f.Select(true);
    if (!check(static_cast<bool>(selected), "new primary publication")) return false;
    HostViewSetRequest request;
    request.targetView.viewId = "0";
    request.opacity = 0.75;
    const auto display = [&](auto getApplied, auto complete) {
        return f.coordinator->SendDisplayComplete(std::move(getApplied), std::move(complete));
    };
    if (!check(f.router->Dispatch(std::move(request), [&](bool succeeded) {
        ++completions; completed = succeeded;
    }, display), "real Router display request")) return false;
    const auto preparing = f.coordinator->SendUpdates();
    if (!check((preparing == HostFrameCoordinator::FlushStatus::Deferred
            || (!hasReadyPeer && preparing == HostFrameCoordinator::FlushStatus::Completed))
        && completions == 0 && f.strategies[0].back() != oldStrategy
        && f.strategies[0].back()->attachments == 0
        && f.ports[1].app.view->GetViewState().isPresentationPending,
        "real candidate did not remain pending")) return false;
    const auto previous = f.frames.GetSceneStates();
    const auto epoch = f.coordinator->GetCommittedEpoch();
    struct AllowCleanup final {
        std::shared_ptr<CandidateStrategyProbe> strategy;
        ~AllowCleanup() { strategy->isDetachRejected = false; }
    } allowCleanup{f.strategies[0].back()};
    allowCleanup.strategy->isDetachRejected = rejectsCleanup;
    f.strategies[0].back()->status = terminal;
    if (!check(f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Failed,
        "terminal nextStrategy was hidden by another pending View")) return false;
    const auto failed = f.ports[0].app.view->GetViewState();
    if (!check(failed.hasPresentationFailed && !failed.isPresentationPending
        && oldStrategy->status == RenderProductStatus::Idle
        && completions == 1 && !completed && f.coordinator->GetCommittedEpoch() == epoch,
        "real projection or exactly-once failed display completion")) return false;
    const auto scenes = f.frames.GetSceneStates();
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        if (!check(scenes[index].sceneEpoch == previous[index].sceneEpoch
            && scenes[index].presentation && previous[index].presentation
            && scenes[index].presentation->dataRevision == previous[index].presentation->dataRevision
            && scenes[index].presentation->bindingRevision == previous[index].presentation->bindingRevision,
            "terminal candidate changed the committed scene")) return false;
    }
    // 混批时连显式绘制也必须暂缓；一致的旧帧仍允许调用方单独重绘。
    if (hasReadyPeer && !check(f.frames.SendFrameRender({{"0", "1", "2"}, .001},
        [] { return true; }).status == HostRenderStatus::Deferred, "mixed candidate allowed an explicit draw")) return false;
    if (!check(std::all_of(f.contexts.begin(), f.contexts.end(), [](const auto& context) { return context->renderCount == 0; }),
        "terminal candidate allowed a draw")) return false;
    // 无关图提交不使同一输入上的失败消失。
    DataTransaction unrelated;
    unrelated.outputs.push_back({f.data->CreateDataEntityId(), 0, f.original->data->type,
        {}, f.original->data->payload, {}});
    if (!check(f.data->SetDataCommit(std::move(unrelated)).status == DataCommitStatus::Succeeded
        && f.ports[0].app.view->GetViewState().hasPresentationFailed
        && f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Failed
        && completions == 1, "unrelated graph commit hid or repeated the failure")) return false;
    if (rejectsCleanup) {
        HostViewSetRequest blocked;
        blocked.targetView.viewId = "0";
        blocked.mode = HostRenderMode::Volume;
        bool blockedSucceeded = true;
        if (!f.router->Dispatch(std::move(blocked), [&](bool succeeded) {
            ++blockedCompletions; blockedSucceeded = succeeded;
        }, display)) return false;
        if (!check(f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Failed
            && blockedCompletions == 1 && !blockedSucceeded
            && f.coordinator->GetCommittedEpoch() == epoch,
            "a new request hid failure to clear the prior candidate")) return false;
        allowCleanup.strategy->isDetachRejected = false;
    }
    // 返回原数据但采用新 binding，旧候选终态不能因 ABA 再次生效。
    const auto retryTarget = terminal == RenderProductStatus::Cancelled
        ? selected->data->self : f.original->data->self;
    const auto restored = hasReadyPeer ? f.Select(false, retryTarget) : selected;
    if (!check(restored && (!hasReadyPeer || !f.ports[0].app.view->GetViewState().hasPresentationFailed),
        "new input inherited a stale candidate failure")) return false;
    f.nextStatus.fill(RenderProductStatus::Idle);
    if (!hasReadyPeer) {
        f.strategies[1].back()->status = RenderProductStatus::Idle;
        f.strategies[2].back()->status = RenderProductStatus::Idle;
    }
    HostViewSetRequest retry;
    retry.targetView.viewId = "0";
    retry.opacity = 0.8;
    if (!f.router->Dispatch(std::move(retry), [&](bool succeeded) {
        ++recoveredCompletions; recovered = succeeded;
    }, display)) return false;
    if (!check(!f.ports[0].app.view->GetViewState().hasPresentationFailed,
        "accepted new parameters inherited an old terminal state")) return false;
    if (!check(f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Completed
        && f.coordinator->GetCommittedEpoch() == epoch + 1
        && completions == 1 && recoveredCompletions == 1 && recovered,
        "new input did not recover exactly once")) return false;
    for (const auto& scene : f.frames.GetSceneStates()) {
        if (!check(scene.presentation && scene.presentation->dataRevision == restored->data->self
            && scene.presentation->bindingRevision == restored->binding->revision,
            "recovery used an old data or binding revision")) return false;
    }
    return check(f.Stop(), "real App workers did not stop");
}

bool GetRealCandidateRetryValid(const RenderProductStatus terminal, const bool retriesWhileBlocked)
{
    int failedCompletions = 0, blockedCompletions = 0, retryCompletions = 0;
    bool failedSucceeded = true, blockedSucceeded = true, retrySucceeded = false;
    RealCandidateFixture f;
    const auto check = [](const bool value, const char* reason) {
        if (!value) std::cerr << "Same-binding candidate retry: " << reason << '\n';
        return value;
    };
    if (!check(f.Build(), "fixture construction")) return false;
    f.nextStatus = {RenderProductStatus::Preparing, RenderProductStatus::Preparing, RenderProductStatus::Idle};
    const auto selected = f.Select(true);
    if (!check(static_cast<bool>(selected), "primary publication")) return false;
    const auto display = [&](auto applied, auto complete) {
        return f.coordinator->SendDisplayComplete(std::move(applied), std::move(complete));
    };
    const auto send = [&](const double opacity, auto complete) {
        HostViewSetRequest request;
        request.targetView.viewId = "0";
        request.opacity = opacity;
        return f.router->Dispatch(std::move(request), std::move(complete), display);
    };
    if (!send(0.75, [&](const bool success) { ++failedCompletions; failedSucceeded = success; })
        || !check(f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Deferred,
            "candidate was not deferred")) return false;
    const auto previous = f.frames.GetSceneStates();
    const auto epoch = f.coordinator->GetCommittedEpoch();
    const auto failed = f.strategies[0].back();
    struct AllowCleanup final {
        std::shared_ptr<CandidateStrategyProbe> strategy;
        ~AllowCleanup() { strategy->isDetachRejected = false; }
    } allowCleanup{failed};
    failed->isDetachRejected = true;
    failed->status = terminal;
    const auto count = f.strategies[0].size();
    const auto oldFrameKept = [&] {
        const auto scenes = f.frames.GetSceneStates();
        if (scenes.size() != previous.size() || f.coordinator->GetCommittedEpoch() != epoch) return false;
        for (std::size_t index = 0; index < scenes.size(); ++index)
            if (scenes[index].sceneEpoch != previous[index].sceneEpoch
                || scenes[index].presentationRevision != previous[index].presentationRevision
                || !scenes[index].presentation || !previous[index].presentation
                || scenes[index].presentation->dataRevision != previous[index].presentation->dataRevision
                || scenes[index].presentation->bindingRevision != previous[index].presentation->bindingRevision) return false;
        return true;
    };
    if (!check(f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Failed
        && failedCompletions == 1 && !failedSucceeded && oldFrameKept(),
        "initial terminal or cleanup failure did not preserve the old frame")) return false;
    if (retriesWhileBlocked) {
        if (!send(0.78, [&](const bool success) { ++blockedCompletions; blockedSucceeded = success; })
            || !check(!f.ports[0].app.view->GetViewState().hasPresentationFailed,
                "new request inherited the prior failure on acceptance")) return false;
        if (!check(f.coordinator->SendUpdates() == HostFrameCoordinator::FlushStatus::Failed
            && blockedCompletions == 1 && !blockedSucceeded && f.strategies[0].size() == count
            && f.ports[0].app.view->GetViewState().hasPresentationFailed && oldFrameKept(),
            "rejected cleanup was reported as completed or lost retry ownership")) return false;
    }
    // 清理恢复后，仅接纳新 opacity；主数据、binding 和生产参数均保持不变。
    failed->isDetachRejected = false;
    f.nextStatus.fill(RenderProductStatus::Idle);
    for (std::size_t index = 1; index < f.strategies.size(); ++index)
        f.strategies[index].back()->status = RenderProductStatus::Idle;
    if (!send(0.8, [&](const bool success) { ++retryCompletions; retrySucceeded = success; })
        || !check(!f.ports[0].app.view->GetViewState().hasPresentationFailed,
            "retry was not accepted as a new attempt")) return false;
    const auto result = f.coordinator->SendUpdates();
    if (!check(result == HostFrameCoordinator::FlushStatus::Completed
        && retryCompletions == 1 && retrySucceeded && failedCompletions == 1
        && blockedCompletions == (retriesWhileBlocked ? 1 : 0)
        && f.strategies[0].size() == count + 1 && f.strategies[0].back() != failed
        && !f.ports[0].app.view->GetViewState().hasPresentationFailed
        && f.coordinator->GetCommittedEpoch() == epoch + 1,
        "healthy new parameters consumed the old terminal candidate")) {
        std::cerr << "retry status=" << static_cast<int>(result) << " callbacks=" << retryCompletions
            << " success=" << retrySucceeded << " strategies=" << count << "->" << f.strategies[0].size()
            << " epoch=" << epoch << "->" << f.coordinator->GetCommittedEpoch() << '\n';
        return false;
    }
    for (const auto& scene : f.frames.GetSceneStates())
        if (!check(scene.presentation && scene.presentation->dataRevision == selected->data->self
            && scene.presentation->bindingRevision == selected->binding->revision,
            "retry changed the primary input identity")) return false;
    return check(std::all_of(f.contexts.begin(), f.contexts.end(), [](const auto& context) {
        return context->renderCount == 0;
    }) && f.Stop(), "retry drew a frame or did not stop its workers");
}

class DetachWindowProbe final : public vtkGenericOpenGLRenderWindow {
public:
    static DetachWindowProbe* New() { return new DetachWindowProbe; }
    vtkTypeMacro(DetachWindowProbe, vtkGenericOpenGLRenderWindow);
    int draws = 0;
    void Render() override { ++draws; InvokeEvent(vtkCommand::StartEvent); InvokeEvent(vtkCommand::EndEvent); }
    void* GetGenericWindowId() override { return this; }
protected:
    DetachWindowProbe() { Mapped = 1; SetReadyForRendering(true); }
};
class DetachInteractorProbe final : public vtkRenderWindowInteractor {
public:
    static DetachInteractorProbe* New() { return new DetachInteractorProbe; }
    vtkTypeMacro(DetachInteractorProbe, vtkRenderWindowInteractor);
    void Initialize() override { Initialized = 1; Enabled = 1; }
    void Start() override {}
};
class DetachFeatureProbe final : public HostFeature {
public:
    HostFeatureContext context;
    int detaches = 0;
    std::string_view GetFeatureId() const noexcept override { return "actual-detach-pending-frame"; }
    bool AttachHost(const HostFeatureContext& value) override { context = value; return context.host && context.host->SetActiveViews({"detach"}); }
    bool DetachHost() override { ++detaches; context = {}; return true; }
    bool OnHostTick() override { return true; }
};
bool GetActualDetachPendingFrameValid()
{
    int completions = 0;
    bool wasRevoked = false;
    auto window = vtkSmartPointer<DetachWindowProbe>::New();
    auto interactor = vtkSmartPointer<DetachInteractorProbe>::New();
    window->SetInteractor(interactor);
    interactor->SetRenderWindow(window);
    HostRenderViewConfig view;
    view.id = "detach";
    view.role = HostRenderViewRole::Primary3D;
    view.renderWindow = window;
    view.inputMode = HostInputMode::HostInjected;
    HostSessionConfig config;
    config.driveMode = HostDriveMode::HostDriven;
    config.renderViews = {view};
    VtkAppHostSession session(std::move(config));
    struct StopSession final {
        VtkAppHostSession& session;
        ~StopSession() { (void)session.Stop(); }
    } stop{session};
    auto feature = std::make_shared<DetachFeatureProbe>();
    if (!session.BuildSession() || !session.AttachFeature(feature)) {
        std::cerr << "Actual detach: session/feature construction\n";
        return false;
    }
    const auto update = session.SendUpdates();
    const auto before = session.GetSceneViewStates();
    const auto saved = feature->context;
    if (update.status != HostUpdateStatus::Completed || update.renderViewIds.empty()
        || before.size() != 1 || window->draws != 0
        || !saved.host->SendOwnerComplete([&] {
            ++completions;
            wasRevoked = !GetDataEntityIdValid(saved.data->CreateDataEntityId());
        })) {
        std::cerr << "Actual detach: pending frame/owner completion status=" << static_cast<int>(update.status)
            << " renders=" << update.renderViewIds.size() << " scenes=" << before.size()
            << " draws=" << window->draws << '\n';
        return false;
    }
    if (!session.DetachFeature(*feature) || feature->detaches != 1
        || completions != 1 || !wasRevoked || window->draws != 0) {
        std::cerr << "Actual detach: detach outcome count=" << feature->detaches
            << " completes=" << completions << " revoked=" << wasRevoked << " draws=" << window->draws << '\n';
        return false;
    }
    const auto after = session.GetSceneViewStates();
    return after.size() == 1 && after[0].sceneEpoch == before[0].sceneEpoch
        && after[0].activeFeatureIds == before[0].activeFeatureIds
        && !session.DetachFeature(*feature) && completions == 1
        && session.SendUpdates().status == HostUpdateStatus::Completed
        && window->draws == 0 && session.Stop() && completions == 1;
}

class ReadyProbe final : public IStateEventSink {
public:
    void SendFlags(UpdateFlags flags) override {
        if ((flags & UpdateFlags::DataReady) != UpdateFlags::None) ++readyCount;
    }
    int readyCount = 0;
};
bool GetActivationValid()
{
    const auto fail = [](int line) {
        std::cerr << "Activation assertion failed at line " << line << '\n';
        return false;
    };
    RawVolumeDataManager data;
    ImageMetadata metadata;
    metadata.identity.datasetId = "host-runtime-test";
    metadata.source.kind = ImageSourceKind::Memory;
    metadata.source.uri = "memory://host-runtime-test";
    const auto layout = VolumeLayout::Create({2,2,2}, {1.0f,1.0f,1.0f}, {},
        std::array<double,9>{1,0,0,0,1,0,0,0,1}, metadata);
    const auto buffer = layout ? VolumeBuffer::Create({0,1,2,3,4,5,6,7}, *layout)
        : std::optional<VolumeBuffer>{};
    if (!buffer || !data.SetFromBuffer(*buffer)) return fail(__LINE__);
    VtkImageGridSnapshot published;
    if (!data.SetLoadCommit(data.GetLoadStage(), published)) return fail(__LINE__);
    auto sink = std::make_shared<ReadyProbe>();
    SharedInteractionState state(sink);
    state.SetModelMatrix({1,0,0,4, 0,1,0,5, 0,0,1,6, 0,0,0,1});
    PrimaryDataActivation activation(data, state);
    DataReadyState staged;
    if (!PrimaryDataActivation::GetDataReadyState(published, &state, staged)) return fail(__LINE__);
    // 正确staged与失配staged分别走原有发布/重新投影路径。
    staged.cursorWorld = {11,12,13};
    activation.SetLoadReady(staged);
    if (state.GetCursorWorld() != staged.cursorWorld || sink->readyCount != 1) return fail(__LINE__);
    ++staged.bindingRevision;
    activation.SetLoadReady(staged);
    DataReadyState expected;
    if (!PrimaryDataActivation::GetDataReadyState(published, &state, expected)
        || state.GetCursorWorld() != expected.cursorWorld || sink->readyCount != 2) return fail(__LINE__);
    state.SetCursorWorld(7,8,9);
    const auto primary = data.GetPrimaryImage();
    DataTransaction samePrimary;
    samePrimary.bindings.push_back({std::string(primaryVolumeBinding),
        primary->binding->revision, true, primary->data->self, primary->data->self});
    if (activation.SetDataCommit(samePrimary).status != DataCommitStatus::Succeeded
        || state.GetCursorWorld() != std::array<double,3>{7,8,9}
        || sink->readyCount != 3) return fail(__LINE__);
    // 旧CAS拒绝和历史提交都不得发布激活事件。
    if (activation.SetDataCommit(samePrimary).status != DataCommitStatus::Rejected
        || sink->readyCount != 3) return fail(__LINE__);
    samePrimary.policy = DataPublishPolicy::AllowHistoricalResult;
    DataRevisionDraft historical;
    historical.entityId = data.CreateDataEntityId();
    historical.type = primary->data->type;
    historical.payload = primary->data->payload;
    samePrimary.outputs.push_back(std::move(historical));
    if (activation.SetDataCommit(samePrimary).status != DataCommitStatus::SucceededHistorical
        || sink->readyCount != 3) return fail(__LINE__);
    DataTransaction derived;
    derived.bindings.push_back({"test.derived",0,true,std::nullopt,primary->data->self});
    return activation.SetDataCommit(std::move(derived)).status == DataCommitStatus::Succeeded
        && sink->readyCount == 3;
}
}
bool GetDataTransitionTests();
bool GetResourceLifetimeTests();
int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "transition") return GetDataTransitionTests() ? 0 : 1;
    if (argc == 2 && std::string(argv[1]) == "resources") return GetResourceLifetimeTests() ? 0 : 1;
    if (argc == 2 && std::string(argv[1]) == "defaults") return GetAutoDisplayDefaultsFailCount() == 0 ? 0 : 1;
    int failures = 0;
    const auto check = [&](bool value, const char* name) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!value) ++failures;
    };
    check(GetResourceLifetimeTests(), "queued readers and derived allocation retirement");
    check(GetDataTransitionTests(), "candidate input transition and publication rollback");
    check(GetRulerCopyValid(), "draw publishes ruler without copying full presentation state");
    check(GetFrameFailuresValid(), "apply barrier and dirty recovery");
    check(GetFailedFrameWakeValid(), "failed frame retains changes without self-waking and resumes on new work");
    check(GetInputTransitionBarrierValid(false), "binding transition rejects partial frames and recovers on new work");
    check(GetInputTransitionBarrierValid(true), "data transition rejects partial frames and recovers on new work");
    check(GetInputTransitionBarrierValid(true, TransitionExit::Failed), "failed candidate terminates a deferred display request exactly once");
    check(GetInputTransitionBarrierValid(true, TransitionExit::Cancelled), "cancelled candidate terminates a deferred display request exactly once");
    check(GetInputTransitionBarrierValid(true, TransitionExit::Stop), "Stop cancels a deferred input transition exactly once");
    check(GetInputTransitionBarrierValid(true, TransitionExit::Detach), "Detach cancels deferred display ownership without blocking convergence");
    for (const auto terminal : {RenderProductStatus::Failed, RenderProductStatus::Cancelled})
        for (const bool readyPeer : {false, true})
            check(GetRealCandidateFailureValid(terminal, readyPeer), "real AppPort candidate terminal fails the frame and Router completion");
    check(GetActualDetachPendingFrameValid(), "actual DetachFeature drains owned completion while a frame is pending without drawing");
    check(GetRealCandidateFailureValid(RenderProductStatus::Failed, true, true),
        "new request reports prior-candidate cleanup failure and later recovers");
    for (const auto terminal : {RenderProductStatus::Failed, RenderProductStatus::Cancelled})
        for (const bool blocked : {false, true})
            check(GetRealCandidateRetryValid(terminal, blocked),
                "same-binding parameter retry replaces terminal candidates after cleanup recovers");
    check(GetRenderRetryValid(), "render retry preserves new dirty and completed views");
    check(GetStoppedStageValid(), "stopped view cannot reappear from staged projection");
    check(GetProjectionReuseValid(), "stable projection refreshes independent state and samples feature IDs once");
    check(GetPendingRollbackValid(), "stage rollback preserves inherited pending renders");
    check(GetActivationValid(), "primary activation cursor and publication ownership");
    check(GetAutoDisplayDefaultsFailCount() == 0, "automatic display defaults and asynchronous fallback lifecycle");
    return failures;
}
