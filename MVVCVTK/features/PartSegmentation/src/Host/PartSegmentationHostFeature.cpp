#include "Host/PartSegmentationHostFeature.h"

#include "Data/DataPayloads.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Strategies/PartOverlayStrategies.h"
#include "Services/PartSegmentationService.h"

#include <vtkImageData.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "PartSegmentation";
constexpr std::string_view partResultBinding =
    "analysis.parts.active";
const DataTypeId partTableType{
    "org.mvvcvtk.part-segmentation.part-table", 1 };
const DataTypeId partResultSetType{
    "org.mvvcvtk.part-segmentation.result-set", 1 };
const DataFacetId partRecordsFacet{ "tabular-part-records" };

bool GetTargetsUsed(const HostViewTargets& targets)
{
    return !targets.viewIds.empty() || !targets.viewRoles.empty();
}

bool GetRoleSupported(const HostRenderViewRole role)
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

std::shared_ptr<FeatureOverlay> CreateOverlay(
    const HostRenderViewRole role)
{
    if (role == HostRenderViewRole::Primary3D) {
        return std::make_shared<PartSurfaceOverlayStrategy>();
    }
    if (role == HostRenderViewRole::TopDownSlice) {
        return std::make_shared<PartSliceOverlayStrategy>(
            Orientation::Top_down);
    }
    if (role == HostRenderViewRole::FrontBackSlice) {
        return std::make_shared<PartSliceOverlayStrategy>(
            Orientation::Front_back);
    }
    if (role == HostRenderViewRole::LeftRightSlice) {
        return std::make_shared<PartSliceOverlayStrategy>(
            Orientation::Left_right);
    }
    return nullptr;
}

GridGeometry3D GetLabelGeometry(const PartLabelCandidate& candidate)
{
    GridGeometry3D geometry;
    geometry.extent = candidate.extent;
    geometry.dimensions = candidate.dimensions;
    geometry.spacing = candidate.spacing;
    geometry.origin = candidate.origin;
    geometry.direction = candidate.direction;
    return geometry;
}

std::shared_ptr<const RecordTablePayload> CreatePartTable(
    const std::vector<PartRecord>& parts)
{
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> voxelCounts;
    std::vector<double> volumes;
    std::vector<std::array<std::int64_t, 6>> extents;
    std::vector<std::array<double, 6>> bounds;
    std::vector<std::array<double, 3>> centroids;
    std::vector<double> confidence;
    std::vector<std::uint8_t> reviewed;
    std::vector<std::uint8_t> edited;
    ids.reserve(parts.size());
    voxelCounts.reserve(parts.size());
    volumes.reserve(parts.size());
    extents.reserve(parts.size());
    bounds.reserve(parts.size());
    centroids.reserve(parts.size());
    confidence.reserve(parts.size());
    reviewed.reserve(parts.size());
    edited.reserve(parts.size());
    for (const auto& part : parts) {
        ids.push_back(part.partId);
        voxelCounts.push_back(part.voxelCount);
        volumes.push_back(part.physicalVolumeMM3);
        std::array<std::int64_t, 6> extent{};
        std::transform(
            part.voxelExtent.begin(), part.voxelExtent.end(),
            extent.begin(),
            [](const int value) { return static_cast<std::int64_t>(value); });
        extents.push_back(extent);
        bounds.push_back(part.worldBounds);
        centroids.push_back(part.centroidWorld);
        confidence.push_back(part.confidence.value_or(
            std::numeric_limits<double>::quiet_NaN()));
        reviewed.push_back(part.isReviewed ? 1U : 0U);
        edited.push_back(part.isEdited ? 1U : 0U);
    }
    auto table = std::make_shared<const RecordTablePayload>(
        partTableType,
        "part-segmentation.parts",
        std::vector<RecordColumn>{
            { "part-id", std::move(ids) },
            { "voxel-count", std::move(voxelCounts) },
            { "physical-volume-mm3", std::move(volumes) },
            { "voxel-extent", std::move(extents) },
            { "world-bounds", std::move(bounds) },
            { "centroid-world", std::move(centroids) },
            { "confidence", std::move(confidence) },
            { "is-reviewed", std::move(reviewed) },
            { "is-edited", std::move(edited) } });
    return table->GetValid() ? table : nullptr;
}

