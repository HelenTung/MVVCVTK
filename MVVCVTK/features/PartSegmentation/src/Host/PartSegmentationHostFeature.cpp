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
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <iomanip>
#include <locale>
#include <string>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "part-segmentation";
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
    std::vector<std::shared_ptr<const void>> GetDataResources() const override
    {
        return {m_catalog};
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
        edited.push_back(entry.isEdited ? 1U : 0U);
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
    bool AttachInput(std::weak_ptr<PartSegmentationHostFeature> owner);
    bool DetachHost();
    bool OnHostTick();
    PartSegmentationAdmission SendRequest(
        PartSegmentationRequest request,
        PartSegmentationCallback onComplete);
    PartSegmentationState GetState() const;
    std::vector<FeatureOperationState> GetOperationStates() const;
    PartSegmentationAdmission SendEditRequest(PartEditRequest request,
        PartSegmentationCallback onComplete);
    std::shared_ptr<const PartEditPreview> GetEditPreview() const;
    PartSegmentationAdmission SetEditCommit(std::uint64_t previewId,
        PartSegmentationCallback onComplete);
    PartMutationResult ClearEditPreview(std::uint64_t previewId);
    std::shared_ptr<const PartSetSnapshot> GetPartSetSnapshot() const;
    PartMutationResult SetPartState(
        const PartBindingRef& part,
        const PartStatePatch& patch,
        std::uint64_t expectedCatalogRevision);
    PartMutationResult SetPreviousPart(std::uint64_t expectedCatalogRevision);
    PartMutationResult SetPartState(const HostSemanticTarget& target,
        const PartStatePatch& patch, std::uint64_t expectedCatalogRevision);
    std::optional<HostSemanticTarget> GetInputTarget(const InteractionEvent& event) const;
    InteractionResult SendTargetInput(const InteractionEvent& event, const HostSemanticTarget& target);

private:
    struct HistoryEntry final {
        DataSnapshot labels;
        DataRevisionRef catalogRef;
        std::shared_ptr<const PartCatalog> catalog;
    };
    struct PendingComplete final {
        std::weak_ptr<PartSegmentationResult> result;
        bool isEditCommitted = false;
    };
    struct PreviewRetention final {
        std::weak_ptr<const std::vector<PartLabelId>> labels;
        std::weak_ptr<const PartSetSnapshot> parts;
        std::size_t partBytes = 0;
    };
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureOverlay> overlay;
        std::shared_ptr<PartOverlayControl> control;
        std::string viewId;
    };
    struct SelectionPreview final {
        HostSemanticTarget target;
        std::uint64_t catalogRevision = 0;
    };
    static std::string GetObjectText(const PartObjectId& id);
    bool ClearPreview();

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
        PartSegmentationState& state,
        const std::vector<DataInputRef>& editInputs = {},
        const std::vector<DataExpectation>& editExpected = {},
        const DataProvenance* editProvenance = nullptr,
        DataSnapshot* publishedLabels = nullptr,
        DataPreparedResource resource = {});
    bool GetHistoryBytes(std::size_t& bytes);
    std::optional<HistoryEntry> GetHistoryEntry() const;
    static DataProvenance BuildEditProvenance(const PartEditRequest& request);
    void ClearEditState();
    void CancelEditComplete(std::uint64_t previewId);
    void SetEditComplete(PartLabelCandidate candidate);
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
        std::vector<std::string> requiredViewIds = {},
        bool isEditCommitted = false) const noexcept;
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
    bool SetDisplay(std::uint64_t requestId);
    static void RemoveBindings(
        std::vector<OverlayBinding>& bindings) noexcept;
    bool SetVisibility(bool isVisible);
    bool ClearResult();
    bool ClearResultScopes();
    void SetSourceStale();
    void SetBindingStale();
    void SetRequestComplete(PartLabelCandidate candidate);
    void SetRequestFailed(PartFailureReason reason);

    PartSegmentationConfig m_config;
    mutable std::mutex m_stateMutex;
    PartSegmentationState m_state;
    PartSegmentationState m_stateBeforeRequest;
    FeatureOperationState m_operation;
    std::string m_requestParameters;
    FeatureOperationState m_displayOperation;
    FeatureOperationState m_resultOperation;
    DataRevisionRef m_displayLabels;
    DataRevisionRef m_displayResultSet;
    bool m_hasDisplayPending = false;
    std::shared_ptr<const PartSetSnapshot> m_publicSnapshot;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<PartSegmentationService> m_service;
    VtkImageGridSnapshot m_requestSource;
    DataBinding m_requestResultBinding;
    DataBinding m_resultBinding;
    std::vector<DataLifetimeRetirement> m_resultScopes;
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
    mutable std::vector<PendingComplete> m_pendingCompleteResults;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextRequestId = 1;
    std::uint64_t m_activeRequestId = 0;
    bool m_isAttached = false;
    bool m_isClosing = false;
    bool m_isSourceChanged = false;
    bool m_isStopRequested = false;
    bool m_isActiveViewClearPending = false;
    bool m_isInputAttached = false;
    std::optional<SelectionPreview> m_preview;
    std::atomic<bool> m_isPublishing{ false };
    std::optional<PartEditRequest> m_editRequest;
    std::optional<PartLabelCandidate> m_editCandidate;
    std::shared_ptr<const PartEditPreview> m_editPreview;
    std::vector<DataInputRef> m_editInputs;
    std::vector<DataExpectation> m_editExpected;
    std::vector<HistoryEntry> m_undo;
    std::vector<HistoryEntry> m_redo;
    std::vector<HistoryEntry> m_commitUndo;
    std::vector<HistoryEntry> m_commitRedo;
    std::optional<DataProvenance> m_editProvenance;
    std::vector<PreviewRetention> m_previewRetained;
};

std::string PartSegmentationHostFeature::Impl::GetObjectText(const PartObjectId& id)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text(32, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        text[15 - index] = digits[(id.high >> (index * 4)) & 15U];
        text[31 - index] = digits[(id.low >> (index * 4)) & 15U];
    }
    return text;
}

bool PartSegmentationHostFeature::Impl::AttachInput(
    std::weak_ptr<PartSegmentationHostFeature> owner)
{
    if (!m_config.isSelectionEnabled) return true;
    if (!m_host || owner.expired()) return false;
    HostInputBinding binding;
    binding.featureId = std::string(featureId);
    binding.targetViews.viewRoles = { HostRenderViewRole::Primary3D,
        HostRenderViewRole::Composite3D, HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice, HostRenderViewRole::LeftRightSlice,
        HostRenderViewRole::Auxiliary };
    binding.getTarget = [owner](const InteractionEvent& event) {
        const auto feature = owner.lock();
        return feature && feature->m_impl ? feature->m_impl->GetInputTarget(event) : std::nullopt;
    };
    binding.onTargetInput = [owner](const InteractionEvent& event, const HostSemanticTarget& target) {
        const auto feature = owner.lock();
        return feature && feature->m_impl ? feature->m_impl->SendTargetInput(event, target)
            : InteractionResult{ true, true, true, InteractionFailureReason::None };
    };
    m_isInputAttached = m_host->AttachInput(std::move(binding));
    return m_isInputAttached;
}

std::optional<HostSemanticTarget> PartSegmentationHostFeature::Impl::GetInputTarget(
    const InteractionEvent& event) const
{
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_host || !m_views
        || !m_activeLabels || !m_activeLabels->data || m_activeRequestId != 0
        || m_editCandidate || m_isPublishing.load(std::memory_order_acquire)
        || event.eventKind != InteractionEventKind::PrimaryPress) return std::nullopt;
    const auto snapshot = GetPartSetSnapshot();
    auto target = m_host->GetDisplayTarget(event.viewId, "parts");
    if (!snapshot || snapshot->isStale || !target
        || target->display.data != m_activeLabels->data->self) return std::nullopt;
    const auto view = m_views->GetInputView({ event.viewId, false, HostRenderViewRole::Auxiliary });
    const auto binding = std::find_if(m_bindings.begin(), m_bindings.end(),
        [&](const auto& entry) { return entry.viewId == event.viewId; });
    if (!view || !view->renderer || binding == m_bindings.end() || !binding->control)
        return std::nullopt;
    const auto label = binding->control->GetPickedLabel(event.x, event.y, view->renderer);
    if (!label) return std::nullopt;
    const auto part = std::find_if(snapshot->parts.begin(), snapshot->parts.end(),
        [&](const auto& value) { return value.labelId == *label; });
    if (part == snapshot->parts.end() || !part->presentation.isVisible) return std::nullopt;
    target->objectId = GetObjectText(part->binding.object.objectId);
    target->resultRevision = part->binding.resultRevision;
    return target;
}

