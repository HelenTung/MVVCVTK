#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeature.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostViewRuntimeRegistry.h"

#include "App/AppState.h"
#include "App/Services/AppServiceFactory.h"
#include "Data/DataManager.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

std::function<TrustedImageSnapshot()>
HostCoreServices::GetImageReader() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData]() {
        const auto data = weakData.lock();
        return data
            ? data->GetImageSnapshot()
            : TrustedImageSnapshot{};
    };
}

std::function<std::optional<ImageReadState>()>
HostCoreServices::GetImageReadState() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData]() {
        const auto data = weakData.lock();
        return data
            ? data->GetImageReadState()
            : std::optional<ImageReadState>{};
    };
}

std::function<ImageReadResult(const ImageReadRequest&)>
HostCoreServices::GetImageReadResult() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData](const ImageReadRequest& request) {
        const auto data = weakData.lock();
        if (data) {
            return data->GetImageReadResult(
                request, TaskStopToken{});
        }
        return ImageReadResult{};
    };
}

std::function<ImageReadChunkResult(
    const ImageReadRequest&,
    std::size_t)>
HostCoreServices::GetImageReadChunk() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    return [weakData](
        const ImageReadRequest& request,
        const std::size_t voxelOffset) {
        const auto data = weakData.lock();
        if (data) {
            return data->GetImageReadChunk(
                request, voxelOffset, TaskStopToken{});
        }
        return ImageReadChunkResult{};
    };
}

std::function<bool(
    TrustedImageState,
    const TrustedImageSnapshot&,
    TrustedImageSnapshot&)>
HostCoreServices::GetImageWriter() const
{
    const std::weak_ptr<AbstractDataManager> weakData =
        sharedDataMgr;
    const std::weak_ptr<SharedInteractionState> weakState =
        sharedState;
    return [weakData, weakState](
        TrustedImageState state,
        const TrustedImageSnapshot& expectedSnapshot,
        TrustedImageSnapshot& publishedSnapshot) {
        publishedSnapshot.reset();
        const auto data = weakData.lock();
        const auto sharedState = weakState.lock();
        if (!data
            || !sharedState
            || !expectedSnapshot
            || !state.image
            || !data->SetCurrentData(
                std::move(state),
                expectedSnapshot,
                publishedSnapshot)) {
            return false;
        }

        // current 已发布；观察者异常不能把成功的 CAS 事务改报为失败。
        try {
            (void)sharedState->SetImageDataReady(
                publishedSnapshot->scalarRange[0],
                publishedSnapshot->scalarRange[1],
                publishedSnapshot->spacing);
        }
        catch (...) {
        }
        return true;
    };
}

class VtkAppHostSession::Impl final {
public:
    using StopToken = std::uint64_t;

    struct PendingStopEntry;

    struct FeatureEntry final {
        std::string id;
        // attached Feature 属于 Session aggregate；只有 Detach/Stop 成功后才释放，
        // 从而保证 Feature 内的 VTK 绑定始终在 owner thread 上确定性清理。
        std::shared_ptr<HostFeature> feature;
    };

    struct OwnerCompleteState final {
        struct ImageReadEntry final {
            ImageReadCallback callback;
            std::optional<ImageReadResult> result;
            bool isReady = false;
        };

        std::mutex mutex;
        std::vector<std::function<void()>> completes;
        std::shared_ptr<ImageReadEntry> imageRead;
        bool isActive = true;
    };

