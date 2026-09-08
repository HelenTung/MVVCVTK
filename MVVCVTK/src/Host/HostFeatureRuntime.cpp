#include "Host/Internal/HostFeatureRuntime.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/HostFrameCoordinator.h"
#include "Host/HostWorkSignal.h"
#include "Host/FeatureModelTransformPort.h"
#include "App/Services/PrimaryDataActivation.h"
#include "App/AppState.h"
#include "Data/DataService.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class HostFeatureRuntime::Impl final {
public:
    struct FeatureCompleteEntry final {
        std::function<void()> complete;

        void Send() noexcept
        {
            // 仅 owner thread 消费；先取走终态，重入/本批旧槽位不能重放。
            auto callback = std::exchange(complete, {});
            if (!callback) return;
            try { callback(); }
            catch (...) {}
        }
    };

    struct FeatureLifetime final {
        std::uint64_t id = 0;
        std::atomic<bool> isActive{ true };
        std::weak_ptr<AbstractDataManager> data;
        // 仅 owner thread 修改；完成队列可跨线程读取 isActive。
        std::vector<DataObserverId> observers;
        std::mutex completeMutex;
        std::vector<std::weak_ptr<FeatureCompleteEntry>> completes;

        void Stop(const bool shouldComplete)
        {
            std::vector<std::weak_ptr<FeatureCompleteEntry>> pendingCompletes;
            {
                const std::lock_guard<std::mutex> lock(completeMutex);
                isActive.store(false);
                pendingCompletes.swap(completes);
            }
            const auto manager = data.lock();
            auto pending = std::move(observers);
            observers.clear();
            if (manager) {
                for (const auto observer : pending) {
                    // 具体 DataGraphStore 的 false 仅表示该订阅已不存在。
                    (void)manager->DetachDataChange(observer);
                }
            }
            for (const auto& pendingComplete : pendingCompletes) {
                const auto entry = pendingComplete.lock();
                if (!entry) continue;
                if (shouldComplete) entry->Send();
                else entry->complete = {};
            }
        }
    };

    struct FeatureEntry final {
        std::string id;
        // attached Feature 属于 Session aggregate；只有 Detach/Stop 成功后才释放，
        // 从而保证 Feature 内的 VTK 绑定始终在 owner thread 上确定性清理。
        std::shared_ptr<HostFeature> feature;
        // DetachHost 成功后单调置位；后置 input 门禁失败时不重放 Feature teardown。
        bool isHostDetached = false;
        std::shared_ptr<FeatureLifetime> lifetime;
    };

    // Feature 只持有窄能力对象；具体 RuntimeRegistry/HotkeyRouter 只在组合根内部可见，
    // StopOwner 后所有跨层调用稳定返回失败。
    class FeatureHostBridge final {
    public:
        bool GetIsOwnerActive() const noexcept
        { return GetOwnerPorts().views != nullptr; }

        bool StartOwner(
            HostViewRuntimeRegistry& views,
            HostInputPort& input,
            const std::shared_ptr<HostFrameCoordinator>& frames,
            const std::thread::id ownerThread)
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_isActive || !frames) return false;
            m_views = &views;
            m_input = &input;
            m_frames = frames;
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
            m_frames.reset();
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

        bool SendSceneDelta(
            const std::string& featureId,
            FeatureSceneDelta delta,
            const std::shared_ptr<FeatureLifetime>& lifetime)
        {
            std::shared_ptr<HostFrameCoordinator> frames;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_isActive) return false;
                frames = m_frames.lock();
            }
            return frames
                && lifetime && lifetime->isActive.load()
                && frames->Enqueue(featureId, std::move(delta), lifetime->id,
                    std::shared_ptr<const std::atomic<bool>>(lifetime, &lifetime->isActive));
        }

        std::optional<HostSceneViewState> GetSceneViewState(const std::string& viewId) const
        {
            const auto ports = GetOwnerPorts();
            return ports.views ? ports.views->GetSceneViewState(
                { viewId, false, HostRenderViewRole::Auxiliary }) : std::nullopt;
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
        std::weak_ptr<HostFrameCoordinator> m_frames;
        std::thread::id m_ownerThread;
        bool m_isActive = false;
    };

    class FeatureViewDirectoryPort final
        : public FeatureViewDirectory {
    public:
        explicit FeatureViewDirectoryPort(
            std::weak_ptr<FeatureHostBridge> bridge,
            std::shared_ptr<FeatureLifetime> lifetime)
            : m_bridge(std::move(bridge))
            , m_lifetime(std::move(lifetime))
        {
        }

        std::vector<HostFeatureView> GetViews(
            const HostViewTargets& targets) const override
        {
            const auto bridge = GetBridge();
            return bridge
                ? bridge->GetViews(targets)
                : std::vector<HostFeatureView>{};
        }

        std::shared_ptr<FeatureViewService> GetFeaturePort(
            const std::string& viewId) const override
        {
            const auto bridge = GetBridge();
            return bridge
                ? bridge->GetFeaturePort(viewId)
                : std::shared_ptr<FeatureViewService>{};
        }

        std::shared_ptr<OverlayService> GetOverlayPort(
            const std::string& viewId) const override
        {
            const auto bridge = GetBridge();
            return bridge
                ? bridge->GetOverlayPort(viewId)
                : std::shared_ptr<OverlayService>{};
        }

        std::optional<HostInputView> GetInputView(
            const HostViewTarget& target) const override
        {
            const auto bridge = GetBridge();
            return bridge
                ? bridge->GetInputView(target)
                : std::optional<HostInputView>{};
        }

    private:
        std::shared_ptr<FeatureHostBridge> GetBridge() const
        {
            return m_lifetime->isActive.load() ? m_bridge.lock() : nullptr;
        }

        std::weak_ptr<FeatureHostBridge> m_bridge;
        std::shared_ptr<FeatureLifetime> m_lifetime;
    };

    class FeatureDataPort final
        : public TrustedDataPort {
    public:
        FeatureDataPort(
            const Ports& ports,
            const std::thread::id ownerThread,
            std::shared_ptr<FeatureLifetime> lifetime)
            : m_data(ports.data)
            , m_state(ports.state)
            , m_ownerThread(ownerThread)
            , m_lifetime(std::move(lifetime))
        {
        }

        RoiReadResult GetRoi(const DataGraphSnapshot& graph,
            const DataRevisionRef& roiRef, const DataRevisionRef& sourceRef) const override
        {
            const auto data = GetReadData();
            return data ? data->GetRoi(graph, roiRef, sourceRef) : RoiReadResult{};
        }

        RoiResult SetRoi(const RoiRequest& request) override
        {
            if (m_ownerThread != std::this_thread::get_id()) return {RoiError::WrongThread};
            const auto data = GetWriteData();
            return data ? data->SetRoi(request) : RoiResult{};
        }

        DataGraphSnapshot GetDataGraph() const override
        {
            const auto data = GetReadData();
            return data ? data->GetDataGraph() : DataGraphSnapshot{};
        }

        DataSnapshot GetData(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = GetReadData();
            return data ? data->GetData(graph, ref) : DataSnapshot{};
        }

        DataQueryResult GetDataQuery(
            const DataGraphSnapshot& graph,
            const DataQuery& query) const override
        {
            const auto data = GetReadData();
            return data
                ? data->GetDataQuery(graph, query) : DataQueryResult{};
        }

        std::optional<DataBinding> GetDataBinding(
            const DataGraphSnapshot& graph,
            const std::string_view name) const override
        {
            const auto data = GetReadData();
            return data
                ? data->GetDataBinding(graph, name)
                : std::optional<DataBinding>{};
        }

        ProjectDataSnapshot GetProjectData() const override
        {
            const auto data = GetReadData();
            return data ? data->GetProjectData() : ProjectDataSnapshot{};
        }

        DataRelationStatus GetDataRelation(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref,
            const std::string_view inputRole,
            const std::string_view binding) const override
        {
            const auto data = GetReadData();
            return data
                ? data->GetDataRelation(graph, ref, inputRole, binding)
                : DataRelationStatus::Unknown;
        }

        VtkImageGridSnapshot GetImageGrid(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = GetReadData();
            return data ? data->GetImageGrid(graph, ref) : nullptr;
        }

        VtkImageGridSnapshot GetPrimaryImage() const override
        {
            const auto data = GetReadData();
            return data ? data->GetPrimaryImage() : nullptr;
        }

        VtkLabelMapSnapshot GetLabelMap(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = GetReadData();
            return data ? data->GetLabelMap(graph, ref) : nullptr;
        }

        VtkSurfaceMeshSnapshot GetSurfaceMesh(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = GetReadData();
            return data ? data->GetSurfaceMesh(graph, ref) : nullptr;
        }

        DataEntityId CreateDataEntityId() override
        {
            const auto data = GetWriteData();
            return data ? data->CreateDataEntityId() : DataEntityId{};
        }

        bool SetDataType(DataTypeDescriptor descriptor) override
        {
            const auto data = GetWriteData();
            return data && data->SetDataType(std::move(descriptor));
        }

        DataCommitResult SetDataCommit(DataTransaction transaction) override
        {
            const auto data = GetWriteData();
            if (!data) {
                DataCommitResult result;
                result.message = "Data write requires the Session owner thread.";
                return result;
            }
            const auto state = m_state.lock();
            return state
                ? PrimaryDataActivation(*data, *state).SetDataCommit(std::move(transaction))
                : data->SetDataCommit(std::move(transaction));
        }

        DataObserverId AttachDataChange(
            DataChangeCallback callback) override
        {
            const auto data = GetWriteData();
            if (!data || !callback) return 0;
            const std::weak_ptr<FeatureLifetime> weakLifetime = m_lifetime;
            const auto observer = data->AttachDataChange(
                [weakLifetime, callback = std::move(callback)](
                    const DataChangeSet& change) {
                    const auto lifetime = weakLifetime.lock();
                    if (lifetime && lifetime->isActive.load()) callback(change);
                });
            if (observer == 0) return 0;
            try {
                m_lifetime->observers.push_back(observer);
            }
            catch (...) {
                data->DetachDataChange(observer);
                return 0;
            }
            return observer;
        }

        bool DetachDataChange(const DataObserverId observerId) override
        {
            const auto data = GetWriteData();
            if (!data) return false;
            const auto found = std::find(m_lifetime->observers.begin(),
                m_lifetime->observers.end(), observerId);
            if (found == m_lifetime->observers.end()) return false;
            // 先移除本地记录，闭包析构重入 Detach 时不会重复操作该 ID。
            m_lifetime->observers.erase(found);
            return data->DetachDataChange(observerId);
        }

    private:
        std::shared_ptr<AbstractDataManager> GetReadData() const
        {
            return m_lifetime->isActive.load() ? m_data.lock() : nullptr;
        }

        std::shared_ptr<AbstractDataManager> GetWriteData() const
        {
            if (m_ownerThread == std::thread::id{}
                || m_ownerThread != std::this_thread::get_id()) {
                return {};
            }
            return GetReadData();
        }

        std::weak_ptr<AbstractDataManager> m_data;
        std::weak_ptr<SharedInteractionState> m_state;
        std::thread::id m_ownerThread;
        std::shared_ptr<FeatureLifetime> m_lifetime;
    };

    class FeatureReadPort final : public ImageReadPort {
    public:
        FeatureReadPort(const Ports& ports, std::shared_ptr<FeatureLifetime> lifetime)
            : m_data(ports.data), m_lifetime(std::move(lifetime)) {}
        std::optional<ImageDescriptor> GetImageDescriptor() const override {
            const auto data = m_lifetime->isActive.load() ? m_data.lock() : nullptr;
            return data ? data->GetImageDescriptor() : std::optional<ImageDescriptor>{};
        }
        std::optional<ImageReadState> GetImageReadState() const override {
            const auto data = m_lifetime->isActive.load() ? m_data.lock() : nullptr;
            return data ? data->GetImageReadState() : std::optional<ImageReadState>{};
        }
        ImageReadResult GetImageReadResult(const ImageReadRequest& request) const override {
            const auto data = m_lifetime->isActive.load() ? m_data.lock() : nullptr;
            return data ? data->GetImageReadResult(request, TaskStopToken{}) : ImageReadResult{};
        }
        ImageReadChunkResult GetImageReadChunk(const ImageReadRequest& request,
            std::size_t voxelOffset) const override {
            const auto data = m_lifetime->isActive.load() ? m_data.lock() : nullptr;
            return data ? data->GetImageReadChunk(request, voxelOffset, TaskStopToken{})
                : ImageReadChunkResult{};
        }
    private:
        std::weak_ptr<AbstractDataManager> m_data;
        std::shared_ptr<FeatureLifetime> m_lifetime;
    };

    class FeatureHostControlPort final
        : public FeatureHostControl, public FeatureModelTransformPort {
    public:
        FeatureHostControlPort(
            std::weak_ptr<FeatureHostBridge> bridge,
            std::string featureId,
            std::function<bool(std::function<void()>)> onOwnerComplete,
            std::shared_ptr<FeatureLifetime> lifetime,
            std::weak_ptr<HostWorkSignal> workSignal,
            std::weak_ptr<SharedInteractionState> state)
            : m_bridge(std::move(bridge))
            , m_featureId(std::move(featureId))
            , m_onOwnerComplete(std::move(onOwnerComplete))
            , m_lifetime(std::move(lifetime))
            , m_workSignal(std::move(workSignal))
            , m_state(std::move(state))
        {
        }

        std::optional<ModelTransformSnapshot> GetTransformState() const override
        {
            const auto state = GetStateOwner();
            return state ? std::optional<ModelTransformSnapshot>(
                state->GetTransformState()) : std::nullopt;
        }

        std::optional<std::uint64_t> StartTransform(
            const ModelTransformSnapshot& expected) override
        {
            const auto state = GetStateOwner();
            const auto token = state ? state->StartTransform(m_featureId, expected) : std::nullopt;
            if (token) (void)SendWorkAvailable();
            return token;
        }

        bool SetTransformPreview(std::uint64_t token, std::uint64_t sequence,
            const std::array<double, 16>& matrix) override
        {
            const auto state = GetStateOwner();
            const bool isSet = state && state->SetTransformPreview(
                m_featureId, token, sequence, matrix);
            if (isSet) (void)SendWorkAvailable();
            return isSet;
        }

        bool SetTransformCommit(std::uint64_t token, std::uint64_t sequence,
            const std::array<double, 16>& matrix) override
        {
            const auto state = GetStateOwner();
            const bool isSet = state && state->SetTransformPreview(
                m_featureId, token, sequence, matrix, true);
            if (isSet) (void)SendWorkAvailable();
            return isSet;
        }

        bool StopTransform(std::uint64_t token) override
        {
            const auto state = GetStateOwner();
            const bool isSet = state && state->StopTransform(m_featureId, token);
            if (isSet) (void)SendWorkAvailable();
            return isSet;
        }

        bool SetActiveViews(
            const std::vector<std::string>& viewIds) override
        {
            if (!m_lifetime->isActive.load()) return false;
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->SetActiveViews(m_featureId, viewIds);
        }

        bool SetViewStatus(
            const std::vector<std::string>& viewIds,
            const std::string& status) override
        {
            if (!m_lifetime->isActive.load()) return false;
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->SetViewStatus(viewIds, status);
        }

        bool SendSceneDelta(FeatureSceneDelta delta) override
        {
            if (!m_lifetime || !m_lifetime->isActive.load()) return false;
            for (auto& display : delta.displays) {
                if ((!display.featureId.empty() && display.featureId != m_featureId)
                    || (!display.operation.featureId.empty()
                        && display.operation.featureId != m_featureId)
                    || (display.operation.attachmentId != 0
                        && display.operation.attachmentId != m_lifetime->id)) return false;
                display.featureId = m_featureId;
                display.operation.featureId = m_featureId;
                display.operation.attachmentId = m_lifetime->id;
            }
            const auto bridge = m_bridge.lock();
            const bool isSent = bridge
                && bridge->SendSceneDelta(m_featureId, std::move(delta), m_lifetime);
            if (isSent) (void)SendWorkAvailable();
            return isSent;
        }

        std::uint64_t GetAttachmentId() const noexcept override
        {
            return m_lifetime && m_lifetime->isActive.load() ? m_lifetime->id : 0;
        }

        std::optional<HostSemanticTarget> GetDisplayTarget(
            const std::string& viewId, const std::string& localId) const override
        {
            const auto bridge = m_bridge.lock();
            const auto scene = GetAttachmentId() != 0 && bridge
                ? bridge->GetSceneViewState(viewId) : std::nullopt;
            if (!scene || !scene->isAvailable || scene->sceneEpoch == 0
                || scene->renderedEpoch < scene->sceneEpoch) return std::nullopt;
            for (const auto& display : scene->displays) {
                if (display.featureId == m_featureId && display.localId == localId
                    && display.operation.attachmentId == GetAttachmentId()) {
                    return HostSemanticTarget{ display, {}, scene->sceneEpoch, 0 };
                }
            }
            return std::nullopt;
        }

        bool GetSemanticTargetValid(const HostSemanticTarget& target) const override
        {
            return GetTargetValid(m_bridge, m_lifetime, m_featureId, target);
        }

        bool AttachInput(HostInputBinding binding) override
        {
            if (GetAttachmentId() == 0 || binding.featureId != m_featureId) {
                return false;
            }
            if (binding.getTarget && binding.onTargetInput) {
                binding.onTargetInput = [callback = std::move(binding.onTargetInput),
                    bridge = m_bridge, lifetime = m_lifetime, id = m_featureId](
                    const InteractionEvent& event, const HostSemanticTarget& target) {
                    if (event.eventKind == InteractionEventKind::Cancel)
                        return callback(event, target);
                    if (!GetTargetValid(bridge, lifetime, id, target)) {
                        auto cancel = event;
                        cancel.eventKind = InteractionEventKind::Cancel;
                        (void)callback(cancel, target);
                        return InteractionResult{ true, true, false, InteractionFailureReason::StateRejected };
                    }
                    return callback(event, target);
                };
            }
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->AttachInput(std::move(binding));
        }

        bool DetachInput(
            const std::string_view featureId) override
        {
            if (!m_lifetime->isActive.load()) return false;
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
            if (!m_onOwnerComplete || !complete || !m_lifetime->isActive.load()) return false;
            const auto lifetime = m_lifetime;
            const auto entry = std::make_shared<FeatureCompleteEntry>();
            entry->complete = std::move(complete);
            std::function<void()> guarded = [lifetime, entry]() {
                if (lifetime->isActive.load()) entry->Send();
            };
            bool isSent = false;
            {
                // 私有投递器只入队；完成项接纳与本挂载 Stop 原子决定。
                const std::lock_guard<std::mutex> lock(lifetime->completeMutex);
                if (!lifetime->isActive.load()) return false;
                auto& pending = lifetime->completes;
                pending.erase(std::remove_if(pending.begin(), pending.end(),
                    [](const auto& value) { return value.expired(); }), pending.end());
                pending.push_back(entry);
                isSent = m_onOwnerComplete(std::move(guarded));
            }
            // 宿主工作通知可能执行外部代码，必须在完成队列锁外发出。
            if (isSent) (void)SendWorkAvailable();
            return isSent;
        }

        bool SendWorkAvailable() override
        {
            if (!m_lifetime->isActive.load()) return false;
            const auto signal = m_workSignal.lock();
            return signal && signal->SendWorkAvailable();
        }

    private:
        std::shared_ptr<SharedInteractionState> GetStateOwner() const
        {
            const auto bridge = m_bridge.lock();
            return bridge && bridge->GetIsOwnerActive()
                && m_lifetime && m_lifetime->isActive.load() ? m_state.lock() : nullptr;
        }

        static bool GetTargetValid(const std::weak_ptr<FeatureHostBridge>& weakBridge,
            const std::shared_ptr<FeatureLifetime>& lifetime, const std::string& id,
            const HostSemanticTarget& target)
        {
            if (!lifetime || !lifetime->isActive.load() || target.display.featureId != id
                || target.sceneEpoch == 0 || target.objectId.empty() || target.resultRevision == 0
                || target.display.operation.attachmentId != lifetime->id) return false;
            const auto bridge = weakBridge.lock();
            const auto scene = bridge ? bridge->GetSceneViewState(target.display.viewId) : std::nullopt;
            return scene && scene->isAvailable && target.sceneEpoch <= scene->sceneEpoch
                && std::find(scene->displays.begin(), scene->displays.end(), target.display)
                    != scene->displays.end();
        }
        std::weak_ptr<FeatureHostBridge> m_bridge;
        std::string m_featureId;
        std::function<bool(std::function<void()>)> m_onOwnerComplete;
        std::shared_ptr<FeatureLifetime> m_lifetime;
        std::weak_ptr<HostWorkSignal> m_workSignal;
        std::weak_ptr<SharedInteractionState> m_state;
    };

    static std::atomic<std::uint64_t> s_nextAttachmentId;
    std::optional<std::vector<FeatureOperationState>> GetOperationStates() const;
    bool AttachFeature(const std::shared_ptr<HostFeature>& feature);
    DetachResult DetachFeature(const HostFeature& feature);
    bool DetachFeatures();
    void SendFeatureTicks() noexcept;
    class MutationGuard final {
    public:
        explicit MutationGuard(bool& isChanging) : m_isChanging(isChanging) { m_isChanging = true; }
        ~MutationGuard() { m_isChanging = false; }
    private:
        bool& m_isChanging;
    };
    bool m_isChanging = false;
    Ports m_ports;
    std::vector<FeatureEntry> features;
    std::shared_ptr<FeatureHostBridge> featureBridge;
};

