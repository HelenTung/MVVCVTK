#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeature.h"
#include "Host/Internal/HostFeatureRuntime.h"
#include "Host/Internal/HostImageReadRuntime.h"
#include "Host/HostFrameCoordinator.h"
#include "Host/HostWorkSignal.h"
#include "Host/HostInputRegistry.h"
#include "Interaction/InteractionControllerAccess.h"
#include "Host/HostViewRuntimeRegistry.h"

#include "App/AppState.h"
#include "App/Services/AppServiceFactory.h"
#include "Data/DataManager.h"
#include "Data/DataPayloads.h"
#include "Data/LabelMapReader.h"
#include "Render/Support/RenderFrameLifetime.h"
#include "Data/RoiService.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <limits>
#include <cmath>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

bool RoiService::ReleaseTransientScopes(AbstractDataManager& data,
    std::vector<DataEntityId>& releasingScopes)
{
    releasingScopes.erase(std::remove_if(releasingScopes.begin(), releasingScopes.end(),
        [&](const auto& scope) { return data.SetDataRelease(scope).status == DataLifetimeStatus::Released; }),
        releasingScopes.end());
    const auto graph = data.GetDataGraph();
    if (!graph.view) return releasingScopes.empty();
    std::map<DataEntityId, DataLifetimeState> scopes;
    std::set<DataRevisionRef> retiringRefs;
    const auto dependsOnFeatureResult = [&](const DataSnapshot& revision) {
        std::vector<DataRevisionRef> pending;
        std::set<DataRevisionRef> visited;
        for (const auto& input : revision->inputs) pending.push_back(input.source);
        while (!pending.empty()) {
            const auto ref = pending.back(); pending.pop_back();
            if (!visited.insert(ref).second) continue;
            const auto input = graph.view->GetData(ref);
            if (!input) continue;
            const bool isHostRoi = input->type == DataTypes::roiGeometry || input->type == DataTypes::roiCatalog
                || (input->provenance && input->provenance->producerId == "Host.Roi");
            const bool isHostSource = input->provenance && input->provenance->producerId == "org.mvvcvtk.host";
            if (GetDataEntityIdValid(input->lifetimeScope) && !isHostRoi && !isHostSource) return true;
            for (const auto& parent : input->inputs) pending.push_back(parent.source);
        }
        return false;
    };
    {
        const auto revisions = graph.view->GetDataQuery({});
        for (const auto& revision : revisions.data) {
            // ROI 几何/目录由 Host 薄服务管理；掩码仅接管同一服务产生的输出。
            const bool isRoi = revision->type == DataTypes::roiGeometry
                || revision->type == DataTypes::roiCatalog
                || (revision->provenance && revision->provenance->producerId == "Host.Roi"
                    && revision->type == DataTypes::labelMap3D);
            if (!isRoi || revision->lifetimePolicy != DataLifetimePolicy::ExplicitRetirement
                || !GetDataEntityIdValid(revision->lifetimeScope) || !dependsOnFeatureResult(revision)) continue;
            const auto lifetime = data.GetDataLifetime(revision->lifetimeScope);
            if (lifetime.status == DataLifetimeStatus::Published) {
                scopes.emplace(lifetime.scopeId, lifetime);
                retiringRefs.insert(lifetime.ownedRevisions.begin(), lifetime.ownedRevisions.end());
            }
        }
    }
    if (scopes.empty()) return releasingScopes.empty();
    // 显式 ROI 的下游 Surface/计量等结果同样保留到用户卸载，Feature detach 不负责退休。
    // Session 停止时收集这一显式派生闭包；OwnerRetirement 消费者仍由其 Feature 先清理。
    std::vector<DataRevisionRef> pending(retiringRefs.begin(), retiringRefs.end());
    for (std::size_t index = 0; index < pending.size(); ++index)
        for (const auto& ref : graph.view->GetDerivedData(pending[index])) {
            const auto derived = graph.view->GetData(ref);
            if (!derived || derived->lifetimePolicy != DataLifetimePolicy::ExplicitRetirement
                || !GetDataEntityIdValid(derived->lifetimeScope) || scopes.count(derived->lifetimeScope)) continue;
            const auto lifetime = data.GetDataLifetime(derived->lifetimeScope);
            if (lifetime.status != DataLifetimeStatus::Published) continue;
            scopes.emplace(lifetime.scopeId, lifetime);
            for (const auto& owned : lifetime.ownedRevisions)
                if (retiringRefs.insert(owned).second) pending.push_back(owned);
        }
    const auto scopeCount = scopes.size();
    // ROI 可能再次成为裁切等生产者的输入。先回收没有集合外消费者的叶层，
    // 其余 scope 等对应 Feature 关闭后重试，不能让上游 ROI 阻塞整个清理批次。
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto scope = scopes.begin(); scope != scopes.end();) {
            bool blocked = false;
            for (const auto& revision : scope->second.ownedRevisions)
                for (const auto& derived : graph.view->GetDerivedData(revision))
                    if (!retiringRefs.count(derived) && graph.view->GetData(derived)) blocked = true;
            if (!blocked) { ++scope; continue; }
            for (const auto& revision : scope->second.ownedRevisions)
                for (const auto& derived : graph.view->GetDerivedData(revision)) {
                    const auto dependent = graph.view->GetData(derived);
                    if (!retiringRefs.count(derived) && dependent)
                        std::cerr << "[Roi] Transient scope waits for type=" << dependent->type.name
                            << " producer=" << (dependent->provenance ? dependent->provenance->producerId : "none")
                            << " operation=" << (dependent->provenance ? dependent->provenance->operationId : "none") << '\n';
                }
            for (const auto& revision : scope->second.ownedRevisions) retiringRefs.erase(revision);
            scope = scopes.erase(scope);
            changed = true;
        }
    }
    if (scopes.empty()) return false;
    DataTransaction transaction;
    for (const auto& item : scopes)
        transaction.retireScopes.push_back({item.first, DataLifetimeStatus::Published,
            item.second.ownedRevisions, true});
    for (const auto& binding : graph.view->GetDataBindings()) {
        if (!binding.target || !retiringRefs.count(*binding.target)) continue;
        std::optional<DataRevisionRef> replacement;
        if (binding.name == roiCatalogBinding) {
            const auto catalogData = graph.view->GetData(*binding.target);
            const auto* catalog = catalogData ? dynamic_cast<const RoiCatalogPayload*>(catalogData->payload.get()) : nullptr;
            if (!catalog || catalogData->self.generation == std::numeric_limits<DataGeneration>::max()) return false;
            auto entries = catalog->GetEntries();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return retiringRefs.count(entry.geometry) != 0; }), entries.end());
            // 保留普通来源的 ROI 与原目录身份，不将其他数据集一并清空。
            replacement = DataRevisionRef{catalogData->self.entityId, catalogData->self.generation + 1};
            std::vector<DataInputRef> inputs;
            for (std::size_t index = 0; index < entries.size(); ++index)
                inputs.push_back({"roi-" + std::to_string(index), entries[index].geometry});
            transaction.outputs.push_back({replacement->entityId, replacement->generation - 1,
                DataTypes::roiCatalog, std::move(inputs),
                std::make_shared<const RoiCatalogPayload>(std::move(entries)),
                DataProvenance{"Host.Roi", "release-transient-scopes", "1", {}}});
            // 只要保留项仍依赖 scoped ROI，替代目录仍需可退休，不能制造永久结构 blocker。
            for (const auto& input : transaction.outputs.back().inputs)
                if (GetInputTransient(graph.view->GetData(input.source))) {
                    transaction.outputs.back().lifetimeScope = data.CreateDataEntityId();
                    transaction.outputs.back().lifetimePolicy = DataLifetimePolicy::ExplicitRetirement;
                    break;
                }
        }
        transaction.bindings.push_back({binding.name, binding.revision, true, binding.target, replacement});
    }
    const auto committed = data.SetDataCommit(std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded) {
        std::cerr << "[Roi] Transient retirement pending: " << committed.message << '\n';
        for (const auto& blocker : committed.blockers) {
            const auto blocked = graph.view->GetData(blocker.revision);
            std::cerr << "[Roi] Retirement blocker: " << blocker.owner
                << " type=" << (blocked ? blocked->type.name : "unavailable")
                << " scoped=" << (blocked && GetDataEntityIdValid(blocked->lifetimeScope)) << '\n';
        }
        return false;
    }
    for (const auto& item : scopes) releasingScopes.push_back(item.first);
    releasingScopes.erase(std::remove_if(releasingScopes.begin(), releasingScopes.end(),
        [&](const auto& scope) { return data.SetDataRelease(scope).status == DataLifetimeStatus::Released; }),
        releasingScopes.end());
    return scopes.size() == scopeCount && releasingScopes.empty();
}

