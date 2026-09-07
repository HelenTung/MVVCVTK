#include "Host/SurfaceDeterminationHostFeature.h"
#include "Data/DataPayloads.h"

#include "Render/Contracts/OverlayService.h"
#include "SurfaceDeterminationService.h"
#include "SurfaceGenerationStore.h"
#include "SurfaceContracts.h"
#include "SurfaceOverlayStrategy.h"

#include <vtkCellArray.h>
#include <vtkImageData.h>
#include "Render/Contracts/SlicePlaneState.h"
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "surface-determination";
constexpr std::size_t completionBatchLimit = 8;

bool GetTargetsUsed(const HostViewTargets& targets)
{
    return !targets.viewIds.empty() || !targets.viewRoles.empty();
}

bool GetRoleSupported(const HostRenderViewRole role)
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D
        || role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

bool GetMethodValid(const SurfaceDeterminationMethod method)
{
    switch (method) {
    case SurfaceDeterminationMethod::GlobalIsoPreview:
    case SurfaceDeterminationMethod::LocalAdaptiveIso50:
    case SurfaceDeterminationMethod::GradientPeak:
    case SurfaceDeterminationMethod::AutomaticIso50:
        return true;
    default:
        return false;
    }
}

bool GetSelectionValid(const SurfaceComponentSelection selection)
{
    switch (selection) {
    case SurfaceComponentSelection::Largest:
    case SurfaceComponentSelection::Seeded:
    case SurfaceComponentSelection::All:
        return true;
    default:
        return false;
    }
}

bool GetOptionalPositive(const std::optional<double>& value)
{
    return !value || (std::isfinite(*value) && *value > 0.0);
}

bool GetStartValid(const SurfaceDeterminationStartParams& params)
{
    if ((params.method == SurfaceDeterminationMethod::AutomaticIso50 && params.initialIsoValue)
        || !SurfaceContract::GetInputValid(params)
        || !GetMethodValid(params.method)
        || !GetSelectionValid(params.componentSelection)
        || params.minimumObjectVoxels == 0
        || !std::isfinite(params.minimumContrast)
        || params.minimumContrast < 0.0
        || (params.initialIsoValue
            && !std::isfinite(*params.initialIsoValue))
        || !GetOptionalPositive(params.profileHalfLengthModel)
        || !GetOptionalPositive(params.profileSampleStepModel)
        || !GetOptionalPositive(params.maximumOffsetModel)
        || !GetOptionalPositive(params.profileSmoothingSigmaModel)) {
        return false;
    }
    if (params.componentSelection == SurfaceComponentSelection::Seeded
        && !params.seedModelPoint) {
        return false;
    }
    if (params.seedModelPoint
        && !std::all_of(
            params.seedModelPoint->begin(),
            params.seedModelPoint->end(),
            [](const double value) { return std::isfinite(value); })) {
        return false;
    }
    if (params.roiModelBounds) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double minimum = (*params.roiModelBounds)[axis * 2];
            const double maximum = (*params.roiModelBounds)[axis * 2 + 1];
            if (!std::isfinite(minimum)
                || !std::isfinite(maximum)
                || minimum >= maximum) {
                return false;
            }
        }
    }
    return true;
}

bool GetConfigValid(const SurfaceDeterminationConfig& config)
{
    if (config.maxWorkingBytes == 0) return false;
    auto params = config.defaultStart;
    if (!GetTargetsUsed(params.targetViews)) {
        params.targetViews.viewRoles.push_back(
            HostRenderViewRole::Primary3D);
    }
    return GetStartValid(params);
}

std::array<double, 3> GetSliceNormal(const HostRenderViewRole role)
{
    const auto orientation = role == HostRenderViewRole::TopDownSlice ? Orientation::Top_down
        : role == HostRenderViewRole::FrontBackSlice ? Orientation::Front_back : Orientation::Left_right;
    return SlicePlaneState::Build(orientation, {}).worldNormal;
}
std::shared_ptr<FeatureOverlay> CreateOverlay(
    const HostRenderViewRole role)
{
    if (role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D) {
        return std::make_shared<SurfaceOverlayStrategy>();
    }
    if (role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice) {
        return std::make_shared<SurfaceSliceOverlayStrategy>(
            GetSliceNormal(role));
    }
    return nullptr;
}

vtkSmartPointer<vtkPolyData> BuildDisplayData(
    const std::vector<SurfacePointRecord>& points,
    const std::vector<std::uint32_t>& triangleIndices)
{
    if (points.empty() || triangleIndices.empty()
        || triangleIndices.size() % 3U != 0U
        || points.size() > static_cast<std::size_t>(
            std::numeric_limits<vtkIdType>::max())) {
        return nullptr;
    }
    auto vtkPointsData = vtkSmartPointer<vtkPoints>::New();
    vtkPointsData->SetDataTypeToDouble();
    vtkPointsData->SetNumberOfPoints(
        static_cast<vtkIdType>(points.size()));
    for (std::size_t index = 0; index < points.size(); ++index) {
        vtkPointsData->SetPoint(
            static_cast<vtkIdType>(index),
            points[index].positionModel.data());
    }

    auto cells = vtkSmartPointer<vtkCellArray>::New();
    cells->AllocateEstimate(
        static_cast<vtkIdType>(triangleIndices.size() / 3U), 3);
    for (std::size_t index = 0;
        index < triangleIndices.size(); index += 3) {
        const std::array<vtkIdType, 3> ids{
            static_cast<vtkIdType>(triangleIndices[index]),
            static_cast<vtkIdType>(triangleIndices[index + 1]),
            static_cast<vtkIdType>(triangleIndices[index + 2])
        };
        if (ids[0] < 0 || ids[1] < 0 || ids[2] < 0
            || ids[0] >= static_cast<vtkIdType>(points.size())
            || ids[1] >= static_cast<vtkIdType>(points.size())
            || ids[2] >= static_cast<vtkIdType>(points.size())) {
            return nullptr;
        }
        cells->InsertNextCell(3, ids.data());
    }
    auto surface = vtkSmartPointer<vtkPolyData>::New();
    surface->SetPoints(vtkPointsData);
    surface->SetPolys(cells);
    return surface;
}

vtkSmartPointer<vtkPolyData> BuildDisplayData(
    const SurfaceGenerationSnapshot& snapshot)
{
    if (!snapshot.points || !snapshot.triangleIndices) return nullptr;
    return BuildDisplayData(*snapshot.points, *snapshot.triangleIndices);
}

SurfaceDeterminationResult BuildResult(
    const std::uint64_t requestId,
    const SurfaceResultStatus status,
    const SurfaceFailureReason failureReason,
    const DataRevisionRef sourceRevision,
    const std::uint64_t resultRevision,
    const std::uint64_t pointCount,
    const std::uint32_t objectCount,
    std::string message)
{
    SurfaceDeterminationResult result;
    result.requestId = requestId;
    result.status = status;
    result.failureReason = failureReason;
    result.sourceRevision = sourceRevision;
    result.resultRevision = resultRevision;
    result.pointCount = pointCount;
    result.objectCount = objectCount;
    result.message = std::move(message);
    return result;
}

} // namespace

class SurfaceDeterminationHostFeature::Impl final {
public:
    explicit Impl(SurfaceDeterminationConfig config)
        : m_config(std::move(config))
    {
        m_state.isOverlayVisible = m_config.isOverlayVisible;
        m_stateBeforeRequest = m_state;
    }

    bool AttachHost(const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    SurfaceDeterminationAdmission SendRequest(
        SurfaceDeterminationRequest request,
        SurfaceDeterminationCallback onComplete);
    SurfaceDeterminationState GetState() const;
    std::vector<FeatureOperationState> GetOperationStates() const;
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetSurfaceSnapshot() const;
    std::shared_ptr<const SurfaceGenerationSnapshot> GetSurfaceSnapshot(std::string_view scope) const
        { return m_store.GetCurrentGeneration(scope); }
    std::shared_ptr<const SurfaceGenerationSnapshot> GetSurfaceSnapshot(DataRevisionRef revision) const
        { return m_store.GetGeneration(revision); }
    std::shared_ptr<const SurfaceGenerationSnapshot> GetPreviewSnapshot() const
        { const std::lock_guard<std::mutex> lock(m_stateMutex); return m_preview; }

private:
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureOverlay> overlay;
        std::string viewId;
    };