PartSegmentationResult BuildResult(
    const PartSegmentationState& state,
    const std::uint64_t requestId,
    const PartResultStatus status,
    const PartFailureReason failureReason,
    const std::size_t partCount,
    std::string message)
{
    PartSegmentationResult result;
    result.requestId = requestId;
    result.status = status;
    result.failureReason = failureReason;
    result.commitId = state.commitId;
    result.sourceRevision = state.sourceRevision;
    result.labelMap = state.labelMap;
    result.partTable = state.partTable;
    result.resultSet = state.resultSet;
    result.partCount = partCount;
    result.message = std::move(message);
    return result;
}

} // namespace

class PartSegmentationHostFeature::Impl final {
public:
    explicit Impl(PartSegmentationConfig value)
        : m_config(std::move(value))
    {
        m_state.isOverlayVisible = m_config.isOverlayVisible;
    }

    bool AttachHost(const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    PartSegmentationAdmission SendRequest(
        PartSegmentationRequest request,
        PartSegmentationCallback onComplete);
    PartSegmentationState GetState() const;

private:
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureOverlay> overlay;
        std::string viewId;
    };

    bool GetIsOwnerThread() const noexcept;
    std::uint64_t GetNextRequestId() noexcept;
    std::vector<HostFeatureView> GetTargetViews(
        const HostViewTargets& targets) const;
    bool GetSourceSame(const VtkImageGridSnapshot& source) const;
    std::optional<DataBinding> GetResultBinding(
        const DataGraphSnapshot& graph) const;
    bool SetDataTypes();
    void SetState(PartSegmentationState state);
    void SetRequestRunning(
        std::uint64_t requestId,
        const DataRevisionRef& source);
    void SetRequestProgress(double progress);
    void SendComplete(
        PartSegmentationCallback callback,
        PartSegmentationResult result) const noexcept;
    bool AttachDisplay(
        vtkImageData* labelImage,
        const std::vector<HostFeatureView>& views,
        std::vector<OverlayBinding>& nextBindings);
    bool RemoveDisplay();
    static void RemoveBindings(
        std::vector<OverlayBinding>& bindings) noexcept;
    bool SetVisibility(bool isVisible);
    bool ClearResult();
    void SetSourceStale();
    void SetRequestComplete(PartLabelCandidate candidate);
    void SetRequestFailed(PartFailureReason reason);

    PartSegmentationConfig m_config;
    mutable std::mutex m_stateMutex;
    PartSegmentationState m_state;
    PartSegmentationState m_stateBeforeRequest;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<PartSegmentationService> m_service;
    VtkImageGridSnapshot m_requestSource;
    DataBinding m_requestResultBinding;
    VtkLabelMapSnapshot m_activeLabels;
    std::vector<HostFeatureView> m_requestViews;
    std::vector<HostFeatureView> m_activeViews;
    std::vector<OverlayBinding> m_bindings;
    PartSegmentationCallback m_startCallback;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextRequestId = 1;
    std::uint64_t m_activeRequestId = 0;
    bool m_isAttached = false;
    bool m_isSourceChanged = false;
    bool m_isStopRequested = false;
    bool m_isActiveViewClearPending = false;
};

bool PartSegmentationHostFeature::Impl::AttachHost(
    const HostFeatureContext& context)
{
    if (m_isAttached || !context.views || !context.data || !context.host
        || m_config.maxWorkingBytes == 0
        || !std::isfinite(m_config.defaultStart.threshold)
        || m_config.defaultStart.minPartVoxels == 0) {
        return false;
    }
    m_data = context.data;
    if (!SetDataTypes()) {
        m_data.reset();
        return false;
    }
    try {
        m_service = std::make_unique<PartSegmentationService>();
    }
    catch (...) {
        m_data.reset();
        return false;
    }
    m_views = context.views;
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
    m_isActiveViewClearPending = false;
    m_isAttached = true;
    return true;
}