class VtkAppHostSession::Impl final {
public:
    using StopToken = std::uint64_t;

    struct PendingStopEntry;

    class InputEndpoint final : public HostInputEndpoint {
    public:
        explicit InputEndpoint(Impl& owner) noexcept
            : m_owner(owner)
        {
        }

        HostInputResult SendInput(
            const HostInputEvent& event) override
        {
            return m_owner.SendInput(event);
        }

    private:
        Impl& m_owner;
    };

    struct OwnerCompleteState final {
        std::mutex mutex;
        std::weak_ptr<HostWorkSignal> workSignal;
        std::vector<std::function<void()>> completes;
        bool isActive = true;
        bool isStopRetryQueued = false;
    };

    explicit Impl(HostSessionConfig sessionConfig)
        : config(std::move(sessionConfig))
        , inputEndpoint(*this)
        , ownerCompleteState(
            std::make_shared<OwnerCompleteState>())
    {
    }

    ~Impl();

    bool BuildSession();
    HostUpdateResult SendUpdates();
    HostRenderResult SendRender(const HostRenderRequest& request);
    bool DispatchRequest(
        HostRequest&& request,
        HostCompleteCallback onComplete);
    HostAdmission SendRequest(
        HostRequest&& request,
        HostResultCallback onComplete);
    HostInputResult SendInput(const HostInputEvent& event);
    std::optional<HostRenderViewState> GetRenderViewState(
        const HostViewTarget& target);
    std::vector<HostRenderViewState> GetRenderViewStates();
    std::optional<HostSceneViewState> GetSceneViewState(
        const HostViewTarget& target);
    std::vector<HostSceneViewState> GetSceneViewStates();
    std::optional<HostStateSnapshot> GetStateSnapshot() const;
    std::optional<ImageDescriptor> GetImageDescriptor();
    RoiResult SetRoi(const RoiRequest& request);
    RoiOperationResult SetRoiOperation(const RoiOperationRequest& request);
    RoiArchiveReadResult GetRoiArchiveReader(const std::vector<RoiArchiveSelection>&);
    RoiArchiveRestoreResult LoadRoiArchiveReader(const RoiArchiveReader&,const std::vector<RoiSourceMapping>&,const RoiArchiveRestoreOptions& options);
    std::vector<RoiDescriptor> GetRoiDescriptors(bool includeArchived);
    std::optional<RoiDescriptor> GetRoiDescriptor(const DataRevisionRef& ref);
    std::vector<LabelMapDescriptor> GetLabelMapDescriptors();
    std::optional<LabelMapDescriptor> GetLabelMapDescriptor(const std::string& id);
    LabelMapReadResult GetLabelMapReadResult(const LabelMapReadRequest& request);
    LabelMapReadChunkResult GetLabelMapReadChunk(const LabelMapReadRequest& request, std::size_t voxelOffset);
    std::optional<ImageReadState> GetImageReadState();
    ImageReadResult GetImageReadResult(std::size_t maxReadBytes);
    ImageReadResult GetImageReadResult(
        const ImageReadRequest& request);
    ImageReadChunkResult GetImageReadChunk(
        const ImageReadRequest& request,
        std::size_t voxelOffset);
    ImageReadAdmission StartImageRead(
        ImageReadRequest request,
        ImageReadCallback onComplete);
    bool AttachTimer(const HostTimerConfig& timerConfig);
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    bool DetachFeature(const HostFeature& feature);
    bool Stop() noexcept;
    static void SendStopRetry(std::weak_ptr<OwnerCompleteState> weak,
        const std::function<bool(std::function<void()>)>& dispatch, Impl* owner) noexcept;
    bool GetIsReady() const noexcept;
    bool GetIsStopped() const noexcept;
    HostStopState GetStopState() const noexcept;
    static bool SendOwnerStop(
        std::unique_ptr<Impl>& impl) noexcept;
    static bool SendPendingStops() noexcept;
    static std::size_t GetPendingStopCount() noexcept;

    HostSessionConfig config;
    HostCoreServices core;
    std::vector<DataEntityId> roiReleasingScopes;
    HostViewRuntimeRegistry renderViews;
    std::shared_ptr<HostFrameCoordinator> frameCoordinator;
    std::shared_ptr<HostCommandRouter> commandRouter;
    std::vector<HostRenderViewEndpoint> endpoints;
    std::unique_ptr<HostInputRegistry> inputRegistry;
    InputEndpoint inputEndpoint;
    HostFeatureRuntime featureRuntime;
    std::shared_ptr<HostImageReadRuntime> imageReadRuntime;
    // 保存实际已安装 handler 的目标；补偿失败时允许暂存双绑定并由后续请求重试收敛。
    std::vector<HostViewTarget> timerTargets;
    std::shared_ptr<OwnerCompleteState> ownerCompleteState;
    std::thread::id ownerThread;
    std::uint64_t nextSessionGeneration = 1;
    bool isBuilt = false;
    bool isStarted = false;
    bool isFrameExecuting = false;
    bool isRendering = false;
    std::shared_ptr<HostWorkSignal> workSignal;
    std::atomic<HostStopState> stopState{ HostStopState::Stopped };
    mutable std::recursive_mutex m_sessionMutex;

private:
    static bool SetPendingStop(
        std::unique_ptr<Impl>& impl,
        StopToken token) noexcept;
    static bool SendPendingStop(StopToken token) noexcept;
    static StopToken GetStopToken() noexcept;
    static HostCoreServices BuildCore();
    static bool SetOwnerComplete(
        const std::shared_ptr<OwnerCompleteState>& state,
        std::function<void()> complete, bool shouldNotify = true);
    void SendDiagnostic(const std::string& message) const noexcept;
    void SendImageReadComplete(bool isStopping) noexcept;
    void SendFeatureTicks() noexcept;
    void SendPendingFrameWork() noexcept;
    void SendOwnerCompletions(bool isStopping = false) noexcept;
    void OnViewTimer();
    void OnHostTimer();
    bool DetachTimer();
    bool DetachFeatures();

    static std::mutex s_stopMutex;
    static std::list<std::unique_ptr<PendingStopEntry>>
        s_pendingStops;
    static std::atomic<StopToken> s_nextStopToken;
};

struct VtkAppHostSession::Impl::PendingStopEntry final {
    StopToken token = 0;
    std::thread::id ownerThread;
    std::unique_ptr<Impl> impl;
};

std::mutex VtkAppHostSession::Impl::s_stopMutex;
std::list<std::unique_ptr<VtkAppHostSession::Impl::PendingStopEntry>>
VtkAppHostSession::Impl::s_pendingStops;
std::atomic<VtkAppHostSession::Impl::StopToken>
VtkAppHostSession::Impl::s_nextStopToken{ 1 };

HostCoreServices VtkAppHostSession::Impl::BuildCore()
{
    HostCoreServices value;
    value.sharedDataMgr =
        std::make_shared<RawVolumeDataManager>();
    value.sharedStateBroadcaster =
        std::make_shared<SharedStateBroadcaster>();
    value.sharedState =
        std::make_shared<SharedInteractionState>(
            value.sharedStateBroadcaster);
    return value;
}

bool VtkAppHostSession::Impl::SetOwnerComplete(
    const std::shared_ptr<OwnerCompleteState>& state,
    std::function<void()> complete, const bool shouldNotify)
{
    if (!state || !complete) return false;
    try {
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->isActive) return false;
            state->completes.push_back(std::move(complete));
        }
        if (shouldNotify) {
            if (const auto signal = state->workSignal.lock())
                (void)signal->SendWorkAvailable();
        }
    }
    catch (...) {
        return false;
    }
    return true;
}

VtkAppHostSession::Impl::~Impl()
{
    // 外壳或 StopPending reaper 只在 Stop 成功后删除 Impl。
}

void VtkAppHostSession::Impl::SendDiagnostic(
    const std::string& message) const noexcept
{
    try {
        if (config.sendDiagnostic) config.sendDiagnostic(message);
    }
    catch (...) {
    }
    try { std::cerr << message << '\n'; }
    catch (...) {
    }
}

