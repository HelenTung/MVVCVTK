#include "Host/ModelRotationHostFeature.h"
#include "Host/FeatureModelTransformPort.h"
#include "App/Services/FeatureViewService.h"
#include "Algorithms/ModelRotationAlgorithm.h"

#include <vtkCamera.h>
#include <vtkMath.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

class ModelRotationHostFeature::Impl final {
public:
    using Math = ModelRotationAlgorithm;
    struct History final {
        ModelTransformSnapshot before;
        Math::Matrix after;
        std::uint64_t afterRevision = 0;
    };
    struct Gesture final {
        std::string viewId;
        ToolMode toolMode = ToolMode::Navigation;
        Math::Point center{}, position{}, focal{}, up{}, right{}, toward{};
        std::array<double, 4> viewport{};
        std::array<int, 2> size{};
        double centerX = 0, centerY = 0, startX = 0, startY = 0, radius = 0;
        double parallelScale = 0, viewAngle = 0;
        bool isParallel = false, isSlice = false;
    };
    explicit Impl(ModelRotationConfig config) : m_config(std::move(config))
    {
        m_history.reserve(100);
    }

    bool GetOwnerReady() const
    {
        return m_port && m_owner == std::this_thread::get_id()
            && m_port->GetTransformState().has_value();
    }

    bool ClearSource()
    {
        if (!m_sourcePort) return true;
        if (!m_sourcePort->SetInteracting({ "ModelRotation", "drag" }, false)) return false;
        m_sourcePort.reset();
        return true;
    }

    bool SetCompletion()
    {
        if (!GetOwnerReady()) return false;
        const auto state = *m_port->GetTransformState();
        if (m_token && state.editToken != m_token) {
            if (!ClearSource()) return false;
            if (state.completedToken == m_token
                && state.completion == ModelTransformStatus::Committed) {
                if (m_isUndo) {
                    if (!m_history.empty()) m_history.pop_back();
                    if (!m_history.empty()) m_history.back().afterRevision = state.transformRevision;
                }
                else if (m_start.modelToWorld != state.modelToWorld) {
                    if (m_history.size() == 100) m_history.erase(m_history.begin());
                    m_history.push_back({ m_start, state.modelToWorld, state.transformRevision });
                }
                m_status = ModelRotationStatus::Succeeded;
            }
            else {
                m_status = state.completedToken == m_token
                    && state.completion == ModelTransformStatus::Cancelled
                        ? ModelRotationStatus::Cancelled : ModelRotationStatus::Invalidated;
                if (m_status == ModelRotationStatus::Cancelled && !m_history.empty()
                    && m_history.back().after == state.modelToWorld)
                    m_history.back().afterRevision = state.transformRevision;
            }
            m_token = 0;
            m_gesture.reset();
            m_isUndo = false;
        }
        if (!m_token && !m_history.empty()) {
            const auto& last = m_history.back();
            if (last.before.sessionGeneration != state.sessionGeneration
                || last.before.dataRevision != state.dataRevision
                || last.before.bindingRevision != state.bindingRevision
                || last.afterRevision != state.transformRevision
                || last.after != state.modelToWorld) m_history.clear();
        }
        return true;
    }

    bool StopGesture()
    {
        if (!SetCompletion()) return false;
        if (m_token && !m_port->StopTransform(m_token)) return false;
        if (!ClearSource()) return false;
        m_gesture.reset();
        if (m_token) m_status = ModelRotationStatus::Pending;
        return true;
    }

    bool StartEdit()
    {
        if (!SetCompletion() || m_token) return false;
        const auto state = m_port->GetTransformState();
        if (!state) return false;
        const auto token = m_port->StartTransform(*state);
        if (!token) return false;
        m_start = *state;
        m_token = *token;
        m_sequence = 0;
        return true;
    }

    std::optional<Math::Point> GetCenter(const ModelTransformSnapshot& state) const
    {
        const auto image = m_context.read->GetImageDescriptor();
        if (!image || image->dataRevision != state.dataRevision
            || image->bindingRevision != state.bindingRevision) return {};
        const auto center = Math::GetModelCenter(*image);
        return center ? std::optional<Math::Point>(
            Math::GetWorldPoint(state.modelToWorld, *center)) : std::nullopt;
    }