bool PartSegmentationHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) return true;
    if (!GetIsOwnerThread() || !m_service) return false;
    m_isStopRequested = m_activeRequestId != 0;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.status = PartSegmentationStatus::Stopping;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!m_service->Stop(deadline)) return false;
    if (m_startCallback) {
        const auto state = GetState();
        SendComplete(
            std::move(m_startCallback),
            BuildResult(
                state,
                m_activeRequestId,
                PartResultStatus::Cancelled,
                PartFailureReason::Cancelled,
                0,
                "Part request was cancelled by detach."));
    }
    if (!ClearResult()) return false;

    m_service.reset();
    m_requestSource.reset();
    m_activeLabels.reset();
    m_requestViews.clear();
    m_activeViews.clear();
    m_startCallback = nullptr;
    m_activeRequestId = 0;
    m_isSourceChanged = false;
    m_isStopRequested = false;
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
    m_isAttached = false;
    PartSegmentationState idle;
    idle.isOverlayVisible = m_config.isOverlayVisible;
    SetState(std::move(idle));
    return true;
}

bool PartSegmentationHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached || !GetIsOwnerThread() || !m_service || !m_data) {
        return false;
    }
    const auto state = GetState();
    if (GetDataRevisionRefValid(state.resultSet)) {
        const auto graph = m_data->GetDataGraph();
        if (m_data->GetDataRelation(
                graph,
                state.resultSet,
                "source-volume",
                primaryVolumeBinding)
            == DataRelationStatus::OutOfDateRelativeToCurrentBinding) {
            SetSourceStale();
        }
    }
    if (m_requestSource && !GetSourceSame(m_requestSource)) {
        m_isSourceChanged = true;
        m_service->StopRequest();
    }
    if (m_activeRequestId != 0) {
        const auto progress = m_service->GetProgress(m_activeRequestId);
        if (progress) SetRequestProgress(*progress);
    }
    auto complete = m_service->GetComplete();
    if (!complete || complete->requestId != m_activeRequestId) return true;
    SetRequestComplete(std::move(*complete));
    return true;
}

PartSegmentationAdmission PartSegmentationHostFeature::Impl::SendRequest(
    PartSegmentationRequest request,
    PartSegmentationCallback onComplete)
{
    PartSegmentationAdmission admission;
    if (!m_isAttached || !GetIsOwnerThread() || !m_service || !m_data) {
        admission.status = PartAdmissionStatus::Unavailable;
        return admission;
    }
    if (request.action == PartSegmentationAction::Start) {
        if (m_service->GetIsBusy() || m_activeRequestId != 0) {
            admission.status = PartAdmissionStatus::Busy;
            return admission;
        }
        const auto params = request.start.value_or(m_config.defaultStart);
        if (!std::isfinite(params.threshold)
            || params.minPartVoxels == 0
            || !GetTargetsUsed(params.targetViews)) {
            return admission;
        }
        auto targetViews = GetTargetViews(params.targetViews);
        if (targetViews.empty()) return admission;
        auto source = m_data->GetPrimaryImage();
        if (!source || !source->image || !source->data || !source->binding) {
            admission.status = PartAdmissionStatus::Unavailable;
            return admission;
        }
        const auto requestId = GetNextRequestId();
        const auto status = m_service->Start(
            source, params, m_config.maxWorkingBytes, requestId);
        admission.status = status;
        if (status != PartAdmissionStatus::Accepted) return admission;
        admission.requestId = requestId;
        m_requestResultBinding = GetResultBinding(source->graph).value_or(
            DataBinding{ std::string(partResultBinding), {}, 0 });
        m_requestSource = std::move(source);
        m_requestViews = std::move(targetViews);
        m_startCallback = std::move(onComplete);
        m_activeRequestId = requestId;
        m_isSourceChanged = false;
        m_isStopRequested = false;
        SetRequestRunning(requestId, m_requestSource->data->self);
        return admission;
    }

    if (request.action == PartSegmentationAction::Stop) {
        const auto requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        m_isStopRequested = m_activeRequestId != 0;
        m_service->StopRequest();
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                state,
                requestId,
                PartResultStatus::Succeeded,
                PartFailureReason::None,
                0,
                "Part stop was requested."));
        return admission;
    }
    if (m_service->GetIsBusy() || m_activeRequestId != 0) {
        admission.status = PartAdmissionStatus::Busy;
        return admission;
    }
    if (request.action == PartSegmentationAction::SetVisibility
        && request.isVisible) {
        const auto requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        const bool isSucceeded = SetVisibility(*request.isVisible);
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                state,
                requestId,
                isSucceeded ? PartResultStatus::Succeeded
                    : PartResultStatus::Failed,
                isSucceeded ? PartFailureReason::None
                    : PartFailureReason::DisplayFailed,
                0,
                isSucceeded ? "Part visibility was updated."
                    : "Part visibility update failed."));
        return admission;
    }
    if (request.action == PartSegmentationAction::Clear) {
        const auto requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        const bool isSucceeded = ClearResult();
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                state,
                requestId,
                isSucceeded ? PartResultStatus::Succeeded
                    : PartResultStatus::Failed,
                isSucceeded ? PartFailureReason::None
                    : PartFailureReason::DisplayFailed,
                0,
                isSucceeded ? "Part result was cleared."
                    : "Part result clear failed."));
        return admission;
    }
    return admission;
}

