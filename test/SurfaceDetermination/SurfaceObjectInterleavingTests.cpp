// 测试用途：通过受控只读源、真实数据图和真实帧提交核验对象高亮的退役与取消交错。
#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationTestSupport.h"
#include "../ControlledTaskGate.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "SurfaceGenerationStore.h"
#include "../../MVVCVTK/features/common/FeatureInputReads.h"
#include "Host/Internal/HostFrameRuntime.h"
#include "Interaction/AbstractViewContext.h"
#include "App/Services/FeatureViewService.h"
#include "Render/Contracts/OverlayService.h"

#include <algorithm>
#include <deque>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <set>
#include <optional>
#include <utility>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <crtdbg.h>

namespace SurfaceObjectAllocationProbe {
thread_local std::size_t remaining = 0, hits = 0;
void Arm(std::size_t count) noexcept { remaining = count; hits = 0; }
void Stop() noexcept { remaining = 0; }
struct Reset final { ~Reset() { Stop(); } };
}
// 只在测试 owner 的指定阶段启用一次；其他线程及其他用例沿用普通分配。
void* operator new(std::size_t bytes)
{
    using namespace SurfaceObjectAllocationProbe;
    if (remaining && --remaining == 0) { ++hits; throw std::bad_alloc(); }
    if (auto* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace {
using TaskInterleaving::Gate;
using SurfaceTest::Checks;
constexpr std::string_view surfaceFeatureId = "surface-determination";

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// 只拦截实际读取受控 points 的对象工作线程，避免把其他后台端口通知当成终末通知。
class ObjectNoticeProbe final {
public:
    void RememberWorker() {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_workers.insert(std::this_thread::get_id());
    }
    void HoldTerminalNotice() {
        bool objectWorker = false;
        { const std::lock_guard<std::mutex> lock(m_mutex); objectWorker = m_workers.count(std::this_thread::get_id()) != 0; }
        if (objectWorker) terminal->Hold();
    }
    std::shared_ptr<Gate> terminal = std::make_shared<Gate>();
private:
    std::mutex m_mutex;
    std::set<std::thread::id> m_workers;
};

class HeldPoints final : public MeshValueSource<SurfacePointRecord> {
public:
    HeldPoints(std::shared_ptr<const MeshReadArray<SurfacePointRecord>> points,
        std::shared_ptr<Gate> gate, std::shared_ptr<ObjectNoticeProbe> notices)
        : m_points(std::move(points)), m_gate(std::move(gate)), m_notices(std::move(notices)), m_owner(std::this_thread::get_id()) {}
    std::size_t GetCount() const noexcept override { return m_points->size(); }
    std::size_t GetResidentBytes() const noexcept override { return m_points->GetResidentBytes(); }
    void ReadValues(std::size_t offset, std::size_t count, SurfacePointRecord* output) const override
    {
        if (std::this_thread::get_id() != m_owner) { m_notices->RememberWorker(); m_gate->Hold(); }
        if (offset > m_points->size() || count > m_points->size() - offset)
            throw std::out_of_range("HeldPoints range");
        std::copy_n(m_points->begin() + offset, count, output);
    }
private:
    std::shared_ptr<const MeshReadArray<SurfacePointRecord>> m_points;
    std::shared_ptr<Gate> m_gate;
    std::shared_ptr<ObjectNoticeProbe> m_notices;
    std::thread::id m_owner;
};

class DataPort final : public TestDataPort {
public:
    DataPort(std::shared_ptr<Gate> gate, std::shared_ptr<ObjectNoticeProbe> notices)
        : m_gate(std::move(gate)), m_notices(std::move(notices)) {}
    DataCommitResult SetDataCommit(DataTransaction transaction) override
    {
        for (auto& output : transaction.outputs) {
            // 真实加载源采用显式退役域；旧单元夹具的无域源不会触发自动结果退役。
            if (output.type == DataTypes::imageGrid3D && output.inputs.empty()) {
                output.lifetimeScope = CreateDataEntityId();
                output.lifetimePolicy = DataLifetimePolicy::ExplicitRetirement;
            }
            if (!m_wrapped && output.type == surfaceGenerationType) {
                const auto payload = std::dynamic_pointer_cast<const SurfaceGenerationPayload>(output.payload);
                Require(payload && payload->GetGeneration(), "generation wrapper requires formal payload");
                auto generation = std::make_shared<SurfaceGenerationSnapshot>(*payload->GetGeneration());
                generation->points = std::make_shared<const MeshReadArray<SurfacePointRecord>>(
                    std::make_shared<const HeldPoints>(generation->points, m_gate, m_notices));
                output.payload = std::make_shared<const SurfaceGenerationPayload>(generation,
                    payload->GetStatistics(), payload->GetStorage(), payload->GetDisplaySource(), payload->GetDisplayError());
                m_wrapped = true;
            }
        }
        return TestDataPort::SetDataCommit(std::move(transaction));
    }
private:
    std::shared_ptr<Gate> m_gate;
    std::shared_ptr<ObjectNoticeProbe> m_notices;
    bool m_wrapped = false;
};

class OverlayPort final : public OverlayService, public FeatureViewService {
public:
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> overlay) override
    { if (rejectAttach) return false; overlays.push_back(std::move(overlay)); return true; }
    void RemoveOverlay(std::shared_ptr<FeatureOverlay> overlay) noexcept override
    { overlays.erase(std::remove(overlays.begin(), overlays.end(), overlay), overlays.end()); }
    void ClearOverlays() noexcept override { overlays.clear(); }
    bool SetInteracting(const InteractionSource&, bool) override { return true; }
    std::optional<std::array<double,16>> GetModelToWorld() const override
    { return std::array<double,16>{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
    std::optional<std::array<double,3>> GetWorldPosition(const std::array<double,3>& point) const override { return point; }
    std::optional<RenderInputStamp> GetRenderInputStamp() const override { return RenderInputStamp{source}; }
    bool AttachRenderEffect(std::shared_ptr<RenderEffect>) override { return false; }
    bool DetachRenderEffect(const RenderEffect*) override { return false; }
    bool SetRenderNeeded() override { return true; }
    DataRevisionRef source;
    bool rejectAttach = false;
    std::vector<std::shared_ptr<FeatureOverlay>> overlays;
};

class ViewDirectory final : public FeatureViewDirectory {
public:
    std::vector<HostFeatureView> GetViews(const HostViewTargets& targets) const override
    {
        return std::find(targets.viewIds.begin(), targets.viewIds.end(), "primary") != targets.viewIds.end()
            || std::find(targets.viewRoles.begin(), targets.viewRoles.end(), HostRenderViewRole::Primary3D) != targets.viewRoles.end()
            ? std::vector<HostFeatureView>{{"primary", HostRenderViewRole::Primary3D}} : std::vector<HostFeatureView>{};
    }
    std::shared_ptr<FeatureViewService> GetFeaturePort(const std::string& id) const override { return id == "primary" ? port : nullptr; }
    std::shared_ptr<OverlayService> GetOverlayPort(const std::string& id) const override { return id == "primary" ? port : nullptr; }
    std::optional<HostInputView> GetInputView(const HostViewTarget&) const override { return {}; }
    std::shared_ptr<OverlayPort> port = std::make_shared<OverlayPort>();
};

// 与 HostRuntimeTests 的同类探针相同：只替代窗口端口，帧事务与数据校验保留真实实现。
class AppView final : public AppViewPort {
public:
    bool SetViewConfig(const PreInitConfig&) override { return true; }
    bool SendViewUpdate(const AppViewUpdate&) override { return true; }
    bool SetViewState(const AppViewState&, std::uint64_t) override { return true; }
    AppViewState GetViewState() const override { return state; }
    AppViewState state;
};
class Updates final : public RenderUpdatePort {
public:
    bool SendUpdates() override { return true; }
    bool SendPendingUpdates() override { return true; }
    void SendCompletions() override {}
    bool SetRenderNeeded() override { dirty = true; return true; }
    bool GetRenderNeeded() const override { return dirty; }
    bool ResetRenderNeeded() override { return std::exchange(dirty, false); }
    bool dirty = true;
};
class ViewContext final : public AbstractViewContext {
public:
    bool SendRender() override { return true; }
    std::optional<ViewCameraState> GetCameraState() const override { return ViewCameraState{}; }
    bool SetCameraStyle(VizMode) override { return true; }
    bool SetInteractorReady() override { return true; }
    bool SetInputEnabled(bool) override { return true; }
    bool GetIsInputIdle() const override { return true; }
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
};

class HostControl final : public FeatureHostControl {
public:
    bool AttachInput(HostInputBinding) override { return true; }
    bool DetachInput(std::string_view) override { ++detachInputCount; return true; }
    bool SetInputOwner(std::string_view, bool, const std::vector<std::string>&) override { return true; }
    bool SetActiveViews(const std::vector<std::string>& views) override
    { activeViews = views; if (afterActiveViews) afterActiveViews(); return true; }
    bool SetViewStatus(const std::vector<std::string>&, const std::string&) override { return true; }
    bool SendSceneDelta(FeatureSceneDelta delta) override
    {
        ++sceneDeltaCalls;
        objectSubmitted = std::any_of(delta.displays.begin(), delta.displays.end(),
            [](const auto& display) { return display.localId == "surface-object"; });
        const bool accepted = frames && frames->Enqueue(std::string(surfaceFeatureId), std::move(delta), GetAttachmentId());
        if (accepted) (void)SendWorkAvailable();
        if (afterSceneDelta) afterSceneDelta();
        return accepted;
    }
    bool SendOwnerComplete(std::function<void()> complete) override
    {
        if (completeSynchronously) complete();
        else { completes.push_back(std::move(complete)); (void)SendWorkAvailable(); }
        return true;
    }
    bool SendWorkAvailable() override
    {
        { const std::lock_guard<std::mutex> lock(m_workMutex); m_pendingWork = true; }
        m_workReady.notify_one();
        if (notices) notices->HoldTerminalNotice();
        return true;
    }
    bool GetWorkPending()
    { const std::lock_guard<std::mutex> lock(m_workMutex); return m_pendingWork; }
    bool TakeWork()
    {
        const std::lock_guard<std::mutex> lock(m_workMutex);
        if (!std::exchange(m_pendingWork, false)) return false;
        ++consumedNotices;
        return true;
    }
    bool WaitTakeWork(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(m_workMutex);
        if (!m_workReady.wait_until(lock, deadline, [this] { return m_pendingWork; })) return false;
        m_pendingWork = false; ++consumedNotices;
        return true;
    }
    std::uint64_t GetAttachmentId() const noexcept override { return 1; }
    std::optional<HostSemanticTarget> GetDisplayTarget(const std::string& view, const std::string& role) const override
    { return getDisplayTarget ? getDisplayTarget(view, role) : std::nullopt; }
    void Drain()
    {
        if (beforeDrain) beforeDrain();
        auto ready = std::move(completes); completes.clear();
        for (auto& complete : ready) complete();
    }
    std::shared_ptr<HostFrameCoordinator> frames;
    std::deque<std::function<void()>> completes;
    std::vector<std::string> activeViews;
    std::function<std::optional<HostSemanticTarget>(const std::string&, const std::string&)> getDisplayTarget;
    std::function<void()> beforeDrain;
    std::function<void()> afterActiveViews, afterSceneDelta;
    bool objectSubmitted = false;
    bool completeSynchronously = false;
    int detachInputCount = 0;
    std::shared_ptr<ObjectNoticeProbe> notices;
    std::size_t consumedNotices = 0;
    std::size_t sceneDeltaCalls = 0;
private:
    std::mutex m_workMutex;
    std::condition_variable m_workReady;
    bool m_pendingWork = false;
};

struct Fixture final {
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::shared_ptr<ObjectNoticeProbe> notices = std::make_shared<ObjectNoticeProbe>();
    std::shared_ptr<DataPort> data = std::make_shared<DataPort>(gate, notices);
    std::shared_ptr<HostControl> host = std::make_shared<HostControl>();
    std::shared_ptr<ViewDirectory> views = std::make_shared<ViewDirectory>();
    std::vector<HostRenderViewRuntime> participants{1};
    std::shared_ptr<FeatureViewLease> lease = std::make_shared<FeatureViewLease>(std::this_thread::get_id());
    HostFrameRuntime runtime{participants, lease, [this](const HostRenderViewRuntime&) {
        return host->activeViews.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::string(surfaceFeatureId)};
    }};
    std::unique_ptr<SurfaceDeterminationHostFeature> feature;
    HostFrameStageStatus lastStage = HostFrameStageStatus::Unchanged;
    HostFrameCoordinator::FlushStatus lastFlush = HostFrameCoordinator::FlushStatus::Completed;
    std::size_t failedStages = 0;
    bool rejectObjectFrame = false;
    std::size_t tickExceptions = 0;

    Fixture()
    {
        host->notices = notices;
        const auto sphere = SurfaceTest::BuildSphere();
        const auto source = data->SetPrimaryImage(sphere->image);
        Require(source && source->binding, "scoped primary source must publish");
        auto app = std::make_shared<AppView>();
        app->state.dataRevision = source->data->self;
        app->state.bindingRevision = source->binding->revision;
        views->port->source = source->data->self;
        auto& view = participants.front();
        view.config.id = "primary"; view.config.role = HostRenderViewRole::Primary3D;
        view.app.view = app; view.interaction.update = std::make_shared<Updates>();
        view.featureView = views->port; view.context = std::make_shared<ViewContext>(); view.isAvailable = true;
        runtime.BuildSceneStates(); runtime.SetDataRead(data);
        Require(runtime.SetFrameGeneration(1), "frame generation must initialize");
        HostFrameCoordinator::Callbacks callbacks;
        callbacks.collectUpdates = [this] { return runtime.CollectFrameUpdates(); };
        callbacks.sendFeatureTicks = [this] {
            // 与真实 HostFeatureRuntime 一致：Feature 异常隔离在 owner tick 边界，不能变成夹具异常出口。
            try { if (feature) (void)feature->OnHostTick(); } catch (...) { ++tickExceptions; }
        };
        callbacks.setIntents = [this](const auto& intents) { return runtime.SetFrameIntents(intents); };
        callbacks.applyFeatureUpdates = [this] { return runtime.ApplyFrameUpdates(); };
        callbacks.buildStage = [this](auto epoch) {
            lastStage = rejectObjectFrame && host->objectSubmitted
                ? HostFrameStageStatus::Failed : runtime.BuildFrameStage(epoch);
            if (lastStage == HostFrameStageStatus::Failed) ++failedStages;
            return lastStage;
        };
        callbacks.setCommit = [this](auto epoch) { runtime.SetFrameCommit(epoch); };
        callbacks.sendRender = [this](auto epoch) { return runtime.SendFrameRender(epoch); };
        callbacks.getRenderPending = [this] { return runtime.GetFrameRenderPending(); };
        callbacks.sendCompletions = [this] { runtime.SendFrameCompletions(); };
        callbacks.clearStage = [this] { runtime.ClearFrameStage(); };
        callbacks.sendReadyCompletions = [this] { host->Drain(); };
        host->frames = std::make_shared<HostFrameCoordinator>(1, std::move(callbacks));
        host->getDisplayTarget = [this](const auto& view, const auto& role) -> std::optional<HostSemanticTarget> {
            const auto scene = runtime.GetSceneState(0);
            if (!scene || scene->sceneEpoch == 0 || scene->id != view) return {};
            for (const auto& display : scene->displays)
                if (display.featureId == surfaceFeatureId && display.localId == role)
                    return HostSemanticTarget{display, {}, scene->sceneEpoch, 0};
            return {};
        };
        SurfaceDeterminationConfig config; config.maxWorkingBytes = 128U * 1024U * 1024U;
        feature = std::make_unique<SurfaceDeterminationHostFeature>(config);
        HostFeatureContext context; context.data = data; context.views = views; context.host = host;
        Require(feature->AttachHost(context), "Surface fixture must attach");
    }
    ~Fixture()
    {
        SurfaceObjectAllocationProbe::Stop();
        host->afterActiveViews = {}; host->afterSceneDelta = {};
        host->getDisplayTarget = {};
        gate->Release(); notices->terminal->Release();
        if (feature) (void)feature->DetachHost();
        host->Drain();
        host->frames->Stop();
    }
    void Tick() { lastFlush = host->frames->SendUpdates(); }
    DataEntityId GetScope(DataRevisionRef ref) const
    {
        const auto value = data->GetData(data->GetDataGraph(), ref);
        Require(bool(value), "scope requires a readable exact revision");
        return value->lifetimeScope;
    }
    std::size_t GetObjectReaderCount(DataEntityId scope) const
    {
        const auto state = data->GetDataLifetime(scope);
        return std::count_if(state.blockers.begin(), state.blockers.end(),
            [](const auto& blocker) { return blocker.owner == "surface-object-reader"; });
    }
    DataCommitResult Retire(DataEntityId scope) const
    {
        const auto state = data->GetDataLifetime(scope);
        DataTransaction transaction;
        transaction.retireScopes.push_back({scope, DataLifetimeStatus::Published, state.ownedRevisions, true});
        return data->SetDataCommit(std::move(transaction));
    }
    bool Pump(const std::function<bool()>& ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + TaskInterleaving::synchronizationTimeout;
        while (!ready() && std::chrono::steady_clock::now() < deadline) { Tick(); std::this_thread::yield(); }
        return ready();
    }
    bool PumpQueued(const std::function<bool()>& ready, const std::function<void()>& afterEvent = {})
    {
        const auto deadline = std::chrono::steady_clock::now() + TaskInterleaving::synchronizationTimeout;
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            if (!host->WaitTakeWork(deadline)) return ready();
            Tick();
            if (afterEvent) afterEvent();
        }
        return ready();
    }
    void DrainQueued()
    {
        const auto deadline = std::chrono::steady_clock::now() + TaskInterleaving::synchronizationTimeout;
        while (host->TakeWork()) {
            Tick();
            Require(std::chrono::steady_clock::now() < deadline, "queued owner notifications did not drain");
        }
    }
    void StartFormalFromNotices()
    {
        struct Completion { bool ready = false; SurfaceDeterminationResult result; };
        const auto completed = std::make_shared<Completion>();
        auto params = SurfaceTest::GetParams(); params.targetViews = {};
        const auto admission = feature->SendRequest(SurfaceStartRequest{params}, [completed](auto value) {
            completed->result = std::move(value); completed->ready = true;
        });
        Require(admission.status == SurfaceAdmissionStatus::Accepted && PumpQueued([&] { return completed->ready; }),
            "formal control must complete using actual queued notifications only");
        Require(completed->result.status == SurfaceResultStatus::Succeeded && completed->result.isPublished,
            "event-driven formal control must publish");
        DrainQueued();
    }
    void StartFormal()
    {
        bool ready = false; SurfaceDeterminationResult result;
        const auto admission = feature->SendRequest(SurfaceStartRequest{SurfaceTest::GetParams()},
            [&](auto value) { result = std::move(value); ready = true; });
        Require(admission.status == SurfaceAdmissionStatus::Accepted, "formal request must be accepted");
        Require(Pump([&] { return ready; }), "formal request must complete through owner dispatcher");
        Require(result.status == SurfaceResultStatus::Succeeded && result.isPublished, "formal request must publish");
    }
    bool GetObjectCommitted(DataRevisionRef mesh, std::uint64_t requestId) const
    {
        const auto state = runtime.GetSceneState(0);
        return state && std::any_of(state->displays.begin(), state->displays.end(), [&](const auto& display) {
            return display.localId == "surface-object" && display.data == mesh && display.operation.requestId == requestId;
        });
    }
};

void TestObjectRetirement(Checks& checks, bool protectWithReader)
{
    Fixture fixture;
    TaskInterleaving::ReleaseOnExit release(fixture.gate);
    fixture.StartFormal();
    const auto first = fixture.feature->GetSurfaceSnapshot();
    Require(first && first->objects && !first->objects->empty(), "formal generation must have an object");
    const auto scene = fixture.runtime.GetSceneState(0);
    Require(scene && std::any_of(scene->displays.begin(), scene->displays.end(), [&](const auto& display) {
        return display.localId == "surface" && display.data == first->meshRevision;
    }), "control: first formal mesh must actually commit");
    const auto objectScope = fixture.GetScope(first->dataRevision);
    const auto sourceScope = fixture.GetScope(first->sourceRevision);
    std::optional<FeatureInternal::InputReads> readers;
    if (protectWithReader) {
        readers = FeatureInternal::StartInputReads(fixture.data->GetDataGraph(),
            std::vector<DataRevisionRef>{first->dataRevision, first->meshRevision}, "object-test-control-reader");
        Require(readers.has_value(), "control Reader must acquire exact generation and mesh");
    }
    fixture.gate->Arm();
    int completed = 0; bool callbackOnOwner = false; SurfaceDeterminationResult highlighted;
    const auto owner = std::this_thread::get_id();
    const SurfaceObjectSelectionRequest selection{first->dataRevision, first->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    const auto admission = fixture.feature->SendRequest(selection, [&](auto result) {
        highlighted = std::move(result); ++completed; callbackOnOwner = std::this_thread::get_id() == owner;
    });
    Require(admission.status == SurfaceAdmissionStatus::Accepted && fixture.gate->WaitEntered(),
        "object worker must enter the held immutable point source");
    fixture.StartFormal();
    Require(fixture.gate->GetIsHeld() && completed == 0, "replacement must publish while the old object worker remains held");
    const auto graph = fixture.data->GetDataGraph();
    const bool generationReadable = bool(fixture.data->GetData(graph, first->dataRevision));
    const bool meshReadable = bool(fixture.data->GetData(graph, first->meshRevision));
    Require(generationReadable && meshReadable && fixture.GetObjectReaderCount(objectScope) == 1,
        "the preparing highlight must retain its exact generation and mesh across replacement");
    Require(fixture.data->GetPrimaryImage()->data->self == first->sourceRevision, "source volume must remain unchanged");
    const auto epochBefore = fixture.host->frames->GetCommittedEpoch();
    const auto failuresBefore = fixture.failedStages;
    fixture.gate->Release();
    Require(fixture.Pump([&] { return completed == 1; }), "object callback must complete after gate release");
    const bool committed = fixture.GetObjectCommitted(first->meshRevision, admission.requestId);
    const bool succeeded = highlighted.status == SurfaceResultStatus::Succeeded;
    std::cout << "object-retirement reader=" << protectWithReader << " generation_readable=" << generationReadable
        << " mesh_readable=" << meshReadable << " callback_succeeded=" << succeeded
        << " committed_object=" << committed << " frame_failed=" << (fixture.lastFlush == HostFrameCoordinator::FlushStatus::Failed)
        << " epoch_before=" << epochBefore << " epoch_after=" << fixture.host->frames->GetCommittedEpoch() << '\n';
    checks.Get(succeeded == committed, "object callback success must match an actual committed surface-object display");
    checks.Get(callbackOnOwner && completed == 1 && highlighted.dataRevision == first->dataRevision,
        "highlight must complete once on owner with the requested generation");
    checks.Get(succeeded && committed && fixture.failedStages == failuresBefore,
        "the exact historical object commits with its own Reader in both control modes");
    readers.reset();
    fixture.StartFormal();
    checks.Get(fixture.GetObjectCommitted(first->meshRevision, admission.requestId)
        && fixture.Retire(objectScope).failureReason == DataCommitFailure::ResultInUse,
        "an adopted highlight retains its exact outputs across a further formal publication");
    (void)fixture.feature->SendRequest(SurfaceObjectSelectionRequest{});
    fixture.Tick();
    checks.Get(fixture.GetObjectReaderCount(objectScope) == 0 && fixture.GetObjectReaderCount(sourceScope) == 0
        && fixture.Retire(objectScope).status == DataCommitStatus::Succeeded
        && !fixture.data->GetData(fixture.data->GetDataGraph(), first->meshRevision),
        "clear releases the adopted highlight Reader and permits exact output retirement");
}

void TestObjectOwnerWait(Checks& checks, bool replace)
{
    Fixture fixture;
    TaskInterleaving::ReleaseOnExit release(fixture.gate);
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    Require(generation && generation->objects && !generation->objects->empty(), "owner wait fixture must have an object");
    fixture.gate->Arm();
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    std::vector<SurfaceResultStatus> completed;
    bool callbacksOnOwner = true; const auto owner = std::this_thread::get_id();
    const auto callback = [&](auto result) {
        completed.push_back(result.status); callbacksOnOwner = callbacksOnOwner && std::this_thread::get_id() == owner;
    };
    Require(fixture.feature->SendRequest(selection, callback).status == SurfaceAdmissionStatus::Accepted
        && fixture.gate->WaitEntered(), "owner wait object worker must enter the gate");
    SurfaceDeterminationAdmission control;
    const auto observation = TaskInterleaving::ObserveOwnerCall(fixture.gate, [&] {
        if (replace) control = fixture.feature->SendRequest(selection, callback);
        else control = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{});
    });
    Require(control.status == SurfaceAdmissionStatus::Accepted, "clear or replace must be accepted");
    Require(fixture.Pump([&] { return completed.size() == (replace ? 2U : 1U); }), "cancelled highlight must complete once");
    checks.Get(callbacksOnOwner && completed.front() == SurfaceResultStatus::Cancelled,
        "cancelled highlight completes once on the owner thread");
    if (replace) checks.Get(completed.back() == SurfaceResultStatus::Succeeded, "replacement highlight must remain usable");
    std::cout << (replace ? "object-replace-wait" : "object-clear-wait")
        << " elapsed_ms=" << observation.elapsedMs << " returned_while_held=" << observation.returnedWhileHeld << '\n';
    checks.Get(observation.returnedWhileHeld, replace
        ? "replacing a highlight must return while its cancelled worker is held"
        : "clearing a highlight must return while its cancelled worker is held");
}

void TestDetachHeldObject(Checks& checks, bool stopFrames = false, bool retireBeforeStop = false)
{
    Fixture fixture;
    TaskInterleaving::ReleaseOnExit release(fixture.gate);
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    const auto sourceScope = fixture.GetScope(generation->sourceRevision);
    fixture.gate->Arm();
    int calls = 0; SurfaceDeterminationResult result; bool callbackOnOwner = false;
    const auto owner = std::this_thread::get_id();
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    Require(fixture.feature->SendRequest(selection, [&](auto value) {
        result = std::move(value); ++calls; callbackOnOwner = std::this_thread::get_id() == owner;
    }).status == SurfaceAdmissionStatus::Accepted && fixture.gate->WaitEntered(), "detach worker must enter gate");
    if (retireBeforeStop)
        Require(fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}).status == SurfaceAdmissionStatus::Accepted
            && calls == 1, "Stop control owns a previously cancelled retired worker");
    if (stopFrames) fixture.host->frames->Stop();
    const auto submittedBeforeStop = fixture.host->sceneDeltaCalls;
    bool detached = true; bool retained = false; SurfaceAdmissionStatus stopping{};
    const auto observation = TaskInterleaving::ObserveOwnerCall(fixture.gate, [&] {
        detached = fixture.feature->DetachHost();
        retained = fixture.gate->GetIsHeld() && fixture.GetObjectReaderCount(scope) == 1
            && fixture.GetObjectReaderCount(sourceScope) == 1 && fixture.host->detachInputCount == 0
            && fixture.feature->GetState().stage == SurfaceDeterminationStage::Stopping;
        stopping = fixture.feature->SendRequest(selection).status;
    });
    checks.Get(!detached && retained && stopping == SurfaceAdmissionStatus::Stopping && observation.returnedWhileHeld,
        "detach must return retryable failure while preserving a held task, Readers and attachment");
    Require(fixture.Pump([&] { if (stopFrames) detached = fixture.feature->DetachHost();
        return calls == 1 && fixture.GetObjectReaderCount(scope) == 0
        && fixture.GetObjectReaderCount(sourceScope) == 0; }), "closing tick must reap the completed cancelled task");
    checks.Get(callbackOnOwner && result.status == SurfaceResultStatus::Cancelled && fixture.feature->DetachHost()
        && fixture.host->detachInputCount == 1 && calls == 1, "detach retry succeeds after owner reclamation without duplicate callback");
    if (stopFrames) checks.Get(fixture.host->sceneDeltaCalls == submittedBeforeStop,
        "held or retired object Stop never submits a new frame after frame admission closes");
}