    struct RequestEntry final {
        VtkImageGridSnapshot source;
        std::vector<HostFeatureView> views;
        SurfaceDeterminationCallback onComplete;
        bool isSourceChanged = false;
        DataBinding resultBinding;
        std::shared_ptr<std::atomic<bool>> completeActive;
        FeatureOperationState operation;
        SurfaceDeterminationStartParams params;
        bool isSuperseded = false;
    };

    bool GetIsOwnerThread() const noexcept;
    std::uint64_t GetNextRequestId() noexcept;
    bool GetSourceSame(const VtkImageGridSnapshot& source,
        DataPublishPolicy policy = DataPublishPolicy::RequireCurrentInputs) const;
    std::shared_ptr<const SurfaceGenerationSnapshot> BuildGeneration(
        const RequestEntry& request, SurfaceAlgorithmResult& result, std::uint64_t requestId, bool isFormal);
    void SetTransientResult(const RequestEntry& request, SurfaceAlgorithmResult& result, std::uint64_t requestId);
    bool ClearPreview();
    std::shared_ptr<const SurfaceGenerationSnapshot> GetDisplayGeneration() const;
    std::vector<HostFeatureView> GetTargetViews(
        const HostViewTargets& targets) const;
    static std::vector<std::string> GetViewIds(
        const std::vector<HostFeatureView>& views);
    void SetState(SurfaceDeterminationState state);
    void SetRequestRunning(
        std::uint64_t requestId,
        DataRevisionRef sourceRevision,
        bool isFirstRequest);
    void SetRequestProgress(const SurfaceRequestProgress& progress);
    void SendComplete(
        SurfaceDeterminationCallback callback,
        SurfaceDeterminationResult result,
        DataBindingRevision bindingRevision = 0,
        std::shared_ptr<std::atomic<bool>> requestActive = {}) const noexcept;
    bool BuildBindings(
        vtkSmartPointer<vtkPolyData> displayData,
        const std::vector<HostFeatureView>& views,
        const VtkImageGridSnapshot& source,
        std::vector<OverlayBinding>& bindings);
    static void RemoveBindings(
        std::vector<OverlayBinding>& bindings) noexcept;
    bool RemoveDisplay();
    bool SendDisplayDelta();
    bool SetVisibility(bool isVisible);
    bool ClearResult();
    bool ClearResultBinding();
    DataBinding GetResultBinding() const;
    DataBinding GetResultBinding(std::string_view scope) const;
    void SetBindingProjection();
    bool SetDisplayProjection();
    void SetDisplayFailure(std::uint64_t requestId = 0);
    void SetSourceStale();
    void SetRequestComplete(SurfaceJobComplete complete);
    void SetRequestFailed(
        const RequestEntry& request,
        const SurfaceAlgorithmResult& result);
    DataSnapshot SetRequestSucceeded(
        const RequestEntry& request,
        SurfaceAlgorithmResult result,
        std::uint64_t requestId);

    SurfaceDeterminationConfig m_config;
    mutable std::mutex m_stateMutex;
    SurfaceDeterminationState m_state;
    SurfaceDeterminationState m_stateBeforeRequest;
    FeatureOperationState m_lastOperation;
    FeatureOperationState m_displayOperation;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<SurfaceDeterminationService> m_service;
    SurfaceGenerationStore m_store;
    std::map<std::uint64_t, RequestEntry> m_requests;
    std::string m_activeScope;
    std::shared_ptr<const SurfaceGenerationSnapshot> m_preview;
    VtkImageGridSnapshot m_previewSource;
    std::vector<HostFeatureView> m_previewViews;
    VtkImageGridSnapshot m_activeSource;
    std::vector<HostFeatureView> m_activeViews;
    std::vector<OverlayBinding> m_bindings;
    vtkSmartPointer<vtkPolyData> m_displayData;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextRequestId = 1;
    std::uint64_t m_latestRequestId = 0;
    std::uint64_t m_resultRevision = 0;
    DataBinding m_resultBinding;
    bool m_isDisplayPending = false;
    bool m_isAttached = false;
    bool m_isDetaching = false;
    bool m_isStaleCleanupPending = false;
    std::shared_ptr<std::atomic<bool>> m_completeActive;
};

bool SurfaceDeterminationHostFeature::Impl::AttachHost(
    const HostFeatureContext& context)
{
    if (m_isAttached || !context.views || !context.data || !context.host
        || !GetConfigValid(m_config)) {
        return false;
    }
    const auto graph = context.data->GetDataGraph();
    if (!graph.view || (graph.view->GetDataFacets(surfaceGenerationType).empty()
        && !context.data->SetDataType(DataTypeDescriptor{
            surfaceGenerationType, { surfaceGenerationFacet },
            [](const IDataPayload& value, std::string&) {
                const auto* payload = dynamic_cast<const SurfaceGenerationPayload*>(&value);
                const auto generation = payload ? payload->GetGeneration() : nullptr;
                return generation && generation->points && generation->triangleIndices
                    && generation->objects && GetDataRevisionRefValid(generation->sourceRevision);
            } }))) return false;
    try {
        m_service = std::make_unique<SurfaceDeterminationService>(
            [weakHost = std::weak_ptr<FeatureHostControl>(context.host)] {
                if (const auto host = weakHost.lock())
                    (void)host->SendWorkAvailable();
            });
    }
    catch (...) {
        return false;
    }
    m_views = context.views;
    m_data = context.data;
    m_store.SetDataPort(m_data);
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
    m_completeActive = std::make_shared<std::atomic<bool>>(true);
    m_lastOperation = {};
    m_displayOperation = {};
    m_isAttached = true;
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) return true;
    if (!GetIsOwnerThread() || !m_service || m_isDetaching) return false;
    m_isDetaching = true;
    if (m_completeActive) m_completeActive->store(false);
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.stage = SurfaceDeterminationStage::Stopping;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!m_service->Stop(deadline) || !ClearResult()) {
        m_isDetaching = false;
        return false;
    }
    // 先完成解绑并取走回调，用户代码才可重入 Detach/Attach；不迭代可重入的成员 map。
    auto requests = std::move(m_requests);
    m_requests.clear();
    const auto resultRevision = m_resultRevision;
    m_latestRequestId = 0;
    m_activeSource.reset(); m_activeViews.clear();
    m_service.reset(); m_views.reset(); m_data.reset();
    m_store.SetDataPort({}); m_host.reset(); m_ownerThread = {};
    m_isAttached = false; m_isDetaching = false; m_isStaleCleanupPending = false;
    SurfaceDeterminationState idle;
    idle.isOverlayVisible = m_config.isOverlayVisible;
    SetState(std::move(idle));
    for (auto& item : requests) {
        auto& request = item.second;
        auto result = BuildResult(item.first, SurfaceResultStatus::Cancelled,
            SurfaceFailureReason::Cancelled, request.source && request.source->data
                ? request.source->data->self : DataRevisionRef{}, resultRevision, 0, 0,
            "Surface request was cancelled by detach.");
        result.purpose = SurfaceContract::GetPurpose(request.params);
        result.resultScope = request.params.resultScope;
        SendComplete(std::move(request.onComplete), std::move(result));
    }
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached || m_isDetaching || !GetIsOwnerThread() || !m_service || !m_data) {
        return false;
    }
    if (m_isStaleCleanupPending && RemoveDisplay()) {
        m_isStaleCleanupPending = false;
    }
    if (m_previewSource && !GetSourceSame(m_previewSource)) (void)ClearPreview();
    if (m_activeSource && !GetSourceSame(m_activeSource)) {
        SetSourceStale();
    }
    SetBindingProjection();
    for (auto& item : m_requests) {
        RequestEntry& request = item.second;
        if (!request.isSourceChanged && !GetSourceSame(request.source, request.params.sourcePolicy)) {
            request.isSourceChanged = true;
            m_service->StopRequest(item.first);
            // 完成时只更新该请求；不能清除另一个来源/作用域的正式结果。
        }
    }
    if (m_latestRequestId != 0) {
        const auto progress = m_service->GetProgress(m_latestRequestId);
        if (progress) SetRequestProgress(*progress);
    }
    for (std::size_t index = 0;
        index < completionBatchLimit; ++index) {
        if (!m_isAttached || !m_service) break;
        auto complete = m_service->GetComplete();
        if (!complete) break;
        SetRequestComplete(std::move(*complete));
    }
    return true;
}

