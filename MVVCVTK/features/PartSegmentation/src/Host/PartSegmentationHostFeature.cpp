#include "Host/PartSegmentationHostFeature.h"

#include "Render/Strategies/PartOverlayStrategies.h"
#include "Render/Contracts/OverlayService.h"
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

struct LabelViewCandidate final {
    // image 借用 labels 的稳定地址，成员逆序析构必须先释放 image。
    std::shared_ptr<std::vector<std::uint32_t>> labels;
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

private:
    struct OverlayBinding final {
        std::shared_ptr<OverlayService> service;
        std::shared_ptr<FeatureOverlay> overlay;
        std::string viewId;
    };

    struct StateBackup final {
        PartSegmentationStatus status = PartSegmentationStatus::Idle;
        PartFailureReason failureReason = PartFailureReason::None;
        std::uint64_t requestId = 0;
        DataVersion sourceVersion = 0;
        std::uint64_t resultRevision = 0;
        double progress = 0.0;
        bool isOverlayVisible = true;
    };

    bool GetIsOwnerThread() const noexcept;
    std::uint64_t GetNextRequestId() noexcept;
    std::vector<HostFeatureView> GetTargetViews(
        const HostViewTargets& targets) const;
    bool GetSourceSame(const TrustedImageSnapshot& source) const;
    void SetState(PartSegmentationState state);
    void SetRequestRunning(
        std::uint64_t requestId,
        DataVersion sourceVersion);
    void SetRequestProgress(double progress);
    void SendComplete(
        PartSegmentationCallback callback,
        PartSegmentationResult result) const noexcept;
    bool AttachDisplay(
        vtkSmartPointer<vtkImageData> labelImage,
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
    StateBackup m_stateBeforeRequest;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedFeatureDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::unique_ptr<PartSegmentationService> m_service;
    TrustedImageSnapshot m_requestSource;
    TrustedImageSnapshot m_activeSource;
    // m_labelImage 借用该 vector；声明顺序保证 image 先析构。
    std::shared_ptr<std::vector<std::uint32_t>> m_labelValues;
    vtkSmartPointer<vtkImageData> m_labelImage;
    std::shared_ptr<const PartSurfaceProduct> m_surfaceProduct;
    std::vector<HostFeatureView> m_requestViews;
    std::vector<HostFeatureView> m_activeViews;
    std::vector<OverlayBinding> m_bindings;
    PartSegmentationCallback m_startCallback;
    std::thread::id m_ownerThread;
    std::uint64_t m_nextRequestId = 1;
    std::uint64_t m_activeRequestId = 0;
    std::uint64_t m_resultRevision = 0;
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
                m_resultRevision,
                state.parts.size(),
                "Part request was cancelled by detach."));
    }
    if (!RemoveDisplay()) return false;

    m_service.reset();
    m_requestSource.reset();
    m_activeSource.reset();
    m_surfaceProduct.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
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
    SetState(idle);
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
        const auto status = m_service->Start(
            source, params, m_config.maxWorkingBytes, requestId);
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
                m_resultRevision,
                state.parts.size(),
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
                state.parts.size(),
                isSucceeded ? "Part visibility was updated."
                            : "Part visibility update failed."));
        return admission;
    }

    if (request.action == PartSegmentationAction::Clear) {
        const std::uint64_t requestId = GetNextRequestId();
        admission = { PartAdmissionStatus::Accepted, requestId };
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
                m_resultRevision,
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

void PartSegmentationHostFeature::Impl::SetState(
    PartSegmentationState state)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = std::move(state);
}