bool VtkAppHostSession::Impl::BuildSession()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (isBuilt) {
        return stopState.load() == HostStopState::Running
            && ownerThread == std::this_thread::get_id();
    }
    if (config.renderViews.empty()
        || (config.driveMode != HostDriveMode::Native
            && config.driveMode != HostDriveMode::HostDriven)) {
        return false;
    }
    if (stopState.load() != HostStopState::Stopped) {
        return false;
    }
    if (ownerThread != std::thread::id{}
        && ownerThread != std::this_thread::get_id()) {
        return false;
    }
    ownerThread = std::this_thread::get_id();
    stopState = HostStopState::Building;
    bool isCompleteActive = false;
    if (ownerCompleteState) {
        const std::lock_guard<std::mutex> lock(
            ownerCompleteState->mutex);
        isCompleteActive = ownerCompleteState->isActive;
    }
    if (!isCompleteActive) {
        ownerCompleteState = std::make_shared<OwnerCompleteState>();
    }

    const auto clearBuild = [this]() noexcept {
        bool isStopped = false;
        try {
            if (frameCoordinator) frameCoordinator->Stop();
            (void)featureRuntime.StopOwner();
            endpoints.clear();
            const bool isInputStopped = !inputRegistry || inputRegistry->Stop();
            if (!isInputStopped) {
                stopState = HostStopState::StopPending;
                return false;
            }
            inputRegistry.reset();
            commandRouter.reset();
            isStopped = renderViews.StopLease();
            if (isStopped) frameCoordinator.reset();
        }
        catch (...) {
            isStopped = false;
        }
        if (!isStopped) {
            stopState = HostStopState::StopPending;
            return false;
        }
        imageReadRuntime.reset();
        core = {};
        isBuilt = false;
        isStarted = false;
        ownerThread = {};
        stopState = HostStopState::Stopped;
        return true;
    };
    try {
        workSignal = std::make_shared<HostWorkSignal>(
            config.driveMode == HostDriveMode::HostDriven
                ? config.onWorkAvailable : std::function<void()>{},
            [this, weak = std::weak_ptr<OwnerCompleteState>(ownerCompleteState), dispatch = config.sendOwnerTask] {
                // worker 只操作共享门闩；Session identity 仅由排队后的 owner 任务访问。
                SendStopRetry(weak, dispatch, this);
            });
        ownerCompleteState->workSignal = workSignal;
        core = BuildCore();
        core.isHostDriven = config.driveMode == HostDriveMode::HostDriven;
        if (core.isHostDriven) {
            const std::weak_ptr<HostWorkSignal> weakSignal = workSignal;
            core.onWorkAvailable = [weakSignal] {
                if (const auto signal = weakSignal.lock())
                    (void)signal->SendWorkAvailable();
            };
        }
        if (!renderViews.Build(core, config.renderViews)) {
            (void)clearBuild();
            return false;
        }
        const auto viewDirectory = renderViews.GetViewDirectory();
        commandRouter = std::make_shared<HostCommandRouter>(viewDirectory);
        if (!renderViews.SetInitialVisibility()) {
            (void)clearBuild();
            return false;
        }

        std::uint64_t sessionGeneration = nextSessionGeneration++;
        if (sessionGeneration == 0) {
            sessionGeneration = nextSessionGeneration++;
        }
        if (nextSessionGeneration == 0) nextSessionGeneration = 1;
        core.sharedState->SetTransformGeneration(sessionGeneration);
        HostFrameCoordinator::Callbacks frameCallbacks;
        frameCallbacks.collectUpdates = [this]() {
            return renderViews.CollectFrameUpdates();
        };
        frameCallbacks.sendFeatureTicks = [this]() {
            SendFeatureTicks();
        };
        frameCallbacks.setIntents = [this](
            const std::vector<HostFrameIntent>& intents) {
            return renderViews.SetFrameIntents(intents);
        };
        frameCallbacks.applyFeatureUpdates = [this]() {
            return core.sharedState->StartTransformFrame()
                && renderViews.ApplyFrameUpdates();
        };
        frameCallbacks.buildStage = [this](
            const std::uint64_t epoch) {
            const auto status = renderViews.BuildFrameStage(epoch);
            if (!core.sharedState->GetTransformFrameValid())
                return HostFrameStageStatus::Failed;
            if (status == HostFrameStageStatus::Unchanged
                && !core.sharedState->SetTransformFrameCommit())
                return HostFrameStageStatus::Failed;
            return status;
        };
        frameCallbacks.setCommit = [this](const std::uint64_t epoch) {
            if (!core.sharedState->SetTransformFrameCommit())
                throw std::runtime_error("Model transform input changed before frame commit");
            renderViews.SetFrameCommit(epoch);
        };
        frameCallbacks.sendRender = [this](const std::uint64_t epoch) {
            return renderViews.SendFrameRender(epoch);
        };
        frameCallbacks.getRenderPending = [this]() {
            return renderViews.GetFrameRenderPending();
        };
        frameCallbacks.sendCompletions = []() {};
        frameCallbacks.sendReadyCompletions = [this]() {
            renderViews.SendFrameCompletions();
            SendOwnerCompletions();
            SendImageReadComplete(false);
        };
        frameCallbacks.clearStage = [this]() {
            renderViews.ClearFrameStage();
            if (core.sharedState->ClearTransformFrame()) {
                // 切回已发布姿态后恢复所有投影；失败仍保留 dirty，帧屏障禁止提前 Render。
                (void)renderViews.ApplyFrameUpdates();
            }
        };
        frameCoordinator = std::make_shared<HostFrameCoordinator>(
            sessionGeneration, std::move(frameCallbacks));
        if (!frameCoordinator
            || !renderViews.SetFrameGeneration(sessionGeneration)
            || (!core.isHostDriven && !renderViews.SetFrameHandlers(
                [this]() { OnViewTimer(); }))
            || !renderViews.SetInputsEnabled(false)
            || !renderViews.SetInteractorsReady()) {
            (void)clearBuild();
            return false;
        }
        endpoints = renderViews.BuildEndpoints();
        if (endpoints.size() != config.renderViews.size()) {
            (void)clearBuild();
            return false;
        }
        inputRegistry = std::make_unique<HostInputRegistry>(
            viewDirectory);
        HostViewTargets allViews;
        allViews.viewIds.reserve(config.renderViews.size());
        for (const auto& view : config.renderViews) {
            allViews.viewIds.push_back(view.id);
        }
        if (!inputRegistry
            || !inputRegistry->Start(std::move(allViews))) {
            (void)clearBuild();
            return false;
        }
        HostFeatureRuntime::Ports featurePorts;
        featurePorts.views = &renderViews;
        featurePorts.input = &inputRegistry->GetFeaturePort();
        featurePorts.data = core.sharedDataMgr;
        featurePorts.state = core.sharedState;
        featurePorts.frames = frameCoordinator;
        featurePorts.ownerThread = ownerThread;
        featurePorts.workSignal = workSignal;
        const std::weak_ptr<OwnerCompleteState> weakComplete = ownerCompleteState;
        featurePorts.onOwnerComplete = [weakComplete](std::function<void()> complete) {
            return SetOwnerComplete(weakComplete.lock(), std::move(complete), false);
        };
        imageReadRuntime = std::make_shared<HostImageReadRuntime>();
        if (!featureRuntime.StartOwner(std::move(featurePorts))) {
            (void)clearBuild();
            return false;
        }
        isBuilt = true;
        stopState = HostStopState::Running;
        if (core.isHostDriven) {
            if (!renderViews.SetInputsEnabled(true)) {
                (void)clearBuild();
                return false;
            }
            (void)workSignal->SendWorkAvailable();
        }
        if (!config.sendOwnerTask) {
            SendDiagnostic(
                "[Host] Session has no owner dispatcher; call Stop() on the owner thread before destruction.");
        }
        return true;
    }
    catch (...) {
        (void)clearBuild();
        return false;
    }
}