PartMutationResult PartSegmentationHostFeature::Impl::SetPartState(
    const HostSemanticTarget& target, const PartStatePatch& patch,
    const std::uint64_t expectedCatalogRevision)
{
    const auto snapshot = GetPartSetSnapshot();
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing) return { PartMutationStatus::Unavailable, 0 };
    if (!snapshot || !m_host || !m_host->GetSemanticTargetValid(target)
        || !m_activeLabels || !m_activeLabels->data
        || target.display.data != m_activeLabels->data->self
        || target.resultRevision != snapshot->resultRevision)
        return { PartMutationStatus::StaleReference, snapshot ? snapshot->catalogRevision : 0 };
    const auto part = std::find_if(snapshot->parts.begin(), snapshot->parts.end(),
        [&](const auto& value) { return GetObjectText(value.binding.object.objectId) == target.objectId; });
    if (part == snapshot->parts.end()) return { PartMutationStatus::NotFound, snapshot->catalogRevision };
    return SetPartState(part->binding, patch, expectedCatalogRevision);
}

bool PartSegmentationHostFeature::Impl::ClearPreview()
{
    if (!m_preview) return true;
    if (m_bindings.empty()) { m_preview.reset(); return true; }
    if (!GetIsOwnerThread() || !m_catalogView) return false;
    try {
        const auto states = BuildPartRenderStateTable(*m_catalogView);
        if (!states) return false;
        bool isRestored = true;
        for (const auto& binding : m_bindings)
            isRestored = binding.control && binding.control->SetPartStates(*states) && isRestored;
        if (!isRestored) return false;
        if (!SendSceneDelta(GetNextRequestId(), FeatureScenePriority::Overlay,
                m_activeSource, m_activeViews)) return false;
        m_preview.reset();
        return true;
    }
    catch (...) { return false; }
}

InteractionResult PartSegmentationHostFeature::Impl::SendTargetInput(
    const InteractionEvent& event, const HostSemanticTarget& target)
{
    const auto result = [](const bool isSucceeded) {
        return InteractionResult{ true, true, isSucceeded,
            isSucceeded ? InteractionFailureReason::None : InteractionFailureReason::StateRejected };
    };
    if (event.eventKind == InteractionEventKind::Cancel) return result(ClearPreview());
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing) return result(false);
    if (m_editCandidate || m_isPublishing.load(std::memory_order_acquire)) return result(false);
    if (event.eventKind == InteractionEventKind::PointerMove) return result(true);
    if (event.eventKind == InteractionEventKind::PrimaryRelease) {
        if (!m_preview) return result(true);
        const auto revision = m_preview->catalogRevision;
        if (!ClearPreview()) return result(false);
        PartStatePatch patch;
        patch.isSelected = true;
        return result(SetPartState(target, patch, revision).status == PartMutationStatus::Succeeded);
    }
    if (event.eventKind != InteractionEventKind::PrimaryPress) return {};
    if (!ClearPreview() || !m_host || !m_host->GetSemanticTargetValid(target)
        || !m_catalogView) return result(false);
    const auto snapshot = GetPartSetSnapshot();
    if (!snapshot || snapshot->resultRevision != target.resultRevision) return result(false);
    const auto part = std::find_if(snapshot->parts.begin(), snapshot->parts.end(),
        [&](const auto& value) { return GetObjectText(value.binding.object.objectId) == target.objectId; });
    if (part == snapshot->parts.end()) return result(false);
    try {
        auto candidate = *m_catalogView;
        PartStatePatch patch;
        patch.isSelected = true;
        if (SetPartCatalogState(candidate, part->binding, patch, snapshot->catalogRevision).status
            != PartMutationStatus::Succeeded) return result(false);
        const auto previous = BuildPartRenderStateTable(*m_catalogView);
        const auto next = BuildPartRenderStateTable(candidate);
        if (!previous || !next) return result(false);
        SelectionPreview preview{ target, snapshot->catalogRevision };
        std::vector<std::shared_ptr<PartOverlayControl>> controls;
        for (const auto& binding : m_bindings) controls.push_back(binding.control);
        if (!SetPartStates(controls, *next, *previous)) return result(false);
        m_preview = std::move(preview);
        if (!SendSceneDelta(GetNextRequestId(), FeatureScenePriority::Overlay,
                m_activeSource, m_activeViews)) { (void)ClearPreview(); return result(false); }
        return result(true);
    }
    catch (...) { (void)ClearPreview(); return result(false); }
}

bool PartSegmentationHostFeature::Impl::AttachHost(
    const HostFeatureContext& context)
{
    if (m_isAttached || !context.views || !context.data || !context.host
        || m_config.maxWorkingBytes == 0
        || !std::isfinite(m_config.defaultStart.threshold)
        || m_config.defaultStart.minPartVoxels == 0
        || m_config.maxHistoryBytes == 0 || m_config.maxUndoSteps == 0
        || m_config.maxUndoSteps > 1024 || m_config.editTimeoutMs == 0
        || m_config.editTimeoutMs > 86400000) {
        return false;
    }
    m_data = context.data;
    if (!SetDataTypes()) {
        m_data.reset();
        return false;
    }
    try {
        m_service = std::make_unique<PartSegmentationService>(
            [weakHost = std::weak_ptr<FeatureHostControl>(context.host)] {
                if (const auto host = weakHost.lock())
                    (void)host->SendWorkAvailable();
            });
    }
    catch (...) {
        m_data.reset();
        return false;
    }
    m_views = context.views;
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
    m_isActiveViewClearPending = false;
    m_operation = {};
    m_displayOperation = {};
    m_resultOperation = {};
    m_displayLabels = {};
    m_displayResultSet = {};
    m_hasDisplayPending = false;
    m_isAttached = true;
    return true;
}

bool PartSegmentationHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) return true;
    if (m_isPublishing.load(std::memory_order_acquire)) return false;
    if (!GetIsOwnerThread()) return false;
    m_isClosing = true;
    if (m_isInputAttached) {
        if (!m_host || !m_host->DetachInput(featureId)) return false;
        m_isInputAttached = false;
    }
    if (!ClearPreview()) return false;
    m_isStopRequested = m_activeRequestId != 0;
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.status = PartSegmentationStatus::Stopping;
    }
    if (m_service) {
        // An idle worker only needs its wakeup/join handshake; running computations retry.
        const auto deadline = std::chrono::steady_clock::now()
            + (m_service->GetIsBusy() ? std::chrono::milliseconds(0) : std::chrono::milliseconds(10));
        if (!m_service->Stop(deadline)) return false;
    }
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
    ClearEditState();
    // The stopped service may still own an unpublished completion and its input reader.
    m_service.reset();
    m_requestSource.reset();
    auto batch = m_data->StartDataChanges();
    if (!batch || !ClearResult() || !ClearResultScopes()) return false;

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
    m_isClosing = false;
    PartSegmentationState idle;
    idle.isOverlayVisible = m_config.isOverlayVisible;
    SetPublishedState(std::move(idle), {});
    return true;
}

bool PartSegmentationHostFeature::Impl::OnHostTick()
{
    // Registry owns the detach retry. Do not publish a late worker result while closing.
    if (m_isAttached && GetIsOwnerThread() && m_isClosing) return true;
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_service || !m_data
        || m_isPublishing.load(std::memory_order_acquire)) {
        return false;
    }
    SetBindingStale();
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
    if (m_hasDisplayPending && !m_bindings.empty()) {
        m_hasDisplayPending = !SendSceneDelta(GetNextRequestId(), FeatureScenePriority::Overlay,
            m_activeSource, m_activeViews);
    }
    auto complete = m_service->GetComplete();
    if (!complete || complete->requestId != m_activeRequestId) return true;
    if (m_editRequest) SetEditComplete(std::move(*complete));
    else SetRequestComplete(std::move(*complete));
    return true;
}