    std::optional<Gesture> BuildGesture(const InteractionEvent& event) const
    {
        const auto input = m_context.views->GetInputView({ event.viewId });
        const auto lease = input ? input->lease.lock() : nullptr;
        if (!input || !lease || !lease->GetIsActive() || !lease->GetIsOwnerThread()
            || !input->renderer || !input->renderer->GetRenderWindow()) return {};
        auto* camera = input->renderer->GetActiveCamera();
        if (!camera) return {};
        const auto state = m_port->GetTransformState();
        const auto center = state ? GetCenter(*state) : std::nullopt;
        if (!center) return {};
        Gesture gesture;
        gesture.viewId = event.viewId;
        gesture.toolMode = event.toolMode;
        gesture.center = *center;
        camera->GetPosition(gesture.position.data());
        camera->GetFocalPoint(gesture.focal.data());
        camera->GetViewUp(gesture.up.data());
        gesture.parallelScale = camera->GetParallelScale();
        gesture.viewAngle = camera->GetViewAngle();
        gesture.isParallel = camera->GetParallelProjection() != 0;
        for (int axis = 0; axis < 3; ++axis)
            gesture.toward[axis] = gesture.position[axis] - gesture.focal[axis];
        if (vtkMath::Normalize(gesture.toward.data()) <= 1e-12) return {};
        vtkMath::Cross(gesture.up.data(), gesture.toward.data(), gesture.right.data());
        if (vtkMath::Normalize(gesture.right.data()) <= 1e-12) return {};
        // 保存相机原始 up 用于不兼容检测；计算基向量时再正交化。
        auto* size = input->renderer->GetRenderWindow()->GetSize();
        gesture.size = { size[0], size[1] };
        std::copy_n(input->renderer->GetViewport(), 4, gesture.viewport.begin());
        gesture.radius = 0.5 * std::min(size[0] * (gesture.viewport[2]-gesture.viewport[0]),
            size[1] * (gesture.viewport[3]-gesture.viewport[1]));
        if (!std::isfinite(gesture.radius) || gesture.radius <= 1) return {};
        input->renderer->SetWorldPoint((*center)[0], (*center)[1], (*center)[2], 1);
        input->renderer->WorldToDisplay();
        const auto* display = input->renderer->GetDisplayPoint();
        gesture.centerX = display[0];
        gesture.centerY = display[1];
        if (!std::isfinite(display[0]) || !std::isfinite(display[1])) return {};
        gesture.startX = event.x - display[0];
        gesture.startY = event.y - display[1];
        gesture.isSlice = event.vizMode == VizMode::SliceTop_down
            || event.vizMode == VizMode::SliceFront_back
            || event.vizMode == VizMode::SliceLeft_right;
        return gesture;
    }

    bool GetGestureValid() const
    {
        if (!m_gesture) return false;
        const auto& gesture = *m_gesture;
        const auto input = m_context.views->GetInputView({ gesture.viewId });
        const auto lease = input ? input->lease.lock() : nullptr;
        if (!input || !lease || !lease->GetIsActive() || !lease->GetIsOwnerThread()
            || !input->renderer || !input->renderer->GetRenderWindow()) return false;
        auto* camera = input->renderer->GetActiveCamera();
        if (!camera) return false;
        auto* size = input->renderer->GetRenderWindow()->GetSize();
        const auto same = [](const double* values, const auto& expected) {
            for (std::size_t index = 0; index < expected.size(); ++index)
                if (!std::isfinite(values[index])
                    || std::abs(values[index] - expected[index]) > 1e-8) return false;
            return true;
        };
        return size[0] == gesture.size[0] && size[1] == gesture.size[1]
            && same(camera->GetPosition(), gesture.position)
            && same(camera->GetFocalPoint(), gesture.focal)
            && same(camera->GetViewUp(), gesture.up)
            && same(input->renderer->GetViewport(), gesture.viewport)
            && camera->GetParallelScale() == gesture.parallelScale
            && camera->GetViewAngle() == gesture.viewAngle
            && (camera->GetParallelProjection() != 0) == gesture.isParallel;
    }

    std::optional<Math::Matrix> GetCandidate(int x, int y) const
    {
        const auto& gesture = *m_gesture;
        const double dx = x - gesture.centerX, dy = y - gesture.centerY;
        std::optional<Math::Quaternion> rotation;
        if (gesture.isSlice) {
            if (std::hypot(gesture.startX, gesture.startY) < 2 || std::hypot(dx, dy) < 2)
                return m_start.modelToWorld;
            constexpr double degreesPerRadian = 57.295779513082320876;
            rotation = Math::GetAxisRotation(gesture.toward,
                std::atan2(gesture.startX*dy - gesture.startY*dx,
                    gesture.startX*dx + gesture.startY*dy) * degreesPerRadian);
        }
        else {
            Math::Point up{};
            vtkMath::Cross(gesture.toward.data(), gesture.right.data(), up.data());
            rotation = Math::GetTrackballRotation(gesture.startX, gesture.startY,
                dx, dy, gesture.radius, gesture.right, up, gesture.toward);
        }
        return rotation ? Math::GetRotatedMatrix(m_start.modelToWorld,
            gesture.center, *rotation) : std::nullopt;
    }