SurfaceDeterminationAdmission
SurfaceDeterminationHostFeature::Impl::SendRequest(
    SurfaceDeterminationRequest request,
    SurfaceDeterminationCallback onComplete)
{
    SurfaceDeterminationAdmission admission;
    if (!m_isAttached || !GetIsOwnerThread() || !m_service) {
        admission.status = SurfaceAdmissionStatus::Unavailable;
        return admission;
    }
    if (m_isDetaching) {
        admission.status = SurfaceAdmissionStatus::Stopping;
        return admission;
    }
    SetBindingProjection();

    if (request.action == SurfaceDeterminationAction::Start) {
        auto params = request.start.value_or(m_config.defaultStart);
        if (request.isVisible || request.targetRequestId != 0) return admission;
        if (!GetStartValid(params)) return admission;
        auto targetViews = GetTargetViews(params.targetViews);
        if (GetTargetsUsed(params.targetViews) && targetViews.empty()) return admission;
        if (params.sourcePolicy == DataPublishPolicy::AllowHistoricalResult
            && (!params.sourceVolume || GetTargetsUsed(params.targetViews)
                || SurfaceContract::GetPurpose(params) != SurfaceTaskPurpose::Determine)) return admission;
        auto source = m_data ? (params.sourceVolume
            ? m_data->GetImageGrid(m_data->GetDataGraph(), *params.sourceVolume)
            : m_data->GetPrimaryImage()) : nullptr;
        if (!source || !source->image || !source->data
            || !GetSourceSame(source, params.sourcePolicy)) {
            admission.status = SurfaceAdmissionStatus::Unavailable;
            return admission;
        }
        params.sourceVolume = source->data->self;
        params.purpose = SurfaceContract::GetPurpose(params);
        const std::uint64_t requestId = GetNextRequestId();
        const bool isFirstRequest = m_requests.empty();
        auto requestItem = m_requests.end();
        try {
            const auto inserted = m_requests.emplace(
                requestId,
                RequestEntry{
                    source,
                    targetViews,
                    std::move(onComplete),
                    false,
                    GetResultBinding(params.resultScope),
                    m_completeActive
                });
            if (!inserted.second) return admission;
            requestItem = inserted.first;
            requestItem->second.params = params;
            admission.status = m_service->Start(
                source, params, m_config.maxWorkingBytes, requestId);
        }
        catch (...) {
            if (requestItem != m_requests.end()) {
                m_requests.erase(requestItem);
            }
            admission.status = SurfaceAdmissionStatus::Unavailable;
            return admission;
        }
        if (admission.status != SurfaceAdmissionStatus::Accepted) {
            m_requests.erase(requestItem);
            return admission;
        }
        admission.requestId = requestId;
        for (auto& item : m_requests) if (item.first != requestId
            && item.second.params.resultScope == params.resultScope
            && item.second.params.purpose == params.purpose) item.second.isSuperseded = true;
        requestItem->second.operation.operation = {
            std::string(featureId), m_host->GetAttachmentId(), requestId };
        requestItem->second.operation.inputs = { { "source-volume", source->data->self } };
        requestItem->second.operation.status = FeatureRunStatus::Preparing;
        requestItem->second.operation.stateRevision = 1;
        m_latestRequestId = requestId;
        SetRequestRunning(
            requestId,
            requestItem->second.source->data->self,
            isFirstRequest);
        return admission;
    }

    if (request.action == SurfaceDeterminationAction::Stop) {
        if (request.start || request.isVisible) return admission;
        const std::uint64_t targetRequestId = request.targetRequestId == 0
            ? m_latestRequestId : request.targetRequestId;
        if (targetRequestId == 0
            || m_requests.find(targetRequestId) == m_requests.end()) {
            return admission;
        }
        if (!m_service->StopRequest(targetRequestId)) return admission;
        const std::uint64_t controlRequestId = GetNextRequestId();
        admission = { SurfaceAdmissionStatus::Accepted, controlRequestId };
        if (targetRequestId == m_latestRequestId) {
            const std::lock_guard<std::mutex> lock(m_stateMutex);
            m_state.stage = SurfaceDeterminationStage::Stopping;
        }
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                controlRequestId,
                SurfaceResultStatus::Succeeded,
                SurfaceFailureReason::None,
                state.sourceRevision,
                m_resultRevision,
                state.pointCount,
                state.objectCount,
                "Surface stop was requested."));
        return admission;
    }

    if (m_service->GetIsBusy() || !m_requests.empty()) {
        admission.status = SurfaceAdmissionStatus::Busy;
        return admission;
    }
    const std::uint64_t requestId = GetNextRequestId();
    if (request.action == SurfaceDeterminationAction::SetVisibility
        && request.isVisible
        && SetVisibility(*request.isVisible)) {
        admission = { SurfaceAdmissionStatus::Accepted, requestId };
    }
    else if (request.action == SurfaceDeterminationAction::ClearPreview
        && !request.start && !request.isVisible && request.targetRequestId == 0 && ClearPreview()) {
        admission = { SurfaceAdmissionStatus::Accepted, requestId };
    }
    else if (request.action == SurfaceDeterminationAction::Clear
        && !request.start && !request.isVisible
        && request.targetRequestId == 0
        && ClearResult()) {
        admission = { SurfaceAdmissionStatus::Accepted, requestId };
    }
    if (admission.status == SurfaceAdmissionStatus::Accepted) {
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                requestId,
                SurfaceResultStatus::Succeeded,
                SurfaceFailureReason::None,
                state.sourceRevision,
                state.resultRevision,
                state.pointCount,
                state.objectCount,
                "Surface control request succeeded."));
    }
    return admission;
}

SurfaceDeterminationState
SurfaceDeterminationHostFeature::Impl::GetState() const
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_state;
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::Impl::GetSurfaceSnapshot() const
{
    return m_store.GetCurrentGeneration();
}

bool SurfaceDeterminationHostFeature::Impl::GetIsOwnerThread() const noexcept
{
    return m_ownerThread == std::this_thread::get_id();
}

std::uint64_t
SurfaceDeterminationHostFeature::Impl::GetNextRequestId() noexcept
{
    const std::uint64_t requestId = m_nextRequestId++;
    if (m_nextRequestId == 0) m_nextRequestId = 1;
    return requestId == 0 ? m_nextRequestId++ : requestId;
}

bool SurfaceDeterminationHostFeature::Impl::GetSourceSame(
    const VtkImageGridSnapshot& source, const DataPublishPolicy policy) const
{
    if (!m_data || !source || !source->data) return false;
    const auto graph = m_data->GetDataGraph();
    if (policy == DataPublishPolicy::AllowHistoricalResult)
        return bool(m_data->GetData(graph, source->data->self));
    return SurfaceContract::GetSourceCurrent(*m_data, graph, source->data->self, source->binding);
}