PartSegmentationAdmission PartSegmentationHostFeature::Impl::SendRequest(
    PartSegmentationRequest request,
    PartSegmentationCallback onComplete)
{
    PartSegmentationAdmission admission;
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_service || !m_data) {
        admission.status = PartAdmissionStatus::Unavailable;
        return admission;
    }
    // 隐藏只移除 owner 线程上的显示绑定，不修改 worker 输入或候选数据。
    // 裁切等显示联动必须能在编辑运行和候选待确认期间立即隐藏旧结果。
    const bool isHideRequest = request.action == PartSegmentationAction::SetVisibility
        && request.isVisible.has_value() && !*request.isVisible;
    if (m_isPublishing.load(std::memory_order_acquire)
        || (m_editCandidate && request.action != PartSegmentationAction::Stop
            && request.action != PartSegmentationAction::Clear && !isHideRequest)) {
        admission.status = PartAdmissionStatus::Busy;
        return admission;
    }
    SetBindingStale();
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
        if (!ClearPreview()) return admission;
        auto source = m_data->GetPrimaryImage();
        if (!source || !source->image || !source->data || !source->binding) {
            admission.status = PartAdmissionStatus::Unavailable;
            return admission;
        }
        if (GetDataEntityIdValid(source->data->lifetimeScope)) {
            const auto lifetime = source->data->lifetime.lock();
            auto lease = lifetime ? lifetime->StartResourceUse(source->data->self, "part-segmentation-reader") : nullptr;
            if (!lease) { admission.status = PartAdmissionStatus::Unavailable; return admission; }
            auto retained = std::make_shared<std::pair<VtkImageGridSnapshot, std::shared_ptr<const DataResourceLease>>>(source, std::move(lease));
            auto input = std::make_shared<VtkImageGridView>(*source);
            input->data = DataSnapshot(retained, retained->first->data.get());
            source = std::move(input);
        }
        const std::uint64_t requestId = GetNextRequestId();
        std::ostringstream parameters;
        parameters.imbue(std::locale::classic());
        parameters << std::setprecision(std::numeric_limits<double>::max_digits10)
            << "threshold=" << params.threshold << ";minPartVoxels=" << params.minPartVoxels;
        auto canonicalParameters = parameters.str();
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
        ClearEditState();
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
        m_operation = {};
        m_requestParameters = std::move(canonicalParameters);
        m_operation.operation = { std::string(featureId), m_host->GetAttachmentId(), requestId };
        m_operation.stateRevision = 1;
        m_operation.status = FeatureRunStatus::Preparing;
        m_operation.inputs = { { "source-volume", m_requestSource->data->self } };
        if (expectedResultRevision != 0) {
            m_operation.inputs.push_back({ "previous-labels", m_stateBeforeRequest.labelMap });
            m_operation.inputs.push_back({ "previous-result", m_stateBeforeRequest.resultSet });
        }
        return admission;
    }

    if (request.action == PartSegmentationAction::Stop) {
        if (m_editCandidate) {
            (void)ClearEditPreview(m_editCandidate->requestId);
        }
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
    if ((m_service->GetIsBusy() || m_activeRequestId != 0) && !isHideRequest) {
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
    auto state = m_state;
    if (m_isPublishing.load(std::memory_order_acquire)) state.status = PartSegmentationStatus::Committing;
    return state;
}

std::vector<FeatureOperationState> PartSegmentationHostFeature::Impl::GetOperationStates() const
{
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing) return {};
    std::vector<FeatureOperationState> states;
    if (m_operation.operation.requestId != 0) {
        auto current = m_operation;
        const auto execution = m_service
            ? m_service->GetExecutionState(current.operation.requestId) : std::nullopt;
        if (execution && m_activeRequestId != 0) {
            current.stateRevision += execution->stateRevision;
            if (m_activeRequestId != 0 && execution->status != FeatureRunStatus::Idle) {
                current.status = execution->status;
                current.progress = execution->progress;
            }
        }
        states.push_back(std::move(current));
    }
    if (m_resultOperation.operation.requestId != 0
        && m_resultOperation.operation.requestId != m_operation.operation.requestId)
        states.push_back(m_resultOperation);
    if (!m_bindings.empty() && m_displayOperation.operation.requestId != 0
        && m_displayOperation.operation.requestId != m_resultOperation.operation.requestId
        && m_displayOperation.operation.requestId != m_operation.operation.requestId)
        states.push_back(m_displayOperation);
    return states;
}

std::shared_ptr<const PartSetSnapshot>
PartSegmentationHostFeature::Impl::GetPartSetSnapshot() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_publicSnapshot;
}

std::shared_ptr<const PartEditPreview>
PartSegmentationHostFeature::Impl::GetEditPreview() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_editPreview;
}

void PartSegmentationHostFeature::Impl::ClearEditState()
{
    if (m_editCandidate) CancelEditComplete(m_editCandidate->requestId);
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_editPreview.reset();
    }
    m_editCandidate.reset();
    m_editRequest.reset();
    m_editInputs.clear();
    m_editExpected.clear();
    m_editProvenance.reset();
    m_undo.clear();
    m_redo.clear();
    m_commitUndo.clear();
    m_commitRedo.clear();
}

void PartSegmentationHostFeature::Impl::CancelEditComplete(std::uint64_t previewId)
{
    if (m_operation.operation.requestId == previewId && m_operation.status == FeatureRunStatus::Ready) {
        m_operation.status = FeatureRunStatus::Cancelled;
        ++m_operation.stateRevision;
    }
    const std::lock_guard<std::mutex> lock(m_pendingCompleteMutex);
    for (const auto& weak : m_pendingCompleteResults) {
        const auto result = weak.result.lock();
        if (result && result->requestId == previewId && result->status == PartResultStatus::PreviewReady) {
            result->status = PartResultStatus::Cancelled;
            result->failureReason = PartFailureReason::Cancelled;
            result->message = "Edit preview was cancelled before completion delivery.";
        }
    }
}

std::optional<PartSegmentationHostFeature::Impl::HistoryEntry>
PartSegmentationHostFeature::Impl::GetHistoryEntry() const
{
    const auto state = GetState();
    if (!m_data || !m_catalogView || !m_activeLabels || !m_activeLabels->data) return {};
    const auto graph = m_data->GetDataGraph();
    const auto result = m_data->GetData(graph, state.resultSet);
    const auto collection = result
        ? std::dynamic_pointer_cast<const DataCollectionPayload>(result->payload) : nullptr;
    if (!collection) return {};
    const auto found = std::find_if(collection->GetItems().begin(), collection->GetItems().end(),
        [](const auto& item) { return item.role == "catalog"; });
    if (found == collection->GetItems().end()) return {};
    return HistoryEntry{ m_activeLabels->data, found->data, m_catalogView };
}

bool PartSegmentationHostFeature::Impl::GetHistoryBytes(std::size_t& bytes)
{
    bytes = 0;
    if (!m_data) return false;
    m_previewRetained.erase(std::remove_if(m_previewRetained.begin(), m_previewRetained.end(),
        [](const auto& item) { return item.labels.expired() && item.parts.expired(); }), m_previewRetained.end());
    if (m_previewRetained.size() >= 4096) return false;
    // 只扫有界修订 metadata，不扫描任何体素。历史即使已被 Undo 栈弹出仍计费。
    DataQuery query;
    query.producerId = std::string(featureId);
    constexpr std::size_t revisionLimit = 4096;
    query.limit = revisionLimit + 1;
    const auto data = m_data->GetDataQuery(m_data->GetDataGraph(), query);
    if (data.data.size() > revisionLimit) return false;
    const auto add = [&](std::size_t count, std::size_t width) {
        if (width != 0 && count > (std::numeric_limits<std::size_t>::max() - bytes) / width) return false;
        bytes += count * width;
        return bytes <= m_config.maxHistoryBytes;
    };
    // 撤销/重做修订和外部候选可共享同一不可变标签缓冲，只计费一次。
    // 去重表由既有修订/候选数量上限约束；不同分配仍独立计费。
    std::vector<const void*> countedLabels;
    countedLabels.reserve(data.data.size() + m_previewRetained.size());
    if (!add(countedLabels.capacity(), sizeof(const void*))) return false;
    const auto addLabels = [&](const auto& values) {
        using Item = typename std::decay_t<decltype(values)>::element_type::value_type;
        if (!values) return false;
        const auto* identity = static_cast<const void*>(values.get());
        if (std::find(countedLabels.begin(), countedLabels.end(), identity) != countedLabels.end()) return true;
        countedLabels.push_back(identity);
        return add(values->capacity(), sizeof(Item));
    };
    for (const auto& revision : data.data) {
        if (!revision || !revision->payload || !add(1, 2048)
            || !add(revision->inputs.capacity(), sizeof(DataInputRef) + 128)
            || (revision->provenance && !add(revision->provenance->canonicalParameters.capacity(), 1))) return false;
        if (const auto labels = std::dynamic_pointer_cast<const LabelMap3DPayload>(revision->payload)) {
            const bool fits = std::visit(addLabels, labels->GetValues());
            if (!fits) return false;
        }
        else if (const auto catalog = std::dynamic_pointer_cast<const PartCatalogPayload>(revision->payload)) {
            std::size_t catalogBytes = 0;
            if (!catalog->GetCatalog() || !GetPartCatalogStorageBytes(*catalog->GetCatalog(), catalogBytes)
                || !add(1, catalogBytes)) return false;
        }
        else if (const auto table = std::dynamic_pointer_cast<const RecordTablePayload>(revision->payload)) {
            for (const auto& column : table->GetColumns()) {
                const bool fits = std::visit([&](const auto& values) {
                    using Item = typename std::decay_t<decltype(values)>::value_type;
                    if (!add(values.capacity(), sizeof(Item))) return false;
                    if constexpr (std::is_same_v<Item, std::string>) {
                        for (const auto& text : values) if (!add(text.capacity(), 1)) return false;
                    }
                    return true;
                }, column.values);
                if (!fits) return false;
            }
        }
        else if (const auto collection = std::dynamic_pointer_cast<const DataCollectionPayload>(revision->payload)) {
            if (!add(collection->GetItems().capacity(), sizeof(DataCollectionEntry) + 128)) return false;
        }
        else return false;
    }
    if (!add(m_previewRetained.capacity(), sizeof(PreviewRetention))) return false;
    for (const auto& item : m_previewRetained) {
        const auto labels = item.labels.lock();
        if (labels && !addLabels(labels)) return false;
        if (!item.parts.expired() && !add(1, item.partBytes)) return false;
    }
    if (!add(m_undo.capacity() + m_redo.capacity() + m_commitUndo.capacity() + m_commitRedo.capacity(), sizeof(HistoryEntry))) return false;
    return true;
}