void PartSegmentationHostFeature::Impl::SetRequestRunning(
    const std::uint64_t requestId,
    const DataVersion sourceVersion)
{
    const std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stateBeforeRequest = {
        m_state.status,
        m_state.failureReason,
        m_state.requestId,
        m_state.sourceVersion,
        m_state.resultRevision,
        m_state.progress,
        m_state.isOverlayVisible
    };
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
        auto overlay = CreateOverlay(view.role);
        if (!service || !overlay) {
            RemoveBindings(nextBindings);
            return false;
        }
        if (view.role == HostRenderViewRole::Primary3D) {
            overlay->SetInputData(surfaceProduct->surface);
        }
        else {
            overlay->SetInputData(labelImage);
        }
        if (!service->AttachOverlay(overlay)) {
            RemoveBindings(nextBindings);
            return false;
        }
        nextBindings.push_back({ service, overlay, view.id });
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
    if (!m_labelValues || !m_labelImage || !m_surfaceProduct
        || m_activeViews.empty()) {
        state.isOverlayVisible = true;
        SetState(state);
        return true;
    }
    std::vector<OverlayBinding> nextBindings;
    if (!AttachDisplay(
            m_labelImage,
            m_surfaceProduct,
            m_activeViews,
            nextBindings)) {
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
    m_surfaceProduct.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_activeSource.reset();
    m_activeViews.clear();
    PartSegmentationState state;
    state.isOverlayVisible = GetState().isOverlayVisible;
    SetState(state);
    return true;
}

void PartSegmentationHostFeature::Impl::SetSourceStale()
{
    if (!m_activeSource && !m_requestSource) return;
    auto state = GetState();
    state.status = PartSegmentationStatus::Stale;
    state.failureReason = PartFailureReason::SourceChanged;
    state.progress = 0.0;
    SetState(state);
    // Stale 先表达 source 已失效；显示清理失败时保留完整 generation，
    // 下一次 owner tick 会重试，避免悬空或半清理。
    if (!RemoveDisplay()) return;
    m_activeSource.reset();
    m_surfaceProduct.reset();
    m_labelImage = nullptr;
    m_labelValues.reset();
    m_activeViews.clear();
}

void PartSegmentationHostFeature::Impl::SetRequestComplete(
    PartLabelCandidate candidate)
{
    auto callback = std::move(m_startCallback);
    const std::uint64_t requestId = m_activeRequestId;
    const DataVersion sourceVersion = m_requestSource
        ? m_requestSource->version : candidate.sourceVersion;

    if (m_isSourceChanged || !GetSourceSame(m_requestSource)) {
        SetRequestFailed(PartFailureReason::SourceChanged);
        SendComplete(
            std::move(callback),
            BuildResult(
                requestId,
                PartResultStatus::Failed,
                PartFailureReason::SourceChanged,
                sourceVersion,
                m_resultRevision,
                GetState().parts.size(),
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
                m_resultRevision,
                GetState().parts.size(),
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
                m_resultRevision,
                GetState().parts.size(),
                message));
    }
    else {
        auto labelView = BuildLabelView(candidate);
        std::vector<OverlayBinding> nextBindings;
        const bool isVisible = GetState().isOverlayVisible;
        bool isDisplayReady = true;
        if (isVisible) {
            isDisplayReady = AttachDisplay(
                labelView.image,
                candidate.surface,
                m_requestViews,
                nextBindings);
        }
        else if (m_host) {
            isDisplayReady = m_host->SetActiveViews({});
        }
        if (!labelView.labels || !labelView.image
            || !candidate.surface || !candidate.surface->surface
            || !isDisplayReady) {
            RemoveBindings(nextBindings);
            SetRequestFailed(PartFailureReason::DisplayFailed);
            SendComplete(
                std::move(callback),
                BuildResult(
                    requestId,
                    PartResultStatus::Failed,
                    PartFailureReason::DisplayFailed,
                    sourceVersion,
                    m_resultRevision,
                    GetState().parts.size(),
                    "Part display candidate failed."));
        }
        else {
            RemoveBindings(m_bindings);
            m_surfaceProduct.reset();
            m_labelImage = nullptr;
            m_labelValues.reset();
            m_bindings = std::move(nextBindings);
            m_labelValues = std::move(labelView.labels);
            m_labelImage = std::move(labelView.image);
            m_surfaceProduct = std::move(candidate.surface);
            m_activeViews = std::move(m_requestViews);
            m_activeSource = m_requestSource;
            ++m_resultRevision;
            PartSegmentationState state;
            state.status = PartSegmentationStatus::Succeeded;
            state.failureReason = PartFailureReason::None;
            state.requestId = requestId;
            state.sourceVersion = sourceVersion;
            state.resultRevision = m_resultRevision;
            state.progress = 1.0;
            state.isOverlayVisible = isVisible;
            state.parts = std::move(candidate.parts);
            const std::size_t partCount = state.parts.size();
            SetState(std::move(state));
            SendComplete(
                std::move(callback),
                BuildResult(
                    requestId,
                    PartResultStatus::Succeeded,
                    PartFailureReason::None,
                    sourceVersion,
                    m_resultRevision,
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
    m_state.status = m_stateBeforeRequest.status;
    m_state.failureReason = m_stateBeforeRequest.failureReason;
    m_state.requestId = m_stateBeforeRequest.requestId;
    m_state.sourceVersion = m_stateBeforeRequest.sourceVersion;
    m_state.resultRevision = m_stateBeforeRequest.resultRevision;
    m_state.progress = m_stateBeforeRequest.progress;
    m_state.isOverlayVisible = m_stateBeforeRequest.isOverlayVisible;
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