void TestDetachClosedFrames(Checks& checks, bool highlight)
{
    int calls = 0;
    Fixture fixture;
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    if (highlight) {
        const auto admission = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{
            generation->dataRevision, generation->objects->front().objectIndex, false, SurfaceTest::GetParams().targetViews},
            [&](auto result) { ++calls; checks.Get(result.status == SurfaceResultStatus::Succeeded,
                "Stop control highlight is actually committed before frame admission closes"); });
        Require(admission.status == SurfaceAdmissionStatus::Accepted && fixture.Pump([&] { return calls == 1; }),
            "Stop control has a completed object highlight");
    }
    // 与 VtkAppHostSession::Stop 的顺序一致，先关闭普通帧准入，再解绑 Feature。
    fixture.host->frames->Stop();
    const auto submittedBeforeStop = fixture.host->sceneDeltaCalls;
    const bool detached = fixture.feature->DetachHost();
    checks.Get(detached, "Surface detaches after the Host has closed frame admission");
    checks.Get(detached && fixture.views->port->overlays.empty()
        && fixture.GetObjectReaderCount(scope) == 0 && fixture.host->detachInputCount == 1,
        "closed-frame Stop releases overlays, exact object readers and input attachment");
    const auto binding = fixture.data->GetDataBinding(fixture.data->GetDataGraph(), std::string(surfaceResultBinding));
    checks.Get(detached && (!binding || !binding->target) && !fixture.feature->GetSurfaceSnapshot(),
        "closed-frame Stop clears the actual formal result binding and private result owner");
    checks.Get(fixture.host->sceneDeltaCalls == submittedBeforeStop,
        "closed-frame Stop does not submit a new scene update");
    fixture.host->Drain();
    checks.Get(calls == (highlight ? 1 : 0), "closed-frame Stop does not repeat a completed highlight callback");
    std::fprintf(stderr, "object-closed-frames highlight=%d detached=%d callbacks=%d\n", highlight, detached, calls);
}

