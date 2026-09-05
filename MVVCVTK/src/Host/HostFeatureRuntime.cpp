#include "Host/Internal/HostFeatureRuntime.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "Host/HostFrameCoordinator.h"
#include "App/Services/PrimaryDataActivation.h"
#include "App/AppState.h"
#include "Data/DataService.h"
#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class HostFeatureRuntime::Impl final {
public:
    struct FeatureEntry final {
        std::string id;
        // attached Feature 属于 Session aggregate；只有 Detach/Stop 成功后才释放，
        // 从而保证 Feature 内的 VTK 绑定始终在 owner thread 上确定性清理。
        std::shared_ptr<HostFeature> feature;
        // DetachHost 成功后单调置位；后置 input 门禁失败时不重放 Feature teardown。
        bool isHostDetached = false;
    };

    // Feature 只持有窄能力对象；具体 RuntimeRegistry/HotkeyRouter 只在组合根内部可见，
    // StopOwner 后所有跨层调用稳定返回失败。
    class FeatureHostBridge final {
    public:
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
            FeatureSceneDelta delta)
        {
            std::shared_ptr<HostFrameCoordinator> frames;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_isActive) return false;
                frames = m_frames.lock();
            }
            return frames
                && frames->Enqueue(featureId, std::move(delta));
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
        : public TrustedDataPort {
    public:
        FeatureDataPort(
            const Ports& ports,
            const std::thread::id ownerThread)
            : m_data(ports.data)
            , m_state(ports.state)
            , m_ownerThread(ownerThread)
        {
        }

        DataGraphSnapshot GetDataGraph() const override
        {
            const auto data = m_data.lock();
            return data ? data->GetDataGraph() : DataGraphSnapshot{};
        }

        DataSnapshot GetData(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = m_data.lock();
            return data ? data->GetData(graph, ref) : DataSnapshot{};
        }

        DataQueryResult GetDataQuery(
            const DataGraphSnapshot& graph,
            const DataQuery& query) const override
        {
            const auto data = m_data.lock();
            return data
                ? data->GetDataQuery(graph, query) : DataQueryResult{};
        }

        std::optional<DataBinding> GetDataBinding(
            const DataGraphSnapshot& graph,
            const std::string_view name) const override
        {
            const auto data = m_data.lock();
            return data
                ? data->GetDataBinding(graph, name)
                : std::optional<DataBinding>{};
        }

        ProjectDataSnapshot GetProjectData() const override
        {
            const auto data = m_data.lock();
            return data ? data->GetProjectData() : ProjectDataSnapshot{};
        }

        DataRelationStatus GetDataRelation(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref,
            const std::string_view inputRole,
            const std::string_view binding) const override
        {
            const auto data = m_data.lock();
            return data
                ? data->GetDataRelation(graph, ref, inputRole, binding)
                : DataRelationStatus::Unknown;
        }

        VtkImageGridSnapshot GetImageGrid(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = m_data.lock();
            return data ? data->GetImageGrid(graph, ref) : nullptr;
        }

        VtkImageGridSnapshot GetPrimaryImage() const override
        {
            const auto data = m_data.lock();
            return data ? data->GetPrimaryImage() : nullptr;
        }

        VtkLabelMapSnapshot GetLabelMap(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = m_data.lock();
            return data ? data->GetLabelMap(graph, ref) : nullptr;
        }

        VtkSurfaceMeshSnapshot GetSurfaceMesh(
            const DataGraphSnapshot& graph,
            const DataRevisionRef& ref) const override
        {
            const auto data = m_data.lock();
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
            return data
                ? data->AttachDataChange(std::move(callback)) : 0;
        }

        bool DetachDataChange(const DataObserverId observerId) override
        {
            const auto data = GetWriteData();
            return data && data->DetachDataChange(observerId);
        }

    private:
        std::shared_ptr<AbstractDataManager> GetWriteData() const
        {
            if (m_ownerThread == std::thread::id{}
                || m_ownerThread != std::this_thread::get_id()) {
                return {};
            }
            return m_data.lock();
        }

        std::weak_ptr<AbstractDataManager> m_data;
        std::weak_ptr<SharedInteractionState> m_state;
        std::thread::id m_ownerThread;
    };

    class FeatureReadPort final : public ImageReadPort {
    public:
        explicit FeatureReadPort(const Ports& ports) : m_data(ports.data) {}
        std::optional<ImageDescriptor> GetImageDescriptor() const override {
            const auto data = m_data.lock();
            return data ? data->GetImageDescriptor() : std::optional<ImageDescriptor>{};
        }
        std::optional<ImageReadState> GetImageReadState() const override {
            const auto data = m_data.lock();
            return data ? data->GetImageReadState() : std::optional<ImageReadState>{};
        }
        ImageReadResult GetImageReadResult(const ImageReadRequest& request) const override {
            const auto data = m_data.lock();
            return data ? data->GetImageReadResult(request, TaskStopToken{}) : ImageReadResult{};
        }
        ImageReadChunkResult GetImageReadChunk(const ImageReadRequest& request,
            std::size_t voxelOffset) const override {
            const auto data = m_data.lock();
            return data ? data->GetImageReadChunk(request, voxelOffset, TaskStopToken{})
                : ImageReadChunkResult{};
        }
    private:
        std::weak_ptr<AbstractDataManager> m_data;
    };

    class FeatureHostControlPort final
        : public FeatureHostControl {
    public:
        FeatureHostControlPort(
            std::weak_ptr<FeatureHostBridge> bridge,
            std::string featureId,
            std::function<bool(std::function<void()>)> onOwnerComplete)
            : m_bridge(std::move(bridge))
            , m_featureId(std::move(featureId))
            , m_onOwnerComplete(std::move(onOwnerComplete))
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

        bool SendSceneDelta(FeatureSceneDelta delta) override
        {
            const auto bridge = m_bridge.lock();
            return bridge
                && bridge->SendSceneDelta(
                    m_featureId, std::move(delta));
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
            return m_onOwnerComplete && m_onOwnerComplete(std::move(complete));
        }

    private:
        std::weak_ptr<FeatureHostBridge> m_bridge;
        std::string m_featureId;
        std::function<bool(std::function<void()>)> m_onOwnerComplete;
    };

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

void HostFeatureRuntime::Impl::SendFeatureTicks() noexcept
{
    // 用户 tick 可重入 Detach/Stop；遍历固定批次，并在每次调用前核验仍挂载。
    std::vector<FeatureEntry> tickEntries;
    try { tickEntries = features; }
    catch (...) { return; }
    const auto bridge = featureBridge;
    for (const auto& entry : tickEntries) {
        if (bridge != featureBridge) break;
        const auto current = std::find_if(features.begin(), features.end(),
            [&entry](const FeatureEntry& value) {
                return value.feature == entry.feature && value.id == entry.id;
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
    try {
        context.views =
            std::make_shared<FeatureViewDirectoryPort>(weakBridge);
        context.read = std::make_shared<FeatureReadPort>(m_ports);
        context.data = std::make_shared<FeatureDataPort>(m_ports, m_ports.ownerThread);
        context.host = std::make_shared<FeatureHostControlPort>(
            weakBridge,
            id,
            m_ports.onOwnerComplete);
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
        features.insert(insertAt, FeatureEntry{ id, feature, false });
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