bool VtkAppHostSession::Impl::DispatchRequest(
    HostRequest&& request,
    HostCompleteCallback onComplete)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!BuildSession() || isRendering
        || ownerThread != std::this_thread::get_id()
        || !commandRouter) {
        return false;
    }

    HostCompleteCallback renderedComplete;
    if (onComplete) {
        auto callback = std::make_shared<HostCompleteCallback>(
            std::move(onComplete));
        auto isQueued = std::make_shared<std::atomic<bool>>(false);
        const std::weak_ptr<OwnerCompleteState> weakState =
            ownerCompleteState;
        renderedComplete = [callback, isQueued, weakState](
            const bool isSucceeded) noexcept {
            if (isQueued->exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            const auto send = [callback, isSucceeded]() noexcept {
                try { (*callback)(isSucceeded); }
                catch (...) {}
            };
            if (!Impl::SetOwnerComplete(weakState.lock(), send)) {
                // 已接纳 callback 不能丢失；排队失败以失败终态同步收口。
                try { (*callback)(false); }
                catch (...) {}
            }
        };
    }
    const bool isSent = commandRouter->Dispatch(
        std::move(request),
        std::move(renderedComplete),
        [frames = frameCoordinator](HostCommandRouter::DisplayCheck getApplied,
            HostCompleteCallback complete) {
            return frames && frames->SendDisplayComplete(
                std::move(getApplied), std::move(complete));
        });
    if (isSent && workSignal) (void)workSignal->SendWorkAvailable();
    return isSent;
}

HostAdmission VtkAppHostSession::Impl::SendRequest(
    HostRequest&& request,
    HostResultCallback onComplete)
{
    std::unique_lock<std::recursive_mutex> lock(m_sessionMutex);
    if (ownerThread != std::thread::id{}
        && ownerThread != std::this_thread::get_id()) {
        return { HostAdmissionStatus::Rejected, HostErrorCode::WrongThread,
            "Request must run on the session owner thread." };
    }
    if (!BuildSession() || !commandRouter) {
        return { HostAdmissionStatus::Rejected, HostErrorCode::SessionNotReady,
            "Host session is not ready." };
    }
    // Dispatch 返回前可能产生同步完成。先保存结果，接纳确定后才兑现，
    // 避免拒绝路径误取得回调，也避免持有 Session 锁调用上位机代码。
    struct Completion final {
        enum class Phase { Pending, Accepted, Rejected, Completed };
        std::mutex mutex;
        HostResultCallback callback;
        Phase phase = Phase::Pending;
        std::optional<bool> pending;
    };
    auto completion = std::make_shared<Completion>();
    completion->callback = std::move(onComplete);
    const HostCompleteCallback sendComplete = [completion](bool succeeded) {
        HostResultCallback callback;
        {
            const std::lock_guard<std::mutex> guard(completion->mutex);
            if (completion->phase == Completion::Phase::Rejected
                || completion->phase == Completion::Phase::Completed) return;
            if (completion->phase == Completion::Phase::Pending) {
                if (!completion->pending) completion->pending = succeeded;
                return;
            }
            completion->phase = Completion::Phase::Completed;
            callback = std::move(completion->callback);
        }
        if (callback) {
            try { callback({ succeeded, succeeded ? HostErrorCode::None
                : HostErrorCode::OperationFailed, succeeded ? std::string{}
                : "The accepted host operation failed." }); }
            catch (...) {}
        }
    };
    bool accepted = false;
    try {
        accepted = DispatchRequest(std::move(request),
            completion->callback ? sendComplete : HostCompleteCallback{});
    }
    catch (...) {
        const std::lock_guard<std::mutex> guard(completion->mutex);
        completion->phase = Completion::Phase::Rejected;
        completion->callback = {};
        return { HostAdmissionStatus::Rejected, HostErrorCode::OperationFailed,
            "The host request raised an exception." };
    }
    std::optional<bool> pending;
    {
        const std::lock_guard<std::mutex> guard(completion->mutex);
        completion->phase = accepted ? Completion::Phase::Accepted : Completion::Phase::Rejected;
        pending = completion->pending;
        if (!accepted) completion->callback = {};
    }
    lock.unlock();
    if (!accepted) return { HostAdmissionStatus::Rejected, HostErrorCode::RequestRejected,
        "The host request was rejected before execution." };
    if (pending) sendComplete(*pending);
    return { HostAdmissionStatus::Accepted, HostErrorCode::None, {} };
}

HostInputResult VtkAppHostSession::Impl::SendInput(
    const HostInputEvent& event)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (ownerThread != std::thread::id{}
        && ownerThread != std::this_thread::get_id()) {
        return {
            false,
            false,
            false,
            HostErrorCode::WrongThread,
            "Host input must run on the session owner thread." };
    }
    // endpoint 从不惰性构建 Session，避免未绑定时由任意调用线程篡取 owner。
    if (!isBuilt || isRendering || stopState.load() != HostStopState::Running
        || !inputRegistry) {
        return {
            false,
            false,
            false,
            HostErrorCode::SessionNotReady,
            "Host session is not ready for input." };
    }
    return inputRegistry->SendInput(event);
}

std::optional<ImageDescriptor> VtkAppHostSession::Impl::GetImageDescriptor()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    return core.sharedDataMgr->GetImageDescriptor();
}


RoiResult VtkAppHostSession::Impl::SetRoi(const RoiRequest& request)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (ownerThread != std::thread::id{} && ownerThread != std::this_thread::get_id()) return {RoiError::WrongThread};
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    const auto data = core.sharedDataMgr;
    return data->SetRoi(request);
}

RoiOperationResult VtkAppHostSession::Impl::SetRoiOperation(const RoiOperationRequest& request)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (ownerThread!=std::thread::id{} && ownerThread!=std::this_thread::get_id()) return {RoiError::WrongThread};
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    const auto data=core.sharedDataMgr;
    return data->SetRoiOperation(request);
}

std::vector<RoiDescriptor> VtkAppHostSession::Impl::GetRoiDescriptors(bool includeArchived)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    return RoiService::GetDescriptors(core.sharedDataMgr->GetDataGraph(), includeArchived);
}

std::optional<RoiDescriptor> VtkAppHostSession::Impl::GetRoiDescriptor(const DataRevisionRef& ref)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    return RoiService::GetDescriptor(core.sharedDataMgr->GetDataGraph(), ref);
}

std::vector<LabelMapDescriptor> VtkAppHostSession::Impl::GetLabelMapDescriptors()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    return LabelMapReader(core.sharedDataMgr->GetDataGraph()).GetDescriptors();
}

std::optional<LabelMapDescriptor> VtkAppHostSession::Impl::GetLabelMapDescriptor(const std::string& id)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return {};
    return LabelMapReader(core.sharedDataMgr->GetDataGraph()).GetDescriptor(id);
}

LabelMapReadResult VtkAppHostSession::Impl::GetLabelMapReadResult(const LabelMapReadRequest& request)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return { LabelMapError::Unavailable, 0, {} };
    return LabelMapReader(core.sharedDataMgr->GetDataGraph()).GetReadResult(request);
}

LabelMapReadChunkResult VtkAppHostSession::Impl::GetLabelMapReadChunk(const LabelMapReadRequest& request, std::size_t voxelOffset)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) return { LabelMapError::Unavailable, 0, 0, false, {} };
    return LabelMapReader(core.sharedDataMgr->GetDataGraph()).GetReadChunk(request, voxelOffset);
}

std::optional<ImageReadState>
VtkAppHostSession::Impl::GetImageReadState()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) {
        return std::nullopt;
    }
    return core.sharedDataMgr->GetImageReadState();
}

ImageReadResult VtkAppHostSession::Impl::GetImageReadResult(
    const std::size_t maxReadBytes)
{
    ImageReadRequest request;
    request.maxBytes = maxReadBytes;
    return GetImageReadResult(request);
}

ImageReadResult VtkAppHostSession::Impl::GetImageReadResult(
    const ImageReadRequest& request)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) {
        return {};
    }
    return core.sharedDataMgr->GetImageReadResult(
        request, TaskStopToken{});
}

ImageReadChunkResult VtkAppHostSession::Impl::GetImageReadChunk(
    const ImageReadRequest& request,
    const std::size_t voxelOffset)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady() || !core.sharedDataMgr) {
        return {};
    }
    return core.sharedDataMgr->GetImageReadChunk(
        request, voxelOffset, TaskStopToken{});
}

ImageReadAdmission VtkAppHostSession::Impl::StartImageRead(
    ImageReadRequest request, ImageReadCallback onComplete)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!onComplete) return ImageReadAdmission::InvalidRequest;
    if (!GetIsReady() || !imageReadRuntime) return ImageReadAdmission::Unavailable;
    return imageReadRuntime->StartImageRead(core.sharedDataMgr,
        renderViews.GetTaskExecutor(), std::move(request), std::move(onComplete));
}

std::optional<HostRenderViewState>
VtkAppHostSession::Impl::GetRenderViewState(
    const HostViewTarget& target)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady()) {
        return std::nullopt;
    }
    return renderViews.GetViewState(target);
}

