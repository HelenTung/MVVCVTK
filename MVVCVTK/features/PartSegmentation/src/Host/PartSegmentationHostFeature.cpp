#include "Host/PartSegmentationHostFeature.h"

#include "App/Services/FeatureViewService.h"
#include "Render/Strategies/PartOverlayStrategies.h"
#include "Render/PartRenderStateTable.h"
#include "Render/Contracts/OverlayService.h"
#include "Model/PartCatalog.h"
#include "Data/DataPayloads.h"
#include "Services/PartSegmentationService.h"

#include <vtkImageData.h>
#include <vtkMatrix3x3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
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
const DataTypeId partCatalogType{
    "org.mvvcvtk.part-segmentation.catalog", 1 };
const DataFacetId partCatalogFacet{ "stable-part-catalog" };

// 目录仅由图中的不可变修订持有；Feature 缓存的是该载荷的只读投影。
class PartCatalogPayload final : public IDataPayload {
public:
    explicit PartCatalogPayload(const PartCatalog& catalog)
        : m_catalog(std::make_shared<const PartCatalog>(catalog)) {}
    DataTypeId GetDataType() const override { return partCatalogType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        return std::make_shared<const PartCatalogPayload>(*this);
    }
    const std::shared_ptr<const PartCatalog>& GetCatalog() const noexcept
    {
        return m_catalog;
    }
private:
    std::shared_ptr<const PartCatalog> m_catalog;
};

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

bool GetImageGeometrySame(
    vtkImageData& left,
    vtkImageData& right)
{
    int leftExtent[6]{};
    int rightExtent[6]{};
    double leftSpacing[3]{};
    double rightSpacing[3]{};
    double leftOrigin[3]{};
    double rightOrigin[3]{};
    left.GetExtent(leftExtent);
    right.GetExtent(rightExtent);
    left.GetSpacing(leftSpacing);
    right.GetSpacing(rightSpacing);
    left.GetOrigin(leftOrigin);
    right.GetOrigin(rightOrigin);
    if (!std::equal(leftExtent, leftExtent + 6, rightExtent)
        || !std::equal(leftSpacing, leftSpacing + 3, rightSpacing)
        || !std::equal(leftOrigin, leftOrigin + 3, rightOrigin)) {
        return false;
    }
    auto* leftDirection = left.GetDirectionMatrix();
    auto* rightDirection = right.GetDirectionMatrix();
    if (!leftDirection || !rightDirection) return false;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            if (leftDirection->GetElement(row, column)
                != rightDirection->GetElement(row, column)) {
                return false;
            }
        }
    }
    return true;
}

struct PartOverlayCandidate final {
    std::shared_ptr<FeatureOverlay> overlay;
    std::shared_ptr<PartOverlayControl> control;
};