void TestObjectBacklog(Checks& checks)
{
    Fixture fixture;
    TaskInterleaving::ReleaseOnExit release(fixture.gate);
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    fixture.gate->Arm();
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    int calls = 0; int rejectedCalls = 0;
    const auto cancelled = [&](auto result) { ++calls; checks.Get(result.status == SurfaceResultStatus::Cancelled, "backlogged selection cancels"); };
    Require(fixture.feature->SendRequest(selection, cancelled).status == SurfaceAdmissionStatus::Accepted
        && fixture.gate->WaitEntered(), "backlog worker must enter gate");
    SurfaceDeterminationAdmission replacement, rejected;
    const auto observation = TaskInterleaving::ObserveOwnerCall(fixture.gate, [&] {
        replacement = fixture.feature->SendRequest(selection, cancelled);
        rejected = fixture.feature->SendRequest(selection, [&](auto) { ++rejectedCalls; });
        Require(fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}).status == SurfaceAdmissionStatus::Accepted,
            "clear remains accepted at object capacity");
        checks.Get(fixture.GetObjectReaderCount(scope) <= 2, "object task Readers stay bounded while blocked");
    });
    checks.Get(observation.returnedWhileHeld && replacement.status == SurfaceAdmissionStatus::Accepted
        && rejected.status == SurfaceAdmissionStatus::Busy && rejectedCalls == 0,
        "bounded object preparation rejects extra admission without blocking or completing a rejected request");
    Require(fixture.Pump([&] { return calls == 2 && fixture.GetObjectReaderCount(scope) == 0; }),
        "both cancelled tasks must release their Readers on later ticks");
}