    // Feature 只持有窄能力对象；具体 RuntimeRegistry/HotkeyRouter 只在组合根内部可见，
    // StopOwner 后所有跨层调用稳定返回失败。
    class FeatureHostBridge final {
    public:
        bool StartOwner(
            HostViewRuntimeRegistry& views,
            HostInputPort& input,
            const std::thread::id ownerThread)
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_isActive) return false;
            m_views = &views;
            m_input = &input;
            m_ownerThread = ownerThread;
            m_isActive = true;
            return true;
        }

        bool StopOwner()
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_isActive
                && m_ownerThread != std::this_thread::get_id()) {
                return false;
            }
            m_isActive = false;
            m_views = nullptr;
            m_input = nullptr;
            m_ownerThread = {};
            return true;
        }

        std::vector<HostFeatureView> GetViews(
            const HostViewTargets& targets) const
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.views ? ports.views->GetFeatureViews(targets)
                : std::vector<HostFeatureView>{};
        }

        std::shared_ptr<FeatureViewService> GetFeaturePort(
            const std::string& viewId) const
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.views ? ports.views->GetFeaturePort(viewId)
                : std::shared_ptr<FeatureViewService>{};
        }

        std::shared_ptr<OverlayService> GetOverlayPort(
            const std::string& viewId) const
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.views ? ports.views->GetOverlayPort(viewId)
                : std::shared_ptr<OverlayService>{};
        }

        std::optional<HostInputView> GetInputView(
            const HostViewTarget& target) const
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.views ? ports.views->GetInputView(target)
                : std::optional<HostInputView>{};
        }

        bool SetActiveViews(
            const std::string& featureId,
            const std::vector<std::string>& viewIds)
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.views
                && ports.views->SetFeatureViews(featureId, viewIds);
        }

        bool SetViewStatus(
            const std::vector<std::string>& viewIds,
            const std::string& status)
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.views
                && ports.views->SetViewStatus(viewIds, status);
        }

        bool AttachInput(HostInputBinding binding)
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.input
                && ports.input->AttachInput(std::move(binding));
        }

        bool DetachInput(
            const std::string_view featureId)
        {
            const OwnerPorts ports = GetOwnerPorts();
            return ports.input
                && ports.input->DetachInput(featureId);
        }

    private:
        struct OwnerPorts final {
            HostViewRuntimeRegistry* views = nullptr;
            HostInputPort* input = nullptr;
        };

        OwnerPorts GetOwnerPorts() const noexcept
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_isActive || !m_views || !m_input
                || m_ownerThread != std::this_thread::get_id()) {
                return {};
            }

            // StopOwner 只能在同一 owner thread 执行；因此锁外调用期间，
            // Registry/Input 的借用生命周期稳定，同时避免跨层锁顺序反转。
            return { m_views, m_input };
        }

        mutable std::mutex m_mutex;
        HostViewRuntimeRegistry* m_views = nullptr;
        HostInputPort* m_input = nullptr;
        std::thread::id m_ownerThread;
        bool m_isActive = false;
    };

    class FeatureViewDirectoryPort final
        : public FeatureViewDirectory {
    public:
        explicit FeatureViewDirectoryPort(
            std::weak_ptr<FeatureHostBridge> bridge)
            : m_bridge(std::move(bridge))
        {
        }

        std::vector<HostFeatureView> GetViews(
            const HostViewTargets& targets) const override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                ? bridge->GetViews(targets)
                : std::vector<HostFeatureView>{};
        }

        std::shared_ptr<FeatureViewService> GetFeaturePort(
            const std::string& viewId) const override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                ? bridge->GetFeaturePort(viewId)
                : std::shared_ptr<FeatureViewService>{};
        }

        std::shared_ptr<OverlayService> GetOverlayPort(
            const std::string& viewId) const override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                ? bridge->GetOverlayPort(viewId)
                : std::shared_ptr<OverlayService>{};
        }

        std::optional<HostInputView> GetInputView(
            const HostViewTarget& target) const override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                ? bridge->GetInputView(target)
                : std::optional<HostInputView>{};
        }

    private:
        std::weak_ptr<FeatureHostBridge> m_bridge;
    };

    class FeatureDataPort final
        : public TrustedFeatureDataPort {
    public:
        explicit FeatureDataPort(const HostCoreServices& core)
            : m_getSnapshot(core.GetImageReader())
            , m_setImageState(core.GetImageWriter())
        {
        }

        TrustedImageSnapshot GetImageSnapshot() const override
        {
            return m_getSnapshot
                ? m_getSnapshot() : TrustedImageSnapshot{};
        }

        bool SetImageState(
            TrustedImageState imageState,
            const TrustedImageSnapshot& expected,
            TrustedImageSnapshot& published) override
        {
            return m_setImageState
                && m_setImageState(
                    std::move(imageState),
                    expected,
                    published);
        }

    private:
        std::function<TrustedImageSnapshot()> m_getSnapshot;
        std::function<bool(
            TrustedImageState,
            const TrustedImageSnapshot&,
            TrustedImageSnapshot&)> m_setImageState;
    };

    class FeatureReadPort final : public ImageReadPort {
    public:
        explicit FeatureReadPort(const HostCoreServices& core)
            : m_getReadState(core.GetImageReadState())
            , m_getReadResult(core.GetImageReadResult())
            , m_getReadChunk(core.GetImageReadChunk())
        {
        }

        std::optional<ImageReadState> GetImageReadState() const override
        {
            return m_getReadState
                ? m_getReadState()
                : std::optional<ImageReadState>{};
        }

        ImageReadResult GetImageReadResult(
            const ImageReadRequest& request) const override
        {
            return m_getReadResult
                ? m_getReadResult(request)
                : ImageReadResult{};
        }

        ImageReadChunkResult GetImageReadChunk(
            const ImageReadRequest& request,
            const std::size_t voxelOffset) const override
        {
            return m_getReadChunk
                ? m_getReadChunk(request, voxelOffset)
                : ImageReadChunkResult{};
        }

    private:
        std::function<std::optional<ImageReadState>()> m_getReadState;
        std::function<ImageReadResult(const ImageReadRequest&)>
            m_getReadResult;
        std::function<ImageReadChunkResult(
            const ImageReadRequest&,
            std::size_t)> m_getReadChunk;
    };

    class FeatureHostControlPort final
        : public FeatureHostControl {
    public:
        FeatureHostControlPort(
            std::weak_ptr<FeatureHostBridge> bridge,
            std::string featureId,
            std::weak_ptr<OwnerCompleteState> completeState)
            : m_bridge(std::move(bridge))
            , m_featureId(std::move(featureId))
            , m_completeState(std::move(completeState))
        {
        }

        bool SetActiveViews(
            const std::vector<std::string>& viewIds) override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->SetActiveViews(m_featureId, viewIds);
        }

        bool SetViewStatus(
            const std::vector<std::string>& viewIds,
            const std::string& status) override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->SetViewStatus(viewIds, status);
        }

        bool AttachInput(HostInputBinding binding) override
        {
            if (binding.featureId != m_featureId) {
                return false;
            }
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->AttachInput(std::move(binding));
        }

        bool DetachInput(
            const std::string_view featureId) override
        {
            if (featureId != std::string_view(m_featureId)) {
                return false;
            }
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->DetachInput(featureId);
        }

        bool SendOwnerComplete(
            std::function<void()> complete) override
        {
            const auto state = m_completeState.lock();
            if (!state || !complete) {
                return false;
            }
            const std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->isActive) {
                return false;
            }
            state->completes.push_back(std::move(complete));
            return true;
        }

    private:
        std::weak_ptr<FeatureHostBridge> m_bridge;
        std::string m_featureId;
        std::weak_ptr<OwnerCompleteState> m_completeState;
    };

    explicit Impl(HostSessionConfig sessionConfig)
        : config(std::move(sessionConfig))
        , ownerCompleteState(
            std::make_shared<OwnerCompleteState>())
    {
    }

    ~Impl();

    bool BuildSession();
    bool SendRequest(
        HostRequest&& request,
        HostCompleteCallback onComplete);
    bool SendRequestResult(
        HostRequest&& request,
        HostResultCallback onComplete);
    std::optional<HostRenderViewState> GetRenderViewState(
        const HostViewTarget& target);
    std::vector<HostRenderViewState> GetRenderViewStates();
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
    std::shared_ptr<HostCommandRouter> commandRouter;
    std::vector<HostRenderViewEndpoint> endpoints;
    std::unique_ptr<HostHotkeyRouter> hotkeyRouter;
    std::vector<FeatureEntry> features;
    // 保存实际已安装 handler 的目标；补偿失败时允许暂存双绑定并由后续请求重试收敛。
    std::vector<HostViewTarget> timerTargets;
    std::shared_ptr<OwnerCompleteState> ownerCompleteState;
    std::shared_ptr<FeatureHostBridge> featureBridge;
    std::thread::id ownerThread;
    bool isBuilt = false;
    bool isStarted = false;
    std::atomic<HostStopState> stopState{ HostStopState::Stopped };
    mutable std::recursive_mutex m_sessionMutex;