PartOverlayCandidate CreateOverlay(
    const HostRenderViewRole role)
{
    if (role == HostRenderViewRole::Primary3D) {
        auto overlay = std::make_shared<PartSurfaceOverlayStrategy>();
        return { overlay, overlay };
    }
    if (role == HostRenderViewRole::TopDownSlice) {
        auto overlay = std::make_shared<PartSliceOverlayStrategy>(
            Orientation::Top_down);
        return { overlay, overlay };
    }
    if (role == HostRenderViewRole::FrontBackSlice) {
        auto overlay = std::make_shared<PartSliceOverlayStrategy>(
            Orientation::Front_back);
        return { overlay, overlay };
    }
    if (role == HostRenderViewRole::LeftRightSlice) {
        auto overlay = std::make_shared<PartSliceOverlayStrategy>(
            Orientation::Left_right);
        return { overlay, overlay };
    }
    return {};
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
    const PartCatalog& catalog)
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
    ids.reserve((catalog.partsByLabel.size() - 1U));
    voxelCounts.reserve((catalog.partsByLabel.size() - 1U));
    volumes.reserve((catalog.partsByLabel.size() - 1U));
    extents.reserve((catalog.partsByLabel.size() - 1U));
    bounds.reserve((catalog.partsByLabel.size() - 1U));
    centroids.reserve((catalog.partsByLabel.size() - 1U));
    confidence.reserve((catalog.partsByLabel.size() - 1U));
    reviewed.reserve((catalog.partsByLabel.size() - 1U));
    edited.reserve((catalog.partsByLabel.size() - 1U));
    for (std::size_t index = 1; index < catalog.partsByLabel.size(); ++index) {
        const auto& entry = catalog.partsByLabel[index];
        const auto& part = entry.metrics;
        ids.push_back(entry.labelId);
        voxelCounts.push_back(part.voxelCount);
        volumes.push_back(part.physicalVolumeMM3);
        std::array<std::int64_t, 6> extent{};
        std::transform(
            part.voxelExtent.begin(), part.voxelExtent.end(),
            extent.begin(),
            [](const int value) { return static_cast<std::int64_t>(value); });
        extents.push_back(extent);
        bounds.push_back(part.inputPhysicalBounds);
        centroids.push_back(part.centroidInputPhysical);
        confidence.push_back(part.confidence.value_or(
            std::numeric_limits<double>::quiet_NaN()));
        reviewed.push_back(entry.userState.isReviewed ? 1U : 0U);
        edited.push_back(false ? 1U : 0U);
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
    result.resultRevision = state.resultRevision;
    result.catalogRevision = state.catalogRevision;
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
    std::shared_ptr<const PartSetSnapshot> GetPartSetSnapshot() const;
    PartMutationResult SetPartState(
        const PartBindingRef& part,
        const PartStatePatch& patch,
        std::uint64_t expectedCatalogRevision);

private:
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureOverlay> overlay;
        std::shared_ptr<PartOverlayControl> control;
        std::string viewId;
    };

    bool GetIsOwnerThread() const noexcept;
    std::uint64_t GetNextRequestId() noexcept;
    std::vector<HostFeatureView> GetTargetViews(
        const HostViewTargets& targets) const;
    bool GetSourceSame(const VtkImageGridSnapshot& source) const;
    bool GetHistorySourceSame(const VtkImageGridSnapshot& source) const;
    std::optional<DataBinding> GetResultBinding(const DataGraphSnapshot& graph) const;
    bool SetDataTypes();
    bool SetCatalogCommit(const PartCatalog& catalog,
        const VtkImageGridSnapshot& source, const DataBinding& expected,
        const std::shared_ptr<const LabelMap3DPayload>& labels,
        PartSegmentationState& state);
    void SetState(PartSegmentationState state);
    void SetPublishedState(
        PartSegmentationState state,
        std::shared_ptr<const PartSetSnapshot> snapshot);
    void SetRequestRunning(
        std::uint64_t requestId,
        const DataRevisionRef& source);
    void SetRequestProgress(double progress);
    void QueueComplete(
        PartSegmentationCallback callback,
        PartSegmentationResult result,
        std::optional<RenderInputStamp> requiredInput = {},
        std::vector<std::string> requiredViewIds = {}) const noexcept;
    static std::vector<std::string> GetViewIds(
        const std::vector<HostFeatureView>& views);
    bool SendSceneDelta(
        std::uint64_t requestId,
        FeatureScenePriority priority,
        const VtkImageGridSnapshot& source,
        const std::vector<HostFeatureView>& views) const;
    void CancelQueuedCompletes() noexcept;
    bool AttachDisplay(
        vtkSmartPointer<vtkImageData> labelImage,
        const PartRenderStateTable& renderStates,
        std::shared_ptr<const PartSurfaceProduct> surfaceProduct,
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
    std::shared_ptr<const PartSetSnapshot> m_publicSnapshot;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<PartSegmentationService> m_service;
    VtkImageGridSnapshot m_requestSource;
    DataBinding m_requestResultBinding;
    VtkLabelMapSnapshot m_activeLabels;
    VtkImageGridSnapshot m_activeSource;
    // m_labelImage 借用该 vector；声明顺序保证 image 先析构。
    std::shared_ptr<const std::vector<PartLabelId>> m_labelValues;
    vtkSmartPointer<vtkImageData> m_labelImage;
    std::shared_ptr<const PartCatalog> m_catalogView;
    std::shared_ptr<const PartSurfaceProduct> m_surfaceProduct;
    std::vector<HostFeatureView> m_requestViews;
    std::vector<HostFeatureView> m_activeViews;
    std::vector<OverlayBinding> m_bindings;
    PartSegmentationCallback m_startCallback;
    mutable std::mutex m_pendingCompleteMutex;
    mutable std::vector<std::weak_ptr<PartSegmentationResult>>
        m_pendingCompleteResults;
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
        QueueComplete(
            std::move(m_startCallback),
            BuildResult(
                state,
                m_activeRequestId,
                PartResultStatus::Cancelled,
                PartFailureReason::Cancelled,
                state.partCount,
                "Part request was cancelled by detach."));
    }
    CancelQueuedCompletes();
    if (!ClearResult()) return false;

    m_service.reset();
    m_requestSource.reset();
    m_activeSource.reset();
    m_surfaceProduct.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_catalogView.reset();
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
    SetPublishedState(std::move(idle), {});
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
        const std::uint64_t requestId = GetNextRequestId();
        PartHistorySnapshot previous;
        std::uint64_t expectedResultRevision = 0;
        std::uint64_t expectedCatalogRevision = 0;
        if (GetHistorySourceSame(source)) {
            previous.labels = m_labelValues;
            previous.catalog = m_catalogView;
            expectedResultRevision = m_catalogView->resultRevision;
            expectedCatalogRevision = m_catalogView->catalogRevision;
        }
        const auto status = m_service->Start(
            source,
            params,
            m_config.maxWorkingBytes,
            requestId,
            std::move(previous),
            expectedResultRevision,
            expectedCatalogRevision,
            m_surfaceProduct ? m_surfaceProduct->actualBytes : 0);
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
        QueueComplete(
            std::move(onComplete),
            BuildResult(
                state,
                requestId,
                PartResultStatus::Succeeded,
                PartFailureReason::None,
                state.partCount,
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
        const bool hadVisibleDisplay = !m_bindings.empty();
        const auto deltaSource = m_activeSource;
        const auto deltaViews = m_activeViews;
        bool isSucceeded = SetVisibility(*request.isVisible);
        const bool hasVisibleChange = isSucceeded
            && hadVisibleDisplay != !m_bindings.empty();
        if (hasVisibleChange) {
            isSucceeded = SendSceneDelta(
                requestId,
                FeatureScenePriority::Overlay,
                deltaSource,
                deltaViews);
        }
        const auto state = GetState();
        const std::optional<RenderInputStamp> requiredInput =
            isSucceeded && hasVisibleChange && deltaSource
            ? std::optional<RenderInputStamp>(RenderInputStamp{
                deltaSource->data->self })
            : std::nullopt;
        const auto requiredViewIds = requiredInput
            ? GetViewIds(deltaViews) : std::vector<std::string>{};
        QueueComplete(
            std::move(onComplete),
            BuildResult(
                state,
                requestId,
                isSucceeded ? PartResultStatus::Succeeded
                    : PartResultStatus::Failed,
                isSucceeded ? PartFailureReason::None
                            : PartFailureReason::DisplayFailed,
                state.partCount,
                isSucceeded ? "Part visibility was updated."
                            : "Part visibility update failed."),
            requiredInput,
            requiredViewIds);
        return admission;
    }
    if (request.action == PartSegmentationAction::Clear) {
        const auto requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        const auto previousState = GetState();
        const bool hadVisibleDisplay = !m_bindings.empty();
        const auto deltaSource = m_activeSource;
        const auto deltaViews = m_activeViews;
        bool isSucceeded = ClearResult();
        if (isSucceeded && hadVisibleDisplay) {
            isSucceeded = SendSceneDelta(
                requestId,
                FeatureScenePriority::Overlay,
                deltaSource,
                deltaViews);
        }
        const std::optional<RenderInputStamp> requiredInput =
            isSucceeded && hadVisibleDisplay && deltaSource
            ? std::optional<RenderInputStamp>(RenderInputStamp{
                deltaSource->data->self })
            : std::nullopt;
        const auto requiredViewIds = requiredInput
            ? GetViewIds(deltaViews) : std::vector<std::string>{};
        QueueComplete(
            std::move(onComplete),
            BuildResult(
                previousState,
                requestId,
                isSucceeded ? PartResultStatus::Succeeded
                    : PartResultStatus::Failed,
                isSucceeded ? PartFailureReason::None
                            : PartFailureReason::DisplayFailed,
                0,
                isSucceeded ? "Part result was cleared."
                            : "Part result clear failed."),
            requiredInput,
            requiredViewIds);
        return admission;
    }
    return admission;
}