DataProvenance PartSegmentationHostFeature::Impl::BuildEditProvenance(const PartEditRequest& request)
{
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(std::numeric_limits<double>::max_digits10) << std::boolalpha;
    const auto array = [&](const auto& values) {
        json << '[';
        bool isFirst = true;
        for (const auto& value : values) { if (!isFirst) json << ','; isFirst = false; json << value; }
        json << ']';
    };
    const auto binding = [&](const PartBindingRef& value) {
        array(std::array<std::uint64_t, 5>{ value.object.partSetId.high, value.object.partSetId.low,
            value.object.objectId.high, value.object.objectId.low, value.resultRevision });
    };
    const auto bindings = [&](const auto& values) {
        json << '[';
        bool isFirst = true;
        for (const auto& value : values) { if (!isFirst) json << ','; isFirst = false; binding(value); }
        json << ']';
    };
    const auto points = [&](const auto& values) {
        json << '[';
        bool isFirst = true;
        for (const auto& value : values) { if (!isFirst) json << ','; isFirst = false; array(value); }
        json << ']';
    };
    DataProvenance result{ std::string(featureId), "", "label-edit-1", "" };
    json << "{\"scopeExtent\":";
    if (request.scope.extent) array(*request.scope.extent); else json << "null";
    json << ",\"protectedParts\":"; bindings(request.scope.protectedParts);
    json << ",\"operation\":{";
    std::visit([&](const auto& op) {
        using Op = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<Op, PartHistoryEdit>) {
            result.operationId = op.isRedo ? "redo" : "undo";
            json << "\"isRedo\":" << op.isRedo;
        }
        else if constexpr (std::is_same_v<Op, PartMergeEdit>) {
            result.operationId = "merge";
            json << "\"parts\":"; bindings(op.parts);
        }
        else {
            json << "\"target\":"; binding(op.target);
            if constexpr (std::is_same_v<Op, PartBrushEdit>) {
                result.operationId = op.isErase ? "erase" : "brush";
                json << ",\"radiusMM\":" << op.radiusMM << ",\"sourcePoints\":"; points(op.sourcePoints);
                json << ",\"overwriteParts\":"; bindings(op.overwriteParts);
                json << ",\"isBackgroundAllowed\":" << op.isBackgroundAllowed << ",\"slice\":";
                if (!op.slice) json << "null";
                else {
                    json << "{\"origin\":"; array(op.slice->origin);
                    json << ",\"normal\":"; array(op.slice->normal);
                    json << ",\"thicknessMM\":" << op.slice->thicknessMM << '}';
                }
            }
            else if constexpr (std::is_same_v<Op, PartFillEdit>) {
                result.operationId = "fill";
                json << ",\"seed\":"; array(op.seed);
            }
            else if constexpr (std::is_same_v<Op, PartIslandEdit>) {
                result.operationId = "remove-islands";
                json << ",\"minIslandVoxels\":" << op.minIslandVoxels;
            }
            else if constexpr (std::is_same_v<Op, PartGrowEdit>) {
                result.operationId = "grow";
                json << ",\"minimum\":" << op.minimum << ",\"maximum\":" << op.maximum << ",\"seeds\":"; points(op.seeds);
                json << ",\"overwriteParts\":"; bindings(op.overwriteParts);
                json << ",\"isBackgroundAllowed\":" << op.isBackgroundAllowed;
            }
            else if constexpr (std::is_same_v<Op, PartSplitEdit>) {
                result.operationId = "split";
                result.algorithmVersion = "seed-geodesic-6-v1";
                json << ",\"seeds\":[";
                for (std::size_t i = 0; i < op.seeds.size(); ++i) {
                    if (i != 0) json << ',';
                    json << "{\"index\":"; array(op.seeds[i].imageIndex);
                    json << ",\"target\":" << op.seeds[i].target << '}';
                }
                json << "],\"barriers\":[";
                for (std::size_t i = 0; i < op.barriers.size(); ++i) {
                    if (i != 0) json << ',';
                    json << "{\"index\":"; array(op.barriers[i].imageIndex);
                    json << ",\"axis\":" << static_cast<unsigned>(op.barriers[i].axis) << '}';
                }
                json << ']';
            }
        }
    }, request.operation);
    json << "}}";
    result.canonicalParameters = json.str();
    return result;
}