std::vector<HostFeatureView>
SurfaceDeterminationHostFeature::Impl::GetTargetViews(
    const HostViewTargets& targets) const
{
    auto views = m_views
        ? m_views->GetViews(targets)
        : std::vector<HostFeatureView>{};
    if (views.empty()
        || !std::all_of(
            views.begin(), views.end(),
            [](const HostFeatureView& view) {
                return !view.id.empty() && GetRoleSupported(view.role);
            })) {
        return {};
    }
    std::sort(views.begin(), views.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    views.erase(
        std::unique(
            views.begin(), views.end(),
            [](const auto& left, const auto& right) {
                return left.id == right.id;
            }),
        views.end());
    return views;
}

std::vector<std::string>
SurfaceDeterminationHostFeature::Impl::GetViewIds(
    const std::vector<HostFeatureView>& views)
{
    std::vector<std::string> viewIds;
    viewIds.reserve(views.size());
    for (const HostFeatureView& view : views) viewIds.push_back(view.id);
    return viewIds;
}

void SurfaceDeterminationHostFeature::Impl::SetState(
    SurfaceDeterminationState state)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = std::move(state);
}

void SurfaceDeterminationHostFeature::Impl::SetRequestRunning(
    const std::uint64_t requestId,
    const DataRevisionRef sourceRevision,
    const bool isFirstRequest)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    (void)isFirstRequest; // 只在正式结果采用/绑定投影时更新恢复状态。
    m_state.stage = SurfaceDeterminationStage::Preparing;
    m_state.failureReason = SurfaceFailureReason::None;
    m_state.requestId = requestId;
    m_state.sourceRevision = sourceRevision;
    m_state.progress01 = 0.0;
    m_state.errorMessage.clear();
}

void SurfaceDeterminationHostFeature::Impl::SetRequestProgress(
    const SurfaceRequestProgress& progress)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_state.requestId != m_latestRequestId) return;
    m_state.stage = progress.stage;
    m_state.progress01 = std::min(progress.progress01, 0.999);
}

void SurfaceDeterminationHostFeature::Impl::SendComplete(
    SurfaceDeterminationCallback callback,
    SurfaceDeterminationResult result,
    const DataBindingRevision bindingRevision,
    std::shared_ptr<std::atomic<bool>> requestActive) const noexcept
{
    if (!callback) return;
    try {
        const auto active = requestActive ? std::move(requestActive) : m_completeActive;
        const std::weak_ptr<TrustedDataPort> data = m_data;
        const auto pending = std::make_shared<SurfaceDeterminationResult>(std::move(result));
        const auto once = std::make_shared<std::atomic<bool>>(false);
        const auto send = [active, data, pending, once, bindingRevision, callback = std::move(callback)]() mutable {
            if (once->exchange(true)) return;
            if (pending->status == SurfaceResultStatus::Succeeded) {
                if (!active || !active->load()) {
                    pending->status = SurfaceResultStatus::Cancelled;
                    pending->failureReason = SurfaceFailureReason::Cancelled;
                }
                else if (!pending->isPublished && GetDataRevisionRefValid(pending->sourceRevision)) {
                    const auto port = data.lock();
                    const auto graph = port ? port->GetDataGraph() : DataGraphSnapshot{};
                    const auto binding = port && bindingRevision != 0
                        ? port->GetDataBinding(graph, primaryVolumeBinding) : std::optional<DataBinding>{};
                    if (!port || !SurfaceContract::GetSourceCurrent(*port, graph, pending->sourceRevision)
                        || (bindingRevision != 0 && (!binding || binding->revision != bindingRevision
                            || binding->target != pending->sourceRevision))) {
                        pending->status = SurfaceResultStatus::Failed;
                        pending->failureReason = SurfaceFailureReason::SourceChanged;
                    }
                }
            }
            try { callback(std::move(*pending)); } catch (...) {}
        };
        if (!m_host || !m_host->SendOwnerComplete(send)) {
            if (pending->status == SurfaceResultStatus::Succeeded) {
                pending->status = SurfaceResultStatus::Cancelled;
                pending->failureReason = SurfaceFailureReason::Cancelled;
            }
            auto fallback = send;
            fallback();
        }
    }
    catch (...) {
    }
}

bool SurfaceDeterminationHostFeature::Impl::BuildBindings(
    vtkSmartPointer<vtkPolyData> displayData,
    const std::vector<HostFeatureView>& views,
    const VtkImageGridSnapshot& source,
    std::vector<OverlayBinding>& bindings)
{
    if (!displayData || !source || !source->image || !m_views) return false;
    bindings.reserve(views.size());
    for (const HostFeatureView& view : views) {
        auto service = m_views->GetOverlayPort(view.id);
        auto overlay = service
            ? CreateOverlay(view.role) : nullptr;
        if (!service || !overlay) {
            RemoveBindings(bindings);
            return false;
        }
        overlay->SetInputData(displayData);
        OverlayBinding binding{ service, overlay, view.id };
        bool isAttached = false;
        try { isAttached = service->AttachOverlay(overlay); }
        catch (...) {}
        if (!isAttached) {
            service->RemoveOverlay(overlay);
            RemoveBindings(bindings);
            return false;
        }
        bindings.push_back(std::move(binding));
    }
    return true;
}

void SurfaceDeterminationHostFeature::Impl::RemoveBindings(
    std::vector<OverlayBinding>& bindings) noexcept
{
    for (auto item = bindings.rbegin(); item != bindings.rend(); ++item) {
        if (item->service && item->overlay) {
            item->service->RemoveOverlay(item->overlay);
        }
    }
    bindings.clear();
}

bool SurfaceDeterminationHostFeature::Impl::RemoveDisplay()
{
    RemoveBindings(m_bindings);
    m_displayData = nullptr;
    return !m_host || m_host->SetActiveViews({});
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::Impl::GetDisplayGeneration() const
{
    const auto preview = GetPreviewSnapshot();
    return preview ? preview : m_store.GetGeneration();
}

bool SurfaceDeterminationHostFeature::Impl::SendDisplayDelta()
{
    if (m_bindings.empty()) return true;
    try {
        const auto generation = GetDisplayGeneration();
        const bool isPreview = generation && generation->purpose == SurfaceTaskPurpose::Preview;
        const auto source = isPreview ? m_previewSource : m_activeSource;
        const auto& views = isPreview ? m_previewViews : m_activeViews;
        if (!m_host || !source || !source->data || !generation) return false;
        FeatureSceneDelta delta;
        delta.requestId = GetNextRequestId();
        delta.priority = FeatureScenePriority::Scene;
        delta.scope = FeatureSceneScope::RequiredAllViews;
        delta.inputStamp = { source->data->self };
        delta.viewIds = GetViewIds(views);
        delta.hasDisplayUpdate = true;
        delta.inputs = { { "source-volume", source->data->self } };
        if (!isPreview) {
            delta.inputs.push_back({ "mesh", generation->meshRevision });
            for (const auto& binding : m_bindings)
                delta.displays.push_back({ binding.viewId, std::string(featureId), "surface",
                    generation->meshRevision, m_displayOperation.operation });
        }
        return m_host->SendSceneDelta(std::move(delta));
    }
    catch (...) { return false; }
}

std::vector<FeatureOperationState> SurfaceDeterminationHostFeature::Impl::GetOperationStates() const
{
    if (!m_isAttached || !GetIsOwnerThread() || !m_service) return {};
    std::vector<FeatureOperationState> states;
    for (const auto& [id, request] : m_requests) {
        auto state = request.operation;
        const auto execution = m_service->GetExecutionState(id);
        state.stateRevision += execution.stateRevision;
        state.status = execution.status;
        state.progress = execution.progress;
        states.push_back(std::move(state));
    }
    if (m_lastOperation.operation.requestId != 0
        && m_requests.find(m_lastOperation.operation.requestId) == m_requests.end())
        states.push_back(m_lastOperation);
    if (!m_bindings.empty() && m_displayOperation.operation.requestId != 0
        && m_displayOperation.operation.requestId != m_lastOperation.operation.requestId
        && m_requests.find(m_displayOperation.operation.requestId) == m_requests.end())
        states.push_back(m_displayOperation);
    return states;
}

bool SurfaceDeterminationHostFeature::Impl::SetVisibility(const bool isVisible)
{
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.isOverlayVisible = isVisible;
        m_stateBeforeRequest.isOverlayVisible = isVisible;
    }
    if (!isVisible) { m_isDisplayPending = false; return RemoveDisplay(); }
    m_isDisplayPending = !SetDisplayProjection();
    return !m_isDisplayPending;
}