PartSegmentationState PartSegmentationHostFeature::Impl::GetState() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state;
}

std::shared_ptr<const PartSetSnapshot>
PartSegmentationHostFeature::Impl::GetPartSetSnapshot() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_publicSnapshot;
}

PartMutationResult PartSegmentationHostFeature::Impl::SetPartState(
    const PartBindingRef& part,
    const PartStatePatch& patch,
    const std::uint64_t expectedCatalogRevision)
{
    PartMutationResult result;
    if (!m_isAttached || !GetIsOwnerThread() || !m_service) {
        result.status = PartMutationStatus::Unavailable;
        return result;
    }
    if (m_service->GetIsBusy() || m_activeRequestId != 0) {
        result.status = PartMutationStatus::Busy;
        result.catalogRevision = GetState().catalogRevision;
        return result;
    }
    const auto state = GetState();
    if (state.status == PartSegmentationStatus::Stale) {
        result.status = PartMutationStatus::StaleReference;
        result.catalogRevision = state.catalogRevision;
        return result;
    }
    const auto currentSnapshot = GetPartSetSnapshot();
    if (currentSnapshot && currentSnapshot->isStale) {
        result.status = PartMutationStatus::StaleReference;
        result.catalogRevision = currentSnapshot->catalogRevision;
        return result;
    }
    if (!currentSnapshot || !m_catalogView || !m_labelValues) {
        result.status = PartMutationStatus::Unavailable;
        return result;
    }

    try {
        auto candidate = std::make_shared<PartCatalog>(*m_catalogView);
        result = SetPartCatalogState(
            *candidate, part, patch, expectedCatalogRevision);
        if (result.status != PartMutationStatus::Succeeded
            || result.catalogRevision
                == m_catalogView->catalogRevision) {
            return result;
        }

        const auto previousStates =
            BuildPartRenderStateTable(*m_catalogView);
        const auto nextStates = BuildPartRenderStateTable(*candidate);
        const auto nextSnapshot = BuildPartSetSnapshot(
            *candidate, currentSnapshot->sourceRevision, false);
        if (!previousStates || !nextStates || !nextSnapshot) {
            return {
                PartMutationStatus::DisplayFailed,
                m_catalogView->catalogRevision
            };
        }

        std::vector<std::shared_ptr<PartOverlayControl>> controls;
        controls.reserve(m_bindings.size());
        for (const auto& binding : m_bindings) {
            controls.push_back(binding.control);
        }
        if (!controls.empty()
            && !SetPartStates(controls, *nextStates, *previousStates)) {
            return {
                PartMutationStatus::DisplayFailed,
                m_catalogView->catalogRevision
            };
        }

        const bool hasPresentationPatch = patch.isVisible || patch.isSelected
            || patch.opacity || patch.color;
        if (!controls.empty() && hasPresentationPatch) {
            bool isFrameAccepted = false;
            try {
                isFrameAccepted = SendSceneDelta(
                    GetNextRequestId(),
                    FeatureScenePriority::Overlay,
                    m_activeSource,
                    m_activeViews);
            }
            catch (...) {
            }
            if (!isFrameAccepted) {
                // 帧意图与目录候选必须一起接纳；拒绝时恢复各 View 的显示状态。
                (void)SetPartStates(controls, *previousStates, *nextStates);
                return {
                    PartMutationStatus::DisplayFailed,
                    m_catalogView->catalogRevision
                };
            }
        }

        auto nextState = GetState();
        const auto binding = GetResultBinding(m_data->GetDataGraph());
        if (!binding || binding->target != std::optional<DataRevisionRef>{state.resultSet}
            || !SetCatalogCommit(*candidate, m_activeSource, *binding, {}, nextState)) {
            (void)SetPartStates(controls, *previousStates, *nextStates);
            return { PartMutationStatus::RevisionConflict, state.catalogRevision };
        }
        SetPublishedState(std::move(nextState), nextSnapshot);
        return result;
    }
    catch (...) {
        result.status = PartMutationStatus::DisplayFailed;
        result.catalogRevision = m_catalogView
            ? m_catalogView->catalogRevision : 0;
        return result;
    }
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
    graph = m_data->GetDataGraph();
    const bool hasCatalog = hasType(partCatalogType)
        || m_data->SetDataType(DataTypeDescriptor{
            partCatalogType, { partCatalogFacet },
            [](const IDataPayload& value, std::string&) {
                const auto* payload = dynamic_cast<const PartCatalogPayload*>(&value);
                const auto catalog = payload ? payload->GetCatalog() : nullptr;
                return catalog && GetPartSetIdValid(catalog->partSetId)
                    && catalog->resultRevision != 0 && catalog->catalogRevision != 0;
            } });
    return hasTable && hasResult && hasCatalog;
}