PartSegmentationState PartSegmentationHostFeature::Impl::GetState() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state;
}

bool PartSegmentationHostFeature::Impl::GetIsOwnerThread() const noexcept
{
    return m_ownerThread != std::thread::id{}
        && m_ownerThread == std::this_thread::get_id();
}

std::uint64_t PartSegmentationHostFeature::Impl::GetNextRequestId() noexcept
{
    const auto requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    return requestId == 0 ? GetNextRequestId() : requestId;
}

std::vector<HostFeatureView>
PartSegmentationHostFeature::Impl::GetTargetViews(
    const HostViewTargets& targets) const
{
    if (!m_views) return {};
    auto views = m_views->GetViews(targets);
    if (views.empty()) return {};
    for (const auto& view : views) {
        if (view.id.empty() || !GetRoleSupported(view.role)) return {};
    }
    return views;
}

bool PartSegmentationHostFeature::Impl::GetSourceSame(
    const VtkImageGridSnapshot& source) const
{
    if (!m_data || !source || !source->data || !source->binding) return false;
    const auto current = m_data->GetPrimaryImage();
    return current && current->data && current->binding
        && current->data->self == source->data->self
        && current->binding->revision == source->binding->revision;
}

std::optional<DataBinding>
PartSegmentationHostFeature::Impl::GetResultBinding(
    const DataGraphSnapshot& graph) const
{
    return m_data
        ? m_data->GetDataBinding(graph, partResultBinding)
        : std::optional<DataBinding>{};
}

bool PartSegmentationHostFeature::Impl::SetDataTypes()
{
    if (!m_data) return false;
    auto graph = m_data->GetDataGraph();
    const auto hasType = [&graph](const DataTypeId& type) {
        return graph.view && !graph.view->GetDataFacets(type).empty();
    };
    const bool hasTable = hasType(partTableType)
        || m_data->SetDataType(GetRecordTableDescriptor(
            partTableType,
            { DataFacets::tabularRecords, partRecordsFacet }));
    graph = m_data->GetDataGraph();
    const bool hasResult = hasType(partResultSetType)
        || m_data->SetDataType(GetDataCollectionDescriptor(
            partResultSetType));
    return hasTable && hasResult;
}

void PartSegmentationHostFeature::Impl::SetState(
    PartSegmentationState state)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = std::move(state);
}

void PartSegmentationHostFeature::Impl::SetRequestRunning(
    const std::uint64_t requestId,
    const DataRevisionRef& source)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stateBeforeRequest = m_state;
    m_state.status = PartSegmentationStatus::Running;
    m_state.failureReason = PartFailureReason::None;
    m_state.requestId = requestId;
    m_state.sourceRevision = source;
    m_state.progress = 0.0;
}

void PartSegmentationHostFeature::Impl::SetRequestProgress(
    const double progress)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_state.status != PartSegmentationStatus::Running
        || m_state.requestId != m_activeRequestId) {
        return;
    }
    constexpr double runningLimit = 0.999;
    m_state.progress = std::max(
        m_state.progress, std::clamp(progress, 0.0, runningLimit));
}

void PartSegmentationHostFeature::Impl::SendComplete(
    PartSegmentationCallback callback,
    PartSegmentationResult result) const noexcept
{
    if (!callback) return;
    try {
        callback(std::move(result));
    }
    catch (...) {
    }
}

