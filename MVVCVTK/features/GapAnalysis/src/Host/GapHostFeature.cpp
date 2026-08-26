#include "Host/GapHostFeature.h"

#include "Services/GapAnalysisService.h"

#include <vtkImageData.h>

#include <cmath>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class GapHostFeature::Impl final {
public:
    struct CompleteItem final {
        std::mutex mutex;
        GapHostCallback onComplete;
        bool isActive = true;
        bool isSent = false;
    };

    struct ViewCandidate final {
        GapViewRequest request;
        std::vector<std::string> activeViewIds;
        DataVersion version = 0;
    };

    explicit Impl(GapHostConfig config)
        : m_config(std::move(config))
        , m_service(std::make_unique<GapAnalysisService>())
    {
    }

    bool AttachHost(
        GapHostFeature& owner,
        const HostFeatureContext& context);
    bool DetachHost();
    bool OnHostTick();
    bool SendRequest(
        GapHostRequest request,
        GapHostCallback onComplete);
    GapHostState GetState() const;

    static constexpr std::string_view FeatureId =
        "GapAnalysis";

private:
    static bool GetImageReady(vtkImageData* image);
    static bool GetCharMatched(
        const InteractionEvent& event,
        char keyCode);
    static bool GetChordMatched(
        const InteractionEvent& event,
        const HostKeyChord& chord);
    static bool GetChordValid(const HostKeyChord& chord);
    static bool GetStartValid(
        const GapHostStartParams& params);
    static bool GetSnapshotValid(
        const TrustedImageSnapshot& snapshot);
    static std::optional<Orientation> GetSliceOrient(
        HostRenderViewRole role);
    static bool SendComplete(
        const std::shared_ptr<CompleteItem>& item,
        bool isSuccess);

    std::optional<ViewCandidate> GetViewCandidate(
        const GapHostStartParams& start) const;
    bool StartView(
        const GapHostStartParams& start,
        GapHostCallback onComplete);
    bool SwitchOverlay();
    bool ExitView();
    InteractionResult OnInput(
        const InteractionEvent& event);
    bool SetActiveViews(
        const std::vector<std::string>& viewIds) const;
    bool ClearComplete();
    bool ClearBorrowed();
    bool GetOwnerThread() const;

    GapHostConfig m_config;
    std::unique_ptr<GapAnalysisService> m_service;
    std::shared_ptr<FeatureViewDirectory> m_views;
    std::shared_ptr<TrustedFeatureDataPort> m_data;
    std::shared_ptr<FeatureHostControl> m_host;
    std::shared_ptr<CompleteItem> m_completeItem;
    std::optional<DataVersion> m_activeVersion;
    std::thread::id m_ownerThread;
    bool m_isSwitchDown = false;
    bool m_isExitDown = false;
    bool m_isExitPending = false;
    bool m_isInputAttached = false;
    bool m_isAttached = false;
};

bool GapHostFeature::Impl::GetImageReady(
    vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }
    int dimensions[3] = {};
    image->GetDimensions(dimensions);
    return dimensions[0] > 0
        && dimensions[1] > 0
        && dimensions[2] > 0;
}

bool GapHostFeature::Impl::GetCharMatched(
    const InteractionEvent& event,
    const char keyCode)
{
    if (keyCode == 0) {
        return false;
    }
    const char upper = keyCode >= 'a' && keyCode <= 'z'
        ? static_cast<char>(keyCode - 'a' + 'A')
        : keyCode;
    return event.keyCode == keyCode
        || event.keyCode == upper
        || event.keySym == std::string(1, keyCode)
        || event.keySym == std::string(1, upper);
}

bool GapHostFeature::Impl::GetChordMatched(
    const InteractionEvent& event,
    const HostKeyChord& chord)
{
    const bool hasKey = GetCharMatched(
        event, chord.keyCode)
        || (!chord.keySym.empty()
            && event.keySym == chord.keySym);
    return hasKey
        && event.isCtrlDown == chord.isCtrlDown
        && event.isAltDown == chord.isAltDown
        && event.isShiftDown == chord.isShiftDown;
}

bool GapHostFeature::Impl::GetChordValid(
    const HostKeyChord& chord)
{
    return chord.keyCode != 0 || !chord.keySym.empty();
}