bool PartSegmentationHostFeature::Impl::SetCatalogCommit(
    const PartCatalog& catalog,
    const VtkImageGridSnapshot& source,
    const DataBinding& expected,
    const std::shared_ptr<const LabelMap3DPayload>& labels,
    PartSegmentationState& state)
{
    if (!m_data || !source || !source->data || !source->binding) return false;
    const auto table = CreatePartTable(catalog);
    const auto catalogPayload = std::make_shared<const PartCatalogPayload>(catalog);
    if (!table || (labels && !labels->GetValid())) return false;
    const auto createRef = [this]() {
        return DataRevisionRef{ m_data->CreateDataEntityId(), 1 };
    };
    const auto labelRef = labels ? createRef() : state.labelMap;
    const auto tableRef = createRef();
    const auto catalogRef = createRef();
    const auto resultRef = createRef();
    if (!GetDataRevisionRefValid(labelRef)) return false;
    const DataInputRef sourceInput{ "source-volume", source->data->self };
    const DataInputRef labelInput{ "labels", labelRef };
    const DataProvenance provenance{
        std::string(featureId), labels ? "segment" : "edit-catalog", "1", "{}" };
    DataTransaction transaction;
    DataExpectation sourceExpected;
    sourceExpected.kind = DataExpectationKind::Binding;
    sourceExpected.binding = std::string(primaryVolumeBinding);
    sourceExpected.expectedBindingRevision = source->binding->revision;
    sourceExpected.isTargetChecked = true;
    sourceExpected.expectedTarget = source->data->self;
    transaction.expectations.push_back(std::move(sourceExpected));
    if (labels) transaction.outputs.push_back({
        labelRef.entityId, 0, DataTypes::labelMap3D,
        { sourceInput }, labels, provenance });
    transaction.outputs.push_back({ tableRef.entityId, 0, partTableType,
        { sourceInput, labelInput }, table, provenance });
    transaction.outputs.push_back({ catalogRef.entityId, 0, partCatalogType,
        { sourceInput, labelInput }, catalogPayload, provenance });
    const auto collection = std::make_shared<const DataCollectionPayload>(
        partResultSetType, std::vector<DataCollectionEntry>{
            { "labels", labelRef }, { "parts", tableRef }, { "catalog", catalogRef } });
    transaction.outputs.push_back({ resultRef.entityId, 0, partResultSetType,
        { sourceInput, labelInput, { "parts", tableRef }, { "catalog", catalogRef } },
        collection, provenance });
    transaction.bindings.push_back({ std::string(partResultBinding),
        expected.revision, true, expected.target, resultRef });
    const auto committed = m_data->SetDataCommit(std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded) return false;
    // CreateSnapshot 与 Store 共享的只有这个已隔离、不可变的目录 owner。
    m_catalogView = catalogPayload->GetCatalog();
    state.commitId = committed.commitId;
    state.sourceRevision = source->data->self;
    state.labelMap = labelRef;
    state.partTable = tableRef;
    state.resultSet = resultRef;
    state.partSetId = catalog.partSetId;
    state.resultRevision = catalog.resultRevision;
    state.catalogRevision = catalog.catalogRevision;
    state.partCount = catalog.partsByLabel.size() - 1U;
    return true;
}

