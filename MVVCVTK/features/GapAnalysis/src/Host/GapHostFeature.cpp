#include "Host/GapHostFeature.h"

#include "AppService.h"
#include "Host/HostRenderViewSet.h"
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
        const GapHostStartRequest& request);
    static bool GetSnapshotValid(
        const ImageSnapshot& snapshot);
    static std::optional<Orientation> GetSliceOrient(
        HostRenderViewRole role);
    static bool SendComplete(
        const std::shared_ptr<CompleteItem>& item,
        bool isSuccess);

    std::optional<ViewCandidate> GetViewCandidate(
        const GapHostStartRequest& start) const;
    bool StartView(
        const GapHostStartRequest& start,
        GapHostCallback onComplete);
    bool SwitchOverlay();
    bool ExitView();
    InteractionResult OnInput(
        const InteractionEvent& event);
    bool ClearComplete();
    bool ClearBorrowed();
    bool GetOwnerThread() const;

    GapHostConfig m_config;
    std::unique_ptr<GapAnalysisService> m_service;
    const HostRenderViewSet* m_renderViews = nullptr;
    HostInputPort* m_inputPort = nullptr;
    std::function<ImageSnapshot()> m_getImageSnapshot;
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
    const GapHostStartRequest& request)
{
    if (request.targetViews.viewIds.empty()
        && request.targetViews.viewRoles.empty()) {
        return false;
    }
    if (!std::isfinite(request.surface.dataRangeRatio)
        || !std::isfinite(request.surface.absoluteIsoValue)
        || !std::isfinite(request.voidParams.grayMin)
        || !std::isfinite(request.voidParams.grayMax)
        || !std::isfinite(request.voidParams.minVolumeMM3)
        || !std::isfinite(
            request.voidParams.angleThresholdDeg)) {
        return false;
    }

    switch (request.surface.isoMode) {
    case GapIsoMode::DataRangeRatio:
        if (request.surface.dataRangeRatio < 0.0
            || request.surface.dataRangeRatio > 1.0) {
            return false;
        }
        break;
    case GapIsoMode::AbsoluteValue:
        break;
    default:
        return false;
    }

    return request.voidParams.grayMin
            <= request.voidParams.grayMax
        && request.voidParams.minVolumeMM3 >= 0.0
        && request.voidParams.angleThresholdDeg >= 0.0f
        && request.voidParams.angleThresholdDeg <= 180.0f
        && request.voidParams.tensorWindowSize > 0
        && request.voidParams.erosionIterations >= 0;
}

bool GapHostFeature::Impl::GetSnapshotValid(
    const ImageSnapshot& snapshot)
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
    const GapHostStartRequest& start) const
{
    if (!m_renderViews
        || !m_getImageSnapshot
        || !GetStartValid(start)) {
        return std::nullopt;
    }

    const auto snapshot = m_getImageSnapshot();
    if (!GetSnapshotValid(snapshot)) {
        return std::nullopt;
    }

    const auto views =
        m_renderViews->GetViewsByTargets(
            start.targetViews);
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

    for (const auto* view : views) {
        if (!view || !view->service) {
            continue;
        }
        std::shared_ptr<OverlayService> overlay =
            view->service;
        if (view->config.role
                == HostRenderViewRole::Primary3D
            || view->config.role
                == HostRenderViewRole::Composite3D) {
            candidate.request.meshTargets.push_back(
                std::move(overlay));
            continue;
        }
        if (const auto orientation =
                GetSliceOrient(view->config.role)) {
            candidate.request.sliceTargets.push_back(
                { *orientation, std::move(overlay) });
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
    if (!context.renderViews
        || !context.getImageSnapshot
        || !context.inputPort
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

    m_renderViews = context.renderViews;
    m_inputPort = context.inputPort;
    m_getImageSnapshot = context.getImageSnapshot;
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
    if (!m_inputPort->AttachInput(std::move(binding))) {
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
    if (m_isInputAttached && m_inputPort) {
        try {
            isInputDetached =
                m_inputPort->DetachInput(FeatureId);
        }
        catch (...) {
            isInputDetached = false;
        }
        if (isInputDetached) {
            m_isInputAttached = false;
        }
    }

    // 即使输入端口暂时拒绝移除，也必须先完成强清理；失败重试只保留 inputPort/owner thread。
    ClearComplete();
    if (m_service) {
        m_service->ClearView();
    }
    m_activeVersion.reset();
    m_renderViews = nullptr;
    m_getImageSnapshot = {};
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
        const auto snapshot = m_getImageSnapshot
            ? m_getImageSnapshot() : ImageSnapshot{};
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
        m_activeVersion.reset();
        m_isExitPending = false;
        ClearComplete();
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
        if (!std::holds_alternative<GapHostStartRequest>(
                request.payload)) {
            return false;
        }
        return StartView(
            std::get<GapHostStartRequest>(
                std::move(request.payload)),
            std::move(onComplete));
    case GapHostAction::Overlay:
        if (onComplete
            || !std::holds_alternative<std::monostate>(
                request.payload)) {
            return false;
        }
        return SwitchOverlay();
    case GapHostAction::Exit:
        if (onComplete
            || !std::holds_alternative<std::monostate>(
                request.payload)) {
            return false;
        }
        return ExitView();
    case GapHostAction::None:
        return false;
    }
    return false;
}

bool GapHostFeature::Impl::StartView(
    const GapHostStartRequest& start,
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
        request.payload = std::monostate{};
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
        request.payload = std::monostate{};
    }
    else {
        request.action = GapHostAction::Start;
        request.payload = m_config.defaultStart;
    }
    (void)SendRequest(std::move(request), nullptr);
    return { true, true };
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
    m_renderViews = nullptr;
    m_inputPort = nullptr;
    m_getImageSnapshot = {};
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