private:
    static bool SetPendingStop(
        std::unique_ptr<Impl>& impl,
        StopToken token) noexcept;
    static bool SendPendingStop(StopToken token) noexcept;
    static StopToken GetStopToken() noexcept;
    static HostCoreServices BuildCore();
    void SendDiagnostic(const std::string& message) const noexcept;
    void SendImageReadComplete(bool isStopping) noexcept;
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
    if (config.renderViews.empty()) {
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
            if (featureBridge) (void)featureBridge->StopOwner();
            endpoints.clear();
            hotkeyRouter.reset();
            commandRouter.reset();
            isStopped = renderViews.StopLease();
        }
        catch (...) {
            isStopped = false;
        }
        if (!isStopped) {
            stopState = HostStopState::StopPending;
            return false;
        }
        core = {};
        isBuilt = false;
        isStarted = false;
        ownerThread = {};
        stopState = HostStopState::Stopped;
        return true;
    };
    try {
        core = BuildCore();
        if (!renderViews.Build(core, config.renderViews)) {
            (void)clearBuild();
            return false;
        }
        const auto viewDirectory = renderViews.GetViewDirectory();
        commandRouter = std::make_shared<HostCommandRouter>(viewDirectory);
        if (!renderViews.SetInitialVisibility()
            || !renderViews.SetInteractorsReady()) {
            (void)clearBuild();
            return false;
        }
        endpoints = renderViews.BuildEndpoints();
        if (endpoints.size() != config.renderViews.size()) {
            (void)clearBuild();
            return false;
        }
        hotkeyRouter = std::make_unique<HostHotkeyRouter>(
            viewDirectory,
            commandRouter);
        if (!hotkeyRouter) {
            (void)clearBuild();
            return false;
        }
        if (!featureBridge) {
            featureBridge = std::make_shared<FeatureHostBridge>();
        }
        if (!featureBridge->StartOwner(
                renderViews,
                hotkeyRouter->GetInputPort(),
                ownerThread)) {
            (void)clearBuild();
            return false;
        }
        isBuilt = true;
        stopState = HostStopState::Running;
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
    if (!BuildSession()
        || ownerThread != std::this_thread::get_id()
        || !commandRouter) {
        return false;
    }
    return commandRouter->Dispatch(
        std::move(request),
        std::move(onComplete));
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
        if (commandRouter->Dispatch(
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
    ImageReadRequest request,
    ImageReadCallback onComplete)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!onComplete) return ImageReadAdmission::InvalidRequest;
    if (!GetIsReady() || !core.sharedDataMgr) {
        return ImageReadAdmission::Unavailable;
    }
    const auto executor = renderViews.GetTaskExecutor();
    if (!executor || !ownerCompleteState) {
        return ImageReadAdmission::Unavailable;
    }

    std::shared_ptr<OwnerCompleteState::ImageReadEntry> entry;
    try {
        entry = std::make_shared<
            OwnerCompleteState::ImageReadEntry>();
        entry->callback = std::move(onComplete);
    }
    catch (...) {
        return ImageReadAdmission::Unavailable;
    }
    {
        const std::lock_guard<std::mutex> completeLock(
            ownerCompleteState->mutex);
        if (!ownerCompleteState->isActive) {
            return ImageReadAdmission::Stopping;
        }
        if (ownerCompleteState->imageRead) {
            return ImageReadAdmission::Busy;
        }
        ownerCompleteState->imageRead = entry;
    }

    const auto data = core.sharedDataMgr;
    const std::weak_ptr<OwnerCompleteState> weakComplete =
        ownerCompleteState;
    AppTaskWork work;
    try {
        work = AppTaskWork([
            data,
            request = std::move(request),
            weakComplete,
            entry](const TaskStopToken stopToken) mutable {
            ImageReadResult result;
            try {
                result = data->GetImageReadResult(
                    request, stopToken);
            }
            catch (...) {
                result.error = stopToken.GetIsStopped()
                    ? ImageReadError::Cancelled
                    : ImageReadError::CopyFailed;
            }
            const auto complete = weakComplete.lock();
            if (!complete) return false;
            const std::lock_guard<std::mutex> completeLock(
                complete->mutex);
            if (!complete->isActive
                || complete->imageRead != entry) {
                return false;
            }
            entry->result = std::move(result);
            entry->isReady = true;
            return true;
        });
    }
    catch (...) {
        const std::lock_guard<std::mutex> completeLock(
            ownerCompleteState->mutex);
        if (ownerCompleteState->imageRead == entry) {
            ownerCompleteState->imageRead.reset();
        }
        return ImageReadAdmission::Unavailable;
    }

    const auto admission = SendReadTask(executor, std::move(work));
    if (admission == TaskAdmissionResult::Accepted) {
        return ImageReadAdmission::Accepted;
    }
    {
        const std::lock_guard<std::mutex> completeLock(
            ownerCompleteState->mutex);
        if (ownerCompleteState->imageRead == entry) {
            ownerCompleteState->imageRead.reset();
        }
    }
    switch (admission) {
    case TaskAdmissionResult::InvalidRequest:
        return ImageReadAdmission::InvalidRequest;
    case TaskAdmissionResult::Busy:
        return ImageReadAdmission::Busy;
    case TaskAdmissionResult::QueueFull:
        return ImageReadAdmission::QueueFull;
    case TaskAdmissionResult::Stopping:
        return ImageReadAdmission::Stopping;
    default:
        return ImageReadAdmission::Unavailable;
    }
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

bool VtkAppHostSession::Impl::DetachTimer()
{
    bool isCleared = true;
    auto target = timerTargets.begin();
    while (target != timerTargets.end()) {
        if (renderViews.ClearTimerHandler(*target)) {
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

    if (!renderViews.ClearTimerHandler(oldTarget)) {
        // 只有新 handler 的补偿清理确实成功，才能从实际绑定记录中移除它。
        if (renderViews.ClearTimerHandler(timerTargets.back())) {
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

void VtkAppHostSession::Impl::OnHostTimer()
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (ownerThread != std::this_thread::get_id()) {
        return;
    }

    auto output = features.begin();
    for (auto input = features.begin();
        input != features.end(); ++input) {
        const auto& feature = input->feature;
        if (!feature) continue;
        *output++ = *input;
        try {
            (void)feature->OnHostTick();
        }
        catch (...) {
            std::cerr
                << "[Host] Feature tick failed: "
                << input->id << '\n';
        }
    }
    features.erase(output, features.end());

    std::vector<std::function<void()>> completes;
    if (ownerCompleteState) {
        const std::lock_guard<std::mutex> lock(
            ownerCompleteState->mutex);
        if (ownerCompleteState->isActive) {
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
    SendImageReadComplete(false);
}

void VtkAppHostSession::Impl::SendImageReadComplete(
    const bool isStopping) noexcept
{
    ImageReadCallback callback;
    std::optional<ImageReadResult> result;
    if (ownerCompleteState) {
        const std::lock_guard<std::mutex> lock(
            ownerCompleteState->mutex);
        const auto& entry = ownerCompleteState->imageRead;
        if (!entry || (!ownerCompleteState->isActive && !isStopping)) {
            return;
        }
        // StopLease 已停止并 join 共享 executor；若任务尚未来得及写入结果，
        // owner thread 在关闭 timer 前补齐 Cancelled 终态，Accepted 请求不会失去回调。
        if (isStopping && (!entry->isReady || !entry->result)) {
            ImageReadResult cancelled;
            cancelled.error = ImageReadError::Cancelled;
            entry->result = std::move(cancelled);
            entry->isReady = true;
        }
        if (entry->isReady && entry->result) {
            callback = std::move(entry->callback);
            result = std::move(entry->result);
            ownerCompleteState->imageRead.reset();
        }
    }
    if (callback && result) {
        try {
            callback(std::move(*result));
        }
        catch (...) {
        }
    }
}

bool VtkAppHostSession::Impl::AttachFeature(
    const std::shared_ptr<HostFeature>& feature)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!isBuilt
        || ownerThread != std::this_thread::get_id()
        || !feature
        || !hotkeyRouter) {
        return false;
    }

    std::string id;
    try {
        id = feature->GetFeatureId();
    }
    catch (...) {
        return false;
    }
    if (id.empty()) {
        return false;
    }
    for (const auto& entry : features) {
        const auto& current = entry.feature;
        if (entry.id == id
            || (current && current.get() == feature.get())) {
            return false;
        }
    }

    if (!featureBridge) return false;
    const std::weak_ptr<FeatureHostBridge> weakBridge =
        featureBridge;
    HostFeatureContext context;
    try {
        context.views =
            std::make_shared<FeatureViewDirectoryPort>(weakBridge);
        context.read = std::make_shared<FeatureReadPort>(core);
        context.data = std::make_shared<FeatureDataPort>(core);
        context.host = std::make_shared<FeatureHostControlPort>(
            weakBridge,
            id,
            ownerCompleteState);
    }
    catch (...) {
        return false;
    }

    const auto clearRejectedAttach = [&]() noexcept {
        try {
            (void)feature->DetachHost();
        }
        catch (...) {
        }
        try {
            (void)hotkeyRouter->GetInputPort().DetachInput(id);
        }
        catch (...) {
        }
        try {
            (void)renderViews.SetFeatureViews(id, {});
        }
        catch (...) {
        }
    };
    try {
        if (!feature->AttachHost(context)) {
            clearRejectedAttach();
            return false;
        }
        features.push_back(
            FeatureEntry{ id, feature });
    }
    catch (...) {
        clearRejectedAttach();
        return false;
    }
    return true;
}

bool VtkAppHostSession::Impl::DetachFeature(
    const HostFeature& feature)
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (!isBuilt
        || ownerThread != std::this_thread::get_id()) {
        return false;
    }
    const auto entry = std::find_if(
        features.begin(),
        features.end(),
        [&feature](const FeatureEntry& current) {
            const auto& value = current.feature;
            return value && value.get() == &feature;
        });
    if (entry == features.end()) {
        return false;
    }
    // 先由组合根收回跨视图状态，再允许 Feature 丢弃宿主回调；
    // DetachHost 拒绝时恢复精确旧绑定，避免 attached Feature 留在半解绑状态。
    const auto oldViewIds =
        renderViews.GetFeatureViewIds(entry->id);
    if (!renderViews.SetFeatureViews(entry->id, {})) {
        return false;
    }
    bool isDetached = false;
    try {
        isDetached = const_cast<HostFeature&>(feature).DetachHost();
    }
    catch (...) {
        isDetached = false;
    }
    if (!isDetached) {
        if (!renderViews.SetFeatureViews(entry->id, oldViewIds)) {
            std::cerr
                << "[Host] Feature detach rollback did not restore all views.\n";
            stopState = HostStopState::StopPending;
        }
        return false;
    }
    features.erase(entry);
    return true;
}

bool VtkAppHostSession::Impl::DetachFeatures()
{
    while (!features.empty()) {
        auto& entry = features.back();
        if (!renderViews.SetFeatureViews(entry.id, {})) {
            return false;
        }
        const auto& feature = entry.feature;
        if (!feature) return false;
        try {
            if (!feature->DetachHost()) {
                return false;
            }
        }
        catch (...) {
            return false;
        }
        features.pop_back();
    }
    return true;
}

bool VtkAppHostSession::Impl::Stop() noexcept
{
    const std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if (ownerThread != std::thread::id{}
        && ownerThread != std::this_thread::get_id()) {
        stopState = HostStopState::StopRequested;
        return false;
    }

    stopState = HostStopState::Stopping;
    try {
        // timer/hotkey handler 随 context 一起停止；因此在 StopLease 失败时仍保留，
        // owner 修复底层 timer 后可以继续交互并重试。
        if (!DetachFeatures()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        if (!renderViews.StopLease()) {
            stopState = HostStopState::StopPending;
            return false;
        }
        // StopLease 成功后 timer 与 executor 都已停止；必须在任何后续可失败清理前，
        // 由当前 owner thread 兑现已接纳图像读取的唯一终态回调。
        SendImageReadComplete(true);
        endpoints.clear();
        timerTargets.clear();
        if (featureBridge) (void)featureBridge->StopOwner();
        if (hotkeyRouter && !hotkeyRouter->ClearHotkeys()) {
            stopState = HostStopState::StopPending;
            return false;
        }

        if (ownerCompleteState) {
            const std::lock_guard<std::mutex> lock(
                ownerCompleteState->mutex);
            ownerCompleteState->isActive = false;
            ownerCompleteState->completes.clear();
            ownerCompleteState->imageRead.reset();
        }
        hotkeyRouter.reset();
        commandRouter.reset();
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
        && !hotkeyRouter
        && features.empty();
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
    if (!BuildSession() || m_impl->isStarted) {
        return false;
    }
    if (!m_impl->renderViews.SendRenderAll()) {
        return false;
    }
    m_impl->isStarted = true;
    if (!m_impl->renderViews.StartStandaloneView()) {
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