bool PartSegmentationHostFeature::Impl::GetHistorySourceSame(
    const VtkImageGridSnapshot& source) const
{
    return source
        && source->image
        && m_activeSource
        && source->data && m_activeSource->data
        && m_activeSource->data->self == source->data->self
        && GetSourceSame(m_activeSource)
        && m_labelValues
        && m_labelImage
        && m_catalogView
        && GetImageGeometrySame(*source->image, *m_labelImage);
}

void PartSegmentationHostFeature::Impl::SetState(
    PartSegmentationState state)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = std::move(state);
}

void PartSegmentationHostFeature::Impl::SetPublishedState(
    PartSegmentationState state,
    std::shared_ptr<const PartSetSnapshot> snapshot)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = std::move(state);
    m_publicSnapshot = std::move(snapshot);
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

void PartSegmentationHostFeature::Impl::QueueComplete(
    PartSegmentationCallback callback,
    PartSegmentationResult result,
    std::optional<RenderInputStamp> requiredInput,
    std::vector<std::string> requiredViewIds) const noexcept
{
    if (!callback) return;
    try {
        auto sharedCallback =
            std::make_shared<PartSegmentationCallback>(std::move(callback));
        auto sharedResult =
            std::make_shared<PartSegmentationResult>(std::move(result));
        auto isSent = std::make_shared<std::atomic<bool>>(false);
        {
            const std::lock_guard<std::mutex> lock(
                m_pendingCompleteMutex);
            m_pendingCompleteResults.erase(
                std::remove_if(
                    m_pendingCompleteResults.begin(),
                    m_pendingCompleteResults.end(),
                    [](const auto& current) { return current.expired(); }),
                m_pendingCompleteResults.end());
            m_pendingCompleteResults.push_back(sharedResult);
        }
        const std::weak_ptr<TrustedDataPort> weakData = m_data;
        const std::weak_ptr<FeatureViewDirectory> weakViews = m_views;
        const auto send = [sharedCallback, sharedResult, isSent,
            weakData, weakViews, requiredInput,
            requiredViewIds = std::move(requiredViewIds)]() noexcept {
            if (isSent->exchange(true, std::memory_order_acq_rel)) return;
            if (requiredInput
                && sharedResult->status == PartResultStatus::Succeeded) {
                const auto data = weakData.lock();
                const auto current = data
                    ? data->GetPrimaryImage() : VtkImageGridSnapshot{};
                if (!current
                    || !current->data
                    || current->data->self != requiredInput->dataRevision) {
                    sharedResult->status = PartResultStatus::Failed;
                    sharedResult->failureReason =
                        PartFailureReason::SourceChanged;
                    sharedResult->message =
                        "Part source changed before rendered completion.";
                }
                else {
                    const auto views = weakViews.lock();
                    const bool areViewsCurrent = views
                        && !requiredViewIds.empty()
                        && std::all_of(
                            requiredViewIds.begin(), requiredViewIds.end(),
                            [&views, &requiredInput](const auto& viewId) {
                                const auto port =
                                    views->GetFeaturePort(viewId);
                                const auto stamp = port
                                    ? port->GetRenderInputStamp()
                                    : std::optional<RenderInputStamp>{};
                                return stamp && *stamp == *requiredInput;
                            });
                    if (!areViewsCurrent) {
                        sharedResult->status = PartResultStatus::Failed;
                        sharedResult->failureReason =
                            PartFailureReason::DisplayFailed;
                        sharedResult->message =
                            "Part target view changed before rendered completion.";
                    }
                }
            }
            try { (*sharedCallback)(std::move(*sharedResult)); }
            catch (...) {}
        };
        if (!m_host || !m_host->SendOwnerComplete(send)) {
            if (sharedResult->status == PartResultStatus::Succeeded) {
                sharedResult->status = PartResultStatus::Cancelled;
                sharedResult->failureReason = PartFailureReason::Cancelled;
                sharedResult->message =
                    "Part completion was cancelled while stopping.";
            }
            send();
        }
    }
    catch (...) {
        try { callback(std::move(result)); }
        catch (...) {}
    }
}

std::vector<std::string> PartSegmentationHostFeature::Impl::GetViewIds(
    const std::vector<HostFeatureView>& views)
{
    std::vector<std::string> viewIds;
    viewIds.reserve(views.size());
    for (const auto& view : views) {
        if (view.id.empty()
            || std::find(viewIds.begin(), viewIds.end(), view.id)
                != viewIds.end()) {
            continue;
        }
        viewIds.push_back(view.id);
    }
    return viewIds;
}