bool PartSegmentationHostFeature::Impl::AttachDisplay(
    vtkImageData* labelImage,
    const std::vector<HostFeatureView>& views,
    std::vector<OverlayBinding>& nextBindings)
{
    if (!m_views || !m_host || !labelImage || views.empty()) return false;
    std::vector<std::string> viewIds;
    viewIds.reserve(views.size());
    nextBindings.reserve(views.size());
    for (const auto& view : views) {
        auto service = m_views->GetOverlayPort(view.id);
        auto overlay = CreateOverlay(view.role);
        if (!service || !overlay) {
            RemoveBindings(nextBindings);
            return false;
        }
        overlay->SetInputData(labelImage);
        if (!service->AttachOverlay(overlay)) {
            RemoveBindings(nextBindings);
            return false;
        }
        nextBindings.push_back({ service, overlay, view.id });
        viewIds.push_back(view.id);
    }
    if (!m_host->SetActiveViews(viewIds)) {
        m_isActiveViewClearPending = true;
        RemoveBindings(nextBindings);
        return false;
    }
    m_isActiveViewClearPending = false;
    return true;
}

bool PartSegmentationHostFeature::Impl::RemoveDisplay()
{
    const bool needsActiveViewClear =
        !m_bindings.empty() || m_isActiveViewClearPending;
    const bool isActiveViewCleared = !needsActiveViewClear
        || (m_host && m_host->SetActiveViews({}));
    m_isActiveViewClearPending = !isActiveViewCleared;
    // Host 元数据同步失败也不能让已退休的数据继续留在画面上；
    // pending 标记保留下一次 tick/detach 的重试能力。
    RemoveBindings(m_bindings);
    return isActiveViewCleared;
}

void PartSegmentationHostFeature::Impl::RemoveBindings(
    std::vector<OverlayBinding>& bindings) noexcept
{
    for (auto binding = bindings.rbegin();
        binding != bindings.rend(); ++binding) {
        if (binding->service && binding->overlay) {
            binding->service->RemoveOverlay(binding->overlay);
        }
    }
    bindings.clear();
}

bool PartSegmentationHostFeature::Impl::SetVisibility(
    const bool isVisible)
{
    auto state = GetState();
    if (state.isOverlayVisible == isVisible) {
        return !isVisible && m_isActiveViewClearPending
            ? RemoveDisplay() : true;
    }
    if (!isVisible) {
        const bool isDisplayRemoved = RemoveDisplay();
        state.isOverlayVisible = false;
        SetState(std::move(state));
        return isDisplayRemoved;
    }
    if (!GetDataRevisionRefValid(state.labelMap) || m_activeViews.empty()) {
        state.isOverlayVisible = true;
        SetState(std::move(state));
        return true;
    }
    const auto graph = m_data->GetDataGraph();
    auto labels = m_data->GetLabelMap(graph, state.labelMap);
    if (!labels || !labels->labels) return false;
    std::vector<OverlayBinding> nextBindings;
    if (!AttachDisplay(labels->labels, m_activeViews, nextBindings)) {
        return false;
    }
    RemoveBindings(m_bindings);
    m_bindings = std::move(nextBindings);
    m_activeLabels = std::move(labels);
    state.isOverlayVisible = true;
    SetState(std::move(state));
    return true;
}

bool PartSegmentationHostFeature::Impl::ClearResult()
{
    if (m_data) {
        const auto graph = m_data->GetDataGraph();
        const auto binding = GetResultBinding(graph);
        if (binding && binding->target) {
            DataTransaction transaction;
            transaction.bindings.push_back(DataBindingUpdate{
                std::string(partResultBinding),
                binding->revision,
                true,
                binding->target,
                {} });
            if (m_data->SetDataCommit(std::move(transaction)).status
                != DataCommitStatus::Succeeded) {
                return false;
            }
        }
    }
    const bool isDisplayRemoved = RemoveDisplay();
    m_activeLabels.reset();
    m_activeViews.clear();
    PartSegmentationState state;
    state.isOverlayVisible = GetState().isOverlayVisible;
    SetState(std::move(state));
    return isDisplayRemoved;
}