std::vector<HostRenderViewState>
VtkAppHostSession::Impl::GetRenderViewStates()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady()) {
        return {};
    }
    return renderViews.GetViewStates();
}

std::optional<HostSceneViewState>
VtkAppHostSession::Impl::GetSceneViewState(
    const HostViewTarget& target)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady()) {
        return std::nullopt;
    }
    return renderViews.GetSceneViewState(target);
}

std::vector<HostSceneViewState>
VtkAppHostSession::Impl::GetSceneViewStates()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!GetIsReady()) {
        return {};
    }
    return renderViews.GetSceneViewStates();
}

std::optional<HostStateSnapshot> VtkAppHostSession::Impl::GetStateSnapshot() const
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!isBuilt || ownerThread != std::this_thread::get_id()
        || stopState.load() != HostStopState::Running || !frameCoordinator
        || !core.sharedDataMgr) return std::nullopt;
    try {
        HostStateSnapshot snapshot;
        snapshot.load = core.sharedDataMgr->GetLoadState();
        snapshot.sessionGeneration = frameCoordinator->GetSessionGeneration();
        const auto operations = featureRuntime.GetOperationStates();
        if (!operations || !isBuilt || stopState.load() != HostStopState::Running
            || !frameCoordinator || snapshot.sessionGeneration != frameCoordinator->GetSessionGeneration())
            return std::nullopt;
        snapshot.operations = *operations;
        snapshot.scenes = renderViews.GetSceneViewStates();
        const auto graph = core.sharedDataMgr->GetDataGraph();
        snapshot.graphCommitId = graph.commitId;
        for (const auto& operation : snapshot.operations) {
            for (const auto& output : operation.outputs) {
                const auto data = graph.view ? graph.view->GetData(output) : nullptr;
                if (!data || data->self != output) return std::nullopt;
            }
        }
        return snapshot;
    }
    catch (...) {
        return std::nullopt;
    }
}

bool VtkAppHostSession::Impl::DetachTimer()
{
    bool isCleared = true;
    auto target = timerTargets.begin();
    while (target != timerTargets.end()) {
        if (renderViews.SetTimerHandler(
                *target, [this]() { OnViewTimer(); })) {
            target = timerTargets.erase(target);
        }
        else {
            isCleared = false;
            ++target;
        }
    }
    return isCleared && timerTargets.empty();
}

bool VtkAppHostSession::Impl::AttachTimer(
    const HostTimerConfig& timerConfig)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!isBuilt
        || ownerThread != std::this_thread::get_id()) {
        return false;
    }
    if (!timerConfig.isTimerEnabled) {
        return DetachTimer();
    }
    if (config.driveMode == HostDriveMode::HostDriven) return false;
    const auto getTargetSame = [](
        const HostViewTarget& first,
        const HostViewTarget& second) {
        return first.viewId == second.viewId
            && first.isViewRoleUsed == second.isViewRoleUsed
            && (!first.isViewRoleUsed
                || first.viewRole == second.viewRole);
    };

    // 上次补偿若留下双绑定，先按真实记录逐一清理；全部成功后本次请求可继续安装。
    if (timerTargets.size() > 1 && !DetachTimer()) {
        return false;
    }
    if (timerTargets.size() == 1
        && getTargetSame(timerTargets.front(), timerConfig.targetView)) {
        return renderViews.SetTimerHandler(
            timerTargets.front(), [this]() { OnHostTimer(); });
    }

    HostViewTarget nextTarget;
    HostViewTarget oldTarget;
    const bool hadOldTarget = !timerTargets.empty();
    try {
        nextTarget = timerConfig.targetView;
        if (hadOldTarget) oldTarget = timerTargets.front();
        timerTargets.reserve(2);
    }
    catch (...) {
        return false;
    }
    if (!renderViews.SetTimerHandler(
            nextTarget, [this]() { OnHostTimer(); })) {
        return false;
    }
    timerTargets.push_back(std::move(nextTarget));
    if (!hadOldTarget) return true;

    if (!renderViews.SetTimerHandler(
            oldTarget, [this]() { OnViewTimer(); })) {
        // 只有新 handler 的补偿清理确实成功，才能从实际绑定记录中移除它。
        if (renderViews.SetTimerHandler(
                timerTargets.back(), [this]() { OnViewTimer(); })) {
            timerTargets.pop_back();
        }
        else {
            // 新旧 handler 都仍存在；Session 保留两个真实目标并进入统一
            // StopPending，避免继续对外表现为健康 Running。
            stopState = HostStopState::StopPending;
        }
        return false;
    }
    timerTargets.erase(timerTargets.begin());
    return true;
}

void VtkAppHostSession::Impl::SendFeatureTicks() noexcept
{
    featureRuntime.SendFeatureTicks();
}

void VtkAppHostSession::Impl::SendOwnerCompletions(const bool isStopping) noexcept
{
    std::vector<std::function<void()>> completes;
    if (ownerCompleteState) {
        const std::lock_guard<std::mutex> lock(
            ownerCompleteState->mutex);
        if (ownerCompleteState->isActive || isStopping) {
            completes.swap(ownerCompleteState->completes);
        }
    }
    for (auto& complete : completes) {
        try {
            complete();
        }
        catch (...) {
        }
    }
}

void VtkAppHostSession::Impl::OnViewTimer()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (config.driveMode == HostDriveMode::HostDriven
        || ownerThread != std::this_thread::get_id()
        || !frameCoordinator || !timerTargets.empty()) {
        return;
    }
    const auto frames = frameCoordinator;
    (void)frames->FlushOnOwnerTick(true);
}

void VtkAppHostSession::Impl::OnHostTimer()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (config.driveMode == HostDriveMode::HostDriven
        || ownerThread != std::this_thread::get_id()
        || !frameCoordinator) {
        return;
    }
    const auto frames = frameCoordinator;
    (void)frames->FlushOnOwnerTick(true);
}

void VtkAppHostSession::Impl::SendImageReadComplete(const bool isStopping) noexcept
{
    // 回调可重入 Stop/rebuild；局部共享拥有保持当前分发器存活。
    const auto runtime = imageReadRuntime;
    if (runtime) runtime->SendComplete(isStopping);
}

bool VtkAppHostSession::Impl::AttachFeature(const std::shared_ptr<HostFeature>& feature)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    return isBuilt && !isFrameExecuting && ownerThread == std::this_thread::get_id()
        && featureRuntime.AttachFeature(feature);
}

bool VtkAppHostSession::Impl::DetachFeature(const HostFeature& feature)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!isBuilt || isFrameExecuting || ownerThread != std::this_thread::get_id()) return false;
    const auto result = featureRuntime.DetachFeature(feature);
    if (result == HostFeatureRuntime::DetachResult::StopPending)
        stopState = HostStopState::StopPending;
    return result == HostFeatureRuntime::DetachResult::Detached;
}

bool VtkAppHostSession::Impl::DetachFeatures()
{
    return featureRuntime.DetachFeatures();
}

void VtkAppHostSession::Impl::SendStopRetry(std::weak_ptr<OwnerCompleteState> weak,
    const std::function<bool(std::function<void()>)>& dispatch, Impl* owner) noexcept
{
    const auto state = weak.lock();
    if (!state || !dispatch) return;
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->isActive || state->isStopRetryQueued) return;
        state->isStopRetryQueued = true;
    }
    try {
        if (dispatch([owner, weak] {
                const auto state = weak.lock();
                if (!state) return;
                {
                    const std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->isActive) return;
                    state->isStopRetryQueued = false;
                }
                const auto current = owner->stopState.load();
                if (current == HostStopState::Stopping || current == HostStopState::StopPending)
                    (void)owner->Stop();
            })) return;
    } catch (...) {}
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->isStopRetryQueued = false;
}