bool SurfaceDeterminationHostFeature::Impl::ClearPreview()
{
    { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview.reset(); }
    m_previewSource.reset(); m_previewViews.clear();
    if (!RemoveDisplay()) return false;
    m_isDisplayPending = !SetDisplayProjection();
    const bool visible = GetState().isOverlayVisible;
    auto restored = m_store.GetGeneration() ? m_stateBeforeRequest : SurfaceDeterminationState{};
    restored.isOverlayVisible = visible;
    SetState(std::move(restored));
    if (m_isDisplayPending) SetDisplayFailure();
    return !m_isDisplayPending;
}

bool SurfaceDeterminationHostFeature::Impl::ClearResult()
{
    if (!ClearResultBinding()) return false;
    if (!RemoveDisplay()) return false;
    { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview.reset(); }
    m_previewSource.reset(); m_previewViews.clear();
    m_store.ClearGeneration();
    m_activeSource.reset(); m_activeViews.clear();
    m_isDisplayPending = false;
    SurfaceDeterminationState idle;
    idle.isOverlayVisible = GetState().isOverlayVisible;
    m_stateBeforeRequest = idle;
    SetState(std::move(idle));
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::ClearResultBinding()
{
    const auto generation = m_store.GetGeneration();
    if (!generation || !m_data) return true;
    const auto binding = m_data->GetDataBinding(m_data->GetDataGraph(), SurfaceContract::GetBindingName(m_activeScope));
    if (!binding || binding->revision != m_resultBinding.revision
        || binding->target != m_resultBinding.target) return true;
    DataTransaction transaction;
    transaction.bindings.push_back({ binding->name,
        binding->revision, true, binding->target, {} });
    return m_data->SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded;
}

DataBinding SurfaceDeterminationHostFeature::Impl::GetResultBinding() const
{ return GetResultBinding(m_activeScope); }

DataBinding SurfaceDeterminationHostFeature::Impl::GetResultBinding(const std::string_view scope) const
{
    const auto name = SurfaceContract::GetBindingName(scope);
    return m_data ? m_data->GetDataBinding(m_data->GetDataGraph(), name)
        .value_or(DataBinding{ name, {}, 0 }) : DataBinding{};
}

void SurfaceDeterminationHostFeature::Impl::SetDisplayFailure(const std::uint64_t requestId)
{
    m_isDisplayPending = true;
    const auto generation = m_store.GetGeneration();
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_latestRequestId != 0 && m_state.stage != SurfaceDeterminationStage::Ready
        && (requestId == 0 || requestId != m_latestRequestId)) return;
    if (requestId != 0 && generation) {
        m_state.stage = SurfaceDeterminationStage::Ready;
        m_state.sourceRevision = generation->sourceRevision;
        m_state.resultRevision = generation->resultRevision;
        m_state.pointCount = generation->points->size();
        m_state.objectCount = static_cast<std::uint32_t>(generation->objects->size());
    }
    m_state.failureReason = SurfaceFailureReason::DisplayFailed;
    m_state.errorMessage = "Surface candidate is ready; display is pending and will be retried.";
}

bool SurfaceDeterminationHostFeature::Impl::SetDisplayProjection()
{
    const auto generation = GetDisplayGeneration();
    if (!generation || !m_host) return RemoveDisplay();
    const bool isPreview = generation->purpose == SurfaceTaskPurpose::Preview;
    const auto& views = isPreview ? m_previewViews : m_activeViews;
    const auto source = isPreview ? m_previewSource : m_activeSource;
    if (!GetState().isOverlayVisible || views.empty()) return RemoveDisplay();
    if (!m_displayData) m_displayData = BuildDisplayData(*generation);
    if (m_bindings.empty()) {
        std::vector<OverlayBinding> bindings;
        try {
            if (!BuildBindings(m_displayData, views, source, bindings)
                || !m_host->SetActiveViews(GetViewIds(views))) {
                RemoveBindings(bindings); return false;
            }
        }
        catch (...) { RemoveBindings(bindings); return false; }
        m_bindings = std::move(bindings);
    }
    return SendDisplayDelta();
}

void SurfaceDeterminationHostFeature::Impl::SetBindingProjection()
{
    if (!m_isAttached || m_isDetaching || !m_data) return;
    try {
        const auto graph = m_data->GetDataGraph();
        const auto binding = GetResultBinding();
        if (binding.revision != m_resultBinding.revision
            || binding.target != m_resultBinding.target) {
            // 外部重新激活（包括 ABA）是新的业务意图；退休旧展示但不写图。
            m_resultBinding = binding;
            if (!RemoveDisplay()) m_isStaleCleanupPending = true;
            const auto snapshot = binding.target ? m_data->GetData(graph, *binding.target) : DataSnapshot{};
            const auto* payload = snapshot
                ? dynamic_cast<const SurfaceGenerationPayload*>(snapshot->payload.get()) : nullptr;
            const auto generation = payload ? payload->GetGeneration() : nullptr;
            const auto source = generation ? m_data->GetImageGrid(graph, generation->sourceRevision) : nullptr;
            if (!generation || !source || !source->data
                || generation->dataRevision != snapshot->self || generation->resultScope != m_activeScope
                || generation->purpose != SurfaceTaskPurpose::Determine
                || !SurfaceContract::GetSourceCurrent(*m_data, graph, generation->sourceRevision, generation->sourceBinding)) {
                m_store.ClearGeneration();
                m_activeSource.reset();
                m_isDisplayPending = false;
                auto state = GetState();
                m_stateBeforeRequest = {};
                m_stateBeforeRequest.isOverlayVisible = state.isOverlayVisible;
                if (state.stage == SurfaceDeterminationStage::Ready) {
                    const bool isVisible = state.isOverlayVisible;
                    state = {};
                    state.isOverlayVisible = isVisible;
                    if (binding.target) {
                        state.stage = SurfaceDeterminationStage::Stale;
                        state.failureReason = SurfaceFailureReason::SourceChanged;
                    }
                    SetState(std::move(state));
                }
                return;
            }
            m_store.SetGeneration(snapshot);
            // Historical activation is a new display attempt; it does not rewrite graph history.
            m_displayOperation = {};
            m_displayOperation.operation = { std::string(featureId), m_host->GetAttachmentId(), GetNextRequestId() };
            m_displayOperation.stateRevision = 1;
            m_displayOperation.status = FeatureRunStatus::Succeeded;
            m_displayOperation.progress = 1.0;
            m_displayOperation.inputs = { { "source-volume", generation->sourceRevision } };
            if (GetDataRevisionRefValid(generation->meshRevision))
                m_displayOperation.outputs.push_back(generation->meshRevision);
            m_displayOperation.outputs.push_back(snapshot->self);
            auto activeSource = std::make_shared<VtkImageGridView>(*source);
            activeSource->binding = generation->sourceBinding;
            m_activeSource = std::move(activeSource);
            if (m_activeViews.empty()) {
                auto targets = m_config.defaultStart.targetViews;
                if (!GetTargetsUsed(targets)) targets.viewRoles = { HostRenderViewRole::Primary3D };
                m_activeViews = GetTargetViews(targets);
            }
            m_resultRevision = std::max(m_resultRevision, generation->resultRevision);
            SurfaceDeterminationState ready;
            ready.stage = SurfaceDeterminationStage::Ready;
            ready.sourceRevision = generation->sourceRevision;
            ready.resultRevision = generation->resultRevision;
            ready.progress01 = 1.0;
            ready.pointCount = generation->points ? generation->points->size() : 0;
            ready.objectCount = generation->objects
                ? static_cast<std::uint32_t>(generation->objects->size()) : 0;
            const auto& statistics = payload->GetStatistics();
            ready.acceptedPointCount = statistics.acceptedPointCount;
            ready.lowContrastPointCount = statistics.lowContrastPointCount;
            ready.rejectedPointCount = statistics.rejectedPointCount;
            ready.truncatedPointCount = statistics.truncatedPointCount;
            ready.nonManifoldObjectCount = statistics.nonManifoldObjectCount;
            ready.isOverlayVisible = GetState().isOverlayVisible;
            if (!GetPreviewSnapshot() && (m_latestRequestId == 0
                || GetState().stage == SurfaceDeterminationStage::Ready)) SetState(ready);
            m_stateBeforeRequest = std::move(ready);
            m_isDisplayPending = true;
        }
        if (m_isDisplayPending) {
            if (SetDisplayProjection()) {
                m_isDisplayPending = false;
                const std::lock_guard<std::mutex> lock(m_stateMutex);
                if (m_state.failureReason == SurfaceFailureReason::DisplayFailed) {
                    m_state.failureReason = SurfaceFailureReason::None;
                    m_state.errorMessage.clear();
                }
            }
            else { SetDisplayFailure(); }
        }
    }
    catch (...) { SetDisplayFailure(); }
}