bool GapHostFeature::Impl::GetStartValid(
    const GapHostStartParams& params)
{
    if (params.targetViews.viewIds.empty()
        && params.targetViews.viewRoles.empty()) {
        return false;
    }
    if (!std::isfinite(params.surface.dataRangeRatio)
        || !std::isfinite(params.surface.absoluteIsoValue)
        || !std::isfinite(params.voidParams.grayMin)
        || !std::isfinite(params.voidParams.grayMax)
        || !std::isfinite(params.voidParams.minVolumeMM3)
        || !std::isfinite(
            params.voidParams.angleThresholdDeg)) {
        return false;
    }

    switch (params.surface.isoMode) {
    case GapIsoMode::DataRangeRatio:
        if (params.surface.dataRangeRatio < 0.0
            || params.surface.dataRangeRatio > 1.0) {
            return false;
        }
        break;
    case GapIsoMode::AbsoluteValue:
        break;
    default:
        return false;
    }

    return params.voidParams.grayMin
            <= params.voidParams.grayMax
        && params.voidParams.minVolumeMM3 >= 0.0
        && params.voidParams.angleThresholdDeg >= 0.0f
        && params.voidParams.angleThresholdDeg <= 180.0f
        && params.voidParams.tensorWindowSize > 0
        && params.voidParams.erosionIterations >= 0;
}

bool GapHostFeature::Impl::GetSnapshotValid(
    const TrustedImageSnapshot& snapshot)
{
    if (!snapshot
        || snapshot->version == 0
        || !GetImageReady(snapshot->image)
        || !std::isfinite(snapshot->scalarRange[0])
        || !std::isfinite(snapshot->scalarRange[1])
        || snapshot->scalarRange[0]
            > snapshot->scalarRange[1]) {
        return false;
    }

    int dimensions[3] = {};
    snapshot->image->GetDimensions(dimensions);
    for (int axis = 0; axis < 3; ++axis) {
        if (dimensions[axis] != snapshot->dims[axis]
            || !std::isfinite(snapshot->spacing[axis])
            || snapshot->spacing[axis] <= 0.0
            || !std::isfinite(snapshot->origin[axis])) {
            return false;
        }
    }
    return true;
}

std::optional<Orientation>
GapHostFeature::Impl::GetSliceOrient(
    const HostRenderViewRole role)
{
    switch (role) {
    case HostRenderViewRole::TopDownSlice:
        return Orientation::Top_down;
    case HostRenderViewRole::FrontBackSlice:
        return Orientation::Front_back;
    case HostRenderViewRole::LeftRightSlice:
        return Orientation::Left_right;
    default:
        return std::nullopt;
    }
}

bool GapHostFeature::Impl::SendComplete(
    const std::shared_ptr<CompleteItem>& item,
    const bool isSuccess)
{
    if (!item) {
        return false;
    }

    GapHostCallback callback;
    {
        const std::lock_guard<std::mutex> lock(item->mutex);
        if (!item->isActive || item->isSent) {
            return false;
        }
        item->isSent = true;
        callback = std::move(item->onComplete);
    }
    if (callback) {
        try {
            callback(isSuccess);
        }
        catch (...) {
        }
    }
    return true;
}

std::optional<GapHostFeature::Impl::ViewCandidate>
GapHostFeature::Impl::GetViewCandidate(
    const GapHostStartParams& start) const
{
    if (!m_views
        || !m_data
        || !GetStartValid(start)) {
        return std::nullopt;
    }

    const auto snapshot = m_data->GetImageSnapshot();
    if (!GetSnapshotValid(snapshot)) {
        return std::nullopt;
    }

    const auto views = m_views->GetViews(start.targetViews);
    if (views.empty()) {
        return std::nullopt;
    }

    ViewCandidate candidate;
    candidate.version = snapshot->version;
    candidate.request.inputImage = snapshot->image;
    candidate.request.validityMask =
        snapshot->validityMask;
    candidate.request.surface = start.surface;
    candidate.request.voidParams = start.voidParams;

    for (const auto& view : views) {
        const auto port = m_views->GetOverlayPort(view.id);
        if (view.id.empty() || !port) {
            continue;
        }
        candidate.activeViewIds.push_back(view.id);
        if (view.role
                == HostRenderViewRole::Primary3D
            || view.role
                == HostRenderViewRole::Composite3D) {
            candidate.request.meshTargets.push_back(
                port);
            continue;
        }
        if (const auto orientation =
                GetSliceOrient(view.role)) {
            candidate.request.sliceTargets.push_back(
                { *orientation, port });
        }
    }

    if (candidate.request.meshTargets.empty()
        && candidate.request.sliceTargets.empty()) {
        return std::nullopt;
    }
    return candidate;
}