std::atomic<std::uint64_t> HostFeatureRuntime::Impl::s_nextAttachmentId{ 1 };

std::optional<std::vector<FeatureOperationState>> HostFeatureRuntime::Impl::GetOperationStates() const
{
    if (m_isChanging || !featureBridge || m_ports.ownerThread != std::this_thread::get_id())
        return std::nullopt;
    try {
        const auto bridge = featureBridge;
        std::vector<FeatureOperationState> result;
        const auto entries = features;
        for (const auto& entry : entries) {
            if (entry.isHostDetached || !entry.feature || !entry.lifetime
                || !entry.lifetime->isActive.load()) continue;
            auto operations = entry.feature->GetOperationStates();
            if (!entry.lifetime->isActive.load() || bridge != featureBridge) return std::nullopt;
            std::vector<std::uint64_t> requestIds;
            for (auto& state : operations) {
                if (state.operation.requestId == 0 || state.stateRevision == 0
                    || static_cast<unsigned>(state.status) > static_cast<unsigned>(FeatureRunStatus::Stopping)
                    || (!state.operation.featureId.empty() && state.operation.featureId != entry.id)
                    || (state.operation.attachmentId != 0
                        && state.operation.attachmentId != entry.lifetime->id)
                    || !std::isfinite(state.progress) || state.progress < 0.0 || state.progress > 1.0
                    || std::find(requestIds.begin(), requestIds.end(), state.operation.requestId)
                        != requestIds.end()) return std::nullopt;
                std::vector<std::string> roles;
                for (const auto& input : state.inputs) {
                    if (input.role.empty() || !GetDataRevisionRefValid(input.source)
                        || std::find(roles.begin(), roles.end(), input.role) != roles.end()) return std::nullopt;
                    roles.push_back(input.role);
                }
                if (std::any_of(state.outputs.begin(), state.outputs.end(),
                    [](const auto& output) { return !GetDataRevisionRefValid(output); })) return std::nullopt;
                requestIds.push_back(state.operation.requestId);
                state.operation.featureId = entry.id;
                state.operation.attachmentId = entry.lifetime->id;
                result.push_back(std::move(state));
            }
        }
        if (features.size() != entries.size()) return std::nullopt;
        return result;
    }
    catch (...) { return std::nullopt; }
}