void TestObjectFrameFailure(Checks& checks, bool rejectAttach)
{
    Fixture fixture;
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    fixture.rejectObjectFrame = !rejectAttach;
    fixture.views->port->rejectAttach = rejectAttach;
    int calls = 0; SurfaceDeterminationResult result;
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    const auto admission = fixture.feature->SendRequest(selection, [&](auto value) { result = std::move(value); ++calls; });
    Require(admission.status == SurfaceAdmissionStatus::Accepted, "failed-frame object is admitted");
    if (rejectAttach) {
        Require(fixture.Pump([&] { return calls == 1; }), "rejected overlay preparation completes");
        checks.Get(result.status == SurfaceResultStatus::Failed && result.failureReason == SurfaceFailureReason::DisplayFailed
            && !fixture.GetObjectCommitted(generation->meshRevision, admission.requestId), "rejected preparation reports display failure");
    }
    else {
        Require(fixture.Pump([&] { return fixture.failedStages != 0; }), "object candidate reaches the rejected frame");
        fixture.Tick();
        checks.Get(calls == 0 && !fixture.GetObjectCommitted(generation->meshRevision, admission.requestId),
            "uncommitted frame keeps the highlight pending without a success callback");
        fixture.rejectObjectFrame = false;
        Require(fixture.Pump([&] { return calls == 1; }), "frame retry adopts the pending highlight");
        checks.Get(result.status == SurfaceResultStatus::Succeeded
            && fixture.GetObjectCommitted(generation->meshRevision, admission.requestId), "frame retry succeeds only after actual adoption");
    }
    checks.Get(fixture.feature->GetSurfaceSnapshot() == generation, "object display failure and retry preserve the formal result");
    fixture.rejectObjectFrame = false;
    fixture.views->port->rejectAttach = false;
    (void)fixture.feature->SendRequest(SurfaceObjectSelectionRequest{});
    Require(fixture.Pump([&] { return fixture.GetObjectReaderCount(scope) == 0; }), "failed highlight clear releases Readers");
}