PartSegmentationAdmission PartSegmentationHostFeature::Impl::SendEditRequest(
    PartEditRequest request, PartSegmentationCallback onComplete)
{
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_service || !m_data) return { PartAdmissionStatus::Unavailable, 0 };
    if (m_isPublishing.load(std::memory_order_acquire) || m_service->GetIsBusy()
        || m_activeRequestId != 0 || m_editCandidate) return { PartAdmissionStatus::Busy, 0 };
    SetBindingStale();
    if (!ClearPreview()) return { PartAdmissionStatus::Unavailable, 0 };
    const auto state = GetState();
    if (!GetHistorySourceSame(m_activeSource) || state.status == PartSegmentationStatus::Stale) {
        return { PartAdmissionStatus::Unavailable, 0 };
    }
    if (request.expectedLabelMap != state.labelMap || request.expectedCatalogRevision != state.catalogRevision)
        return { PartAdmissionStatus::RevisionConflict, 0 };
    if (!GetPartEditBytes(request)) return { PartAdmissionStatus::BudgetExceeded, 0 };
    try {
        const auto history = GetHistoryEntry();
        const auto graph = m_data->GetDataGraph();
        auto binding = GetResultBinding(graph);
        if (!history || !binding || binding->target != std::optional<DataRevisionRef>{state.resultSet}) {
            return { PartAdmissionStatus::RevisionConflict, 0 };
        }
        PartEditJob job;
        job.source = m_activeSource;
        job.previous = { m_labelValues, m_catalogView };
        job.request = request;
        job.maxWorkingBytes = m_config.maxWorkingBytes;
        job.timeoutMs = m_config.editTimeoutMs;
        std::vector<DataInputRef> inputs{ { "base-labels", state.labelMap }, { "base-catalog", history->catalogRef } };
        std::vector<DataExpectation> expected;
        const auto addExpected = [&](const DataRevisionRef& ref) {
            DataExpectation item;
            item.kind = DataExpectationKind::EntityHead;
            item.entityId = ref.entityId;
            item.expectedGeneration = ref.generation;
            expected.push_back(std::move(item));
        };
        addExpected(state.sourceRevision);
        addExpected(state.labelMap);
        addExpected(history->catalogRef);
        const auto mask = [&](const std::optional<DataRevisionRef>& ref, const char* role,
            std::shared_ptr<const LabelMap3DPayload>& payload) {
            if (!ref) return true;
            const auto data = m_data->GetData(graph, *ref);
            payload = data ? std::dynamic_pointer_cast<const LabelMap3DPayload>(data->payload) : nullptr;
            if (!payload || !payload->GetValid()) return false;
            inputs.push_back({ role, *ref });
            addExpected(*ref);
            return true;
        };
        if (!mask(request.scope.roiMask, "roi-mask", job.roiMask)
            || !mask(request.scope.protectionMask, "protection-mask", job.protectionMask)) {
            return { PartAdmissionStatus::InvalidRequest, 0 };
        }
        if (const auto restore = std::get_if<PartHistoryEdit>(&request.operation)) {
            if (request.scope.extent || request.scope.roiMask || request.scope.protectionMask
                || !request.scope.protectedParts.empty()) return { PartAdmissionStatus::InvalidRequest, 0 };
            const auto& stack = restore->isRedo ? m_redo : m_undo;
            if (stack.empty()) return { PartAdmissionStatus::Unavailable, 0 };
            const auto& item = stack.back();
            job.restoredPayload = std::dynamic_pointer_cast<const LabelMap3DPayload>(item.labels->payload);
            if (!job.restoredPayload) return { PartAdmissionStatus::Unavailable, 0 };
            job.restored = { job.restoredPayload->GetLabels(), item.catalog };
            inputs.push_back({ "restore-labels", item.labels->self });
            inputs.push_back({ "restore-catalog", item.catalogRef });
        }
        std::size_t historyBytes = 0;
        if (!GetHistoryBytes(historyBytes)) return { PartAdmissionStatus::BudgetExceeded, 0 };
        auto provenance = BuildEditProvenance(request);
        const auto requestBytes = *GetPartEditBytes(request);
        const auto limit = std::numeric_limits<std::size_t>::max();
        if (requestBytes > (limit - historyBytes) / 3U) return { PartAdmissionStatus::InvalidRequest, 0 };
        job.retainedBytes = historyBytes + requestBytes * 3U;
        const auto addRetained = [&](std::size_t count, std::size_t width) {
            if (width != 0 && count > (limit - job.retainedBytes) / width) return false;
            job.retainedBytes += count * width;
            return true;
        };
        if (!addRetained(provenance.canonicalParameters.capacity(), 5)
            || !addRetained(1, m_surfaceProduct ? m_surfaceProduct->actualBytes : 0)) {
            return { PartAdmissionStatus::InvalidRequest, 0 };
        }
        for (const auto& payload : { job.roiMask, job.protectionMask }) {
            if (payload && !std::visit([&](const auto& values) {
                using Item = typename std::decay_t<decltype(values)>::element_type::value_type;
                return values && addRetained(values->capacity(), sizeof(Item));
            }, payload->GetValues())) return { PartAdmissionStatus::InvalidRequest, 0 };
        }
        const auto requestId = GetNextRequestId();
        job.requestId = requestId;
        auto requestViews = m_activeViews;
        FeatureOperationState operation;
        operation.operation = {std::string(featureId), m_host->GetAttachmentId(), requestId};
        operation.stateRevision = 1;
        operation.status = FeatureRunStatus::Preparing;
        operation.inputs = {{"source-volume", m_activeSource->data->self}};
        operation.inputs.insert(operation.inputs.end(), inputs.begin(), inputs.end());
        const auto admission = m_service->StartEdit(std::move(job));
        if (admission != PartAdmissionStatus::Accepted) return { admission, 0 };
        m_operation = std::move(operation);
        m_editRequest = std::move(request);
        m_editInputs = std::move(inputs);
        m_editExpected = std::move(expected);
        m_editProvenance = std::move(provenance);
        m_requestSource = m_activeSource;
        m_requestResultBinding = std::move(*binding);
        m_requestViews = std::move(requestViews);
        m_activeRequestId = requestId;
        m_startCallback = std::move(onComplete);
        m_isSourceChanged = false;
        m_isStopRequested = false;
        SetRequestRunning(requestId, state.sourceRevision);
        return { PartAdmissionStatus::Accepted, requestId };
    }
    catch (...) { return { PartAdmissionStatus::Unavailable, 0 }; }
}

void PartSegmentationHostFeature::Impl::SetEditComplete(PartLabelCandidate candidate)
{
    if (m_isSourceChanged || !GetSourceSame(m_requestSource)) {
        candidate.status = PartResultStatus::Failed;
        candidate.failureReason = PartFailureReason::SourceChanged;
    }
    else if (m_isStopRequested) {
        candidate.status = PartResultStatus::Cancelled;
        candidate.failureReason = PartFailureReason::Cancelled;
    }
    if (candidate.status == PartResultStatus::Succeeded) {
        try {
            auto preview = std::make_shared<PartEditPreview>();
            preview->previewId = candidate.requestId;
            preview->sourceRevision = candidate.sourceRevision;
            preview->baseLabels = m_editRequest->expectedLabelMap;
            preview->labels = candidate.labels;
            preview->parts = candidate.catalog
                ? BuildPartSetSnapshot(*candidate.catalog, candidate.sourceRevision, false) : nullptr;
            if (!preview->labels || !preview->parts || !candidate.labelPayload) throw std::runtime_error("Invalid edit preview.");
            std::size_t partBytes = 0;
            if (!GetPartCatalogStorageBytes(*candidate.catalog, partBytes)) throw std::runtime_error("Invalid preview storage.");
            partBytes += preview->parts->parts.capacity() * sizeof(PartSnapshot);
            std::size_t retained = 0;
            if (!GetHistoryBytes(retained) || partBytes > m_config.maxHistoryBytes - retained
                || preview->labels->capacity() > (m_config.maxHistoryBytes - retained - partBytes) / sizeof(PartLabelId)) {
                candidate.failureReason = PartFailureReason::BudgetExceeded;
                throw std::runtime_error("Retained preview budget exceeded.");
            }
            m_previewRetained.push_back({ preview->labels, preview->parts, partBytes });
            m_editCandidate = std::move(candidate);
            {
                const std::lock_guard<std::mutex> lock(m_stateMutex);
                m_editPreview = preview;
                m_state = m_stateBeforeRequest;
            }
            const auto id = m_activeRequestId;
            const auto execution = m_service->GetExecutionState(id);
            m_operation.stateRevision = execution ? execution->stateRevision + 2 : m_operation.stateRevision + 1;
            m_operation.status = FeatureRunStatus::Ready;
            m_operation.progress = 1.0;
            m_operation.outputs.clear(); // Preview values have no formal DataGraph revisions.
            m_activeRequestId = 0;
            auto callback = std::move(m_startCallback);
            QueueComplete(std::move(callback), BuildResult(GetState(), id, PartResultStatus::PreviewReady,
                PartFailureReason::None, preview->parts->parts.size(), "Edit preview is ready; confirmation is required."));
            return;
        }
        catch (...) {
            candidate.status = PartResultStatus::Failed;
            if (candidate.failureReason != PartFailureReason::BudgetExceeded) candidate.failureReason = PartFailureReason::InternalError;
            candidate.message = "Unable to prepare edit preview.";
        }
    }
    SetRequestComplete(std::move(candidate));
}

PartMutationResult PartSegmentationHostFeature::Impl::ClearEditPreview(std::uint64_t previewId)
{
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing) return { PartMutationStatus::Unavailable, 0 };
    const auto state = GetState();
    if (m_isPublishing.load(std::memory_order_acquire) || m_activeRequestId != 0) return { PartMutationStatus::Busy, state.catalogRevision };
    if (!m_editCandidate || m_editCandidate->requestId != previewId) return { PartMutationStatus::StaleReference, state.catalogRevision };
    CancelEditComplete(previewId);
    m_editCandidate.reset();
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_editPreview.reset();
    }
    m_editRequest.reset();
    m_editInputs.clear();
    m_editExpected.clear();
    m_editProvenance.reset();
    m_requestSource.reset();
    m_requestViews.clear();
    return { PartMutationStatus::Succeeded, state.catalogRevision };
}