    InteractionResult OnInput(const InteractionEvent& event)
    {
        const auto result = [](bool succeeded) {
            return InteractionResult{ true, true, succeeded, succeeded
                ? InteractionFailureReason::None : InteractionFailureReason::StateRejected };
        };
        if (!SetCompletion()) return {};
        const bool isCancel = event.eventKind == InteractionEventKind::Cancel
            || event.eventKind == InteractionEventKind::Exit
            || (event.eventKind == InteractionEventKind::KeyPress
                && (event.keySym == "Escape" || event.keyCode == 27));
        if (isCancel && m_gesture) return result(StopGesture());
        if (m_gesture) {
            if (event.viewId != m_gesture->viewId) return {};
            if (event.toolMode != m_gesture->toolMode || !GetGestureValid())
                return result(StopGesture());
            const bool isRelease = event.eventKind == InteractionEventKind::PrimaryRelease;
            if (event.eventKind == InteractionEventKind::KeyPress
                || event.eventKind == InteractionEventKind::KeyRelease
                || event.eventKind == InteractionEventKind::TextInput) return {};
            if (event.eventKind != InteractionEventKind::PointerMove && !isRelease)
                return result(true);
            const auto candidate = GetCandidate(event.x, event.y);
            if (!candidate) return result(StopGesture());
            const auto sequence = m_sequence + 1;
            const bool accepted = isRelease
                ? m_port->SetTransformCommit(m_token, sequence, *candidate)
                : m_port->SetTransformPreview(m_token, sequence, *candidate);
            if (!accepted) return result(false);
            m_sequence = sequence;
            if (isRelease) {
                m_status = ModelRotationStatus::Pending;
                m_gesture.reset();
                return result(ClearSource());
            }
            return result(true);
        }
        const bool isSlice = event.vizMode == VizMode::SliceTop_down
            || event.vizMode == VizMode::SliceFront_back
            || event.vizMode == VizMode::SliceLeft_right;
        const bool isSliceShortcut = isSlice && event.isCtrlDown;
        if ((!m_isEnabled && !isSliceShortcut)
            || event.eventKind != InteractionEventKind::PrimaryPress
            || event.isShiftDown || (event.isCtrlDown && !isSliceShortcut)
            || event.isAltDown) return {};
        auto gesture = BuildGesture(event);
        if (!gesture || !StartEdit()) return result(false);
        m_sourcePort = m_context.views->GetFeaturePort(event.viewId);
        if (!m_sourcePort || !m_sourcePort->SetInteracting({ "ModelRotation", "drag" }, true)) {
            (void)StopGesture();
            return result(false);
        }
        m_gesture = std::move(gesture);
        m_status = ModelRotationStatus::Dragging;
        return result(true);
    }

    ModelRotationConfig m_config;
    HostFeatureContext m_context;
    std::shared_ptr<FeatureModelTransformPort> m_port;
    std::shared_ptr<FeatureViewService> m_sourcePort;
    std::thread::id m_owner;
    ModelTransformSnapshot m_start;
    std::uint64_t m_token = 0, m_sequence = 0;
    std::optional<Gesture> m_gesture;
    std::vector<History> m_history;
    ModelRotationStatus m_status = ModelRotationStatus::Detached;
    bool m_isEnabled = false, m_isUndo = false;
};