bool VtkAppHostSession::Impl::Stop() noexcept
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (featureRuntime.GetIsChanging()) return false;
    if (stopState.load() == HostStopState::Stopping) return false;
    if (ownerThread != std::thread::id{}
        && ownerThread != std::this_thread::get_id()) {
        stopState = HostStopState::StopRequested;
        return false;
    }

    if (workSignal) workSignal->Stop();
    if (isFrameExecuting) {
        stopState = HostStopState::StopRequested;
        if (frameCoordinator) frameCoordinator->Stop();
        return false;
    }
    stopState = HostStopState::Stopping;
    try {
        (void)RenderFrameLifetime::PollAll();
        // P0 首先关闭普通 frame admission 与输入 gate；清理失败时保持
        // StopPending，但不再恢复可交互状态。
        if (frameCoordinator) frameCoordinator->Stop();
        if (isBuilt && !renderViews.SetInputsEnabled(false)) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if(inputRegistry)if(const auto c=inputRegistry->GetController())InteractionControllerAccess::Stop(*c);
        if (!renderViews.SetDataTasksStopping()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        // 全部 Feature 的停止需要反复解除生产者/消费者依赖。ROI 正式状态属于
        // Host 数据服务，编辑器卸载只释放交互；临时 ROI 由组合根在此退休。
        if (core.sharedDataMgr)
            (void)RoiService::ReleaseTransientScopes(*core.sharedDataMgr, roiReleasingScopes);
        const bool areFeaturesDetached = DetachFeatures();
        const bool areRoisReleased = !core.sharedDataMgr
            || RoiService::ReleaseTransientScopes(*core.sharedDataMgr, roiReleasingScopes);
        if (!areFeaturesDetached || !areRoisReleased) {
            SendDiagnostic("[Host] Stop pending at data tasks / feature detach.");
            stopState = HostStopState::StopPending;
            return false;
        }
        if (inputRegistry && !inputRegistry->Stop()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if (!renderViews.StopLease()) {
            SendDiagnostic("[Host] Stop pending at render view lease.");
            stopState = HostStopState::StopPending;
            return false;
        }
        // StopLease 成功后 timer 与 executor 都已停止；必须在任何后续可失败清理前，
        // 由当前 owner thread 兑现已接纳图像读取的唯一终态回调。
        // 先关闭 completion admission，再排空已接纳队列；回调重入和并发晚到
        // 不得返回成功后被下面的清理丢弃。
        if (ownerCompleteState) {
            const std::lock_guard<std::mutex> completeLock(ownerCompleteState->mutex);
            ownerCompleteState->isActive = false;
        }
        SendImageReadComplete(true);
        SendOwnerCompletions(true);
        endpoints.clear();
        timerTargets.clear();
        (void)featureRuntime.StopOwner();

        if (ownerCompleteState) {
            const std::lock_guard<std::mutex> lock(
                ownerCompleteState->mutex);
            ownerCompleteState->isActive = false;
            ownerCompleteState->completes.clear();
        }
        inputRegistry.reset();
        commandRouter.reset();
        frameCoordinator.reset();
        imageReadRuntime.reset();
        core = {};
        isBuilt = false;
        isStarted = false;
        ownerThread = {};
        stopState = HostStopState::Stopped;
        return true;
    }
    catch (...) {
        stopState = HostStopState::StopPending;
        return false;
    }
}

bool VtkAppHostSession::Impl::GetIsStopped() const noexcept
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    return !isBuilt
        && stopState.load() == HostStopState::Stopped
        && ownerThread == std::thread::id{}
        && endpoints.empty()
        && !frameCoordinator
        && !inputRegistry
        && featureRuntime.GetIsEmpty();
}

bool VtkAppHostSession::Impl::GetIsReady() const noexcept
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    return isBuilt
        && stopState.load() == HostStopState::Running
        && ownerThread == std::this_thread::get_id();
}

HostStopState VtkAppHostSession::Impl::GetStopState() const noexcept
{
    return stopState.load();
}

VtkAppHostSession::Impl::StopToken
VtkAppHostSession::Impl::GetStopToken() noexcept
{
    StopToken token = 0;
    while (token == 0) {
        token = s_nextStopToken.fetch_add(
            1, std::memory_order_relaxed);
    }
    return token;
}

bool VtkAppHostSession::Impl::SetPendingStop(
    std::unique_ptr<Impl>& impl,
    const StopToken token) noexcept
{
    if (!impl || token == 0) return false;

    // 诊断必须发生在调用方仍持有强 owner 时，不能在转移后继续使用 Impl 裸地址。
    impl->stopState = HostStopState::StopPending;
    impl->SendDiagnostic("[Host] Session entered StopPending.");

    std::unique_ptr<PendingStopEntry> entry;
    try {
        entry = std::make_unique<PendingStopEntry>();
        entry->token = token;
        entry->ownerThread = impl->ownerThread;
        entry->impl = std::move(impl);

        const std::lock_guard<std::mutex> lock(s_stopMutex);
        const auto duplicate = std::find_if(
            s_pendingStops.begin(),
            s_pendingStops.end(),
            [token](const auto& current) {
                return current && current->token == token;
            });
        if (duplicate != s_pendingStops.end()) {
            impl = std::move(entry->impl);
            return false;
        }
        s_pendingStops.push_back(std::move(entry));
        return true;
    }
    catch (...) {
        if (entry && !impl) {
            impl = std::move(entry->impl);
        }
        return false;
    }
}

bool VtkAppHostSession::Impl::SendPendingStop(
    const StopToken token) noexcept
{
    if (token == 0) return false;

    std::unique_ptr<Impl> pending;
    {
        const std::lock_guard<std::mutex> lock(s_stopMutex);
        const auto entry = std::find_if(
            s_pendingStops.begin(),
            s_pendingStops.end(),
            [token](const auto& current) {
                return current && current->token == token;
            });
        if (entry == s_pendingStops.end()) {
            // 延迟或重复 dispatcher callback 天然幂等。
            return true;
        }
        if (!(*entry)->impl) {
            // 另一个 owner-thread pump 已经接管该 token。
            return true;
        }
        const auto currentThread = std::this_thread::get_id();
        if ((*entry)->ownerThread != std::thread::id{}
            && (*entry)->ownerThread != currentThread) {
            return false;
        }
        pending = std::move((*entry)->impl);
    }

    const bool isStopped = pending->Stop();
    pending->SendDiagnostic(isStopped
        ? "[Host] StopPending session released on the owner thread."
        : "[Host] StopPending retry failed; the owner reaper retained the session.");

    const std::lock_guard<std::mutex> lock(s_stopMutex);
    const auto entry = std::find_if(
        s_pendingStops.begin(),
        s_pendingStops.end(),
        [token](const auto& current) {
            return current && current->token == token;
        });
    if (entry == s_pendingStops.end()) {
        // 只有本函数能移除正在处理的空 entry；若不变量被破坏，立即失败而不触碰其他 token。
        return false;
    }
    if (isStopped) {
        s_pendingStops.erase(entry);
        return true;
    }
    (*entry)->impl = std::move(pending);
    return false;
}

bool VtkAppHostSession::Impl::SendPendingStops() noexcept
{
    std::vector<StopToken> pendingTokens;
    try {
        const auto currentThread = std::this_thread::get_id();
        const std::lock_guard<std::mutex> lock(s_stopMutex);
        pendingTokens.reserve(s_pendingStops.size());
        for (const auto& entry : s_pendingStops) {
            if (entry && entry->impl
                && (entry->ownerThread == std::thread::id{}
                    || entry->ownerThread == currentThread)) {
                pendingTokens.push_back(entry->token);
            }
        }
    }
    catch (...) {
        return false;
    }

    bool isComplete = true;
    for (const StopToken token : pendingTokens) {
        isComplete = SendPendingStop(token) && isComplete;
    }
    return isComplete;
}

std::size_t VtkAppHostSession::Impl::GetPendingStopCount() noexcept
{
    const std::lock_guard<std::mutex> lock(s_stopMutex);
    return s_pendingStops.size();
}

bool VtkAppHostSession::Impl::SendOwnerStop(
    std::unique_ptr<Impl>& impl) noexcept
{
    if (!impl) return true;
    if (impl->Stop()) {
        impl.reset();
        return true;
    }

    std::function<bool(std::function<void()>)> sendOwnerTask;
    try { sendOwnerTask = impl->config.sendOwnerTask; }
    catch (...) {
    }
    if (!sendOwnerTask) {
        impl->SendDiagnostic(
            "[Host] Owner Stop dispatcher is missing; use SendPendingStops() on the owner thread.");
        const StopToken token = GetStopToken();
        (void)SetPendingStop(impl, token);
        return false;
    }

    const StopToken token = GetStopToken();
    if (!SetPendingStop(impl, token)) return false;

    try {
        const bool isSent = sendOwnerTask([token]() {
            (void)SendPendingStop(token);
        });
        if (!isSent) {
            std::cerr
                << "[Host] Owner-thread Stop was rejected; "
                << "the owner reaper retained the session.\n";
        }
        return isSent;
    }
    catch (...) {
        std::cerr
            << "[Host] Owner-thread Stop dispatch failed; "
            << "the owner reaper retained the session.\n";
        return false;
    }
}