PartSegmentationAdmission PartSegmentationHostFeature::Impl::SetEditCommit(
    std::uint64_t previewId, PartSegmentationCallback onComplete)
{
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_service || !m_data) return { PartAdmissionStatus::Unavailable, 0 };
    if (m_isPublishing.load(std::memory_order_acquire) || m_activeRequestId != 0 || m_service->GetIsBusy()) return { PartAdmissionStatus::Busy, 0 };
    if (!m_editCandidate || !m_editRequest || m_editCandidate->requestId != previewId) return { PartAdmissionStatus::InvalidRequest, 0 };
    const auto state = GetState();
    PartFailureReason failure = PartFailureReason::None;
    try {
        std::size_t retained = 0, catalogBytes = 0;
        const auto& candidate = *m_editCandidate;
        if (!GetHistoryBytes(retained) || !candidate.catalog || !candidate.labels
            || !GetPartCatalogStorageBytes(*candidate.catalog, catalogBytes)) failure = PartFailureReason::BudgetExceeded;
        else {
            const auto add = [&](std::size_t count, std::size_t width) {
                if (width != 0 && count > (std::numeric_limits<std::size_t>::max() - retained) / width) return false;
                retained += count * width;
                return retained <= m_config.maxHistoryBytes;
            };
            if (!add(candidate.labels->capacity(), sizeof(PartLabelId))
                || !add(3, catalogBytes) || !add(candidate.catalog->partsByLabel.size(), 512)
                || !add(1, 16384)
                || !add(m_editProvenance ? m_editProvenance->canonicalParameters.capacity() : 0, 4)) {
                failure = PartFailureReason::BudgetExceeded;
            }
        }
        const auto current = GetHistoryEntry();
        if (!current || state.labelMap != m_editRequest->expectedLabelMap
            || state.catalogRevision != m_editRequest->expectedCatalogRevision) failure = PartFailureReason::RevisionConflict;
        if (failure == PartFailureReason::None) {
            m_commitUndo = m_undo;
            m_commitRedo = m_redo;
            const auto restore = std::get_if<PartHistoryEdit>(&m_editRequest->operation);
            if (!restore) { m_commitUndo.push_back(*current); m_commitRedo.clear(); }
            else if (restore->isRedo) {
                if (m_commitRedo.empty()) failure = PartFailureReason::RevisionConflict;
                else { m_commitUndo.push_back(*current); m_commitRedo.pop_back(); }
            }
            else {
                if (m_commitUndo.empty()) failure = PartFailureReason::RevisionConflict;
                else { m_commitRedo.push_back(*current); m_commitUndo.pop_back(); }
            }
            if (m_commitUndo.size() > m_config.maxUndoSteps) m_commitUndo.erase(m_commitUndo.begin());
            if (m_commitRedo.size() > m_config.maxUndoSteps) m_commitRedo.erase(m_commitRedo.begin());
        }
    }
    catch (...) { failure = PartFailureReason::BudgetExceeded; }
    auto candidate = std::move(*m_editCandidate);
    m_editCandidate.reset();
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_editPreview.reset();
    }
    const auto id = GetNextRequestId();
    candidate.requestId = id;
    m_operation.operation.requestId = id;
    m_operation.stateRevision = 1;
    m_operation.status = FeatureRunStatus::Preparing;
    m_operation.progress = 0.0;
    m_operation.outputs.clear();
    if (failure != PartFailureReason::None) {
        candidate.status = PartResultStatus::Failed;
        candidate.failureReason = failure;
        candidate.message = "Edit confirmation was rejected before publication.";
    }
    m_activeRequestId = id;
    m_startCallback = std::move(onComplete);
    SetRequestRunning(id, state.sourceRevision);
    SetRequestComplete(std::move(candidate));
    return { PartAdmissionStatus::Accepted, id };
}

PartMutationResult PartSegmentationHostFeature::Impl::SetPreviousPart(
    const std::uint64_t expectedCatalogRevision)
{
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_service) {
        return { PartMutationStatus::Unavailable, 0 };
    }
    if (m_isPublishing.load(std::memory_order_acquire) || m_editCandidate)
        return { PartMutationStatus::Busy, GetState().catalogRevision };
    SetBindingStale();
    const auto state = GetState();
    if (m_service->GetIsBusy() || m_activeRequestId != 0) {
        return { PartMutationStatus::Busy, state.catalogRevision };
    }
    const auto snapshot = GetPartSetSnapshot();
    if (state.status == PartSegmentationStatus::Stale
        || (snapshot && snapshot->isStale)) {
        return { PartMutationStatus::StaleReference, state.catalogRevision };
    }
    if (!snapshot) return { PartMutationStatus::Unavailable, 0 };
    if (snapshot->catalogRevision != expectedCatalogRevision) {
        return { PartMutationStatus::RevisionConflict, snapshot->catalogRevision };
    }
    if (snapshot->parts.empty()) {
        return { PartMutationStatus::NotFound, snapshot->catalogRevision };
    }
    const auto selected = std::find_if(snapshot->parts.begin(), snapshot->parts.end(),
        [](const PartSnapshot& part) { return part.presentation.isSelected; });
    const auto previous = selected == snapshot->parts.end() || selected == snapshot->parts.begin()
        ? snapshot->parts.end() - 1 : selected - 1;
    PartStatePatch patch;
    patch.isSelected = true;
    return SetPartState(previous->binding, patch, expectedCatalogRevision);
}