ModelRotationHostFeature::ModelRotationHostFeature(ModelRotationConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {}
ModelRotationHostFeature::~ModelRotationHostFeature() noexcept = default;
std::string_view ModelRotationHostFeature::GetFeatureId() const noexcept { return "ModelRotation"; }

bool ModelRotationHostFeature::AttachHost(const HostFeatureContext& context)
{
    if (m_impl->m_port || !context.host || !context.views || !context.read) return false;
    const auto port = std::dynamic_pointer_cast<FeatureModelTransformPort>(context.host);
    if (!port || !port->GetTransformState()
        || context.views->GetViews(m_impl->m_config.targetViews).empty()) return false;
    const auto weak = weak_from_this();
    if (weak.expired()) return false;
    m_impl->m_context = { context.views, context.read, {}, context.host };
    m_impl->m_port = port;
    m_impl->m_owner = std::this_thread::get_id();
    HostInputBinding binding{ std::string(GetFeatureId()), m_impl->m_config.targetViews,
        [weak](const InteractionEvent& event) {
            const auto feature = weak.lock();
            return feature ? feature->m_impl->OnInput(event) : InteractionResult{};
        } };
    if (!context.host->AttachInput(std::move(binding))) {
        m_impl->m_context = {};
        m_impl->m_port.reset();
        return false;
    }
    m_impl->m_status = ModelRotationStatus::Idle;
    return true;
}

bool ModelRotationHostFeature::DetachHost()
{
    if (!m_impl->m_port) return true;
    if (!m_impl->GetOwnerReady() || !m_impl->StopGesture()) return false;
    if (!m_impl->m_context.host->DetachInput(GetFeatureId())) return false;
    m_impl->m_context = {};
    m_impl->m_port.reset();
    m_impl->m_history.clear();
    m_impl->m_token = 0;
    m_impl->m_isEnabled = false;
    m_impl->m_status = ModelRotationStatus::Detached;
    return true;
}

bool ModelRotationHostFeature::OnHostTick()
{
    if (!m_impl->SetCompletion()) return false;
    if (!m_impl->m_gesture && !m_impl->ClearSource()) return false;
    return !m_impl->m_gesture || m_impl->GetGestureValid() || m_impl->StopGesture();
}

bool ModelRotationHostFeature::SendRequest(const ModelRotationRequest& request)
{
    if (!m_impl->SetCompletion()) return false;
    if (request.action != ModelRotationAction::Rotate
        && (request.angleDeg != 0 || request.worldCenter
            || request.worldAxis != std::array<double,3>{0,0,1})) return false;
    switch (request.action) {
    case ModelRotationAction::SetEnabled:
        if (!request.isEnabled && !m_impl->StopGesture()) return false;
        m_impl->m_isEnabled = request.isEnabled;
        return true;
    case ModelRotationAction::Cancel:
        return m_impl->StopGesture();
    case ModelRotationAction::Undo:
        if (m_impl->m_token || m_impl->m_history.empty() || !m_impl->StartEdit()) return false;
        if (!m_impl->m_port->SetTransformCommit(m_impl->m_token, 1,
                m_impl->m_history.back().before.modelToWorld)) {
            (void)m_impl->StopGesture();
            return false;
        }
        m_impl->m_isUndo = true;
        m_impl->m_status = ModelRotationStatus::Pending;
        return true;
    case ModelRotationAction::Rotate: {
        const auto rotation = Impl::Math::GetAxisRotation(request.worldAxis, request.angleDeg);
        const auto state = m_impl->m_port->GetTransformState();
        const auto center = request.worldCenter ? request.worldCenter
            : state ? m_impl->GetCenter(*state) : std::nullopt;
        const auto matrix = rotation && center && state
            ? Impl::Math::GetRotatedMatrix(state->modelToWorld, *center, *rotation) : std::nullopt;
        if (!matrix || !m_impl->StartEdit()) return false;
        if (!m_impl->m_port->SetTransformCommit(m_impl->m_token, 1, *matrix)) {
            (void)m_impl->StopGesture();
            return false;
        }
        m_impl->m_status = ModelRotationStatus::Pending;
        return true;
    }
    }
    return false;
}

ModelRotationState ModelRotationHostFeature::GetState() const
{
    ModelRotationState result{
        m_impl->m_status, m_impl->m_isEnabled, m_impl->m_history.size() };
    if (!m_impl->m_port) return result;
    if (!m_impl->GetOwnerReady()) return {};
    const auto state = *m_impl->m_port->GetTransformState();
    // Get 只投影已提交结果；历史容器与来源清理由下一 tick/请求推进。
    if (m_impl->m_token && state.editToken != m_impl->m_token) {
        if (m_impl->m_sourcePort) {
            result.status = ModelRotationStatus::Pending;
            return result;
        }
        if (state.completedToken == m_impl->m_token
            && state.completion == ModelTransformStatus::Committed) {
            result.status = ModelRotationStatus::Succeeded;
            result.undoCount = m_impl->m_isUndo
                ? (result.undoCount == 0 ? 0 : result.undoCount - 1)
                : std::min<std::size_t>(100, result.undoCount
                    + (m_impl->m_start.modelToWorld != state.modelToWorld ? 1 : 0));
        }
        else {
            result.status = state.completedToken == m_impl->m_token
                && state.completion == ModelTransformStatus::Cancelled
                    ? ModelRotationStatus::Cancelled : ModelRotationStatus::Invalidated;
            if (result.status == ModelRotationStatus::Invalidated) result.undoCount = 0;
        }
    }
    else if (!m_impl->m_token && !m_impl->m_history.empty()) {
        const auto& last = m_impl->m_history.back();
        if (last.before.sessionGeneration != state.sessionGeneration
            || last.before.dataRevision != state.dataRevision
            || last.before.bindingRevision != state.bindingRevision
            || last.afterRevision != state.transformRevision
            || last.after != state.modelToWorld) result.undoCount = 0;
    }
    return result;
}