void SurfaceDeterminationHostFeature::Impl::SetSourceStale()
{
    if (!ClearResultBinding()) {
        m_isStaleCleanupPending = true;
        return;
    }
    const auto previousState = GetState();
    m_store.ClearGeneration();
    m_activeSource.reset();
    m_activeViews.clear();
    m_isDisplayPending = false;
    if (!RemoveDisplay()) m_isStaleCleanupPending = true;
    const bool isVisible = GetState().isOverlayVisible;
    SurfaceDeterminationState stale;
    stale.stage = SurfaceDeterminationStage::Stale;
    stale.failureReason = SurfaceFailureReason::SourceChanged;
    stale.requestId = m_latestRequestId != 0
        ? m_latestRequestId : previousState.requestId;
    stale.sourceRevision = previousState.sourceRevision;
    stale.resultRevision = m_resultRevision;
    stale.isOverlayVisible = isVisible;
    stale.errorMessage = "Surface source changed before commit.";
    SetState(std::move(stale));
}

void SurfaceDeterminationHostFeature::Impl::SetRequestComplete(SurfaceJobComplete complete)
{
    const auto item = m_requests.find(complete.requestId);
    if (item == m_requests.end()) return;
    RequestEntry request = std::move(item->second);
    m_requests.erase(item);
    request.operation.stateRevision += m_service->GetExecutionState(complete.requestId).stateRevision;
    auto operation = request.operation;
    const bool isLatest = complete.requestId == m_latestRequestId;
    auto& result = complete.result;
    const auto purpose = SurfaceContract::GetPurpose(request.params);
    if (request.isSourceChanged || !GetSourceSame(request.source, request.params.sourcePolicy)) {
        result.status = SurfaceResultStatus::Failed;
        result.failureReason = SurfaceFailureReason::SourceChanged;
        result.message = "Surface source changed before commit.";
    }
    else if (request.isSuperseded) {
        result.status = SurfaceResultStatus::Cancelled;
        result.failureReason = SurfaceFailureReason::Cancelled;
        result.message = "Surface request was superseded in its scope and purpose.";
    }
    else if (result.status == SurfaceResultStatus::Succeeded && purpose == SurfaceTaskPurpose::Determine
        && request.params.sourcePolicy == DataPublishPolicy::RequireCurrentInputs) {
        const auto current = GetResultBinding(request.params.resultScope);
        if (current.revision != request.resultBinding.revision || current.target != request.resultBinding.target) {
            result.status = SurfaceResultStatus::Cancelled;
            result.failureReason = SurfaceFailureReason::Cancelled;
            result.message = "Surface result binding changed before commit.";
        }
    }
    auto callbackResult = BuildResult(complete.requestId, result.status, result.failureReason,
        result.sourceRevision, 0, result.points.size(), static_cast<std::uint32_t>(result.objects.size()), result.message);
    callbackResult.purpose = purpose;
    callbackResult.resultScope = request.params.resultScope;
    callbackResult.isoEstimate = result.isoEstimate;
    DataSnapshot published;
    if (result.status == SurfaceResultStatus::Succeeded) {
        try {
            if (purpose == SurfaceTaskPurpose::Determine) {
                published = SetRequestSucceeded(request, std::move(result), complete.requestId);
                if (!published) {
                    callbackResult.status = SurfaceResultStatus::Failed;
                    callbackResult.failureReason = SurfaceFailureReason::PublishFailed;
                    callbackResult.message = "Surface candidate could not be published.";
                }
            }
            else if (isLatest) SetTransientResult(request, result, complete.requestId);
        }
        catch (...) {
            callbackResult.status = SurfaceResultStatus::Failed;
            callbackResult.failureReason = SurfaceFailureReason::BudgetExceeded;
            callbackResult.message = "Surface candidate allocation failed.";
        }
    }
    if (callbackResult.status != SurfaceResultStatus::Succeeded && isLatest && m_isAttached) {
        SurfaceAlgorithmResult failure;
        failure.status = callbackResult.status; failure.failureReason = callbackResult.failureReason;
        failure.message = callbackResult.message;
        SetRequestFailed(request, failure);
    }
    if (published) {
        const auto* payload = dynamic_cast<const SurfaceGenerationPayload*>(published->payload.get());
        const auto generation = payload->GetGeneration();
        callbackResult.isPublished = true;
        callbackResult.isActivated = request.params.sourcePolicy == DataPublishPolicy::RequireCurrentInputs;
        callbackResult.dataRevision = generation->dataRevision;
        callbackResult.meshRevision = generation->meshRevision;
        callbackResult.resultRevision = generation->resultRevision;
        callbackResult.pointCount = generation->points->size();
        callbackResult.objectCount = static_cast<std::uint32_t>(generation->objects->size());
        operation.outputs = { generation->meshRevision, generation->dataRevision };
    }
    if (isLatest && m_isAttached && callbackResult.status == SurfaceResultStatus::Succeeded
        && request.params.sourcePolicy != DataPublishPolicy::AllowHistoricalResult
        && purpose != SurfaceTaskPurpose::Estimate && (m_isDisplayPending
            || (published && GetResultBinding(request.params.resultScope).target != published->self))) {
        callbackResult.failureReason = SurfaceFailureReason::DisplayFailed;
        callbackResult.message = "Surface calculation completed; display has not followed the result.";
    }
    operation.status = callbackResult.status == SurfaceResultStatus::Succeeded ? FeatureRunStatus::Succeeded
        : callbackResult.status == SurfaceResultStatus::Cancelled ? FeatureRunStatus::Cancelled : FeatureRunStatus::Failed;
    operation.progress = callbackResult.status == SurfaceResultStatus::Succeeded ? 1.0 : 0.0;
    operation.stateRevision += 2;
    if (m_isAttached && operation.operation.requestId >= m_lastOperation.operation.requestId) m_lastOperation = operation;
    if (m_latestRequestId == complete.requestId) m_latestRequestId = 0;
    SendComplete(std::move(request.onComplete), std::move(callbackResult),
        request.source->binding ? request.source->binding->revision : 0, std::move(request.completeActive));
}

