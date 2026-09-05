#include "Host/PartSegmentationHostFeature.h"

#include "Render/Strategies/PartOverlayStrategies.h"
#include "Render/PartRenderStateTable.h"
#include "Render/Contracts/OverlayService.h"
#include "Model/PartCatalog.h"
#include "Services/PartSegmentationService.h"

#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedIntArray.h>

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
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view featureId = "PartSegmentation";

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

struct LabelViewCandidate final {
    // image 借用 labels 的稳定地址，成员逆序析构必须先释放 image。
    std::shared_ptr<std::vector<PartLabelId>> labels;
    vtkSmartPointer<vtkImageData> image;
};

LabelViewCandidate BuildLabelView(
    const PartLabelCandidate& candidate)
{
    LabelViewCandidate view;
    std::size_t voxelCount = 1;
    for (const int dimension : candidate.dimensions) {
        if (dimension <= 0
            || static_cast<std::size_t>(dimension)
                > std::numeric_limits<std::size_t>::max() / voxelCount) {
            return view;
        }
        voxelCount *= static_cast<std::size_t>(dimension);
    }
    if (!candidate.labels
        || voxelCount != candidate.labels->size()
        || voxelCount > static_cast<std::size_t>(
            std::numeric_limits<vtkIdType>::max())) {
        return view;
    }
    static_assert(
        std::is_same_v<std::uint32_t, unsigned int>,
        "The Windows x64 label view requires uint32_t == unsigned int.");

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(
        candidate.extent[0], candidate.extent[1],
        candidate.extent[2], candidate.extent[3],
        candidate.extent[4], candidate.extent[5]);
    image->SetSpacing(candidate.spacing.data());
    image->SetOrigin(candidate.origin.data());
    auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
    direction->DeepCopy(candidate.direction.data());
    image->SetDirectionMatrix(direction);
    if (static_cast<std::size_t>(image->GetNumberOfPoints())
        != voxelCount) {
        return view;
    }

    auto scalars = vtkSmartPointer<vtkUnsignedIntArray>::New();
    scalars->SetNumberOfComponents(1);
    // save=1：VTK 不释放用户数组，labels owner 必须长于 image/overlay。
    scalars->SetArray(
        candidate.labels->data(),
        static_cast<vtkIdType>(voxelCount),
        1);
    image->GetPointData()->SetScalars(scalars);
    if (image->GetScalarPointer() != candidate.labels->data()) return view;

    view.labels = candidate.labels;
    view.image = std::move(image);
    return view;
}

PartSegmentationResult BuildResult(
    const std::uint64_t requestId,
    const PartResultStatus status,
    const PartFailureReason failureReason,
    const DataVersion sourceVersion,
    const std::uint64_t resultRevision,
    const std::size_t partCount,
    std::string message)
{
    PartSegmentationResult result;
    result.requestId = requestId;
    result.status = status;
    result.failureReason = failureReason;
    result.sourceVersion = sourceVersion;
    result.resultRevision = resultRevision;
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
    bool GetSourceSame(const TrustedImageSnapshot& source) const;
    bool GetHistorySourceSame(const TrustedImageSnapshot& source) const;
    void SetState(PartSegmentationState state);
    void SetPublishedState(
        PartSegmentationState state,
        std::shared_ptr<const PartSetSnapshot> snapshot);
    void SetRequestRunning(
        std::uint64_t requestId,
        DataVersion sourceVersion);
    void SetRequestProgress(double progress);
    void SendComplete(
        PartSegmentationCallback callback,
        PartSegmentationResult result) const noexcept;
    bool AttachDisplay(
        vtkSmartPointer<vtkImageData> labelImage,
        const PartRenderStateTable& renderStates,
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
    std::shared_ptr<TrustedFeatureDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<PartSegmentationService> m_service;
    TrustedImageSnapshot m_requestSource;
    TrustedImageSnapshot m_activeSource;
    // m_labelImage 借用该 vector；声明顺序保证 image 先析构。
    std::shared_ptr<std::vector<PartLabelId>> m_labelValues;
    vtkSmartPointer<vtkImageData> m_labelImage;
    std::shared_ptr<const PartCatalog> m_activeCatalog;
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
    try {
        m_service = std::make_unique<PartSegmentationService>();
    }
    catch (...) {
        return false;
    }
    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();
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
        auto callback = std::move(m_startCallback);
        const auto state = GetState();
        SendComplete(
            std::move(callback),
            BuildResult(
                m_activeRequestId,
                PartResultStatus::Cancelled,
                PartFailureReason::Cancelled,
                m_requestSource ? m_requestSource->version : 0,
                state.resultRevision,
                state.partCount,
                "Part request was cancelled by detach."));
    }
    if (!RemoveDisplay()) return false;

    m_service.reset();
    m_requestSource.reset();
    m_activeSource.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_activeCatalog.reset();
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

    if (m_activeSource && !GetSourceSame(m_activeSource)) {
        SetSourceStale();
    }
    if (m_requestSource && !GetSourceSame(m_requestSource)) {
        m_isSourceChanged = true;
        m_service->StopRequest();
        SetSourceStale();
    }

    if (m_activeRequestId != 0) {
        const auto progress = m_service->GetProgress(m_activeRequestId);
        if (progress) SetRequestProgress(*progress);
    }
    auto complete = m_service->GetComplete();
    if (!complete) return true;
    if (complete->requestId != m_activeRequestId) return true;
    SetRequestComplete(std::move(*complete));
    return true;
}