PartMutationResult PartSegmentationHostFeature::Impl::SetPartState(
    const PartBindingRef& part,
    const PartStatePatch& patch,
    const std::uint64_t expectedCatalogRevision)
{
    PartMutationResult result;
    if (!m_isAttached || !GetIsOwnerThread() || m_isClosing || !m_service) {
        result.status = PartMutationStatus::Unavailable;
        return result;
    }
    if (m_isPublishing.load(std::memory_order_acquire) || m_editCandidate
        || m_service->GetIsBusy() || m_activeRequestId != 0) {
        result.status = PartMutationStatus::Busy;
        result.catalogRevision = GetState().catalogRevision;
        return result;
    }
    SetBindingStale();
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
        if (m_displayLabels == state.labelMap) {
            for (const auto& binding : m_bindings) controls.push_back(binding.control);
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

        auto batch = m_data->StartDataChanges();
        if (!batch) {
            (void)SetPartStates(controls, *previousStates, *nextStates);
            return { PartMutationStatus::Unavailable, state.catalogRevision };
        }
        auto nextState = GetState();
        const auto binding = GetResultBinding(m_data->GetDataGraph());
        if (!binding || binding->target != std::optional<DataRevisionRef>{state.resultSet}
            || !SetCatalogCommit(*candidate, m_activeSource, *binding, {}, nextState)) {
            (void)SetPartStates(controls, *previousStates, *nextStates);
            return { PartMutationStatus::RevisionConflict, state.catalogRevision };
        }
        if (!controls.empty()) m_displayResultSet = nextState.resultSet;
        SetPublishedState(std::move(nextState), nextSnapshot);
        if (!controls.empty()) {
            m_hasDisplayPending = true;
            try {
                m_hasDisplayPending = !SendSceneDelta(GetNextRequestId(), FeatureScenePriority::Overlay,
                    m_activeSource, m_activeViews);
            }
            catch (...) {}
        }
        m_undo.clear();
        m_redo.clear();
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
    PartSegmentationState& state,
    const std::vector<DataInputRef>& editInputs,
    const std::vector<DataExpectation>& editExpected,
    const DataProvenance* editProvenance,
    DataSnapshot* publishedLabels,
    DataPreparedResource resource)
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
    const DataProvenance provenance = editProvenance ? *editProvenance : DataProvenance{
        std::string(featureId), labels ? "segment" : "edit-catalog", "1",
        labels ? m_requestParameters : "catalogRevision=" + std::to_string(catalog.catalogRevision) };
    DataTransaction transaction;
    transaction.expectations = editExpected;
    DataExpectation sourceExpected;
    sourceExpected.kind = DataExpectationKind::Binding;
    sourceExpected.binding = std::string(primaryVolumeBinding);
    sourceExpected.expectedBindingRevision = source->binding->revision;
    sourceExpected.isTargetChecked = true;
    sourceExpected.expectedTarget = source->data->self;
    transaction.expectations.push_back(std::move(sourceExpected));
    std::vector<DataInputRef> labelInputs = editProvenance
        ? std::vector<DataInputRef>{sourceInput} : m_operation.inputs;
    labelInputs.insert(labelInputs.end(), editInputs.begin(), editInputs.end());
    if (labels) transaction.outputs.push_back({
        labelRef.entityId, 0, DataTypes::labelMap3D,
        std::move(labelInputs), labels, provenance });
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
    auto scopes = m_resultScopes;
    if (GetDataEntityIdValid(source->data->lifetimeScope)) {
        if (scopes.size() >= 1024) return false;
        const auto scope = m_data->CreateDataEntityId();
        DataLifetimeRetirement retirement{scope, DataLifetimeStatus::Published, {}, true};
        retirement.expectedRevisions.reserve(transaction.outputs.size());
        for (auto& draft : transaction.outputs) {
            draft.lifetimeScope = scope;
            retirement.expectedRevisions.push_back({draft.entityId, draft.expectedGeneration + 1});
        }
        if (resource.lease) transaction.outputs.front().preparedResources.push_back(std::move(resource));
        scopes.push_back(std::move(retirement));
    }
    // DataGraph observer 可同步重入；查询保留整组旧投影，修改和 Detach 均拒绝。
    struct PublishingGuard final {
        explicit PublishingGuard(std::atomic<bool>& value) : flag(value), previous(value.exchange(true, std::memory_order_acq_rel)) {}
        ~PublishingGuard() { flag.store(previous, std::memory_order_release); }
        std::atomic<bool>& flag;
        bool previous;
    } guard(m_isPublishing);
    auto batch = m_data->StartDataChanges();
    if (!batch) return false;
    const auto committed = m_data->SetDataCommit(std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded) return false;
    m_resultScopes.swap(scopes);
    m_resultBinding = committed.bindings.back();
    if (publishedLabels) {
        for (const auto& output : committed.published) {
            if (output && output->self == labelRef) { *publishedLabels = output; break; }
        }
    }
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
    std::vector<std::string> requiredViewIds,
    const bool isEditCommitted) const noexcept
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
                    [](const auto& current) { return current.result.expired(); }),
                m_pendingCompleteResults.end());
            m_pendingCompleteResults.push_back({sharedResult, isEditCommitted});
        }
        const std::weak_ptr<TrustedDataPort> weakData = m_data;
        const std::weak_ptr<FeatureViewDirectory> weakViews = m_views;
        const auto send = [sharedCallback, sharedResult, isSent,
            weakData, weakViews, requiredInput, isEditCommitted,
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
                    sharedResult->status = isEditCommitted ? PartResultStatus::SucceededWithDisplayFailure : PartResultStatus::Failed;
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
                        sharedResult->status = isEditCommitted ? PartResultStatus::SucceededWithDisplayFailure : PartResultStatus::Failed;
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
                sharedResult->status = isEditCommitted ? PartResultStatus::SucceededWithDisplayFailure : PartResultStatus::Cancelled;
                sharedResult->failureReason = isEditCommitted ? PartFailureReason::DisplayFailed : PartFailureReason::Cancelled;
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
        const auto result = weakResult.result.lock();
        if (!result || (result->status != PartResultStatus::Succeeded
            && result->status != PartResultStatus::PreviewReady)) {
            continue;
        }
        result->status = weakResult.isEditCommitted ? PartResultStatus::SucceededWithDisplayFailure : PartResultStatus::Cancelled;
        result->failureReason = weakResult.isEditCommitted ? PartFailureReason::DisplayFailed : PartFailureReason::Cancelled;
        result->message = "Part completion was cancelled while detaching.";
    }
    m_pendingCompleteResults.erase(
        std::remove_if(
            m_pendingCompleteResults.begin(),
            m_pendingCompleteResults.end(),
            [](const auto& current) { return current.result.expired(); }),
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
    delta.hasDisplayUpdate = true;
    delta.inputs = { { "source-volume", source->data->self } };
    if (GetDataRevisionRefValid(m_displayLabels) && !m_bindings.empty()) {
        delta.inputs = m_displayOperation.inputs;
        delta.inputs.push_back({ "labels", m_displayLabels });
        if (GetDataRevisionRefValid(m_displayResultSet))
            delta.inputs.push_back({ "result-set", m_displayResultSet });
        for (const auto& binding : m_bindings) {
            delta.displays.push_back({ binding.viewId, std::string(featureId), "parts",
                m_displayLabels, m_displayOperation.operation });
        }
    }
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
    try {
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
            // 先登记候选 owner，保证 Attach 抛出时也能成对撤销。
            nextBindings.push_back({ service, candidate.overlay, candidate.control, view.id });
            viewIds.push_back(view.id);
            if (!candidate.control->SetPartStates(renderStates)
                || !service->AttachOverlay(candidate.overlay)) {
                RemoveBindings(nextBindings);
                return false;
            }
        }
        if (!m_host->SetActiveViews(viewIds)) {
            m_isActiveViewClearPending = true;
            RemoveBindings(nextBindings);
            return false;
        }
        m_isActiveViewClearPending = false;
        return true;
    }
    catch (...) {
        RemoveBindings(nextBindings);
        return false;
    }
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
    m_hasDisplayPending = false;
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
        if ((!m_bindings.empty() && m_displayLabels == state.labelMap)
            || state.status == PartSegmentationStatus::Stale
            || !GetDataRevisionRefValid(state.resultSet)) return true;
    }
    if (!isVisible) {
        const bool isDisplayRemoved = RemoveDisplay();
        state.isOverlayVisible = false;
        if (m_activeRequestId != 0 || m_editCandidate) {
            // 完成、失败或取消会恢复请求前状态，也必须保留新的显示偏好。
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            m_stateBeforeRequest.isOverlayVisible = false;
        }
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
    if (!SetDisplay(GetNextRequestId())) return false;
    state.isOverlayVisible = true;
    SetState(std::move(state));
    return true;
}

bool PartSegmentationHostFeature::Impl::SetDisplay(const std::uint64_t requestId)
{
    const auto renderStates = m_catalogView
        ? BuildPartRenderStateTable(*m_catalogView)
        : std::optional<PartRenderStateTable>{};
    if (!renderStates || !m_activeLabels || !m_activeLabels->data) return false;
    std::vector<std::string> previousViews;
    for (const auto& binding : m_bindings) previousViews.push_back(binding.viewId);
    auto nextOperation = m_resultOperation;
    std::vector<OverlayBinding> nextBindings;
    if (!AttachDisplay(
            m_labelImage,
            *renderStates,
            m_surfaceProduct,
            m_activeViews,
            nextBindings)) {
        (void)m_host->SetActiveViews(previousViews);
        return false;
    }
    // 旧资源保留到新展示描述被接纳；失败只卸载候选，不回滚已发布数据。
    const auto previousLabels = m_displayLabels;
    const auto previousResultSet = m_displayResultSet;
    m_displayLabels = m_activeLabels->data->self;
    m_displayResultSet = GetState().resultSet;
    std::swap(m_displayOperation, nextOperation);
    m_bindings.swap(nextBindings);
    bool isAccepted = false;
    try {
        isAccepted = SendSceneDelta(requestId, FeatureScenePriority::Scene, m_activeSource, m_activeViews);
    }
    catch (...) {}
    if (!isAccepted) {
        m_bindings.swap(nextBindings);
        m_displayLabels = previousLabels;
        m_displayResultSet = previousResultSet;
        std::swap(m_displayOperation, nextOperation);
        RemoveBindings(nextBindings);
        (void)m_host->SetActiveViews(previousViews);
        return false;
    }
    RemoveBindings(nextBindings);
    m_hasDisplayPending = false;
    return true;
}