void PartSegmentationHostFeature::Impl::CancelQueuedCompletes() noexcept
{
    const std::lock_guard<std::mutex> lock(m_pendingCompleteMutex);
    for (const auto& weakResult : m_pendingCompleteResults) {
        const auto result = weakResult.lock();
        if (!result || result->status != PartResultStatus::Succeeded) {
            continue;
        }
        result->status = PartResultStatus::Cancelled;
        result->failureReason = PartFailureReason::Cancelled;
        result->message = "Part completion was cancelled while detaching.";
    }
    m_pendingCompleteResults.erase(
        std::remove_if(
            m_pendingCompleteResults.begin(),
            m_pendingCompleteResults.end(),
            [](const auto& current) { return current.expired(); }),
        m_pendingCompleteResults.end());
}

bool PartSegmentationHostFeature::Impl::SendSceneDelta(
    const std::uint64_t requestId,
    const FeatureScenePriority priority,
    const VtkImageGridSnapshot& source,
    const std::vector<HostFeatureView>& views) const
{
    if (!m_host || requestId == 0 || !source || !source->image
        || !source->data || !GetDataRevisionRefValid(source->data->self) || views.empty()) {
        return false;
    }
    FeatureSceneDelta delta;
    delta.requestId = requestId;
    delta.priority = priority;
    delta.scope = FeatureSceneScope::RequiredAllViews;
    delta.inputStamp = {
        source->data->self };
    delta.viewIds = GetViewIds(views);
    return !delta.viewIds.empty()
        && m_host->SendSceneDelta(std::move(delta));
}

bool PartSegmentationHostFeature::Impl::AttachDisplay(
    vtkSmartPointer<vtkImageData> labelImage,
    const PartRenderStateTable& renderStates,
    std::shared_ptr<const PartSurfaceProduct> surfaceProduct,
    const std::vector<HostFeatureView>& views,
    std::vector<OverlayBinding>& nextBindings)
{
    if (!m_views || !m_host || !labelImage || !surfaceProduct
        || !surfaceProduct->surface || views.empty()) return false;
    std::vector<std::string> viewIds;
    viewIds.reserve(views.size());
    nextBindings.reserve(views.size());
    for (const auto& view : views) {
        auto service = m_views->GetOverlayPort(view.id);
        auto candidate = CreateOverlay(view.role);
        if (!service || !candidate.overlay || !candidate.control) {
            RemoveBindings(nextBindings);
            return false;
        }
        if (view.role == HostRenderViewRole::Primary3D) {
            candidate.overlay->SetInputData(surfaceProduct->surface);
        }
        else {
            candidate.overlay->SetInputData(labelImage);
        }
        if (!candidate.control->SetPartStates(renderStates)
            || !service->AttachOverlay(candidate.overlay)) {
            RemoveBindings(nextBindings);
            return false;
        }
        nextBindings.push_back({
            service,
            std::move(candidate.overlay),
            std::move(candidate.control),
            view.id
        });
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
        if (!isVisible) return m_isActiveViewClearPending ? RemoveDisplay() : true;
        if (!m_bindings.empty() || state.status == PartSegmentationStatus::Stale
            || !GetDataRevisionRefValid(state.resultSet)) return true;
    }
    if (!isVisible) {
        const bool isDisplayRemoved = RemoveDisplay();
        state.isOverlayVisible = false;
        SetState(std::move(state));
        return isDisplayRemoved;
    }
    if (!m_labelValues || !m_labelImage || !m_surfaceProduct
        || m_activeViews.empty()) {
        if (GetDataRevisionRefValid(state.resultSet)
            && state.status != PartSegmentationStatus::Stale) return false;
        state.isOverlayVisible = true;
        SetState(std::move(state));
        return true;
    }
    const auto renderStates = m_catalogView
        ? BuildPartRenderStateTable(*m_catalogView)
        : std::optional<PartRenderStateTable>{};
    if (!renderStates) return false;
    std::vector<OverlayBinding> nextBindings;
    if (!AttachDisplay(
            m_labelImage,
            *renderStates,
            m_surfaceProduct,
            m_activeViews,
            nextBindings)) {
        return false;
    }
    RemoveBindings(m_bindings);
    m_bindings = std::move(nextBindings);
    state.isOverlayVisible = true;
    SetState(std::move(state));
    return true;
}

bool PartSegmentationHostFeature::Impl::ClearResult()
{
    const auto current = GetState();
    const auto graph = m_data ? m_data->GetDataGraph() : DataGraphSnapshot{};
    const auto binding = GetResultBinding(graph);
    if (binding && binding->target && *binding->target == current.resultSet) {
        DataTransaction transaction;
        transaction.bindings.push_back({ std::string(partResultBinding),
            binding->revision, true, binding->target, {} });
        if (m_data->SetDataCommit(std::move(transaction)).status
            != DataCommitStatus::Succeeded) return false;
    }
    const bool isDisplayRemoved = RemoveDisplay();
    m_activeLabels.reset();
    m_surfaceProduct.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_catalogView.reset();
    m_activeSource.reset();
    m_activeViews.clear();
    PartSegmentationState state;
    state.isOverlayVisible = GetState().isOverlayVisible;
    SetPublishedState(std::move(state), {});
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
    auto staleSnapshot = GetPartSetSnapshot();
    if (staleSnapshot && !staleSnapshot->isStale) {
        try {
            auto nextSnapshot = std::make_shared<PartSetSnapshot>(
                *staleSnapshot);
            nextSnapshot->isStale = true;
            staleSnapshot = std::move(nextSnapshot);
        }
        catch (...) {
            SetState(std::move(state));
            return;
        }
    }
    SetPublishedState(std::move(state), std::move(staleSnapshot));
    // Stale 先表达 source 已失效；显示清理失败时保留完整 generation，
    // 下一次 owner tick 会重试，避免悬空或半清理。
    if (!RemoveDisplay()) return;
    m_activeSource.reset();
    m_surfaceProduct.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_catalogView.reset();
    m_activeViews.clear();
}