bool GapHostFeature::Impl::AttachHost(
    GapHostFeature& owner,
    const HostFeatureContext& context)
{
    if (m_isAttached) {
        return false;
    }
    if (!context.views
        || !context.data
        || !context.host
        || (m_config.inputViews.viewIds.empty()
            && m_config.inputViews.viewRoles.empty())
        || !GetChordValid(m_config.keys.switchOverlay)
        || !GetChordValid(m_config.keys.exit)
        || !GetStartValid(m_config.defaultStart)) {
        return false;
    }

    const auto weakOwner = owner.weak_from_this();
    if (weakOwner.expired()) {
        return false;
    }

    m_views = context.views;
    m_data = context.data;
    m_host = context.host;
    m_ownerThread = std::this_thread::get_id();

    HostInputBinding binding;
    binding.featureId = std::string(FeatureId);
    binding.targetViews = m_config.inputViews;
    binding.onInput = [weakOwner](
        const InteractionEvent& event) {
        const auto feature = weakOwner.lock();
        return feature && feature->m_impl
            ? feature->m_impl->OnInput(event)
            : InteractionResult{};
    };
    if (!m_host->AttachInput(std::move(binding))) {
        ClearBorrowed();
        return false;
    }

    m_isInputAttached = true;
    m_isAttached = true;
    m_isSwitchDown = false;
    m_isExitDown = false;
    m_isExitPending = false;
    return true;
}

bool GapHostFeature::Impl::DetachHost()
{
    if (!m_isAttached) {
        return true;
    }
    if (!GetOwnerThread()) {
        return false;
    }

    bool isInputDetached = !m_isInputAttached;
    if (m_isInputAttached && m_host) {
        try {
            isInputDetached =
                m_host->DetachInput(FeatureId);
        }
        catch (...) {
            isInputDetached = false;
        }
        if (isInputDetached) {
            m_isInputAttached = false;
        }
    }

    // 即使输入端口暂时拒绝移除，也必须先完成强清理；失败重试只保留 Host 控制能力和 owner thread。
    ClearComplete();
    if (m_service) {
        m_service->ClearView();
    }
    (void)SetActiveViews({});
    m_activeVersion.reset();
    m_views.reset();
    m_data.reset();
    m_isSwitchDown = false;
    m_isExitDown = false;
    m_isExitPending = false;

    if (!isInputDetached) {
        return false;
    }

    m_isAttached = false;
    ClearBorrowed();
    return true;
}

bool GapHostFeature::Impl::OnHostTick()
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return false;
    }

    if (m_activeVersion && !m_isExitPending) {
        const auto snapshot = m_data
            ? m_data->GetImageSnapshot() : TrustedImageSnapshot{};
        if (!snapshot
            || snapshot->version != *m_activeVersion) {
            ClearComplete();
            if (m_service->ExitView()) {
                m_isExitPending = true;
            }
        }
    }

    if (m_service->GetDisplayTickNeeded()) {
        m_service->OnDisplayTick(nullptr);
    }
    if (m_isExitPending
        && !m_service->GetDisplayTickNeeded()) {
        if (SetActiveViews({})) {
            m_activeVersion.reset();
            m_isExitPending = false;
            ClearComplete();
        }
    }
    return true;
}

bool GapHostFeature::Impl::SendRequest(
    GapHostRequest request,
    GapHostCallback onComplete)
{
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()) {
        return false;
    }

    switch (request.action) {
    case GapHostAction::Start:
        if (!request.start) {
            return false;
        }
        return StartView(
            *request.start,
            std::move(onComplete));
    case GapHostAction::Overlay:
        if (onComplete) {
            return false;
        }
        return SwitchOverlay();
    case GapHostAction::Exit:
        if (onComplete) {
            return false;
        }
        return ExitView();
    case GapHostAction::None:
        return false;
    }
    return false;
}

GapHostState GapHostFeature::Impl::GetState() const
{
    GapHostState state;
    if (!m_isAttached
        || !m_service
        || !GetOwnerThread()
        || (!m_activeVersion && !m_isExitPending)) {
        return state;
    }

    state.analysisState = m_service->GetAnalysisState();
    state.statistics = m_service->GetStatistics();
    state.isViewActive = m_service->GetViewOn();
    state.isExitPending = m_isExitPending;
    return state;
}

bool GapHostFeature::Impl::StartView(
    const GapHostStartParams& start,
    GapHostCallback onComplete)
{
    auto candidate = GetViewCandidate(start);
    if (!candidate) {
        return false;
    }

    auto completeItem =
        std::make_shared<CompleteItem>();
    completeItem->onComplete = std::move(onComplete);
    const std::weak_ptr<CompleteItem> weakItem =
        completeItem;
    const bool isAccepted = m_service->StartView(
        std::move(candidate->request),
        [weakItem](bool isSuccess) {
            if (const auto item = weakItem.lock()) {
                (void)SendComplete(item, isSuccess);
            }
        });
    if (!isAccepted) {
        return false;
    }
    if (!SetActiveViews(candidate->activeViewIds)) {
        m_service->ClearView();
        ClearComplete();
        m_activeVersion.reset();
        m_isExitPending = false;
        return false;
    }

    ClearComplete();
    m_completeItem = std::move(completeItem);
    m_activeVersion = candidate->version;
    m_isExitPending = false;
    return true;
}