void PartSegmentationHostFeature::Impl::SetSourceStale()
{
    auto state = GetState();
    if (!GetDataRevisionRefValid(state.resultSet)
        && !m_requestSource) {
        return;
    }
    state.status = PartSegmentationStatus::Stale;
    state.failureReason = PartFailureReason::SourceChanged;
    state.progress = 0.0;
    SetState(std::move(state));
    (void)RemoveDisplay();
    m_activeLabels.reset();
    m_activeViews.clear();
}

void PartSegmentationHostFeature::Impl::SetRequestComplete(
    PartLabelCandidate candidate)
{
    auto callback = std::move(m_startCallback);
    const auto requestId = m_activeRequestId;
    if (m_isSourceChanged || !GetSourceSame(m_requestSource)) {
        SetRequestFailed(PartFailureReason::SourceChanged);
        SendComplete(
            std::move(callback),
            BuildResult(
                GetState(),
                requestId,
                PartResultStatus::Failed,
                PartFailureReason::SourceChanged,
                0,
                "Part source changed before commit."));
    }
    else if (m_isStopRequested) {
        SetRequestFailed(PartFailureReason::Cancelled);
        SendComplete(
            std::move(callback),
            BuildResult(
                GetState(),
                requestId,
                PartResultStatus::Cancelled,
                PartFailureReason::Cancelled,
                0,
                "Part request was cancelled."));
    }
    else if (candidate.status != PartResultStatus::Succeeded) {
        SetRequestFailed(candidate.failureReason);
        SendComplete(
            std::move(callback),
            BuildResult(
                GetState(),
                requestId,
                candidate.status,
                candidate.failureReason,
                0,
                candidate.message));
    }
    else {
        const auto labelEntity = m_data->CreateDataEntityId();
        const auto tableEntity = m_data->CreateDataEntityId();
        const auto resultEntity = m_data->CreateDataEntityId();
        const DataRevisionRef labelRef{ labelEntity, 1 };
        const DataRevisionRef tableRef{ tableEntity, 1 };
        const DataRevisionRef resultRef{ resultEntity, 1 };
        auto labels = std::make_shared<const LabelMap3DPayload>(
            GetLabelGeometry(candidate), candidate.labels);
        auto table = CreatePartTable(candidate.parts);
        auto collection = std::make_shared<const DataCollectionPayload>(
            partResultSetType,
            std::vector<DataCollectionEntry>{
                { "labels", labelRef },
                { "parts", tableRef } });
        const auto sourceRef = m_requestSource->data->self;
        DataExpectation sourceExpectation;
        sourceExpectation.kind = DataExpectationKind::Binding;
        sourceExpectation.binding = std::string(primaryVolumeBinding);
        sourceExpectation.expectedBindingRevision =
            m_requestSource->binding->revision;
        sourceExpectation.isTargetChecked = true;
        sourceExpectation.expectedTarget = sourceRef;
        DataTransaction transaction;
        transaction.expectations.push_back(std::move(sourceExpectation));
        transaction.outputs = {
            DataRevisionDraft{
                labelEntity, 0, DataTypes::labelMap3D,
                { DataInputRef{ "source-volume", sourceRef } },
                std::move(labels),
                DataProvenance{
                    std::string(featureId), "segment-labels", "1", "{}" } },
            DataRevisionDraft{
                tableEntity, 0, partTableType,
                { DataInputRef{ "source-volume", sourceRef },
                  DataInputRef{ "labels", labelRef } },
                std::move(table),
                DataProvenance{
                    std::string(featureId), "build-part-table", "1", "{}" } },
            DataRevisionDraft{
                resultEntity, 0, partResultSetType,
                { DataInputRef{ "source-volume", sourceRef },
                  DataInputRef{ "labels", labelRef },
                  DataInputRef{ "parts", tableRef } },
                std::move(collection),
                DataProvenance{
                    std::string(featureId), "collect-results", "1", "{}" } }
        };
        transaction.bindings.push_back(DataBindingUpdate{
            std::string(partResultBinding),
            m_requestResultBinding.revision,
            true,
            m_requestResultBinding.target,
            resultRef });
        const auto commit = m_data->SetDataCommit(std::move(transaction));
        if (commit.status != DataCommitStatus::Succeeded) {
            const auto reason = GetSourceSame(m_requestSource)
                ? PartFailureReason::InternalError
                : PartFailureReason::SourceChanged;
            SetRequestFailed(reason);
            SendComplete(
                std::move(callback),
                BuildResult(
                    GetState(),
                    requestId,
                    PartResultStatus::Failed,
                    reason,
                    0,
                    "Part data transaction was rejected."));
        }
        else {
            PartSegmentationState state;
            state.status = PartSegmentationStatus::Succeeded;
            state.failureReason = PartFailureReason::None;
            state.requestId = requestId;
            state.commitId = commit.commitId;
            state.sourceRevision = sourceRef;
            state.labelMap = labelRef;
            state.partTable = tableRef;
            state.resultSet = resultRef;
            state.progress = 1.0;
            state.isOverlayVisible = GetState().isOverlayVisible;
            const auto graph = m_data->GetDataGraph();
            auto labelView = m_data->GetLabelMap(graph, labelRef);
            m_activeViews = std::move(m_requestViews);
            m_activeLabels = labelView;
            bool isDisplayed = true;
            std::vector<OverlayBinding> nextBindings;
            if (state.isOverlayVisible) {
                isDisplayed = labelView && labelView->labels
                    && AttachDisplay(
                        labelView->labels,
                        m_activeViews,
                        nextBindings);
            }
            if (isDisplayed) {
                RemoveBindings(m_bindings);
                m_bindings = std::move(nextBindings);
            }
            else {
                RemoveBindings(nextBindings);
                (void)RemoveDisplay();
                state.failureReason = PartFailureReason::DisplayFailed;
            }
            SetState(state);
            SendComplete(
                std::move(callback),
                BuildResult(
                    state,
                    requestId,
                    isDisplayed
                        ? PartResultStatus::Succeeded
                        : PartResultStatus::SucceededWithDisplayFailure,
                    state.failureReason,
                    candidate.parts.size(),
                    isDisplayed
                        ? candidate.message
                        : "Part data committed; display attach failed."));
        }
    }

    m_requestSource.reset();
    m_requestViews.clear();
    m_activeRequestId = 0;
    m_isSourceChanged = false;
    m_isStopRequested = false;
}