VtkAppHostSession::VtkAppHostSession(HostSessionConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

VtkAppHostSession::~VtkAppHostSession()
{
    (void)Impl::SendOwnerStop(m_impl);
}
VtkAppHostSession::VtkAppHostSession(
    VtkAppHostSession&&) noexcept = default;
VtkAppHostSession& VtkAppHostSession::operator=(
    VtkAppHostSession&& other) noexcept
{
    if (this == &other) return *this;
    (void)Impl::SendOwnerStop(m_impl);
    m_impl = std::move(other.m_impl);
    return *this;
}

bool VtkAppHostSession::BuildSession()
{
    return m_impl && m_impl->BuildSession();
}

std::shared_ptr<InteractionController> VtkAppHostSession::GetInteractionController() {
    if(!m_impl||!BuildSession()||m_impl->ownerThread!=std::this_thread::get_id())return {};
    return m_impl->inputRegistry?m_impl->inputRegistry->GetController():nullptr;
}

bool VtkAppHostSession::AttachTimer(
    const HostTimerConfig& config)
{
    return BuildSession() && m_impl->AttachTimer(config);
}

bool VtkAppHostSession::AttachFeature(
    const std::shared_ptr<HostFeature>& feature)
{
    return m_impl && m_impl->AttachFeature(feature);
}

bool VtkAppHostSession::DetachFeature(
    const HostFeature& feature)
{
    return m_impl && m_impl->DetachFeature(feature);
}

bool VtkAppHostSession::Start()
{
    if (!m_impl) return false;
    const std::lock_guard<std::recursive_mutex> lock(
        m_impl->m_sessionMutex);
    if (!BuildSession() || m_impl->isStarted
        || m_impl->config.driveMode == HostDriveMode::HostDriven) {
        return false;
    }
    if (!m_impl->frameCoordinator) return false;
    const auto frames = m_impl->frameCoordinator;
    const auto frameStatus = frames->FlushOnOwnerTick(true);
    if (frameStatus
            != HostFrameCoordinator::FlushStatus::Completed
        || !m_impl->GetIsReady() || m_impl->frameCoordinator != frames
        || !m_impl->renderViews.SetInputsEnabled(true)) {
        (void)m_impl->renderViews.SetInputsEnabled(false);
        return false;
    }
    m_impl->isStarted = true;
    if (!m_impl->renderViews.StartStandaloneView()) {
        (void)m_impl->renderViews.SetInputsEnabled(false);
        m_impl->isStarted = false;
        return false;
    }
    return true;
}

HostAdmission VtkAppHostSession::SendRequest(
    HostRequest&& request,
    HostResultCallback onComplete)
{
    return m_impl ? m_impl->SendRequest(std::move(request), std::move(onComplete))
        : HostAdmission{HostAdmissionStatus::Rejected, HostErrorCode::SessionNotReady,
            "Host session is not available."};
}

const std::vector<HostRenderViewEndpoint>&
VtkAppHostSession::GetRenderViewEndpoints()
{
    static const std::vector<HostRenderViewEndpoint> empty;
    if (!m_impl) return empty;
    const std::lock_guard<std::recursive_mutex> lock(
        m_impl->m_sessionMutex);
    if (!m_impl->GetIsReady()) {
        return empty;
    }
    return m_impl->endpoints;
}

const HostRenderViewEndpoint*
VtkAppHostSession::GetRenderViewEndpoint(
    const std::string& viewId)
{
    if (!m_impl) return nullptr;
    const std::lock_guard<std::recursive_mutex> lock(
        m_impl->m_sessionMutex);
    if (!m_impl->GetIsReady()) {
        return nullptr;
    }
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.id == viewId) {
            return &endpoint;
        }
    }
    return nullptr;
}

const HostRenderViewEndpoint*
VtkAppHostSession::GetPrimaryEndpoint()
{
    if (!m_impl) return nullptr;
    const std::lock_guard<std::recursive_mutex> lock(
        m_impl->m_sessionMutex);
    if (!m_impl->GetIsReady()) {
        return nullptr;
    }
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.role == HostRenderViewRole::Primary3D) {
            return &endpoint;
        }
    }
    for (const auto& endpoint : m_impl->endpoints) {
        if (endpoint.role
            == HostRenderViewRole::Composite3D) {
            return &endpoint;
        }
    }
    return m_impl->endpoints.empty()
        ? nullptr : &m_impl->endpoints.front();
}

HostInputEndpoint* VtkAppHostSession::GetInputEndpoint() noexcept
{
    return m_impl ? &m_impl->inputEndpoint : nullptr;
}

bool VtkAppHostSession::Stop() noexcept
{
    return !m_impl || m_impl->Stop();
}

bool VtkAppHostSession::GetIsStopped() const noexcept
{
    return !m_impl || m_impl->GetIsStopped();
}

HostStopState VtkAppHostSession::GetStopState() const noexcept
{
    return m_impl
        ? m_impl->GetStopState()
        : HostStopState::Stopped;
}

bool VtkAppHostSession::SendPendingStops() noexcept
{
    return Impl::SendPendingStops();
}

std::size_t VtkAppHostSession::GetPendingStopCount() noexcept
{
    return Impl::GetPendingStopCount();
}

std::optional<HostRenderViewState>
VtkAppHostSession::GetRenderViewState(
    const HostViewTarget& target)
{
    return m_impl
        ? m_impl->GetRenderViewState(target)
        : std::nullopt;
}

std::vector<HostRenderViewState>
VtkAppHostSession::GetRenderViewStates()
{
    return m_impl ? m_impl->GetRenderViewStates()
        : std::vector<HostRenderViewState>{};
}

std::optional<HostSceneViewState>
VtkAppHostSession::GetSceneViewState(
    const HostViewTarget& target)
{
    return m_impl
        ? m_impl->GetSceneViewState(target)
        : std::nullopt;
}

std::vector<HostSceneViewState>
VtkAppHostSession::GetSceneViewStates()
{
    return m_impl
        ? m_impl->GetSceneViewStates()
        : std::vector<HostSceneViewState>{};
}

std::optional<HostStateSnapshot> VtkAppHostSession::GetStateSnapshot() const
{
    return m_impl ? m_impl->GetStateSnapshot() : std::nullopt;
}

std::optional<ImageReadState>
VtkAppHostSession::GetImageReadState()
{
    return m_impl
        ? m_impl->GetImageReadState()
        : std::optional<ImageReadState>{};
}

ImageReadResult VtkAppHostSession::GetImageReadResult(
    const std::size_t maxReadBytes)
{
    return m_impl
        ? m_impl->GetImageReadResult(maxReadBytes)
        : ImageReadResult{};
}

ImageReadResult VtkAppHostSession::GetImageReadResult(
    const ImageReadRequest& request)
{
    return m_impl
        ? m_impl->GetImageReadResult(request)
        : ImageReadResult{};
}

ImageReadChunkResult VtkAppHostSession::GetImageReadChunk(
    const ImageReadRequest& request,
    const std::size_t voxelOffset)
{
    return m_impl
        ? m_impl->GetImageReadChunk(request, voxelOffset)
        : ImageReadChunkResult{};
}

ImageReadAdmission VtkAppHostSession::StartImageRead(
    ImageReadRequest request,
    ImageReadCallback onComplete)
{
    return m_impl
        ? m_impl->StartImageRead(
            std::move(request), std::move(onComplete))
        : ImageReadAdmission::Unavailable;
}

std::optional<ImageDescriptor> VtkAppHostSession::GetImageDescriptor()
{
    return m_impl ? m_impl->GetImageDescriptor() : std::optional<ImageDescriptor>{};
}

std::optional<HostDataCatalog> VtkAppHostSession::GetDataCatalog(const DataQuery& query) const
{
    if (!m_impl) return {};
    const std::lock_guard<std::recursive_mutex> lock(m_impl->m_sessionMutex);
    if (!m_impl->GetIsReady() || m_impl->ownerThread != std::this_thread::get_id()
        || !m_impl->core.sharedDataMgr) return {};
    const auto graph = m_impl->core.sharedDataMgr->GetDataGraph();
    if (!graph.view) return {};
    const auto values = graph.view->GetDataQuery(query);
    HostDataCatalog catalog;
    catalog.commitId = values.commitId;
    catalog.data.reserve(values.data.size());
    for (const auto& data : values.data) if (data)
        catalog.data.push_back({data->self, data->type, data->inputs, data->provenance});
    return catalog;
}

