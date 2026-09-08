#include "Host/Internal/HostFrameRuntime.h"
#include "App/Services/PrimaryDataActivation.h"
#include "App/Services/FeatureViewService.h"
#include "App/AppState.h"
#include "Data/DataManager.h"
#include "Interaction/AbstractViewContext.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

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
    bool SetRenderNeeded() override { isDirty = true; return true; }
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
    std::function<void()> onComplete;
};
class ContextProbe final : public AbstractViewContext {
public:
    bool SendRender() override { ++renderCount; if (onRender) onRender(); return isRenderAccepted; }
    std::optional<ViewCameraState> GetCameraState() const override {
        return hasCamera ? std::optional<ViewCameraState>{ViewCameraState{}} : std::nullopt;
    }
    bool SetCameraStyle(VizMode) override { return true; }
    bool SetInteractorReady() override { return true; }
    bool SetInputEnabled(bool) override { return true; }
    bool Start() override { return true; }
    bool StopInput() override { return true; }
    bool SetOrientationAxesVisible(bool) override { return true; }
    bool GetOrientationAxesVisible() const override { return false; }
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
    HostFrameRuntime frames{views, lease, [](const HostRenderViewRuntime&) {
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
        || !f.updates[0]->isDirty || !f.updates[1]->isDirty) return false;
    f.updates[1]->isClaimThrowing = false;
    f.contexts[1]->hasCamera = false;
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Failed
        || !f.updates[0]->isDirty || !f.updates[1]->isDirty) return false;
    f.contexts[1]->hasCamera = true;
    if (f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.ClearFrameStage();
    return f.updates[0]->isDirty && f.updates[1]->isDirty
        && f.frames.GetSceneStates()[0].sceneEpoch == 0;
}
bool GetRenderRetryValid()
{
    FrameFixture f;
    if (!f.frames.CollectFrameUpdates()
        || f.frames.BuildFrameStage(1) != HostFrameStageStatus::Ready) return false;
    f.frames.SetFrameCommit(1);
    f.contexts[1]->isRenderAccepted = false;
    bool wasBusy = false;
    f.contexts[0]->onRender = [&]() {
        wasBusy = f.frames.GetIsBusy();
        (void)f.updates[0]->SetRenderNeeded();
    };
    if (f.frames.SendFrameRender(1) || !f.frames.GetFrameRenderPending()
        || !wasBusy || !f.updates[0]->isDirty) return false;
    if (f.frames.BuildFrameStage(2) != HostFrameStageStatus::Failed) return false;
    f.contexts[1]->isRenderAccepted = true;
    if (!f.frames.SendFrameRender(1) || f.contexts[0]->renderCount != 1
        || f.contexts[1]->renderCount != 2 || !f.updates[0]->isDirty) return false;
    f.frames.SendFrameCompletions();
    return f.updates[0]->completeCount == 1 && f.updates[1]->completeCount == 1
        && f.frames.BuildFrameStage(2) == HostFrameStageStatus::Ready;
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
int main()
{
    int failures = 0;
    const auto check = [&](bool value, const char* name) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!value) ++failures;
    };
    check(GetRulerCopyValid(), "draw publishes ruler without copying full presentation state");
    check(GetFrameFailuresValid(), "apply barrier and dirty recovery");
    check(GetRenderRetryValid(), "render retry preserves new dirty and completed views");
    check(GetStoppedStageValid(), "stopped view cannot reappear from staged projection");
    check(GetActivationValid(), "primary activation cursor and publication ownership");
    return failures;
}