void TestClearBeforeCompletion(Checks& checks)
{
    Fixture fixture;
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    bool cleared = false;
    fixture.host->beforeDrain = [&] {
        if (!cleared && fixture.host->objectSubmitted) {
            cleared = true;
            Require(fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}).status == SurfaceAdmissionStatus::Accepted,
                "clear before dispatch is accepted");
        }
    };
    int calls = 0; SurfaceDeterminationResult result;
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    const auto admission = fixture.feature->SendRequest(selection, [&](auto value) { result = std::move(value); ++calls; });
    Require(admission.status == SurfaceAdmissionStatus::Accepted && fixture.Pump([&] { return calls == 1; }),
        "cleared committed candidate must complete once");
    fixture.Tick();
    checks.Get(cleared && result.status == SurfaceResultStatus::Cancelled && calls == 1
        && !fixture.GetObjectCommitted(generation->meshRevision, admission.requestId)
        && fixture.GetObjectReaderCount(scope) == 0, "clear between frame adoption and owner confirmation cancels the candidate and releases Readers");
}

void TestSuccessCallbackReentry(Checks& checks)
{
    Fixture fixture;
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    const auto sourceScope = fixture.GetScope(generation->sourceRevision);
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    int calls = 0; SurfaceDeterminationAdmission nested;
    SurfaceDeterminationCallback receive;
    receive = [&](auto result) {
        ++calls;
        checks.Get(result.status == SurfaceResultStatus::Succeeded
            && fixture.GetObjectCommitted(generation->meshRevision, result.requestId),
            "success callback observes the actual exact frame before reentry");
        checks.Get(fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}).status == SurfaceAdmissionStatus::Accepted
            && fixture.GetObjectReaderCount(scope) == 0 && fixture.GetObjectReaderCount(sourceScope) == 0,
            "success callback can clear and immediately release all highlight Readers");
        if (calls == 1) nested = fixture.feature->SendRequest(selection, receive);
        throw std::runtime_error("deliberate observer exception after committed highlight");
    };
    Require(fixture.feature->SendRequest(selection, receive).status == SurfaceAdmissionStatus::Accepted
        && fixture.Pump([&] { return calls == 2; }), "reentrant success callback must preserve the next accepted highlight");
    fixture.Tick();
    checks.Get(nested.status == SurfaceAdmissionStatus::Accepted && calls == 2
        && fixture.feature->GetSurfaceSnapshot() == generation && fixture.GetObjectReaderCount(scope) == 0,
        "observer exceptions do not roll back the formal result, leak Readers or repeat completion");
}