bool PartSegmentationHostFeature::Impl::ClearResult()
{
    const auto current = GetState();
    const auto graph = m_data ? m_data->GetDataGraph() : DataGraphSnapshot{};
    const auto binding = GetResultBinding(graph);
    if (binding && binding->target && *binding->target == current.resultSet
        && binding->revision == m_resultBinding.revision) {
        DataTransaction transaction;
        transaction.bindings.push_back({ std::string(partResultBinding),
            binding->revision, true, binding->target, {} });
        if (m_data->SetDataCommit(std::move(transaction)).status
            != DataCommitStatus::Succeeded) return false;
    }
    const bool isDisplayRemoved = RemoveDisplay();
    ClearEditState();
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

bool PartSegmentationHostFeature::Impl::ClearResultScopes()
{
    if (!m_data) return m_resultScopes.empty();
    DataTransaction transaction;
    for (const auto& scope : m_resultScopes) {
        const auto state = m_data->GetDataLifetime(scope.scopeId);
        if (state.status == DataLifetimeStatus::Published) transaction.retireScopes.push_back(scope);
        else if (state.status != DataLifetimeStatus::Releasing && state.status != DataLifetimeStatus::Released) return false;
    }
    // All owned revisions retire together: undo/catalog edges are internal to this transaction.
    if (!transaction.retireScopes.empty()
        && m_data->SetDataCommit(std::move(transaction)).status != DataCommitStatus::Succeeded) return false;
    for (auto scope = m_resultScopes.begin(); scope != m_resultScopes.end();) {
        if (m_data->SetDataRelease(scope->scopeId).status == DataLifetimeStatus::Released)
            scope = m_resultScopes.erase(scope);
        else ++scope;
    }
    return m_resultScopes.empty();
}

void PartSegmentationHostFeature::Impl::SetBindingStale()
{
    const auto state = GetState();
    if (!GetDataRevisionRefValid(state.resultSet) || !m_data) return;
    const auto binding = GetResultBinding(m_data->GetDataGraph());
    if (binding && binding->revision == m_resultBinding.revision
        && binding->target == m_resultBinding.target) return;
    if (m_activeRequestId != 0 && m_service) {
        // 旧展示已退休后，允许显式新请求基于外部新绑定重新计算；只有
        // 本请求接纳之后的再次换绑才取消它，不能用旧投影版本反复取消新任务。
        if (binding && binding->revision == m_requestResultBinding.revision
            && binding->target == m_requestResultBinding.target) return;
        m_isStopRequested = true;
        m_service->StopRequest();
    }
    SetSourceStale();
}

void PartSegmentationHostFeature::Impl::SetSourceStale()
{
    if (m_editCandidate) {
        ClearEditState();
        m_requestSource.reset();
        m_requestViews.clear();
    }
    m_undo.clear();
    m_redo.clear();
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
    const bool isEdit = m_editRequest.has_value();
    struct CompletionGuard final {
        explicit CompletionGuard(std::atomic<bool>& value) : flag(value), previous(value.exchange(true, std::memory_order_acq_rel)) {}
        ~CompletionGuard() { flag.store(previous, std::memory_order_release); }
        std::atomic<bool>& flag;
        bool previous;
    } guard(m_isPublishing);
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
                || !candidate.labelPayload || !candidate.labelPayload->GetValid()
                || !candidate.labelImage
                || candidate.labelPayload->GetLabels() != candidate.labels) {
                throw std::runtime_error("Part candidate is invalid.");
            }
            // worker 已将标签冻结并建立只读显示壳，owner 不扫描或复制整幅标签。
            // 提交阶段只额外预留小型目录、表格和图修订。
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
            const auto labels = candidate.labelPayload;
            // 所有容器与显示壳分配都先于正式提交；成功后仅转移冻结 owner。
            auto nextViews = m_requestViews;
            auto labelView = std::make_shared<VtkLabelMapView>(VtkLabelMapView{ {}, candidate.labelImage });
            DataSnapshot labelData;
            auto batch = m_data->StartDataChanges();
            if (!batch) throw std::runtime_error("Part publication batch is unavailable.");
            DataPreparedResource resource;
            if (GetDataEntityIdValid(m_requestSource->data->lifetimeScope))
                resource = VtkPreparedDataView::BuildResourceUse(candidate.labelImage,
                    candidate.surface->surface, candidate.labels);
            if (!publicSnapshot || !renderStates
                || !SetCatalogCommit(*candidate.catalog, m_requestSource,
                    m_requestResultBinding, labels, nextState,
                    m_editInputs, m_editExpected,
                    m_editProvenance ? &*m_editProvenance : nullptr, &labelData, std::move(resource))) {
                reason = m_editRequest ? PartFailureReason::RevisionConflict : PartFailureReason::SourceChanged;
                throw std::runtime_error("Part graph transaction was rejected.");
            }
            isCommitted = true;
            if (m_editRequest) {
                m_undo.swap(m_commitUndo);
                m_redo.swap(m_commitRedo);
            }
            // 正式数据已经发布；之后的显示失败只能报告 presentation failure。
            nextState.status = PartSegmentationStatus::Succeeded;
            nextState.failureReason = PartFailureReason::None;
            nextState.requestId = requestId;
            nextState.progress = 1.0;
            m_operation.status = FeatureRunStatus::Succeeded;
            m_operation.progress = 1.0;
            m_operation.outputs = { nextState.labelMap, nextState.partTable, nextState.resultSet };
            const auto execution = m_service->GetExecutionState(requestId);
            m_operation.stateRevision = execution ? execution->stateRevision + 2 : 2;
            m_resultOperation = m_operation;
            m_surfaceProduct = std::move(candidate.surface);
            m_activeViews.swap(nextViews);
            m_activeSource = m_requestSource;
            const auto labelPayload = labelData
                ? std::dynamic_pointer_cast<const LabelMap3DPayload>(labelData->payload)
                : nullptr;
            // 将准备好的显示壳绑定到实际发布的图修订，始终由图中标签拥有数组。
            labelView->data = std::move(labelData);
            m_labelValues = candidate.labels;
            m_activeLabels = std::move(labelView);
            m_labelImage = m_activeLabels ? m_activeLabels->labels : nullptr;
            SetPublishedState(nextState, publicSnapshot);
            bool isDisplayed = m_labelImage && m_labelValues && labelPayload
                && labelPayload->GetLabels() == candidate.labels;
            if (nextState.isOverlayVisible && isDisplayed) {
                isDisplayed = SetDisplay(requestId);
            }
            else if (!nextState.isOverlayVisible) {
                isDisplayed = RemoveDisplay();
            }
            if (!isDisplayed) {
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
                    || reason == PartFailureReason::RevisionConflict
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
    if (!isCommitted) {
        m_operation.status = reason == PartFailureReason::Cancelled ? FeatureRunStatus::Cancelled
            : FeatureRunStatus::Failed;
        m_operation.outputs.clear();
        const auto execution = m_service->GetExecutionState(requestId);
        m_operation.stateRevision = execution ? execution->stateRevision + 2 : m_operation.stateRevision + 1;
    }
    m_requestSource.reset();
    m_requestViews.clear();
    m_activeRequestId = 0;
    m_isSourceChanged = false;
    m_isStopRequested = false;
    m_editRequest.reset();
    m_editInputs.clear();
    m_editExpected.clear();
    m_editProvenance.reset();
    m_commitUndo.clear();
    m_commitRedo.clear();
    m_isPublishing.store(guard.previous, std::memory_order_release);
    QueueComplete(std::move(callback), std::move(result),
        requiredInput, std::move(requiredViews), isEdit && isCommitted);
}

void PartSegmentationHostFeature::Impl::SetRequestFailed(
    const PartFailureReason reason)
{
    if (reason == PartFailureReason::SourceChanged) {
        SetSourceStale();
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    // 外部绑定已经退休旧投影时，旧任务的取消不能恢复 request 前的成功态。
    if (m_state.status == PartSegmentationStatus::Stale) return;
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
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    if (!m_impl || !m_impl->AttachHost(context)) return false;
    if (m_impl->AttachInput(weak_from_this())) return true;
    (void)m_impl->DetachHost();
    return false;
}

bool PartSegmentationHostFeature::DetachHost()
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    return !m_impl || m_impl->DetachHost();
}

bool PartSegmentationHostFeature::OnHostTick()
{
    // 同步完成回调可以 Detach 并释放调用方的最后一个 shared_ptr。
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    return m_impl && m_impl->OnHostTick();
}

PartSegmentationAdmission PartSegmentationHostFeature::SendRequest(
    PartSegmentationRequest request,
    PartSegmentationCallback onComplete)
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
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

std::vector<FeatureOperationState> PartSegmentationHostFeature::GetOperationStates() const
{
    return m_impl ? m_impl->GetOperationStates() : std::vector<FeatureOperationState>{};
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
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    return m_impl
        ? m_impl->SetPartState(part, patch, expectedCatalogRevision)
        : PartMutationResult{ PartMutationStatus::Unavailable, 0 };
}

PartMutationResult PartSegmentationHostFeature::SetPreviousPart(
    const std::uint64_t expectedCatalogRevision)
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    return m_impl ? m_impl->SetPreviousPart(expectedCatalogRevision)
        : PartMutationResult{ PartMutationStatus::Unavailable, 0 };
}

PartMutationResult PartSegmentationHostFeature::SetPartState(
    const HostSemanticTarget& target, const PartStatePatch& patch,
    const std::uint64_t expectedCatalogRevision)
{
    return m_impl ? m_impl->SetPartState(target, patch, expectedCatalogRevision)
        : PartMutationResult{ PartMutationStatus::Unavailable, 0 };
}

PartSegmentationAdmission PartSegmentationHostFeature::SendEditRequest(
    PartEditRequest request, PartSegmentationCallback onComplete)
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    return m_impl ? m_impl->SendEditRequest(std::move(request), std::move(onComplete))
        : PartSegmentationAdmission{ PartAdmissionStatus::Unavailable, 0 };
}

std::shared_ptr<const PartEditPreview> PartSegmentationHostFeature::GetEditPreview() const
{
    return m_impl ? m_impl->GetEditPreview() : nullptr;
}

PartSegmentationAdmission PartSegmentationHostFeature::SetEditCommit(
    std::uint64_t previewId, PartSegmentationCallback onComplete)
{
    [[maybe_unused]] const auto keepAlive = weak_from_this().lock();
    return m_impl ? m_impl->SetEditCommit(previewId, std::move(onComplete))
        : PartSegmentationAdmission{ PartAdmissionStatus::Unavailable, 0 };
}

PartMutationResult PartSegmentationHostFeature::ClearEditPreview(std::uint64_t previewId)
{
    return m_impl ? m_impl->ClearEditPreview(previewId)
        : PartMutationResult{ PartMutationStatus::Unavailable, 0 };
}