void SurfaceDeterminationHostFeature::Impl::SetRequestFailed(
    const RequestEntry& request,
    const SurfaceAlgorithmResult& result)
{
    if (result.failureReason == SurfaceFailureReason::SourceChanged
        || !GetSourceSame(request.source, request.params.sourcePolicy)) {
        if (m_activeSource && !GetSourceSame(m_activeSource)) SetSourceStale();
        else {
            auto state = m_stateBeforeRequest;
            state.stage = SurfaceDeterminationStage::Stale;
            state.failureReason = SurfaceFailureReason::SourceChanged;
            state.requestId = GetState().requestId;
            state.sourceRevision = request.source->data->self;
            SetState(std::move(state));
        }
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_stateBeforeRequest.stage == SurfaceDeterminationStage::Ready) {
        m_state = m_stateBeforeRequest;
        return;
    }
    const auto requestId = m_state.requestId;
    m_state = m_stateBeforeRequest;
    m_state.requestId = requestId;
    m_state.sourceRevision = request.source && request.source->data ? request.source->data->self : DataRevisionRef{};
    m_state.stage = result.status == SurfaceResultStatus::Cancelled
        ? SurfaceDeterminationStage::Cancelled
        : SurfaceDeterminationStage::Failed;
    m_state.failureReason = result.failureReason;
    m_state.progress01 = 0.0;
    m_state.errorMessage = result.message;
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::Impl::BuildGeneration(const RequestEntry& request,
    SurfaceAlgorithmResult& result, const std::uint64_t requestId, const bool isFormal)
{
    if (result.points.empty() || result.triangleIndices.empty() || result.objects.empty()
        || result.triangleValidity.size() != result.triangleIndices.size() / 3) return {};
    const auto* image = dynamic_cast<const ImageGrid3DPayload*>(request.source->data->payload.get());
    if (!image || image->GetGeometry().coordinateFrame.empty()) return {};
    SurfaceGenerationSnapshot generation;
    generation.requestId = requestId;
    generation.purpose = SurfaceContract::GetPurpose(request.params);
    generation.resultScope = request.params.resultScope;
    generation.coordinateFrame = image->GetGeometry().coordinateFrame;
    generation.modelUnit = request.params.modelUnit;
    generation.requestedParams = request.params;
    generation.requestedParams.targetViews = {};
    generation.resolvedParams = result.resolvedParams;
    generation.canonicalParameters = SurfaceContract::BuildParameters(
        generation.requestedParams, generation.resolvedParams, generation.coordinateFrame, m_config.maxWorkingBytes);
    generation.sourceBinding = request.source->binding;
    if (isFormal) {
        generation.meshRevision = { m_data->CreateDataEntityId(), 1 };
        generation.dataRevision = { m_data->CreateDataEntityId(), 1 };
        generation.resultRevision = m_resultRevision + 1;
    }
    generation.sourceRevision = result.sourceRevision;
    generation.parameterFingerprint = result.parameterFingerprint;
    generation.algorithmRevision = result.algorithmRevision;
    generation.method = result.method;
    generation.isoEstimate = result.isoEstimate;
    generation.points = std::make_shared<const std::vector<SurfacePointRecord>>(std::move(result.points));
    generation.triangleIndices = std::make_shared<const std::vector<std::uint32_t>>(std::move(result.triangleIndices));
    generation.triangleValidity = std::make_shared<const std::vector<std::uint8_t>>(std::move(result.triangleValidity));
    generation.objects = std::make_shared<const std::vector<SurfaceObjectRecord>>(std::move(result.objects));
    return std::make_shared<const SurfaceGenerationSnapshot>(std::move(generation));
}

void SurfaceDeterminationHostFeature::Impl::SetTransientResult(const RequestEntry& request,
    SurfaceAlgorithmResult& result, const std::uint64_t requestId)
{
    const auto purpose = SurfaceContract::GetPurpose(request.params);
    SetBindingProjection(); // 先吸收其他通道刚发布的正式槽，供预览退出时恢复。
    SurfaceDeterminationState ready = m_stateBeforeRequest;
    ready.stage = SurfaceDeterminationStage::Ready;
    ready.failureReason = SurfaceFailureReason::None;
    ready.errorMessage.clear(); ready.requestId = requestId;
    ready.sourceRevision = result.sourceRevision; ready.progress01 = 1.0;
    ready.purpose = purpose; ready.resultScope = request.params.resultScope;
    ready.isoEstimate = result.isoEstimate;
    if (purpose == SurfaceTaskPurpose::Preview) {
        const auto preview = BuildGeneration(request, result, requestId, false);
        if (!preview) throw std::runtime_error("Invalid surface preview");
        ready.pointCount = preview->points->size(); ready.objectCount = static_cast<std::uint32_t>(preview->objects->size());
        ready.acceptedPointCount = 0; ready.rejectedPointCount = ready.pointCount;
        { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview = preview; }
        m_previewSource = request.source; m_previewViews = request.views;
        RemoveBindings(m_bindings); m_displayData = nullptr;
        SetState(ready);
        m_isDisplayPending = !SetDisplayProjection();
        if (m_isDisplayPending) SetDisplayFailure();
    }
    else SetState(std::move(ready));
}

DataSnapshot SurfaceDeterminationHostFeature::Impl::SetRequestSucceeded(
    const RequestEntry& request, SurfaceAlgorithmResult result, const std::uint64_t requestId)
{
    if (!GetSourceSame(request.source, request.params.sourcePolicy)
        || m_resultRevision == std::numeric_limits<std::uint64_t>::max()) return {};
    const auto stagedGeneration = BuildGeneration(request, result, requestId, true);
    if (!stagedGeneration) return {};
    const auto meshRef = stagedGeneration->meshRevision;
    const auto generationRef = stagedGeneration->dataRevision;
    std::vector<double> vertices;
    vertices.reserve(stagedGeneration->points->size() * 3U);
    // 通用网格发布测量消费者所需的最小质量信息。无效项使用有限占位值，
    // measurement.valid 是解释其余字段的前置条件；它不代表完整计量不确定度。
    constexpr std::size_t qualityBytesPerPoint = 7U * sizeof(double) * 2U;
    if (stagedGeneration->points->size()
        > m_config.maxWorkingBytes / qualityBytesPerPoint) return {};
    std::vector<MeshAttribute> attributes{
        { "measurement.valid", 1, {} },
        { "measurement.fit-residual", 1, {} },
        { "measurement.support-ratio", 1, {} },
        { "measurement.localization-sigma", 1, {} },
        { "measurement.normal", 3, {} }
    };
    for (auto& attribute : attributes) {
        attribute.values.reserve(
            stagedGeneration->points->size() * attribute.componentCount);
    }
    for (const auto& point : *stagedGeneration->points) {
        vertices.insert(vertices.end(), point.positionModel.begin(), point.positionModel.end());
        double normalLength = 0.0;
        for (const auto value : point.normalModel) {
            normalLength += static_cast<double>(value) * value;
        }
        normalLength = std::sqrt(normalLength);
        const bool hasQuality = std::isfinite(point.fitResidual)
            && std::isfinite(point.validSupportRatio)
            && std::isfinite(point.estimatedLocalizationSigma)
            && std::isfinite(normalLength) && normalLength > 1e-12
            && point.fitResidual >= 0.0F
            && point.validSupportRatio > 0.0F && point.validSupportRatio <= 1.0F
            && point.estimatedLocalizationSigma >= 0.0F;
        const bool isValid = SurfaceContract::GetPointValid(point, stagedGeneration->method);
        attributes[0].values.push_back(isValid ? 1.0 : 0.0);
        attributes[1].values.push_back(hasQuality ? point.fitResidual : 0.0);
        attributes[2].values.push_back(hasQuality ? point.validSupportRatio : 0.0);
        attributes[3].values.push_back(hasQuality ? point.estimatedLocalizationSigma : 0.0);
        for (const auto value : point.normalModel) {
            attributes[4].values.push_back(hasQuality ? value / normalLength : 0.0);
        }
    }
    std::vector<std::uint64_t> triangles(stagedGeneration->triangleIndices->begin(),
        stagedGeneration->triangleIndices->end());
    const auto mesh = std::make_shared<const SurfaceMeshPayload>(
        std::move(vertices), std::move(triangles), std::move(attributes), stagedGeneration->coordinateFrame);
    if (!mesh->GetValid()) return {};
    const auto& expected = request.resultBinding;
    DataTransaction transaction;
    transaction.policy = request.params.sourcePolicy;
    const bool isHistorical = request.params.sourcePolicy == DataPublishPolicy::AllowHistoricalResult;
    if (!isHistorical) {
        DataExpectation sourceExpected;
        sourceExpected.kind = DataExpectationKind::EntityHead;
        sourceExpected.entityId = request.source->data->self.entityId;
        sourceExpected.expectedGeneration = request.source->data->self.generation;
        transaction.expectations.push_back(sourceExpected);
        if (request.source->binding) {
            sourceExpected = {};
            sourceExpected.kind = DataExpectationKind::Binding;
            sourceExpected.binding = request.source->binding->name;
            sourceExpected.expectedBindingRevision = request.source->binding->revision;
            sourceExpected.isTargetChecked = true;
            sourceExpected.expectedTarget = request.source->data->self;
            transaction.expectations.push_back(sourceExpected);
        }
    }
    const DataInputRef sourceInput{ "source-volume", request.source->data->self };
    const DataProvenance provenance{ std::string(featureId), "determine-surface",
        std::to_string(stagedGeneration->algorithmRevision),
        stagedGeneration->canonicalParameters };
    std::vector<DataInputRef> generationInputs{sourceInput};
    {
        transaction.outputs.push_back({meshRef.entityId, 0, DataTypes::surfaceMesh,
            {sourceInput}, mesh, provenance});
        generationInputs.push_back({"mesh", meshRef});
    }
    transaction.outputs.push_back({generationRef.entityId, 0, surfaceGenerationType,
        std::move(generationInputs),
        std::make_shared<const SurfaceGenerationPayload>(stagedGeneration,
            SurfaceGenerationPayload::Statistics{result.acceptedPointCount, result.lowContrastPointCount,
                result.rejectedPointCount, result.truncatedPointCount, result.nonManifoldObjectCount}), provenance});
    if (!isHistorical) transaction.bindings.push_back({ expected.name,
        expected.revision, true, expected.target, generationRef });

    const bool isVisible = GetState().isOverlayVisible;
    auto nextViews = request.views;
    const auto data = m_data;
    auto committed = data->SetDataCommit(std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded
        && committed.status != DataCommitStatus::SucceededHistorical) {
        return {};
    }
    // 从这里起正式结果已经有效；显示失败不回滚业务数据。
    const auto published = committed.published.back();
    m_resultRevision = std::max(m_resultRevision, stagedGeneration->resultRevision);
    if (isHistorical || requestId != m_latestRequestId) {
        if (isHistorical && m_isAttached && requestId == m_latestRequestId) {
            auto ready = GetState();
            ready.stage = SurfaceDeterminationStage::Ready; ready.progress01 = 1.0;
            ready.purpose = SurfaceTaskPurpose::Determine; ready.resultScope = request.params.resultScope;
            ready.resultRevision = stagedGeneration->resultRevision;
            ready.pointCount = stagedGeneration->points->size(); ready.objectCount = static_cast<std::uint32_t>(stagedGeneration->objects->size());
            ready.acceptedPointCount = result.acceptedPointCount; ready.rejectedPointCount = result.rejectedPointCount;
            SetState(std::move(ready));
        }
        return published;
    }
    if (!m_isAttached || !m_data || !request.completeActive
        || !request.completeActive->load()) return published;
    try {
        m_activeScope = request.params.resultScope;
        { const std::lock_guard<std::mutex> lock(m_stateMutex); m_preview.reset(); }
        m_previewSource.reset(); m_previewViews.clear();
        m_resultBinding = std::move(committed.bindings.back());
        m_store.SetGeneration(published);
        m_displayOperation = request.operation;
        m_displayOperation.status = FeatureRunStatus::Succeeded;
        m_displayOperation.progress = 1.0;
        m_displayOperation.stateRevision += 2;
        for (const auto& output : committed.published) m_displayOperation.outputs.push_back(output->self);
        RemoveBindings(m_bindings);
        m_displayData = nullptr;
        m_activeSource = request.source;
        m_activeViews = std::move(nextViews);
        m_resultRevision = std::max(m_resultRevision, stagedGeneration->resultRevision);
        SurfaceDeterminationState ready;
        ready.stage = SurfaceDeterminationStage::Ready;
        ready.requestId = requestId;
        ready.purpose = SurfaceTaskPurpose::Determine;
        ready.resultScope = request.params.resultScope;
        ready.sourceRevision = stagedGeneration->sourceRevision;
        ready.resultRevision = stagedGeneration->resultRevision;
        ready.progress01 = 1.0;
        ready.pointCount = stagedGeneration->points->size();
        ready.acceptedPointCount = result.acceptedPointCount;
        ready.lowContrastPointCount = result.lowContrastPointCount;
        ready.rejectedPointCount = result.rejectedPointCount;
        ready.truncatedPointCount = result.truncatedPointCount;
        ready.objectCount = static_cast<std::uint32_t>(
            stagedGeneration->objects->size());
        ready.nonManifoldObjectCount = result.nonManifoldObjectCount;
        ready.isOverlayVisible = isVisible;
        // observer 可能已经接纳下一请求；旧提交不得覆盖新的 Preparing/requestId。
        if (m_latestRequestId == requestId) SetState(ready);
        m_stateBeforeRequest = std::move(ready);
        m_isDisplayPending = true;
        SetBindingProjection();
    }
    catch (...) { SetDisplayFailure(requestId); }
    return published;
}

FeatureDataContract SurfaceDeterminationHostFeature::GetDataContract() const
{
    return { { { "source-volume", DataFacets::scalarGrid3D, true } },
        { { "mesh", DataTypes::surfaceMesh, { DataFacets::surfaceMesh } },
          { "generation", surfaceGenerationType, { surfaceGenerationFacet } } } };
}

std::vector<FeatureOperationState> SurfaceDeterminationHostFeature::GetOperationStates() const
{
    return m_impl ? m_impl->GetOperationStates() : std::vector<FeatureOperationState>{};
}

SurfaceDeterminationHostFeature::SurfaceDeterminationHostFeature(
    SurfaceDeterminationConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

SurfaceDeterminationHostFeature::~SurfaceDeterminationHostFeature() noexcept =
    default;

std::string_view
SurfaceDeterminationHostFeature::GetFeatureId() const noexcept
{
    return featureId;
}

bool SurfaceDeterminationHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl && m_impl->AttachHost(context);
}

bool SurfaceDeterminationHostFeature::DetachHost()
{
    return !m_impl || m_impl->DetachHost();
}

bool SurfaceDeterminationHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

SurfaceDeterminationAdmission
SurfaceDeterminationHostFeature::SendRequest(
    SurfaceDeterminationRequest request,
    SurfaceDeterminationCallback onComplete)
{
    return m_impl
        ? m_impl->SendRequest(
            std::move(request), std::move(onComplete))
        : SurfaceDeterminationAdmission{
            SurfaceAdmissionStatus::Unavailable, 0 };
}

SurfaceDeterminationState
SurfaceDeterminationHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : SurfaceDeterminationState{};
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetSurfaceSnapshot() const
{
    return m_impl ? m_impl->GetSurfaceSnapshot() : nullptr;
}

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetSurfaceSnapshot(const std::string_view scope) const
{ return m_impl ? m_impl->GetSurfaceSnapshot(scope) : nullptr; }

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetSurfaceSnapshot(const DataRevisionRef revision) const
{ return m_impl ? m_impl->GetSurfaceSnapshot(revision) : nullptr; }

std::shared_ptr<const SurfaceGenerationSnapshot>
SurfaceDeterminationHostFeature::GetPreviewSnapshot() const
{ return m_impl ? m_impl->GetPreviewSnapshot() : nullptr; }
