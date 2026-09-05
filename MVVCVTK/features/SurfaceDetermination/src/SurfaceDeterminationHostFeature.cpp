#include "Host/SurfaceDeterminationHostFeature.h"
#include "Data/DataPayloads.h"

#include "Render/Contracts/OverlayService.h"
#include "SurfaceDeterminationService.h"
#include "SurfaceGenerationStore.h"
#include "SurfaceOverlayStrategy.h"

#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
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
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "SurfaceDetermination";
constexpr std::size_t completionBatchLimit = 8;
constexpr std::string_view surfaceResultBinding = "analysis.surface-determination.active";

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
    if (!GetTargetsUsed(params.targetViews)
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

std::array<double, 3> GetSliceNormal(
    vtkImageData& image,
    const HostRenderViewRole role)
{
    int column = 0;
    if (role == HostRenderViewRole::TopDownSlice) column = 2;
    else if (role == HostRenderViewRole::FrontBackSlice) column = 1;
    std::array<double, 3> normal{};
    const auto* direction = image.GetDirectionMatrix();
    if (direction) {
        for (std::size_t row = 0; row < 3; ++row) {
            normal[row] = direction->GetElement(
                static_cast<int>(row), column);
        }
        const double length = std::sqrt(
            normal[0] * normal[0]
            + normal[1] * normal[1]
            + normal[2] * normal[2]);
        if (std::isfinite(length)
            && length > std::numeric_limits<double>::epsilon()) {
            for (double& value : normal) value /= length;
            return normal;
        }
    }
    normal = { 0.0, 0.0, 0.0 };
    normal[static_cast<std::size_t>(column)] = 1.0;
    return normal;
}

std::shared_ptr<FeatureOverlay> CreateOverlay(
    const HostRenderViewRole role,
    vtkImageData& source)
{
    if (role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D) {
        return std::make_shared<SurfaceOverlayStrategy>();
    }
    if (role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice) {
        return std::make_shared<SurfaceSliceOverlayStrategy>(
            GetSliceNormal(source, role));
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
    }

    bool AttachHost(const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    SurfaceDeterminationAdmission SendRequest(
        SurfaceDeterminationRequest request,
        SurfaceDeterminationCallback onComplete);
    SurfaceDeterminationState GetState() const;
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetSurfaceSnapshot() const;

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
    };

    bool GetIsOwnerThread() const noexcept;
    std::uint64_t GetNextRequestId() noexcept;
    bool GetSourceSame(const VtkImageGridSnapshot& source) const;
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
        SurfaceDeterminationResult result) const noexcept;
    bool BuildBindings(
        vtkSmartPointer<vtkPolyData> displayData,
        const std::vector<HostFeatureView>& views,
        const VtkImageGridSnapshot& source,
        std::vector<OverlayBinding>& bindings);
    static void RemoveBindings(
        std::vector<OverlayBinding>& bindings) noexcept;
    bool RemoveDisplay();
    bool SetVisibility(bool isVisible);
    bool ClearResult();
    bool ClearResultBinding();
    void SetSourceStale();
    void SetRequestComplete(SurfaceJobComplete complete);
    void SetRequestFailed(
        const RequestEntry& request,
        const SurfaceAlgorithmResult& result);
    bool SetRequestSucceeded(
        const RequestEntry& request,
        SurfaceAlgorithmResult result,
        std::uint64_t requestId);

    SurfaceDeterminationConfig m_config;
    mutable std::mutex m_stateMutex;
    SurfaceDeterminationState m_state;
    SurfaceDeterminationState m_stateBeforeRequest;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<SurfaceDeterminationService> m_service;
    SurfaceGenerationStore m_store;
    std::map<std::uint64_t, RequestEntry> m_requests;
    VtkImageGridSnapshot m_activeSource;
    std::vector<HostFeatureView> m_activeViews;
    std::vector<OverlayBinding> m_bindings;
    vtkSmartPointer<vtkPolyData> m_displayData;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextRequestId = 1;
    std::uint64_t m_latestRequestId = 0;
    std::uint64_t m_resultRevision = 0;
    bool m_isAttached = false;
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
        m_service = std::make_unique<SurfaceDeterminationService>();
    }
    catch (...) {
        return false;
    }
    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
    m_completeActive = std::make_shared<std::atomic<bool>>(true);
    m_isAttached = true;
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) return true;
    if (!GetIsOwnerThread() || !m_service) return false;
    if (m_completeActive) m_completeActive->store(false);
    {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.stage = SurfaceDeterminationStage::Stopping;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!m_service->Stop(deadline)) return false;

    for (auto& item : m_requests) {
        RequestEntry& request = item.second;
        SendComplete(
            std::move(request.onComplete),
            BuildResult(
                item.first,
                SurfaceResultStatus::Cancelled,
                SurfaceFailureReason::Cancelled,
                request.source && request.source->data ? request.source->data->self : DataRevisionRef{},
                m_resultRevision,
                0,
                0,
                "Surface request was cancelled by detach."));
    }
    m_requests.clear();
    m_latestRequestId = 0;
    if (!ClearResult()) return false;
    m_activeSource.reset();
    m_activeViews.clear();
    m_service.reset();
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
    m_isAttached = false;
    m_isStaleCleanupPending = false;
    SurfaceDeterminationState idle;
    idle.isOverlayVisible = m_config.isOverlayVisible;
    SetState(std::move(idle));
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached || !GetIsOwnerThread() || !m_service || !m_data) {
        return false;
    }
    if (m_isStaleCleanupPending && RemoveDisplay()) {
        m_isStaleCleanupPending = false;
    }
    if (m_activeSource && !GetSourceSame(m_activeSource)) {
        SetSourceStale();
    }
    for (auto& item : m_requests) {
        RequestEntry& request = item.second;
        if (!request.isSourceChanged && !GetSourceSame(request.source)) {
            request.isSourceChanged = true;
            m_service->StopRequest(item.first);
            if (item.first == m_latestRequestId) SetSourceStale();
        }
    }
    if (m_latestRequestId != 0) {
        const auto progress = m_service->GetProgress(m_latestRequestId);
        if (progress) SetRequestProgress(*progress);
    }
    for (std::size_t index = 0;
        index < completionBatchLimit; ++index) {
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

    if (request.action == SurfaceDeterminationAction::Start) {
        const auto params = request.start.value_or(m_config.defaultStart);
        if (!GetStartValid(params)) return admission;
        auto targetViews = GetTargetViews(params.targetViews);
        if (targetViews.empty()) return admission;
        auto source = m_data ? m_data->GetPrimaryImage() : nullptr;
        if (!source || !source->image) {
            admission.status = SurfaceAdmissionStatus::Unavailable;
            return admission;
        }
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
                    false
                });
            if (!inserted.second) return admission;
            requestItem = inserted.first;
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
        m_latestRequestId = requestId;
        SetRequestRunning(
            requestId,
            requestItem->second.source->data->self,
            isFirstRequest);
        return admission;
    }

    if (request.action == SurfaceDeterminationAction::Stop) {
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
    return m_store.GetGeneration();
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
    const VtkImageGridSnapshot& source) const
{
    const auto current = m_data ? m_data->GetPrimaryImage() : nullptr;
    return current && source
        && current->data && source->data && current->binding && source->binding
        && current->data->self == source->data->self
        && current->binding->revision == source->binding->revision;
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
    if (isFirstRequest) m_stateBeforeRequest = m_state;
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
    SurfaceDeterminationResult result) const noexcept
{
    if (!callback) return;
    try {
        const auto active = m_completeActive;
        const std::weak_ptr<TrustedDataPort> data = m_data;
        const auto pending = std::make_shared<SurfaceDeterminationResult>(std::move(result));
        const auto send = [active, data, pending, callback = std::move(callback)]() mutable {
            if (pending->status == SurfaceResultStatus::Succeeded) {
                if (!active || !active->load()) {
                    pending->status = SurfaceResultStatus::Cancelled;
                    pending->failureReason = SurfaceFailureReason::Cancelled;
                }
                else if (GetDataRevisionRefValid(pending->sourceRevision)) {
                    const auto port = data.lock();
                    const auto source = port ? port->GetPrimaryImage() : nullptr;
                    if (!source || !source->data || source->data->self != pending->sourceRevision) {
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
    for (const HostFeatureView& view : views) {
        auto service = m_views->GetOverlayPort(view.id);
        auto overlay = service
            ? CreateOverlay(view.role, *source->image) : nullptr;
        if (!service || !overlay) {
            RemoveBindings(bindings);
            return false;
        }
        overlay->SetInputData(displayData);
        if (!service->AttachOverlay(overlay)) {
            RemoveBindings(bindings);
            return false;
        }
        bindings.push_back({
            std::move(service), std::move(overlay), view.id
        });
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
    if (m_host && !m_host->SetActiveViews({})) return false;
    RemoveBindings(m_bindings);
    m_displayData = nullptr;
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::SetVisibility(
    const bool isVisible)
{
    const auto state = GetState();
    if (state.isOverlayVisible == isVisible) return true;
    if (!isVisible) {
        if (!RemoveDisplay()) return false;
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.isOverlayVisible = false;
        return true;
    }

    const auto generation = m_store.GetGeneration();
    if (!generation) {
        const std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.isOverlayVisible = true;
        return true;
    }
    auto displayData = BuildDisplayData(*generation);
    std::vector<OverlayBinding> bindings;
    if (!BuildBindings(
            displayData, m_activeViews, m_activeSource, bindings)) {
        return false;
    }
    if (!m_host || !m_host->SetActiveViews(GetViewIds(m_activeViews))) {
        RemoveBindings(bindings);
        return false;
    }
    RemoveBindings(m_bindings);
    m_bindings = std::move(bindings);
    m_displayData = std::move(displayData);
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state.isOverlayVisible = true;
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::ClearResult()
{
    if (!ClearResultBinding()) return false;
    if (!RemoveDisplay()) return false;
    m_store.ClearGeneration();
    m_activeSource.reset();
    m_activeViews.clear();
    SurfaceDeterminationState idle;
    idle.isOverlayVisible = GetState().isOverlayVisible;
    SetState(std::move(idle));
    return true;
}

bool SurfaceDeterminationHostFeature::Impl::ClearResultBinding()
{
    const auto generation = m_store.GetGeneration();
    if (!generation || !m_data) return true;
    const auto binding = m_data->GetDataBinding(m_data->GetDataGraph(), surfaceResultBinding);
    if (!binding || binding->target != std::optional<DataRevisionRef>{generation->dataRevision}) return true;
    DataTransaction transaction;
    transaction.bindings.push_back({ std::string(surfaceResultBinding),
        binding->revision, true, binding->target, {} });
    return m_data->SetDataCommit(std::move(transaction)).status == DataCommitStatus::Succeeded;
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

void SurfaceDeterminationHostFeature::Impl::SetRequestComplete(
    SurfaceJobComplete complete)
{
    const auto requestItem = m_requests.find(complete.requestId);
    if (requestItem == m_requests.end()) return;
    RequestEntry request = std::move(requestItem->second);
    m_requests.erase(requestItem);
    const bool isLatest = complete.requestId == m_latestRequestId;
    if (request.isSourceChanged || !GetSourceSame(request.source)) {
        complete.result.status = SurfaceResultStatus::Failed;
        complete.result.failureReason = SurfaceFailureReason::SourceChanged;
        complete.result.message = "Surface source changed before commit.";
        complete.result.points.clear();
        complete.result.triangleIndices.clear();
        complete.result.objects.clear();
    }
    else if (!isLatest
        && complete.result.status == SurfaceResultStatus::Succeeded) {
        complete.result.status = SurfaceResultStatus::Cancelled;
        complete.result.failureReason = SurfaceFailureReason::Cancelled;
        complete.result.message = "Surface request was superseded.";
        complete.result.points.clear();
        complete.result.triangleIndices.clear();
        complete.result.objects.clear();
    }

    SurfaceResultStatus callbackStatus = complete.result.status;
    SurfaceFailureReason callbackReason = complete.result.failureReason;
    std::string callbackMessage = complete.result.message;
    if (callbackMessage.empty()
        && callbackStatus == SurfaceResultStatus::Cancelled) {
        callbackMessage = "Surface request was superseded or cancelled.";
    }
    bool didCommit = false;
    if (isLatest) {
        if (complete.result.status == SurfaceResultStatus::Succeeded) {
            if (!SetRequestSucceeded(
                    request,
                    std::move(complete.result),
                    complete.requestId)) {
                SurfaceAlgorithmResult displayFailure;
                displayFailure.status = SurfaceResultStatus::Failed;
                displayFailure.failureReason =
                    SurfaceFailureReason::DisplayFailed;
                displayFailure.message =
                    "Surface display candidate could not be committed.";
                callbackStatus = displayFailure.status;
                callbackReason = displayFailure.failureReason;
                callbackMessage = displayFailure.message;
                SetRequestFailed(request, displayFailure);
            }
            else {
                didCommit = true;
                callbackStatus = SurfaceResultStatus::Succeeded;
                callbackReason = SurfaceFailureReason::None;
            }
        }
        else {
            SetRequestFailed(request, complete.result);
        }
        m_latestRequestId = 0;
    }

    const auto state = GetState();
    if (isLatest && didCommit) {
        callbackStatus = SurfaceResultStatus::Succeeded;
        callbackReason = SurfaceFailureReason::None;
    }
    SendComplete(
        std::move(request.onComplete),
        BuildResult(
            complete.requestId,
            callbackStatus,
            callbackReason,
            request.source && request.source->data ? request.source->data->self : DataRevisionRef{},
            state.resultRevision,
            state.pointCount,
            state.objectCount,
            std::move(callbackMessage)));
}

void SurfaceDeterminationHostFeature::Impl::SetRequestFailed(
    const RequestEntry& request,
    const SurfaceAlgorithmResult& result)
{
    if (result.failureReason == SurfaceFailureReason::SourceChanged
        || !GetSourceSame(request.source)) {
        SetSourceStale();
        return;
    }
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_stateBeforeRequest.stage == SurfaceDeterminationStage::Ready) {
        m_state = m_stateBeforeRequest;
        return;
    }
    m_state.stage = result.status == SurfaceResultStatus::Cancelled
        ? SurfaceDeterminationStage::Cancelled
        : SurfaceDeterminationStage::Failed;
    m_state.failureReason = result.failureReason;
    m_state.progress01 = 0.0;
    m_state.errorMessage = result.message;
}

bool SurfaceDeterminationHostFeature::Impl::SetRequestSucceeded(
    const RequestEntry& request,
    SurfaceAlgorithmResult result,
    const std::uint64_t requestId)
{
    if (!GetSourceSame(request.source)
        || result.points.empty()
        || result.triangleIndices.empty()
        || result.objects.empty()
        || m_resultRevision == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    auto displayData = BuildDisplayData(
        result.points, result.triangleIndices);
    if (!displayData) return false;

    SurfaceGenerationSnapshot generation;
    const DataRevisionRef meshRef{ m_data->CreateDataEntityId(), 1 };
    const DataRevisionRef generationRef{ m_data->CreateDataEntityId(), 1 };
    generation.dataRevision = generationRef;
    generation.meshRevision = meshRef;
    generation.sourceRevision = result.sourceRevision;
    generation.resultRevision = m_resultRevision + 1;
    generation.parameterFingerprint = result.parameterFingerprint;
    generation.algorithmRevision = result.algorithmRevision;
    generation.method = result.method;
    generation.points =
        std::make_shared<const std::vector<SurfacePointRecord>>(
            std::move(result.points));
    generation.triangleIndices =
        std::make_shared<const std::vector<std::uint32_t>>(
            std::move(result.triangleIndices));
    generation.objects =
        std::make_shared<const std::vector<SurfaceObjectRecord>>(
            std::move(result.objects));
    auto stagedGeneration =
        std::make_shared<const SurfaceGenerationSnapshot>(
            std::move(generation));

    std::vector<double> vertices;
    vertices.reserve(stagedGeneration->points->size() * 3U);
    for (const auto& point : *stagedGeneration->points) {
        vertices.insert(vertices.end(), point.positionModel.begin(), point.positionModel.end());
    }
    std::vector<std::uint64_t> triangles(stagedGeneration->triangleIndices->begin(),
        stagedGeneration->triangleIndices->end());
    const auto mesh = std::make_shared<const SurfaceMeshPayload>(
        std::move(vertices), std::move(triangles));
    if (!mesh->GetValid()) return false;
    const auto expected = m_data->GetDataBinding(request.source->graph, surfaceResultBinding)
        .value_or(DataBinding{ std::string(surfaceResultBinding), {}, 0 });
    DataExpectation sourceExpected;
    sourceExpected.kind = DataExpectationKind::Binding;
    sourceExpected.binding = std::string(primaryVolumeBinding);
    sourceExpected.expectedBindingRevision = request.source->binding->revision;
    sourceExpected.isTargetChecked = true;
    sourceExpected.expectedTarget = request.source->data->self;
    DataTransaction transaction;
    transaction.expectations.push_back(std::move(sourceExpected));
    const DataInputRef sourceInput{ "source-volume", request.source->data->self };
    const DataProvenance provenance{ std::string(featureId), "determine-surface",
        std::to_string(stagedGeneration->algorithmRevision),
        std::to_string(stagedGeneration->parameterFingerprint) };
    transaction.outputs = {
        { meshRef.entityId, 0, DataTypes::surfaceMesh, { sourceInput }, mesh, provenance },
        { generationRef.entityId, 0, surfaceGenerationType,
            { sourceInput, { "mesh", meshRef } },
            std::make_shared<const SurfaceGenerationPayload>(stagedGeneration), provenance } };
    transaction.bindings.push_back({ std::string(surfaceResultBinding),
        expected.revision, true, expected.target, generationRef });

    const bool isVisible = GetState().isOverlayVisible;
    std::vector<OverlayBinding> nextBindings;
    if (isVisible
        && !BuildBindings(
            displayData, request.views, request.source, nextBindings)) {
        return false;
    }
    const auto nextViewIds = isVisible
        ? GetViewIds(request.views) : std::vector<std::string>{};
    const auto previousViewIds = GetViewIds(m_activeViews);
    if (!m_host || !m_host->SetActiveViews(nextViewIds)) {
        RemoveBindings(nextBindings);
        return false;
    }
    const auto committed = m_data->SetDataCommit(std::move(transaction));
    if (committed.status != DataCommitStatus::Succeeded) {
        (void)m_host->SetActiveViews(previousViewIds);
        RemoveBindings(nextBindings);
        return false;
    }
    m_store.SetGeneration(committed.published.back());

    RemoveBindings(m_bindings);
    m_bindings = std::move(nextBindings);
    m_displayData = isVisible ? std::move(displayData) : nullptr;
    m_activeSource = request.source;
    m_activeViews = request.views;
    m_resultRevision = stagedGeneration->resultRevision;
    SurfaceDeterminationState ready;
    ready.stage = SurfaceDeterminationStage::Ready;
    ready.requestId = requestId;
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
    SetState(std::move(ready));
    if (isVisible && m_host) {
        FeatureSceneDelta delta;
        delta.requestId = requestId;
        delta.priority = FeatureScenePriority::Scene;
        delta.scope = FeatureSceneScope::RequiredAllViews;
        delta.inputStamp = { request.source->data->self };
        delta.viewIds = nextViewIds;
        (void)m_host->SendSceneDelta(std::move(delta));
    }
    return true;
}

FeatureDataContract SurfaceDeterminationHostFeature::GetDataContract() const
{
    return { { { "source-volume", DataFacets::scalarGrid3D, true } },
        { { "mesh", DataTypes::surfaceMesh, { DataFacets::surfaceMesh } },
          { "generation", surfaceGenerationType, { surfaceGenerationFacet } } } };
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