void PartSegmentationHostFeature::Impl::SetRequestComplete(
    PartLabelCandidate candidate)
{
    auto callback = std::move(m_startCallback);
    const auto requestId = m_activeRequestId;
    PartResultStatus resultStatus = candidate.status;
    PartFailureReason reason = candidate.failureReason;
    std::string message = candidate.message;
    std::optional<RenderInputStamp> requiredInput;
    std::vector<std::string> requiredViews;
    bool isCommitted = false;
    auto nextState = GetState();
    if (m_isSourceChanged || !GetSourceSame(m_requestSource)) {
        resultStatus = PartResultStatus::Failed;
        reason = PartFailureReason::SourceChanged;
        message = "Part source changed before commit.";
    }
    else if (m_isStopRequested) {
        resultStatus = PartResultStatus::Cancelled;
        reason = PartFailureReason::Cancelled;
        message = "Part request was cancelled.";
    }
    else if (candidate.status == PartResultStatus::Succeeded) {
        try {
            const bool hasExpected = candidate.expectedResultRevision != 0
                || candidate.expectedCatalogRevision != 0;
            const bool isExpected = hasExpected
                ? m_catalogView
                    && m_catalogView->resultRevision == candidate.expectedResultRevision
                    && m_catalogView->catalogRevision == candidate.expectedCatalogRevision
                : !m_catalogView;
            if (!isExpected || !candidate.catalog || !candidate.labels
                || !candidate.surface || !candidate.surface->surface
                || !GetPartCatalogValid(*candidate.catalog, *candidate.labels)) {
                throw std::runtime_error("Part candidate is invalid.");
            }
            // 算法峰值之外，提交阶段还会冻结标签、目录和表格，并建立 VTK 标签视图。
            // 按两阶段峰值之和保守预留，拒绝时尚未分配这些副本或写入图。
            std::size_t requiredBytes = candidate.requiredBytes;
            const auto addBytes = [&requiredBytes](std::size_t count, std::size_t width) {
                const auto limit = std::numeric_limits<std::size_t>::max();
                if (width != 0 && count > (limit - requiredBytes) / width) {
                    requiredBytes = limit;
                    return false;
                }
                requiredBytes += count * width;
                return true;
            };
            std::size_t catalogBytes = 0;
            // 每行覆盖 table 的原始列与冻结副本，以及公开目录与渲染状态投影。
            constexpr std::size_t rowBytes = 2U * (2U * sizeof(std::uint64_t)
                + 3U * sizeof(double) + 6U * sizeof(std::int64_t)
                + 9U * sizeof(double) + 2U * sizeof(std::uint8_t))
                + sizeof(PartSnapshot) + sizeof(PartRenderState);
            const bool hasBudget = GetPartCatalogStorageBytes(*candidate.catalog, catalogBytes)
                && addBytes(candidate.labels->size(), 2U * sizeof(PartLabelId))
                && addBytes(m_labelValues ? m_labelValues->size() : 0U, sizeof(PartLabelId))
                && addBytes(2U, catalogBytes)
                && addBytes(candidate.catalog->partsByLabel.size(), rowBytes)
                && addBytes(1U, 9U * sizeof(RecordColumn)
                    + 4U * sizeof(DataRevision) + 16U * sizeof(DataInputRef)
                    + sizeof(DataCollectionPayload) + sizeof(PartSetSnapshot))
                && requiredBytes <= m_config.maxWorkingBytes;
            if (!hasBudget) {
                reason = PartFailureReason::BudgetExceeded;
                throw std::runtime_error("Part graph publication budget exceeded: requiredBytes="
                    + std::to_string(requiredBytes) + ", maxWorkingBytes="
                    + std::to_string(m_config.maxWorkingBytes) + ".");
            }
            message += " graphPublicationBytes=" + std::to_string(requiredBytes) + ".";
            const auto publicSnapshot = BuildPartSetSnapshot(
                *candidate.catalog, m_requestSource->data->self, false);
            const auto renderStates = BuildPartRenderStateTable(*candidate.catalog);
            const auto labels = std::make_shared<const LabelMap3DPayload>(
                GetLabelGeometry(candidate),
                LabelMapValues{ std::shared_ptr<const std::vector<std::uint32_t>>(candidate.labels) },
                std::vector<LabelDefinition>{}, "PartSegmentation.labels", "Part segmentation");
            if (!publicSnapshot || !renderStates
                || !SetCatalogCommit(*candidate.catalog, m_requestSource,
                    m_requestResultBinding, labels, nextState)) {
                reason = PartFailureReason::SourceChanged;
                throw std::runtime_error("Part graph transaction was rejected.");
            }
            isCommitted = true;
            // 正式数据已经发布；之后的显示失败只能报告 presentation failure。
            nextState.status = PartSegmentationStatus::Succeeded;
            nextState.failureReason = PartFailureReason::None;
            nextState.requestId = requestId;
            nextState.progress = 1.0;
            SetPublishedState(nextState, publicSnapshot);
            m_isActiveViewClearPending = m_isActiveViewClearPending || !m_bindings.empty();
            RemoveBindings(m_bindings);
            m_labelImage = nullptr;
            m_labelValues.reset();
            m_activeLabels.reset();
            m_surfaceProduct = std::move(candidate.surface);
            m_activeViews = m_requestViews;
            m_activeSource = m_requestSource;
            const auto graph = m_data->GetDataGraph();
            const auto labelData = m_data->GetData(graph, nextState.labelMap);
            const auto labelPayload = labelData
                ? std::dynamic_pointer_cast<const LabelMap3DPayload>(labelData->payload)
                : nullptr;
            auto labelView = m_data->GetLabelMap(graph, nextState.labelMap);
            RemoveBindings(m_bindings);
            m_labelImage = nullptr;
            m_labelValues = labelPayload ? labelPayload->GetLabels() : nullptr;
            m_activeLabels = std::move(labelView);
            m_labelImage = m_activeLabels ? m_activeLabels->labels : nullptr;
            bool isDisplayed = m_labelImage && m_labelValues;
            if (nextState.isOverlayVisible && isDisplayed) {
                isDisplayed = AttachDisplay(m_labelImage, *renderStates,
                    m_surfaceProduct, m_activeViews, m_bindings);
                isDisplayed = isDisplayed && SendSceneDelta(requestId,
                    FeatureScenePriority::Scene, m_activeSource, m_activeViews);
            }
            else if (!nextState.isOverlayVisible) {
                isDisplayed = RemoveDisplay();
            }
            if (!isDisplayed) {
                (void)RemoveDisplay();
                nextState.failureReason = PartFailureReason::DisplayFailed;
                SetState(nextState);
                resultStatus = PartResultStatus::SucceededWithDisplayFailure;
                reason = PartFailureReason::DisplayFailed;
                message = "Part data committed; display can be retried.";
            }
            else {
                resultStatus = PartResultStatus::Succeeded;
                reason = PartFailureReason::None;
                if (nextState.isOverlayVisible) {
                    requiredInput = RenderInputStamp{ m_activeSource->data->self };
                    requiredViews = GetViewIds(m_activeViews);
                }
            }
        }
        catch (const std::exception& error) {
            resultStatus = isCommitted ? PartResultStatus::SucceededWithDisplayFailure
                : PartResultStatus::Failed;
            reason = isCommitted ? PartFailureReason::DisplayFailed
                : reason == PartFailureReason::SourceChanged
                    || reason == PartFailureReason::BudgetExceeded ? reason
                : PartFailureReason::InternalError;
            message = error.what();
        }
        catch (...) {
            resultStatus = isCommitted ? PartResultStatus::SucceededWithDisplayFailure
                : PartResultStatus::Failed;
            reason = isCommitted ? PartFailureReason::DisplayFailed
                : PartFailureReason::InternalError;
            message = "Part graph publication or display failed.";
        }
    }
    if (!isCommitted) SetRequestFailed(reason);
    else if (reason == PartFailureReason::DisplayFailed) {
        nextState.failureReason = reason;
        SetState(nextState);
    }
    const auto state = GetState();
    auto result = BuildResult(state, requestId, resultStatus, reason,
        state.partCount, std::move(message));
    m_requestSource.reset();
    m_requestViews.clear();
    m_activeRequestId = 0;
    m_isSourceChanged = false;
    m_isStopRequested = false;
    QueueComplete(std::move(callback), std::move(result),
        requiredInput, std::move(requiredViews));
}

void PartSegmentationHostFeature::Impl::SetRequestFailed(
    const PartFailureReason reason)
{
    if (reason == PartFailureReason::SourceChanged) {
        SetSourceStale();
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_stateBeforeRequest.status == PartSegmentationStatus::Idle) {
        m_state = m_stateBeforeRequest;
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
              "catalog", partCatalogType, { partCatalogFacet } },
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

std::shared_ptr<const PartSetSnapshot>
PartSegmentationHostFeature::GetPartSetSnapshot() const
{
    return m_impl ? m_impl->GetPartSetSnapshot() : nullptr;
}

PartMutationResult PartSegmentationHostFeature::SetPartState(
    const PartBindingRef& part,
    const PartStatePatch& patch,
    const std::uint64_t expectedCatalogRevision)
{
    return m_impl
        ? m_impl->SetPartState(part, patch, expectedCatalogRevision)
        : PartMutationResult{ PartMutationStatus::Unavailable, 0 };
}
