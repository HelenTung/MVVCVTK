#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeature.h"
#include "Host/Internal/HostFeatureRuntime.h"
#include "Host/Internal/HostImageReadRuntime.h"
#include "Host/HostFrameCoordinator.h"
#include "Host/HostWorkSignal.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostInputRegistry.h"
#include "Host/HostViewRuntimeRegistry.h"

#include "App/AppState.h"
#include "App/Services/AppServiceFactory.h"
#include "Data/DataManager.h"
#include "Data/LabelMapReader.h"

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
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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
    bool SendRequest(
        HostRequest&& request,
        HostCompleteCallback onComplete);
    bool SendRequestResult(
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
    bool GetIsReady() const noexcept;
    bool GetIsStopped() const noexcept;
    HostStopState GetStopState() const noexcept;
    static bool SendOwnerStop(
        std::unique_ptr<Impl>& impl) noexcept;
    static bool SendPendingStops() noexcept;
    static std::size_t GetPendingStopCount() noexcept;

    HostSessionConfig config;
    HostCoreServices core;
    HostViewRuntimeRegistry renderViews;
    std::shared_ptr<HostFrameCoordinator> frameCoordinator;
    std::shared_ptr<HostCommandRouter> commandRouter;
    std::vector<HostRenderViewEndpoint> endpoints;
    std::unique_ptr<HostInputRegistry> inputRegistry;
    std::unique_ptr<HostHotkeyRouter> hotkeyRouter;
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
            const bool isHotkeyStopped =
                !hotkeyRouter || hotkeyRouter->ClearHotkeys();
            const bool isInputStopped =
                isHotkeyStopped
                && (!inputRegistry || inputRegistry->Stop());
            if (!isInputStopped) {
                stopState = HostStopState::StopPending;
                return false;
            }
            hotkeyRouter.reset();
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
                ? config.onWorkAvailable : std::function<void()>{});
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
        hotkeyRouter = std::make_unique<HostHotkeyRouter>(
            *inputRegistry, commandRouter);
        if (!hotkeyRouter) {
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

bool VtkAppHostSession::Impl::SendRequest(
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

bool VtkAppHostSession::Impl::SendRequestResult(
    HostRequest&& request,
    HostResultCallback onComplete)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!onComplete) return false;
    auto callback =
        std::make_shared<HostResultCallback>(std::move(onComplete));
    auto isComplete = std::make_shared<std::atomic<bool>>(false);
    const auto sendResult = [callback, isComplete](
        HostResult result) noexcept {
        if (isComplete->exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            (*callback)(std::move(result));
        }
        catch (...) {
        }
    };

    if (ownerThread != std::thread::id{}
        && ownerThread != std::this_thread::get_id()) {
        sendResult({ false, HostErrorCode::WrongThread,
            "Request must run on the session owner thread." });
        return false;
    }
    if (!BuildSession() || !commandRouter) {
        sendResult({ false, HostErrorCode::SessionNotReady,
            "Host session is not ready." });
        return false;
    }

    const HostCompleteCallback legacyComplete =
        [sendResult](const bool isSucceeded) {
            sendResult({
                isSucceeded,
                isSucceeded ? HostErrorCode::None
                    : HostErrorCode::OperationFailed,
                isSucceeded ? std::string{}
                    : "The accepted host operation failed." });
    };
    try {
        if (SendRequest(
                std::move(request), legacyComplete)) {
            return true;
        }
    }
    catch (...) {
        sendResult({ false, HostErrorCode::OperationFailed,
            "The host request raised an exception." });
        return false;
    }
    sendResult({ false, HostErrorCode::RequestRejected,
        "The host request was rejected before execution." });
    return false;
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
        // P0 首先关闭普通 frame admission 与输入 gate；清理失败时保持
        // StopPending，但不再恢复可交互状态。
        if (frameCoordinator) frameCoordinator->Stop();
        if (isBuilt && !renderViews.SetInputsEnabled(false)) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if (!DetachFeatures()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if (hotkeyRouter && !hotkeyRouter->ClearHotkeys()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if (inputRegistry && !inputRegistry->Stop()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if (!renderViews.StopLease()) {
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
        hotkeyRouter.reset();
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
        && !hotkeyRouter
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

bool VtkAppHostSession::AttachTimer(
    const HostTimerConfig& config)
{
    return BuildSession() && m_impl->AttachTimer(config);
}

bool VtkAppHostSession::AttachHotkeys(
    const HostHotkeyConfig& config)
{
    if (!m_impl) return false;
    const std::lock_guard<std::recursive_mutex> lock(
        m_impl->m_sessionMutex);
    return BuildSession()
        && (m_impl->config.driveMode == HostDriveMode::Native
            || (!config.isContextInputEnabled && !config.isCommandInputEnabled))
        && m_impl->hotkeyRouter
        && m_impl->hotkeyRouter->AttachHotkeys(config);
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

bool VtkAppHostSession::SendRequest(
    HostRequest&& request,
    HostCompleteCallback onComplete)
{
    return m_impl
        && m_impl->SendRequest(
            std::move(request),
             std::move(onComplete));
}

bool VtkAppHostSession::SendRequestResult(
    HostRequest&& request,
    HostResultCallback onComplete)
{
    if (!m_impl) {
        if (onComplete) {
            try {
                onComplete({ false, HostErrorCode::SessionNotReady,
                    "Host session is not available." });
            }
            catch (...) {
            }
        }
        return false;
    }
    return m_impl->SendRequestResult(
        std::move(request), std::move(onComplete));
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
    isFrameExecuting = false;
    if (stopState.load() == HostStopState::StopRequested) {
        (void)Stop();
        result.status = HostUpdateStatus::Stopped;
        result.renderViewIds.clear();
    }
    return result;
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