std::optional<ImageDescriptor> VtkAppHostSession::GetImageDescriptor(const DataRevisionRef& revision)
{
    if (!m_impl || !GetDataRevisionRefValid(revision)) return {};
    const std::lock_guard<std::recursive_mutex> lock(m_impl->m_sessionMutex);
    if (!m_impl->GetIsReady() || m_impl->ownerThread != std::this_thread::get_id()
        || !m_impl->core.sharedDataMgr) return {};
    const auto graph = m_impl->core.sharedDataMgr->GetDataGraph();
    const auto data = graph.view ? graph.view->GetData(revision) : DataSnapshot{};
    const auto* image = data ? dynamic_cast<const ImageGrid3DPayload*>(data->payload.get()) : nullptr;
    if (!image || !image->GetValid()) return {};
    const auto& geometry = image->GetGeometry();
    ImageDescriptor descriptor;
    descriptor.dataRevision = revision;
    descriptor.metadata = image->GetMetadata();
    descriptor.dims = geometry.dimensions;
    descriptor.extent = geometry.extent;
    descriptor.spacing = geometry.spacing;
    descriptor.origin = geometry.origin;
    descriptor.direction = geometry.direction;
    descriptor.scalarRange = image->GetScalarRange();
    descriptor.valueType = image->GetValueType();
    descriptor.componentBytes = GetImageValueBytes(descriptor.valueType);
    descriptor.componentCount = image->GetComponentCount();
    const auto binding = graph.view->GetDataBinding(primaryVolumeBinding);
    if (binding && binding->target == revision) descriptor.bindingRevision = binding->revision;
    return descriptor;
}

RoiResult VtkAppHostSession::SetRoi(const RoiRequest& request)
{
    return m_impl ? m_impl->SetRoi(request) : RoiResult{};
}

RoiOperationResult VtkAppHostSession::SetRoiOperation(const RoiOperationRequest& request)
{
    return m_impl ? m_impl->SetRoiOperation(request):RoiOperationResult{};
}

std::vector<RoiDescriptor> VtkAppHostSession::GetRoiDescriptors(bool includeArchived)
{
    return m_impl ? m_impl->GetRoiDescriptors(includeArchived) : std::vector<RoiDescriptor>{};
}

std::optional<RoiDescriptor> VtkAppHostSession::GetRoiDescriptor(const DataRevisionRef& ref)
{
    return m_impl ? m_impl->GetRoiDescriptor(ref) : std::optional<RoiDescriptor>{};
}

std::vector<LabelMapDescriptor> VtkAppHostSession::GetLabelMapDescriptors()
{
    return m_impl ? m_impl->GetLabelMapDescriptors() : std::vector<LabelMapDescriptor>{};
}

std::optional<LabelMapDescriptor> VtkAppHostSession::GetLabelMapDescriptor(const std::string& id)
{
    return m_impl ? m_impl->GetLabelMapDescriptor(id) : std::optional<LabelMapDescriptor>{};
}

LabelMapReadResult VtkAppHostSession::GetLabelMapReadResult(const LabelMapReadRequest& request)
{
    return m_impl ? m_impl->GetLabelMapReadResult(request) : LabelMapReadResult{ LabelMapError::Unavailable, 0, {} };
}

LabelMapReadChunkResult VtkAppHostSession::GetLabelMapReadChunk(const LabelMapReadRequest& request, std::size_t voxelOffset)
{
    return m_impl ? m_impl->GetLabelMapReadChunk(request, voxelOffset) : LabelMapReadChunkResult{ LabelMapError::Unavailable, 0, 0, false, {} };
}


HostUpdateResult VtkAppHostSession::Impl::SendUpdates()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    HostUpdateResult result;
    if (!isBuilt || stopState.load() != HostStopState::Running) {
        result.status = HostUpdateStatus::Stopped;
        return result;
    }
    if (config.driveMode != HostDriveMode::HostDriven
        || ownerThread != std::this_thread::get_id()) return result;
    if (isFrameExecuting) {
        result.status = HostUpdateStatus::Deferred;
        return result;
    }
    isFrameExecuting = true;
    if (workSignal) workSignal->SendUpdates();
    const auto frames = frameCoordinator;
    try {
        (void)RenderFrameLifetime::PollAll();
        const auto status = frames->SendUpdates();
        result.sceneEpoch = frames->GetCommittedEpoch();
        result.renderViewIds = renderViews.GetRenderViewIds();
        switch (status) {
        case HostFrameCoordinator::FlushStatus::Completed:
            result.status = HostUpdateStatus::Completed; break;
        case HostFrameCoordinator::FlushStatus::Deferred:
        case HostFrameCoordinator::FlushStatus::RenderPending:
            result.status = HostUpdateStatus::Deferred; break;
        case HostFrameCoordinator::FlushStatus::Stopped:
            result.status = HostUpdateStatus::Stopped; break;
        default: break;
        }
    }
    catch (...) { result.status = HostUpdateStatus::Failed; }
    SendPendingFrameWork();
    isFrameExecuting = false;
    if (stopState.load() == HostStopState::StopRequested) {
        (void)Stop();
        result.status = HostUpdateStatus::Stopped;
        result.renderViewIds.clear();
    }
    return result;
}

void VtkAppHostSession::Impl::SendPendingFrameWork() noexcept
{
    // 仅实际未完成的 GPU 栅栏续约 owner 更新，隐藏视图的待绘制需求不会触发空转。
    if(workSignal)for(const auto& endpoint:endpoints)
        if(RenderFrameLifetime::GetHasPending(endpoint.renderWindow)) {
            (void)workSignal->SendWorkAvailable();break;
        }
}

HostRenderResult VtkAppHostSession::Impl::SendRender(
    const HostRenderRequest& request)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    HostRenderResult result;
    if (!isBuilt || stopState.load() != HostStopState::Running) {
        result.status = HostRenderStatus::Stopped;
        return result;
    }
    if (config.driveMode != HostDriveMode::HostDriven
        || ownerThread != std::this_thread::get_id()) return result;
    if (isFrameExecuting) {
        result.status = HostRenderStatus::Deferred;
        return result;
    }
    isFrameExecuting = true;
    isRendering = true;
    try { result = renderViews.SendFrameRender(request, [this] {
        return stopState.load() == HostStopState::Running;
    }); }
    catch (...) { result.status = HostRenderStatus::Failed; }
    SendPendingFrameWork();
    isRendering = false;
    isFrameExecuting = false;
    if (stopState.load() == HostStopState::StopRequested) {
        (void)Stop();
        result.status = HostRenderStatus::Stopped;
    }
    return result;
}

HostUpdateResult VtkAppHostSession::SendUpdates()
{
    return m_impl ? m_impl->SendUpdates() : HostUpdateResult{HostUpdateStatus::Stopped};
}

HostRenderResult VtkAppHostSession::SendRender(const HostRenderRequest& request)
{
    return m_impl ? m_impl->SendRender(request) : HostRenderResult{HostRenderStatus::Stopped};
}



RoiArchiveReadResult VtkAppHostSession::Impl::GetRoiArchiveReader(const std::vector<RoiArchiveSelection>& selection){
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if(ownerThread!=std::thread::id{} && ownerThread!=std::this_thread::get_id())return {RoiError::WrongThread};
    if(!GetIsReady() || !core.sharedDataMgr)return {};
    return RoiService::GetArchiveReader(core.sharedDataMgr->GetDataGraph(),selection);
}
RoiArchiveRestoreResult VtkAppHostSession::Impl::LoadRoiArchiveReader(const RoiArchiveReader& reader,const std::vector<RoiSourceMapping>& mappings,const RoiArchiveRestoreOptions& options){
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if(ownerThread!=std::thread::id{} && ownerThread!=std::this_thread::get_id())return {RoiError::WrongThread};
    if(!GetIsReady() || !core.sharedDataMgr)return {};
    const auto data=core.sharedDataMgr;return data->LoadRoiArchiveReader(reader,mappings,options);
}
RoiArchiveReadResult VtkAppHostSession::GetRoiArchiveReader(const std::vector<RoiArchiveSelection>& selection)
{return m_impl?m_impl->GetRoiArchiveReader(selection):RoiArchiveReadResult{};}
RoiArchiveRestoreResult VtkAppHostSession::LoadRoiArchiveReader(const RoiArchiveReader& reader,const std::vector<RoiSourceMapping>& mappings,const RoiArchiveRestoreOptions& options)
{return m_impl?m_impl->LoadRoiArchiveReader(reader,mappings,options):RoiArchiveRestoreResult{};}