void PartSegmentationHostFeature::Impl::SetRequestFailed(
    const PartFailureReason reason)
{
    if (reason == PartFailureReason::SourceChanged) {
        SetSourceStale();
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!GetDataRevisionRefValid(m_stateBeforeRequest.resultSet)) {
        m_state.status = reason == PartFailureReason::Cancelled
            ? PartSegmentationStatus::Cancelled
            : PartSegmentationStatus::Failed;
        m_state.failureReason = reason;
        m_state.requestId = m_activeRequestId;
        m_state.sourceRevision = m_requestSource && m_requestSource->data
            ? m_requestSource->data->self : DataRevisionRef{};
        m_state.progress = 0.0;
        return;
    }
    m_state = m_stateBeforeRequest;
}

PartSegmentationHostFeature::PartSegmentationHostFeature(
    PartSegmentationConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

PartSegmentationHostFeature::~PartSegmentationHostFeature() noexcept = default;

std::string_view PartSegmentationHostFeature::GetFeatureId() const noexcept
{
    return featureId;
}

FeatureDataContract PartSegmentationHostFeature::GetDataContract() const
{
    return FeatureDataContract{
        { DataInputSpec{ "source-volume", DataFacets::scalarGrid3D, true } },
        { DataOutputSpec{
              "labels", DataTypes::labelMap3D,
              { DataFacets::labelMap3D } },
          DataOutputSpec{
              "parts", partTableType,
              { DataFacets::tabularRecords, partRecordsFacet } },
          DataOutputSpec{
              "result-set", partResultSetType,
              { DataFacets::dataCollection } } }
    };
}

bool PartSegmentationHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl && m_impl->AttachHost(context);
}

bool PartSegmentationHostFeature::DetachHost()
{
    return !m_impl || m_impl->DetachHost();
}

bool PartSegmentationHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

PartSegmentationAdmission PartSegmentationHostFeature::SendRequest(
    PartSegmentationRequest request,
    PartSegmentationCallback onComplete)
{
    return m_impl
        ? m_impl->SendRequest(
            std::move(request), std::move(onComplete))
        : PartSegmentationAdmission{
            PartAdmissionStatus::Unavailable, 0 };
}

PartSegmentationState PartSegmentationHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : PartSegmentationState{};
}