void TestBindingCancelDetach(Checks& checks, bool requestEntry)
{
    Fixture fixture;
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    int calls = 0; bool detached = false;
    Require(fixture.feature->SendRequest(selection, [&](auto result) {
        ++calls;
        checks.Get(result.status == SurfaceResultStatus::Cancelled, "binding replacement cancels pending confirmation");
        detached = fixture.feature->DetachHost();
    }).status == SurfaceAdmissionStatus::Accepted
        && fixture.Pump([&] { return fixture.host->objectSubmitted; }), "candidate must await owner confirmation");
    const auto binding = fixture.data->GetDataBinding(fixture.data->GetDataGraph(), std::string(surfaceResultBinding));
    Require(binding.has_value() && calls == 0, "binding reentry control has an uncompleted candidate");
    DataTransaction transaction;
    transaction.bindings.push_back({binding->name, binding->revision, true, binding->target, binding->target});
    Require(fixture.data->SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded,
        "external binding revision advances without replacing the source");
    fixture.host->completeSynchronously = true;
    if (requestEntry) {
        const auto control = fixture.feature->SendRequest(SurfaceObjectPickModeRequest{false});
        checks.Get(control.status == SurfaceAdmissionStatus::Unavailable, "request rechecks detach after binding cancellation callback");
    }
    else checks.Get(!fixture.feature->OnHostTick(), "tick stops after binding cancellation callback detaches");
    checks.Get(detached && calls == 1 && fixture.feature->GetState().stage == SurfaceDeterminationStage::Idle,
        "synchronous cancel callback can detach without stale projection, port access or duplicate completion");
}

void TestAppliedQueryException(Checks& checks)
{
    int calls = 0; bool injected = false; SurfaceDeterminationResult result;
    Fixture fixture;
    fixture.StartFormal();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto query = fixture.host->getDisplayTarget;
    fixture.host->getDisplayTarget = [&, query](const auto& view, const auto& role) {
        if (!injected && role == "surface-object") { injected = true; throw std::bad_alloc(); }
        return query(view, role);
    };
    const auto admission = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{
        generation->dataRevision, generation->objects->front().objectIndex, false, SurfaceTest::GetParams().targetViews},
        [&](auto value) { result = std::move(value); ++calls; });
    Require(admission.status == SurfaceAdmissionStatus::Accepted, "query-fault highlight is accepted");
    bool escaped = false;
    try { (void)fixture.Pump([&] { return calls == 1; }); } catch (const std::bad_alloc&) { escaped = true; }
    checks.Get(injected && !escaped && calls == 1 && result.status == SurfaceResultStatus::Failed,
        "display confirmation query failure completes the admitted request once instead of escaping its owner tick");
    fixture.host->getDisplayTarget = query;
    (void)fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}); fixture.Tick();
    checks.Get(calls == 1, "confirmation failure cannot complete again during cleanup");
    std::cout << "object-query-fault injected=" << injected << " escaped=" << escaped << " callbacks=" << calls << '\n';
}

void TestAdoptionAllocationException(Checks& checks)
{
    int calls = 0; bool ownerCallback = true, injected = false, escaped = false; SurfaceDeterminationResult result;
    const auto owner = std::this_thread::get_id();
    Fixture fixture;
    SurfaceObjectAllocationProbe::Reset reset;
    fixture.StartFormal(); fixture.DrainQueued();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto admission = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{
        generation->dataRevision, generation->objects->front().objectIndex, false, SurfaceTest::GetParams().targetViews},
        [&](auto value) { result = std::move(value); ++calls; ownerCallback = std::this_thread::get_id() == owner; });
    Require(admission.status == SurfaceAdmissionStatus::Accepted, "allocation-fault highlight is accepted");
    fixture.host->afterSceneDelta = [&] {
        if (!injected && fixture.host->objectSubmitted) {
            injected = true;
            // 旧路径先分配成功说明，下一次为 try 外捕获 views；按实际阶段而非进程总次数注入。
            SurfaceObjectAllocationProbe::Arm(2);
        }
    };
    try { (void)fixture.Pump([&] { return injected; }); } catch (const std::bad_alloc&) { escaped = true; }
    SurfaceObjectAllocationProbe::Stop();
    const auto hits = SurfaceObjectAllocationProbe::hits;
    fixture.host->afterSceneDelta = {};
    Require(injected, "fault probe reaches the actual object display adoption");
    const bool completed = fixture.Pump([&] { return calls == 1; });
    checks.Get(completed && calls == 1 && ownerCallback,
        "an allocation fault after display adoption cannot destroy the admitted callback");
    checks.Get(completed && ((result.status == SurfaceResultStatus::Succeeded)
        == fixture.GetObjectCommitted(generation->meshRevision, admission.requestId)),
        "allocation recovery success still requires the exact committed object display");
    std::cout << "object-adoption-allocation injected=" << injected << " hits=" << hits
        << " escaped=" << escaped << " callbacks=" << calls
        << " tick_exceptions=" << fixture.tickExceptions << std::endl;
    std::fputs("object-adoption cleanup begin\n", stderr); std::fflush(stderr);
    (void)fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}); fixture.Tick();
    std::fputs("object-adoption cleanup end\n", stderr); std::fflush(stderr);
}