void HostFeatureRuntime::Impl::SendFeatureTicks() noexcept
{
    if (m_isChanging) return;
    // 用户 tick 可重入 Detach/Stop；遍历固定批次，并在每次调用前核验仍挂载。
    std::vector<FeatureEntry> tickEntries;
    try { tickEntries = features; }
    catch (...) { return; }
    const auto bridge = featureBridge;
    for (const auto& entry : tickEntries) {
        if (bridge != featureBridge) break;
        const auto current = std::find_if(features.begin(), features.end(),
            [&entry](const FeatureEntry& value) {
                return value.feature == entry.feature && value.id == entry.id
                    && value.lifetime == entry.lifetime;
            });
        if (current == features.end() || current->isHostDetached || !entry.feature) continue;
        try { (void)entry.feature->OnHostTick(); }
        catch (...) {
            try { std::cerr << "[Host] Feature tick failed: " << entry.id << '\n'; }
            catch (...) {}
        }
    }
}

bool HostFeatureRuntime::Impl::AttachFeature(
    const std::shared_ptr<HostFeature>& feature)
{
    if (m_isChanging) return false;
    const MutationGuard mutation(m_isChanging);
    if (!featureBridge
        || m_ports.ownerThread != std::this_thread::get_id()
        || !feature
        || !m_ports.input) {
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
    std::shared_ptr<FeatureLifetime> lifetime;
    auto attachmentId = s_nextAttachmentId.load();
    do {
        if (attachmentId == std::numeric_limits<std::uint64_t>::max()) return false;
    } while (!s_nextAttachmentId.compare_exchange_weak(attachmentId, attachmentId + 1));
    try {
        lifetime = std::make_shared<FeatureLifetime>();
        lifetime->id = attachmentId;
        lifetime->data = m_ports.data;
        context.views =
            std::make_shared<FeatureViewDirectoryPort>(weakBridge, lifetime);
        context.read = std::make_shared<FeatureReadPort>(m_ports, lifetime);
        context.data = std::make_shared<FeatureDataPort>(m_ports, m_ports.ownerThread, lifetime);
        context.host = std::make_shared<FeatureHostControlPort>(
            weakBridge,
            id,
            m_ports.onOwnerComplete, lifetime, m_ports.workSignal, m_ports.state);
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
        lifetime->Stop(false);
        try {
            (void)m_ports.input->DetachInput(id);
        }
        catch (...) {
        }
        try {
            (void)m_ports.views->SetFeatureViews(
                id, {});
        }
        catch (...) {
        }
    };
    try {
        if (!feature->AttachHost(context)) {
            clearRejectedAttach();
            return false;
        }
        const auto insertAt = std::lower_bound(
            features.begin(), features.end(), id,
            [](const FeatureEntry& entry, const std::string& value) {
                return entry.id < value;
            });
        features.insert(insertAt, FeatureEntry{ id, feature, false, lifetime });
    }
    catch (...) {
        clearRejectedAttach();
        return false;
    }
    return true;
}

HostFeatureRuntime::DetachResult HostFeatureRuntime::Impl::DetachFeature(
    const HostFeature& feature)
{
    if (m_isChanging) return DetachResult::Rejected;
    const MutationGuard mutation(m_isChanging);
    if (!featureBridge
        || m_ports.ownerThread != std::this_thread::get_id()) {
        return DetachResult::Rejected;
    }
    const auto entry = std::find_if(
        features.begin(),
        features.end(),
        [&feature](const FeatureEntry& current) {
            const auto& value = current.feature;
            return value && value.get() == &feature;
        });
    if (entry == features.end()) {
        return DetachResult::Rejected;
    }
    if (!entry->isHostDetached) {
        // 先由组合根收回跨视图状态，再允许 Feature 丢弃宿主回调。
        const auto oldViewIds =
            m_ports.views->GetFeatureViewIds(entry->id);
        if (!m_ports.views->SetFeatureViews(entry->id, {})) {
            return DetachResult::Rejected;
        }
        bool isDetached = false;
        try {
            isDetached = const_cast<HostFeature&>(feature).DetachHost();
        }
        catch (...) {
            isDetached = false;
        }
        if (!isDetached) {
            if (!m_ports.views->SetFeatureViews(entry->id, oldViewIds)) {
                std::cerr
                    << "[Host] Feature detach rollback did not restore all views.\n";
                return DetachResult::StopPending;
            }
            return DetachResult::Rejected;
        }
        entry->isHostDetached = true;
        entry->lifetime->Stop(true);
    }
    if (!m_ports.input
        || !m_ports.input->DetachInput(entry->id)) {
        return DetachResult::Rejected;
    }
    features.erase(entry);
    return DetachResult::Detached;
}

bool HostFeatureRuntime::Impl::DetachFeatures()
{
    if (m_isChanging) return false;
    const MutationGuard mutation(m_isChanging);
    while (!features.empty()) {
        auto& entry = features.back();
        if (!entry.isHostDetached) {
            if (!m_ports.views->SetFeatureViews(entry.id, {})) {
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
            entry.isHostDetached = true;
            entry.lifetime->Stop(true);
        }
        if (!m_ports.input
            || !m_ports.input->DetachInput(entry.id)) {
            return false;
        }
        features.pop_back();
    }
    return true;
}

HostFeatureRuntime::HostFeatureRuntime() : m_impl(std::make_unique<Impl>()) {}
HostFeatureRuntime::~HostFeatureRuntime() = default;

bool HostFeatureRuntime::StartOwner(Ports ports)
{
    if (m_impl->featureBridge || !m_impl->features.empty() || !ports.views
        || !ports.input || !ports.frames || ports.data.expired() || ports.state.expired()
        || ports.ownerThread != std::this_thread::get_id()) return false;
    auto bridge = std::make_shared<Impl::FeatureHostBridge>();
    if (!bridge->StartOwner(*ports.views, *ports.input, ports.frames, ports.ownerThread)) return false;
    m_impl->m_ports = std::move(ports);
    m_impl->featureBridge = std::move(bridge);
    return true;
}

bool HostFeatureRuntime::StopOwner()
{
    if (m_impl->m_isChanging || !m_impl->features.empty()) return false;
    if (m_impl->featureBridge && !m_impl->featureBridge->StopOwner()) return false;
    // 旧端口只持有旧 bridge 的 weak_ptr；重建永远分配新 bridge。
    m_impl->featureBridge.reset();
    m_impl->m_ports = {};
    return true;
}
bool HostFeatureRuntime::AttachFeature(const std::shared_ptr<HostFeature>& feature)
{ return m_impl->AttachFeature(feature); }
HostFeatureRuntime::DetachResult HostFeatureRuntime::DetachFeature(const HostFeature& feature)
{ return m_impl->DetachFeature(feature); }
bool HostFeatureRuntime::DetachFeatures() { return m_impl->DetachFeatures(); }
void HostFeatureRuntime::SendFeatureTicks() noexcept { m_impl->SendFeatureTicks(); }
bool HostFeatureRuntime::GetIsEmpty() const noexcept { return m_impl->features.empty(); }
bool HostFeatureRuntime::GetIsChanging() const noexcept { return m_impl->m_isChanging; }

std::optional<std::vector<FeatureOperationState>> HostFeatureRuntime::GetOperationStates() const
{ return m_impl->GetOperationStates(); }