PartSegmentationAdmission PartSegmentationHostFeature::Impl::SendRequest(
    PartSegmentationRequest request,
    PartSegmentationCallback onComplete)
{
    PartSegmentationAdmission admission;
    if (!m_isAttached || !GetIsOwnerThread() || !m_service) {
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

        auto source = m_data ? m_data->GetImageSnapshot() : nullptr;
        if (!source || !source->image) {
            admission.status = PartAdmissionStatus::Unavailable;
            return admission;
        }
        const std::uint64_t requestId = GetNextRequestId();
        PartHistorySnapshot previous;
        std::uint64_t expectedResultRevision = 0;
        std::uint64_t expectedCatalogRevision = 0;
        if (GetHistorySourceSame(source)) {
            previous.labels = m_labelValues;
            previous.catalog = m_activeCatalog;
            expectedResultRevision = m_activeCatalog->resultRevision;
            expectedCatalogRevision = m_activeCatalog->catalogRevision;
        }
        const auto status = m_service->Start(
            source,
            params,
            m_config.maxWorkingBytes,
            requestId,
            std::move(previous),
            expectedResultRevision,
            expectedCatalogRevision);
        admission.status = status;
        if (status != PartAdmissionStatus::Accepted) return admission;

        admission.requestId = requestId;
        m_requestSource = std::move(source);
        m_requestViews = std::move(targetViews);
        m_startCallback = std::move(onComplete);
        m_activeRequestId = requestId;
        m_isSourceChanged = false;
        m_isStopRequested = false;
        SetRequestRunning(requestId, m_requestSource->version);
        return admission;
    }

    if (request.action == PartSegmentationAction::Stop) {
        const std::uint64_t requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        m_isStopRequested = m_activeRequestId != 0;
        m_service->StopRequest();
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                requestId,
                PartResultStatus::Succeeded,
                PartFailureReason::None,
                state.sourceVersion,
                state.resultRevision,
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
        const std::uint64_t requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        const bool isSucceeded = SetVisibility(*request.isVisible);
        const auto state = GetState();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                requestId,
                isSucceeded ? PartResultStatus::Succeeded
                            : PartResultStatus::Failed,
                isSucceeded ? PartFailureReason::None
                            : PartFailureReason::DisplayFailed,
                state.sourceVersion,
                state.resultRevision,
                state.partCount,
                isSucceeded ? "Part visibility was updated."
                            : "Part visibility update failed."));
        return admission;
    }

    if (request.action == PartSegmentationAction::Clear) {
        const std::uint64_t requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
        const auto previousState = GetState();
        const bool isSucceeded = ClearResult();
        SendComplete(
            std::move(onComplete),
            BuildResult(
                requestId,
                isSucceeded ? PartResultStatus::Succeeded
                            : PartResultStatus::Failed,
                isSucceeded ? PartFailureReason::None
                            : PartFailureReason::DisplayFailed,
                0,
                previousState.resultRevision,
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
    if (!currentSnapshot || !m_activeCatalog || !m_labelValues) {
        result.status = PartMutationStatus::Unavailable;
        return result;
    }

    try {
        auto candidate = std::make_shared<PartCatalog>(*m_activeCatalog);
        result = SetPartCatalogState(
            *candidate, part, patch, expectedCatalogRevision);
        if (result.status != PartMutationStatus::Succeeded
            || result.catalogRevision
                == m_activeCatalog->catalogRevision) {
            return result;
        }

        const auto previousStates =
            BuildPartRenderStateTable(*m_activeCatalog);
        const auto nextStates = BuildPartRenderStateTable(*candidate);
        const auto nextSnapshot = BuildPartSetSnapshot(
            *candidate, currentSnapshot->sourceVersion, false);
        if (!previousStates || !nextStates || !nextSnapshot) {
            return {
                PartMutationStatus::DisplayFailed,
                m_activeCatalog->catalogRevision
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
                m_activeCatalog->catalogRevision
            };
        }

        m_activeCatalog = std::move(candidate);
        auto state = GetState();
        state.catalogRevision = result.catalogRevision;
        SetPublishedState(std::move(state), nextSnapshot);
        return result;
    }
    catch (...) {
        result.status = PartMutationStatus::DisplayFailed;
        result.catalogRevision = m_activeCatalog
            ? m_activeCatalog->catalogRevision : 0;
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
    const std::uint64_t requestId = m_nextRequestId;
    ++m_nextRequestId;
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
    const TrustedImageSnapshot& source) const
{
    if (!m_data || !source) return false;
    const auto current = m_data->GetImageSnapshot();
    return current
        && current.get() == source.get()
        && current->version == source->version;
}

bool PartSegmentationHostFeature::Impl::GetHistorySourceSame(
    const TrustedImageSnapshot& source) const
{
    return source
        && source->image
        && m_activeSource
        && m_activeSource.get() == source.get()
        && m_activeSource->version == source->version
        && m_labelValues
        && m_labelImage
        && m_activeCatalog
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
    const DataVersion sourceVersion)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stateBeforeRequest = m_state;
    m_state.status = PartSegmentationStatus::Running;
    m_state.failureReason = PartFailureReason::None;
    m_state.requestId = requestId;
    m_state.sourceVersion = sourceVersion;
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
    // 1.0 只表示 owner thread 已提交完整 generation。
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
    vtkSmartPointer<vtkImageData> labelImage,
    const PartRenderStateTable& renderStates,
    const std::vector<HostFeatureView>& views,
    std::vector<OverlayBinding>& nextBindings)
{
    if (!m_views || !m_host || !labelImage || views.empty()) return false;
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
        candidate.overlay->SetInputData(labelImage);
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
        RemoveBindings(nextBindings);
        return false;
    }
    return true;
}

bool PartSegmentationHostFeature::Impl::RemoveDisplay()
{
    const bool hasDisplay = !m_bindings.empty() || !m_activeViews.empty();
    if (hasDisplay && m_host && !m_host->SetActiveViews({})) return false;
    RemoveBindings(m_bindings);
    return true;
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
    if (state.isOverlayVisible == isVisible) return true;
    if (!isVisible) {
        if (!RemoveDisplay()) return false;
        state.isOverlayVisible = false;
        SetState(state);
        return true;
    }
    if (!m_labelValues || !m_labelImage || m_activeViews.empty()) {
        state.isOverlayVisible = true;
        SetState(state);
        return true;
    }
    const auto renderStates = m_activeCatalog
        ? BuildPartRenderStateTable(*m_activeCatalog)
        : std::optional<PartRenderStateTable>{};
    if (!renderStates) return false;
    std::vector<OverlayBinding> nextBindings;
    if (!AttachDisplay(
            m_labelImage, *renderStates, m_activeViews, nextBindings)) {
        return false;
    }
    RemoveBindings(m_bindings);
    m_bindings = std::move(nextBindings);
    state.isOverlayVisible = true;
    SetState(state);
    return true;
}

bool PartSegmentationHostFeature::Impl::ClearResult()
{
    if (!RemoveDisplay()) return false;
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_activeCatalog.reset();
    m_activeSource.reset();
    m_activeViews.clear();
    PartSegmentationState state;
    state.isOverlayVisible = GetState().isOverlayVisible;
    SetPublishedState(std::move(state), {});
    return true;
}

void PartSegmentationHostFeature::Impl::SetSourceStale()
{
    if (!m_activeSource && !m_requestSource) return;
    auto state = GetState();
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
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_activeCatalog.reset();
    m_activeViews.clear();
}

void PartSegmentationHostFeature::Impl::SetRequestComplete(
    PartLabelCandidate candidate)
{
    auto callback = std::move(m_startCallback);
    const std::uint64_t requestId = m_activeRequestId;
    const DataVersion sourceVersion = m_requestSource
        ? m_requestSource->version : candidate.sourceVersion;
    const auto activeState = GetState();

    if (m_isSourceChanged || !GetSourceSame(m_requestSource)) {
        SetRequestFailed(PartFailureReason::SourceChanged);
        SendComplete(
            std::move(callback),
            BuildResult(
                requestId,
                PartResultStatus::Failed,
                PartFailureReason::SourceChanged,
                sourceVersion,
                activeState.resultRevision,
                GetState().partCount,
                "Part source changed before commit."));
    }
    else if (m_isStopRequested) {
        SetRequestFailed(PartFailureReason::Cancelled);
        SendComplete(
            std::move(callback),
            BuildResult(
                requestId,
                PartResultStatus::Cancelled,
                PartFailureReason::Cancelled,
                sourceVersion,
                activeState.resultRevision,
                GetState().partCount,
                "Part request was cancelled."));
    }
    else if (candidate.status != PartResultStatus::Succeeded) {
        const PartFailureReason reason = candidate.failureReason;
        const std::string message = candidate.message;
        SetRequestFailed(reason);
        SendComplete(
            std::move(callback),
            BuildResult(
                requestId,
                candidate.status,
                reason,
                sourceVersion,
                activeState.resultRevision,
                GetState().partCount,
                message));
    }
    else {
        auto labelView = BuildLabelView(candidate);
        const bool hasExpectedCatalog = candidate.expectedResultRevision != 0
            || candidate.expectedCatalogRevision != 0;
        const bool isRevisionExpected = hasExpectedCatalog
            ? m_activeCatalog
                && m_activeCatalog->resultRevision
                    == candidate.expectedResultRevision
                && m_activeCatalog->catalogRevision
                    == candidate.expectedCatalogRevision
            : !m_activeCatalog;
        const bool isCatalogValid = candidate.catalog
            && candidate.labels
            && GetPartCatalogValid(*candidate.catalog, *candidate.labels);
        const auto nextSnapshot = isCatalogValid
            ? BuildPartSetSnapshot(*candidate.catalog, sourceVersion, false)
            : std::shared_ptr<const PartSetSnapshot>{};
        const auto nextRenderStates = isCatalogValid
            ? BuildPartRenderStateTable(*candidate.catalog)
            : std::optional<PartRenderStateTable>{};
        std::vector<OverlayBinding> nextBindings;
        const bool isVisible = activeState.isOverlayVisible;
        bool isDisplayReady = true;
        if (isRevisionExpected && isCatalogValid && nextSnapshot
            && nextRenderStates
            && isVisible) {
            isDisplayReady = AttachDisplay(
                labelView.image,
                *nextRenderStates,
                m_requestViews,
                nextBindings);
        }
        else if (isRevisionExpected && isCatalogValid && nextSnapshot
            && nextRenderStates
            && m_host) {
            isDisplayReady = m_host->SetActiveViews({});
        }
        if (!labelView.labels || !labelView.image
            || !isRevisionExpected || !isCatalogValid || !nextSnapshot
            || !nextRenderStates
            || !isDisplayReady) {
            RemoveBindings(nextBindings);
            const PartFailureReason reason = !isRevisionExpected
                ? PartFailureReason::InternalError
                : !isCatalogValid || !nextSnapshot
                    ? PartFailureReason::InternalError
                    : PartFailureReason::DisplayFailed;
            SetRequestFailed(reason);
            SendComplete(
                std::move(callback),
                BuildResult(
                    requestId,
                    PartResultStatus::Failed,
                    reason,
                    sourceVersion,
                    activeState.resultRevision,
                    GetState().partCount,
                    reason == PartFailureReason::DisplayFailed
                        ? "Part display candidate failed."
                        : "Part catalog candidate became invalid."));
        }
        else {
            RemoveBindings(m_bindings);
            m_labelImage = nullptr;
            m_labelValues.reset();
            m_activeCatalog.reset();
            m_bindings = std::move(nextBindings);
            m_labelValues = std::move(labelView.labels);
            m_labelImage = std::move(labelView.image);
            m_activeCatalog = std::move(candidate.catalog);
            m_activeViews = std::move(m_requestViews);
            m_activeSource = m_requestSource;
            PartSegmentationState state;
            state.status = PartSegmentationStatus::Succeeded;
            state.failureReason = PartFailureReason::None;
            state.requestId = requestId;
            state.sourceVersion = sourceVersion;
            state.partSetId = nextSnapshot->partSetId;
            state.resultRevision = nextSnapshot->resultRevision;
            state.catalogRevision = nextSnapshot->catalogRevision;
            state.partCount = nextSnapshot->parts.size();
            state.progress = 1.0;
            state.isOverlayVisible = isVisible;
            const std::size_t partCount = state.partCount;
            SetPublishedState(std::move(state), nextSnapshot);
            SendComplete(
                std::move(callback),
                BuildResult(
                    requestId,
                    PartResultStatus::Succeeded,
                    PartFailureReason::None,
                    sourceVersion,
                    nextSnapshot->resultRevision,
                    partCount,
                    candidate.message));
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
    if (m_stateBeforeRequest.status == PartSegmentationStatus::Idle) {
        m_state = m_stateBeforeRequest;
        m_state.status = reason == PartFailureReason::Cancelled
            ? PartSegmentationStatus::Cancelled
            : PartSegmentationStatus::Failed;
        m_state.failureReason = reason;
        m_state.requestId = m_activeRequestId;
        m_state.sourceVersion = m_requestSource
            ? m_requestSource->version : 0;
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