void TestCancelAllocationException(Checks& checks, bool allocation)
{
    int calls = 0, clearCalls = 0; bool injected = false, escaped = false; SurfaceDeterminationResult result;
    Fixture fixture;
    SurfaceObjectAllocationProbe::Reset reset;
    TaskInterleaving::ReleaseOnExit release(fixture.gate);
    fixture.StartFormal(); fixture.DrainQueued();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto scope = fixture.GetScope(generation->dataRevision);
    fixture.gate->Arm();
    const auto admission = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{
        generation->dataRevision, generation->objects->front().objectIndex, false, SurfaceTest::GetParams().targetViews},
        [&](auto value) { result = std::move(value); ++calls; });
    Require(admission.status == SurfaceAdmissionStatus::Accepted && fixture.gate->WaitEntered(),
        "cancel allocation probe holds a real admitted worker read");
    fixture.host->afterSceneDelta = [&] {
        if (!injected && !fixture.host->objectSubmitted) {
            injected = true;
            if (allocation) SurfaceObjectAllocationProbe::Arm(1);
            else throw std::bad_alloc();
        }
    };
    SurfaceDeterminationAdmission cleared; bool readersHeldAtReturn = false;
    std::fputs("object-cancel action begin\n", stderr); std::fflush(stderr);
    const auto observed = TaskInterleaving::ObserveOwnerCall(fixture.gate, [&] {
        SurfaceObjectAllocationProbe::Reset stopInjection;
        try { cleared = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}, [&](auto) { ++clearCalls; }); }
        catch (const std::bad_alloc&) { escaped = true; }
        SurfaceObjectAllocationProbe::Stop();
        readersHeldAtReturn = fixture.gate->GetIsHeld() && fixture.GetObjectReaderCount(scope) == 1;
    });
    std::fputs("object-cancel action and gate join end\n", stderr); std::fflush(stderr);
    const auto hits = SurfaceObjectAllocationProbe::hits;
    fixture.host->afterSceneDelta = {};
    checks.Get(injected && observed.returnedWhileHeld && !escaped,
        "cancellation retains retirement ownership before any allocation and never joins a held worker on the owner");
    checks.Get(calls == 1 && result.status == SurfaceResultStatus::Cancelled
        && cleared.status == (allocation ? SurfaceAdmissionStatus::Accepted : SurfaceAdmissionStatus::InvalidRequest)
        && clearCalls == (allocation ? 1 : 0),
        "highlight completes once; a rejected clear projection is retryable and has no completion");
    checks.Get(readersHeldAtReturn,
        "cancellation keeps the exact reader until the held worker really exits");
    std::cout << "object-cancel-fault mode=" << (allocation ? "allocation" : "scene-exception")
        << " injected=" << injected << " hits=" << hits
        << " escaped=" << escaped << " returned_while_held=" << observed.returnedWhileHeld
        << " callbacks=" << calls << " clear_callbacks=" << clearCalls
        << " tick_exceptions=" << fixture.tickExceptions << std::endl;
    if (!allocation) {
        cleared = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}, [&](auto) { ++clearCalls; });
        checks.Get(cleared.status == SurfaceAdmissionStatus::Accepted && clearCalls == 1 && calls == 1,
            "clear retry succeeds after the failed projection without repeating the highlight completion");
    }
    fixture.gate->Release();
    Require(fixture.Pump([&] { return fixture.GetObjectReaderCount(scope) == 0; }), "cancelled allocation-probe reader eventually releases");
    checks.Get(calls == 1, "retired allocation-probe worker cannot repeat completion");
}

class ObjectFailureMessage final : public std::exception {
public:
    const char* what() const noexcept override {
        SurfaceObjectAllocationProbe::Arm(1);
        return "original object display failure with an intentionally long diagnostic that cannot fit the prepared fallback string without allocation";
    }
};
void TestFailureDiagnosticAllocation(Checks& checks)
{
    int calls = 0; bool injected = false, escaped = false; SurfaceDeterminationResult result;
    Fixture fixture;
    SurfaceObjectAllocationProbe::Reset reset;
    fixture.StartFormal(); fixture.DrainQueued();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    const auto admission = fixture.feature->SendRequest(SurfaceObjectSelectionRequest{
        generation->dataRevision, generation->objects->front().objectIndex, false, SurfaceTest::GetParams().targetViews},
        [&](auto value) { result = std::move(value); ++calls; });
    Require(admission.status == SurfaceAdmissionStatus::Accepted, "diagnostic-fault highlight is accepted");
    fixture.host->afterActiveViews = [&] { if (!injected) { injected = true; throw ObjectFailureMessage{}; } };
    try { (void)fixture.Pump([&] { return calls == 1; }); } catch (const std::bad_alloc&) { escaped = true; }
    SurfaceObjectAllocationProbe::Stop();
    const auto hits = SurfaceObjectAllocationProbe::hits;
    checks.Get(injected && !escaped && calls == 1 && result.status == SurfaceResultStatus::Failed
        && result.failureReason == SurfaceFailureReason::DisplayFailed,
        "diagnostic allocation failure preserves one failed completion instead of replacing the original failure path");
    checks.Get(calls == 1 && !result.message.empty(), "diagnostic allocation failure retains a prepared fallback message");
    std::cout << "object-diagnostic-allocation injected=" << injected << " hits=" << hits
        << " escaped=" << escaped << " callbacks=" << calls
        << " tick_exceptions=" << fixture.tickExceptions << std::endl;
    fixture.host->afterActiveViews = {};
    (void)fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}); fixture.Tick();
}

enum class NoticeAction { Success, Clear, Detach };