bool GapHostFeature::Impl::SwitchOverlay()
{
    return !m_isExitPending
        && m_service
        && m_service->GetViewOn()
        && m_service->SwitchOverlay();
}

bool GapHostFeature::Impl::ExitView()
{
    if (m_isExitPending
        || !m_service
        || !m_service->GetViewOn()
        || !m_service->ExitView()) {
        return false;
    }
    ClearComplete();
    m_activeVersion.reset();
    m_isExitPending = true;
    return true;
}

InteractionResult GapHostFeature::Impl::OnInput(
    const InteractionEvent& event)
{
    const bool isSwitch = GetChordMatched(
        event, m_config.keys.switchOverlay);
    const bool isExit = GetChordMatched(
        event, m_config.keys.exit);
    if (!isSwitch && !isExit) {
        return {};
    }

    if (event.eventKind
        == InteractionEventKind::KeyRelease) {
        const bool wasDown =
            (isSwitch && m_isSwitchDown)
            || (isExit && m_isExitDown);
        if (isSwitch) {
            m_isSwitchDown = false;
        }
        if (isExit) {
            m_isExitDown = false;
        }
        return wasDown
            ? InteractionResult{ true, true }
            : InteractionResult{};
    }
    if (event.eventKind
        == InteractionEventKind::TextInput) {
        return (isSwitch && m_isSwitchDown)
                || (isExit && m_isExitDown)
            ? InteractionResult{ true, true }
            : InteractionResult{};
    }
    if (event.eventKind
        != InteractionEventKind::KeyPress) {
        return {};
    }

    if (isExit) {
        if (!m_service->GetViewOn()
            || m_isExitPending) {
            return {};
        }
        if (m_isExitDown) {
            return { true, true };
        }
        m_isExitDown = true;
        GapHostRequest request;
        request.action = GapHostAction::Exit;
        (void)SendRequest(std::move(request), nullptr);
        return { true, true };
    }

    if (m_isSwitchDown) {
        return { true, true };
    }
    m_isSwitchDown = true;
    GapHostRequest request;
    if (m_service->GetViewOn()
        && !m_isExitPending) {
        request.action = GapHostAction::Overlay;
    }
    else {
        request.action = GapHostAction::Start;
        request.start = m_config.defaultStart;
    }
    (void)SendRequest(std::move(request), nullptr);
    return { true, true };
}

bool GapHostFeature::Impl::SetActiveViews(
    const std::vector<std::string>& viewIds) const
{
    return m_host
        && m_host->SetActiveViews(viewIds);
}

bool GapHostFeature::Impl::ClearComplete()
{
    if (!m_completeItem) {
        return true;
    }
    {
        const std::lock_guard<std::mutex> lock(
            m_completeItem->mutex);
        m_completeItem->isActive = false;
        m_completeItem->onComplete = nullptr;
    }
    m_completeItem.reset();
    return true;
}

bool GapHostFeature::Impl::ClearBorrowed()
{
    if (m_host) {
        (void)m_host->SetActiveViews({});
    }
    m_views.reset();
    m_data.reset();
    m_host.reset();
    m_ownerThread = {};
    m_isInputAttached = false;
    ClearComplete();
    return true;
}

bool GapHostFeature::Impl::GetOwnerThread() const
{
    return m_ownerThread == std::this_thread::get_id();
}

GapHostFeature::GapHostFeature(GapHostConfig config)
    : m_impl(std::make_unique<Impl>(
        std::move(config)))
{
}

GapHostFeature::~GapHostFeature() noexcept = default;

std::string_view GapHostFeature::GetFeatureId() const noexcept
{
    return Impl::FeatureId;
}

bool GapHostFeature::AttachHost(
    const HostFeatureContext& context)
{
    return m_impl
        && m_impl->AttachHost(*this, context);
}

bool GapHostFeature::DetachHost()
{
    return !m_impl || m_impl->DetachHost();
}

bool GapHostFeature::OnHostTick()
{
    return m_impl && m_impl->OnHostTick();
}

bool GapHostFeature::SendRequest(
    GapHostRequest request,
    GapHostCallback onComplete)
{
    return m_impl
        && m_impl->SendRequest(
            std::move(request),
            std::move(onComplete));
}

GapHostState GapHostFeature::GetState() const
{
    return m_impl ? m_impl->GetState() : GapHostState{};
}