void TestTerminalNoticeBeforeExit(Checks& checks, NoticeAction action)
{
    // 回调槽先于夹具声明，失败清理时也不会引用已经展开的栈对象。
    int calls = 0, recoveredCalls = 0; bool callbackOnOwner = true, detached = false;
    SurfaceDeterminationResult terminal, recovered;
    const auto owner = std::this_thread::get_id();
    Fixture fixture;
    TaskInterleaving::ReleaseOnExit readRelease(fixture.gate);
    TaskInterleaving::ReleaseOnExit noticeRelease(fixture.notices->terminal);
    fixture.StartFormalFromNotices();
    const auto generation = fixture.feature->GetSurfaceSnapshot();
    Require(generation && generation->objects && !generation->objects->empty(), "notice fixture needs a formal object");
    const auto scope = fixture.GetScope(generation->dataRevision);
    const auto sourceScope = fixture.GetScope(generation->sourceRevision);
    fixture.gate->Arm(); fixture.notices->terminal->Arm();
    const SurfaceObjectSelectionRequest selection{generation->dataRevision, generation->objects->front().objectIndex,
        false, SurfaceTest::GetParams().targetViews};
    const auto admission = fixture.feature->SendRequest(selection, [&](auto value) {
        terminal = std::move(value); ++calls; callbackOnOwner = callbackOnOwner && std::this_thread::get_id() == owner;
    });
    Require(admission.status == SurfaceAdmissionStatus::Accepted && fixture.gate->WaitEntered(),
        "object worker must be held inside its immutable source");
    if (action == NoticeAction::Clear)
        Require(fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}).status == SurfaceAdmissionStatus::Accepted,
            "event-driven clear must be accepted");
    if (action == NoticeAction::Detach)
        checks.Get(!fixture.feature->DetachHost(), "held work makes the first detach retryable");
    fixture.DrainQueued();
    const int expectedBeforeFinish = action == NoticeAction::Success ? 0 : 1;
    Require(calls == expectedBeforeFinish && !fixture.host->GetWorkPending(), "initial notifications must be fully consumed");
    fixture.gate->Release();
    Require(fixture.notices->terminal->WaitEntered(), "terminal notification must enter before the worker returns");
    bool followupQueued = false, retainedWhileHeld = false, callbackWaited = false;
    const auto observation = TaskInterleaving::ObserveOwnerCall(fixture.notices->terminal, [&] {
        Require(fixture.host->TakeWork(), "premature owner tick must consume the actual terminal notification");
        fixture.Tick();
        if (action == NoticeAction::Detach) detached = fixture.feature->DetachHost();
        followupQueued = fixture.host->GetWorkPending();
        retainedWhileHeld = fixture.notices->terminal->GetIsHeld()
            && fixture.GetObjectReaderCount(scope) == 1 && fixture.GetObjectReaderCount(sourceScope) == 1;
        callbackWaited = calls == expectedBeforeFinish && !detached;
    });
    checks.Get(observation.returnedWhileHeld && retainedWhileHeld && callbackWaited,
        "owner polling and detach must not wait or adopt a worker still inside terminal notification");
    checks.Get(followupQueued, "consuming a terminal notice before actual exit must leave a queued follow-up wake");
    std::cout << "object-terminal-notice action=" << static_cast<int>(action)
        << " returned_while_held=" << observation.returnedWhileHeld << " followup_queued=" << followupQueued
        << " readers_held=" << retainedWhileHeld << '\n';
    if (!followupQueued) return; // 旧实现的确定性失败；不靠新请求或额外 tick 救活。
    if (action == NoticeAction::Success) {
        Require(fixture.PumpQueued([&] { return calls == 1; }), "success must progress through queued notices alone");
        checks.Get(terminal.status == SurfaceResultStatus::Succeeded && callbackOnOwner
            && fixture.GetObjectCommitted(generation->meshRevision, admission.requestId),
            "event-driven success corresponds to the exact adopted object frame");
        Require(fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}).status == SurfaceAdmissionStatus::Accepted,
            "completed event-driven object can be cleared");
        fixture.DrainQueued();
    }
    else {
        Require(fixture.PumpQueued([&] {
            return fixture.GetObjectReaderCount(scope) == 0 && fixture.GetObjectReaderCount(sourceScope) == 0
                && (action != NoticeAction::Detach || detached);
        }, [&] { if (action == NoticeAction::Detach && !detached) detached = fixture.feature->DetachHost(); }),
            "cancelled object must release Readers and finish detach from queued notices alone");
        checks.Get(calls == 1 && callbackOnOwner && terminal.status == SurfaceResultStatus::Cancelled,
            "event-driven cancellation completes once on the owner");
    }
    checks.Get(fixture.GetObjectReaderCount(scope) == 0 && fixture.GetObjectReaderCount(sourceScope) == 0,
        "terminal event chain releases exact Readers without a new request or unconditional tick");
    std::cout << "object-terminal-recovery action=" << static_cast<int>(action) << " callbacks=" << calls
        << " readers_left=" << fixture.GetObjectReaderCount(scope) + fixture.GetObjectReaderCount(sourceScope)
        << " detached=" << detached << " queued_events=" << fixture.host->consumedNotices << '\n';
    if (action == NoticeAction::Clear) {
        // 先证明租约已经自行释放，再发新请求，避免新请求中的回收掩盖名额泄漏。
        const auto next = fixture.feature->SendRequest(selection, [&](auto value) { recovered = std::move(value); ++recoveredCalls; });
        Require(next.status == SurfaceAdmissionStatus::Accepted && fixture.PumpQueued([&] { return recoveredCalls == 1; }),
            "reclaimed object slot accepts and completes another highlight");
        checks.Get(recovered.status == SurfaceResultStatus::Succeeded && calls == 1, "reclaimed capacity does not repeat cancelled completion");
        (void)fixture.feature->SendRequest(SurfaceObjectSelectionRequest{}); fixture.DrainQueued();
    }
}

template<class Test> void Run(Checks& checks, const char* name, Test&& test)
{
    std::fprintf(stderr, "object-case-begin %s\n", name); std::fflush(stderr);
    try { test(); }
    catch (...) {
        // what() 本身是诊断分配故障的注入点；夹具出口不可再次调用或分配错误字符串。
        SurfaceObjectAllocationProbe::Stop(); ++checks.failureCount;
        std::fprintf(stderr, "FAILED: object case %s escaped its fixture\n", name); std::fflush(stderr);
    }
    std::fprintf(stderr, "object-case-end %s\n", name); std::fflush(stderr);
}
} // namespace

int GetSurfaceObjectRetirementFailCount()
{
    // 故障注入的失败必须进入测试日志/退出码，不能停在交互式 CRT 错误窗口。
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    const auto previousTerminate = std::set_terminate([] {
        std::fputs("object-probe terminate: ", stderr);
        SurfaceObjectAllocationProbe::Stop();
        std::fputs(std::current_exception() ? "active exception" : "no active exception", stderr);
        std::fputc('\n', stderr); std::fflush(stderr); std::_Exit(86);
    });
    Checks checks;
    Run(checks, "unleased retirement", [&] { TestObjectRetirement(checks, false); });
    Run(checks, "Reader control", [&] { TestObjectRetirement(checks, true); });
    Run(checks, "frame rejected", [&] { TestObjectFrameFailure(checks, false); });
    Run(checks, "overlay rejected", [&] { TestObjectFrameFailure(checks, true); });
    Run(checks, "clear before completion", [&] { TestClearBeforeCompletion(checks); });
    Run(checks, "success callback reentry", [&] { TestSuccessCallbackReentry(checks); });
    Run(checks, "binding cancel detach tick", [&] { TestBindingCancelDetach(checks, false); });
    Run(checks, "binding cancel detach request", [&] { TestBindingCancelDetach(checks, true); });
    Run(checks, "confirmation query exception", [&] { TestAppliedQueryException(checks); });
    Run(checks, "adoption allocation exception", [&] { TestAdoptionAllocationException(checks); });
    Run(checks, "cancellation scene exception", [&] { TestCancelAllocationException(checks, false); });
#if _ITERATOR_DEBUG_LEVEL == 0
    Run(checks, "cancellation allocation exception", [&] { TestCancelAllocationException(checks, true); });
#else
    // MSVC xstring 的 noexcept move 在 Debug 分配 iterator proxy；全局注入会终止于标准库，
    // 无法作为本业务的有效分配负例。Debug 使用上面的端口异常，Release 另跑全局分配注入。
    std::fputs("object-cancel-allocation NOT_RUN: Debug STL noexcept iterator-proxy allocation\n", stderr);
#endif
    Run(checks, "failure diagnostic allocation", [&] { TestFailureDiagnosticAllocation(checks); });
    std::set_terminate(previousTerminate);
    return checks.failureCount;
}

int GetSurfaceObjectOwnerWaitFailCount()
{
    Checks checks;
    Run(checks, "clear wait", [&] { TestObjectOwnerWait(checks, false); });
    Run(checks, "replace wait", [&] { TestObjectOwnerWait(checks, true); });
    Run(checks, "detach held", [&] { TestDetachHeldObject(checks); });
    Run(checks, "detach closed frames", [&] { TestDetachClosedFrames(checks, false); });
    Run(checks, "detach highlighted closed frames", [&] { TestDetachClosedFrames(checks, true); });
    Run(checks, "detach held closed frames", [&] { TestDetachHeldObject(checks, true); });
    Run(checks, "detach retired closed frames", [&] { TestDetachHeldObject(checks, true, true); });
    Run(checks, "bounded backlog", [&] { TestObjectBacklog(checks); });
    return checks.failureCount;
}

int GetSurfaceObjectWakeupFailCount()
{
    Checks checks;
    Run(checks, "event-driven success", [&] { TestTerminalNoticeBeforeExit(checks, NoticeAction::Success); });
    Run(checks, "event-driven clear", [&] { TestTerminalNoticeBeforeExit(checks, NoticeAction::Clear); });
    Run(checks, "event-driven detach", [&] { TestTerminalNoticeBeforeExit(checks, NoticeAction::Detach); });
    return checks.failureCount;
}
