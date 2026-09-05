#include "Host/HostInputRegistry.h"

#include "Host/HostViewRuntimeRegistry.h"
#include "Interaction/AbstractViewContext.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::atomic<std::uint64_t> g_nextRegistryDomain{ 1 };

std::uint64_t GetNextRegistryDomain() noexcept
{
    std::uint64_t value = 0;
    while (value == 0) {
        value = g_nextRegistryDomain.fetch_add(
            1, std::memory_order_relaxed);
    }
    return value;
}

bool GetTargetMatched(
    const HostViewTargets& targets,
    const std::string& viewId,
    const HostRenderViewRole role)
{
    return std::find(
        targets.viewIds.begin(), targets.viewIds.end(), viewId)
            != targets.viewIds.end()
        || std::find(
            targets.viewRoles.begin(), targets.viewRoles.end(), role)
            != targets.viewRoles.end();
}

bool GetIsPointerPress(InteractionEventKind kind) noexcept
{
    return kind == InteractionEventKind::PrimaryPress
        || kind == InteractionEventKind::SecondaryPress;
}

InteractionEventKind GetReleaseKind(InteractionEventKind kind) noexcept
{
    return kind == InteractionEventKind::SecondaryPress
        ? InteractionEventKind::SecondaryRelease
        : InteractionEventKind::PrimaryRelease;
}

InteractionEventKind GetInteractionKind(HostInputKind kind) noexcept
{
    switch (kind) {
    case HostInputKind::WheelForward:
        return InteractionEventKind::WheelForward;
    case HostInputKind::WheelBackward:
        return InteractionEventKind::WheelBackward;
    case HostInputKind::PrimaryPress:
        return InteractionEventKind::PrimaryPress;
    case HostInputKind::PrimaryRelease:
        return InteractionEventKind::PrimaryRelease;
    case HostInputKind::SecondaryPress:
        return InteractionEventKind::SecondaryPress;
    case HostInputKind::SecondaryRelease:
        return InteractionEventKind::SecondaryRelease;
    case HostInputKind::PointerMove:
        return InteractionEventKind::PointerMove;
    case HostInputKind::KeyPress:
        return InteractionEventKind::KeyPress;
    case HostInputKind::KeyRelease:
        return InteractionEventKind::KeyRelease;
    case HostInputKind::TextInput:
        return InteractionEventKind::TextInput;
    case HostInputKind::Cancel:
        return InteractionEventKind::Cancel;
    case HostInputKind::None:
        return InteractionEventKind::None;
    }
    return InteractionEventKind::None;
}

void MergeResult(
    InteractionResult& aggregate,
    const InteractionResult& result) noexcept
{
    aggregate.isHandled = aggregate.isHandled || result.isHandled;
    aggregate.isPropagationStopped =
        aggregate.isPropagationStopped || result.isPropagationStopped;
    if (!result.isSucceeded) {
        aggregate.isSucceeded = false;
        if (aggregate.failureReason == InteractionFailureReason::None) {
            aggregate.failureReason = result.failureReason;
        }
    }
}

InteractionResult GetCallbackFailure(
    const InteractionEventKind kind) noexcept
{
    return {
        false,
        false,
        false,
        kind == InteractionEventKind::Cancel
            ? InteractionFailureReason::CleanupRejected
            : InteractionFailureReason::StateRejected };
}

InteractionResult GetRouteFailure(
    const InteractionEventKind kind) noexcept
{
    auto result = GetCallbackFailure(kind);
    result.isHandled = true;
    result.isPropagationStopped = true;
    return result;
}

} // namespace

class HostInputRegistry::Impl final
    : public std::enable_shared_from_this<HostInputRegistry::Impl>
{
public:
    enum class InputPhase {
        FeatureTool,
        HostExtension
    };

    struct Binding final {
        std::uint64_t id = 0;
        std::string featureId;
        HostViewTargets targets;
        HostCallback callback;
        InputPhase phase = InputPhase::FeatureTool;
    };

    struct InstalledRoute final {
        std::string viewId;
        std::weak_ptr<AbstractViewContext> context;
    };

    class InputPort final : public HostInputPort {
    public:
        explicit InputPort(Impl& owner) noexcept
            : m_owner(owner)
        {
        }

        bool AttachInput(HostInputBinding binding) override
        {
            return m_owner.AttachInput(std::move(binding));
        }

        bool DetachInput(std::string_view featureId) override
        {
            return m_owner.DetachInput(featureId);
        }

    private:
        Impl& m_owner;
    };

    class BindingCapture final : public IInteractionCapture {
    public:
        BindingCapture(
            std::weak_ptr<Impl> owner,
            std::shared_ptr<Binding> binding,
            std::string viewId,
            HostRenderViewRole role,
            InteractionEventKind releaseKind)
            : m_owner(std::move(owner))
            , m_binding(std::move(binding))
            , m_viewId(std::move(viewId))
            , m_role(role)
            , m_releaseKind(releaseKind)
        {
        }

        InteractionCaptureKey GetKey() const noexcept override
        {
            const auto owner = m_owner.lock();
            return owner && m_binding
                ? InteractionCaptureKey{ owner->m_domain, m_binding->id }
                : InteractionCaptureKey{};
        }

        InteractionEventKind GetReleaseKind() const noexcept override
        {
            return m_releaseKind;
        }

        InteractionResult Send(const InteractionEvent& event) override
        {
            const auto owner = m_owner.lock();
            return owner && m_binding
                ? owner->SendCaptured(
                    *m_binding, event, m_viewId, m_role)
                : GetRouteFailure(event.eventKind);
        }

    private:
        std::weak_ptr<Impl> m_owner;
        std::shared_ptr<Binding> m_binding;
        std::string m_viewId;
        HostRenderViewRole m_role = HostRenderViewRole::Auxiliary;
        InteractionEventKind m_releaseKind =
            InteractionEventKind::PrimaryRelease;
    };

    explicit Impl(std::weak_ptr<IHostViewDirectory> directory)
        : m_directory(std::move(directory))
        , m_domain(GetNextRegistryDomain())
        , m_inputPort(*this)
    {
    }

    bool Start(const HostViewTargets& allViews);
    HostInputPort& GetFeaturePort() noexcept { return m_inputPort; }
    bool AttachInput(HostInputBinding binding);
    bool DetachInput(std::string_view featureId);
    bool SetHostInput(HostViewTargets targets, HostCallback callback);
    bool ClearHostInput();
    HostInputResult SendInput(const HostInputEvent& event);
    bool Stop();
    InteractionDispatch Route(
        const InteractionEvent& event,
        const std::string& viewId,
        HostRenderViewRole role);

private:
    class DispatchGuard final {
    public:
        explicit DispatchGuard(std::size_t& depth) noexcept
            : m_depth(depth)
        {
            ++m_depth;
        }

        ~DispatchGuard()
        {
            --m_depth;
        }

    private:
        std::size_t& m_depth;
    };

    bool GetTargetsValid(const HostViewTargets& targets) const;
    bool CancelBinding(const std::shared_ptr<Binding>& binding);
    InteractionResult SendCaptured(
        const Binding& binding,
        const InteractionEvent& event,
        const std::string& viewId,
        HostRenderViewRole role);
    InteractionResult SendBinding(
        const Binding& binding,
        const InteractionEvent& event,
        const std::string& viewId,
        HostRenderViewRole role);

    std::weak_ptr<IHostViewDirectory> m_directory;
    const std::uint64_t m_domain;
    std::uint64_t m_nextBindingId = 1;
    InputPort m_inputPort;
    std::vector<std::shared_ptr<Binding>> m_featureBindings;
    std::shared_ptr<Binding> m_hostBinding;
    std::vector<InstalledRoute> m_installedRoutes;
    std::size_t m_dispatchDepth = 0;
    bool m_isStarted = false;
};

bool HostInputRegistry::Impl::GetTargetsValid(
    const HostViewTargets& targets) const
{
    if (targets.viewIds.empty() && targets.viewRoles.empty()) {
        return false;
    }
    const auto directory = m_directory.lock();
    if (!directory) return false;
    const auto routes = directory->GetInputRoutes(targets);
    if (routes.empty()) return false;
    return std::all_of(
        targets.viewIds.begin(), targets.viewIds.end(),
        [&routes](const std::string& id) {
            return !id.empty()
                && std::find_if(
                    routes.begin(), routes.end(),
                    [&id](const HostInputRoute& route) {
                        return route.id == id;
                    }) != routes.end();
        });
}

bool HostInputRegistry::Impl::Start(const HostViewTargets& allViews)
{
    if (m_dispatchDepth != 0) return false;
    if (m_isStarted) return true;
    if (!m_installedRoutes.empty() || !GetTargetsValid(allViews)) {
        return false;
    }

    const auto directory = m_directory.lock();
    if (!directory) return false;
    const auto routes = directory->GetInputRoutes(allViews);
    const std::weak_ptr<Impl> weakOwner = shared_from_this();
    const std::vector<InteractionEventKind> eventKinds{
        InteractionEventKind::WheelForward,
        InteractionEventKind::WheelBackward,
        InteractionEventKind::PrimaryPress,
        InteractionEventKind::PrimaryRelease,
        InteractionEventKind::SecondaryPress,
        InteractionEventKind::SecondaryRelease,
        InteractionEventKind::PointerMove,
        InteractionEventKind::KeyPress,
        InteractionEventKind::KeyRelease,
        InteractionEventKind::TextInput,
        InteractionEventKind::Cancel
    };

    bool isInstalled = true;
    for (const auto& route : routes) {
        const auto context = route.context.lock();
        if (!context) {
            isInstalled = false;
            break;
        }
        const auto viewId = route.id;
        const auto role = route.role;
        if (!context->SetInputHandler(
                [weakOwner, viewId, role](const InteractionEvent& event) {
                    const auto owner = weakOwner.lock();
                    return owner
                        ? owner->Route(event, viewId, role)
                        : InteractionDispatch{
                            GetRouteFailure(event.eventKind), nullptr };
                },
                eventKinds)) {
            isInstalled = false;
            break;
        }
        m_installedRoutes.push_back({ route.id, context });
    }
    if (isInstalled && !m_installedRoutes.empty()) {
        m_isStarted = true;
        return true;
    }

    for (auto route = m_installedRoutes.rbegin();
        route != m_installedRoutes.rend();) {
        const auto context = route->context.lock();
        if (!context || context->ClearInputHandler()) {
            route = decltype(route)(m_installedRoutes.erase(
                std::next(route).base()));
        }
        else {
            ++route;
        }
    }
    return false;
}

bool HostInputRegistry::Impl::AttachInput(HostInputBinding binding)
{
    if (!m_isStarted || m_dispatchDepth != 0
        || binding.featureId.empty() || !binding.onInput
        || !GetTargetsValid(binding.targetViews)) {
        return false;
    }
    const auto duplicate = std::find_if(
        m_featureBindings.begin(), m_featureBindings.end(),
        [&binding](const auto& current) {
            return current && current->featureId == binding.featureId;
        });
    if (duplicate != m_featureBindings.end()) return false;

    try {
        auto value = std::make_shared<Binding>();
        value->id = m_nextBindingId++;
        value->featureId = std::move(binding.featureId);
        value->targets = std::move(binding.targetViews);
        value->phase = InputPhase::FeatureTool;
        value->callback = [callback = std::move(binding.onInput)](
            const InteractionEvent& event,
            const std::string&,
            HostRenderViewRole) {
            return callback(event);
        };
        m_featureBindings.push_back(std::move(value));
        return true;
    }
    catch (...) {
        return false;
    }
}

bool HostInputRegistry::Impl::CancelBinding(
    const std::shared_ptr<Binding>& binding)
{
    if (!binding) return true;
    const auto directory = m_directory.lock();
    if (!directory) return false;
    const InteractionCaptureKey key{ m_domain, binding->id };
    bool isCancelled = true;
    for (const auto& route : directory->GetInputRoutes(binding->targets)) {
        const auto context = route.context.lock();
        if (context && !context->CancelInput(key).isSucceeded) {
            isCancelled = false;
        }
    }
    return isCancelled;
}

bool HostInputRegistry::Impl::DetachInput(
    const std::string_view featureId)
{
    if (featureId.empty() || m_dispatchDepth != 0) return false;
    const auto found = std::find_if(
        m_featureBindings.begin(), m_featureBindings.end(),
        [featureId](const auto& current) {
            return current && current->featureId == featureId;
        });
    if (found == m_featureBindings.end()) return true;
    if (!CancelBinding(*found)) return false;
    m_featureBindings.erase(found);
    return true;
}

bool HostInputRegistry::Impl::SetHostInput(
    HostViewTargets targets,
    HostCallback callback)
{
    if (!m_isStarted || m_dispatchDepth != 0 || !callback
        || !GetTargetsValid(targets)) {
        return false;
    }

    std::shared_ptr<Binding> next;
    try {
        next = std::make_shared<Binding>();
        next->id = m_nextBindingId++;
        next->featureId = "HostExtension";
        next->targets = std::move(targets);
        next->callback = std::move(callback);
        next->phase = InputPhase::HostExtension;
    }
    catch (...) {
        return false;
    }
    if (m_hostBinding && !CancelBinding(m_hostBinding)) {
        return false;
    }
    m_hostBinding = std::move(next);
    return true;
}

bool HostInputRegistry::Impl::ClearHostInput()
{
    if (m_dispatchDepth != 0) return false;
    if (!m_hostBinding) return true;
    if (!CancelBinding(m_hostBinding)) return false;
    m_hostBinding.reset();
    return true;
}

InteractionResult HostInputRegistry::Impl::SendBinding(
    const Binding& binding,
    const InteractionEvent& event,
    const std::string& viewId,
    const HostRenderViewRole role)
{
    if (!binding.callback) return {};
    try {
        return binding.callback(event, viewId, role);
    }
    catch (...) {
        std::cerr << "[Host] Input callback failed: "
                  << binding.featureId << '\n';
        return GetCallbackFailure(event.eventKind);
    }
}

InteractionResult HostInputRegistry::Impl::SendCaptured(
    const Binding& binding,
    const InteractionEvent& event,
    const std::string& viewId,
    const HostRenderViewRole role)
{
    if (m_dispatchDepth != 0) {
        return GetRouteFailure(event.eventKind);
    }
    DispatchGuard guard(m_dispatchDepth);
    auto routedEvent = event;
    routedEvent.viewId = viewId;
    return SendBinding(binding, routedEvent, viewId, role);
}

InteractionDispatch HostInputRegistry::Impl::Route(
    const InteractionEvent& event,
    const std::string& viewId,
    const HostRenderViewRole role)
{
    if (m_dispatchDepth != 0 || event.eventKind == InteractionEventKind::Cancel) {
        return event.eventKind == InteractionEventKind::Cancel
            ? InteractionDispatch{}
            : InteractionDispatch{
                GetRouteFailure(event.eventKind), nullptr };
    }

    std::vector<std::shared_ptr<Binding>> candidates;
    try {
        candidates.reserve(
            m_featureBindings.size() + (m_hostBinding ? 1 : 0));
        for (const auto& binding : m_featureBindings) {
            if (binding
                && GetTargetMatched(binding->targets, viewId, role)) {
                candidates.push_back(binding);
            }
        }
        if (m_hostBinding
            && GetTargetMatched(m_hostBinding->targets, viewId, role)) {
            candidates.push_back(m_hostBinding);
        }
    }
    catch (...) {
        return { GetRouteFailure(event.eventKind), nullptr };
    }

    DispatchGuard guard(m_dispatchDepth);
    auto routedEvent = event;
    routedEvent.viewId = viewId;
    InteractionResult aggregate;
    const bool isPress = GetIsPointerPress(event.eventKind);
    const std::weak_ptr<Impl> weakOwner = shared_from_this();
    for (const auto& binding : candidates) {
        std::unique_ptr<IInteractionCapture> capture;
        if (isPress) {
            try {
                capture = std::make_unique<BindingCapture>(
                    weakOwner,
                    binding,
                    viewId,
                    role,
                    GetReleaseKind(event.eventKind));
            }
            catch (...) {
                MergeResult(aggregate, GetRouteFailure(event.eventKind));
                return { aggregate, nullptr };
            }
        }

        const auto current = SendBinding(
            *binding, routedEvent, viewId, role);
        MergeResult(aggregate, current);
        if (isPress && current.isHandled) {
            return {
                aggregate,
                current.isSucceeded ? std::move(capture) : nullptr };
        }
        if (aggregate.isPropagationStopped) {
            return { aggregate, nullptr };
        }
    }
    return { aggregate, nullptr };
}

HostInputResult HostInputRegistry::Impl::SendInput(
    const HostInputEvent& event)
{
    if (!m_isStarted) {
        return {
            false,
            false,
            false,
            HostErrorCode::SessionNotReady,
            "Host input registry is not running." };
    }
    const auto kind = GetInteractionKind(event.kind);
    if (event.viewId.empty() || kind == InteractionEventKind::None) {
        return {
            false,
            false,
            false,
            HostErrorCode::RequestRejected,
            "Host input event has an invalid view or kind." };
    }

    const auto directory = m_directory.lock();
    if (!directory) {
        return {
            false,
            false,
            false,
            HostErrorCode::SessionNotReady,
            "Host input view directory is not available." };
    }
    HostViewTargets targets;
    targets.viewIds = { event.viewId };
    const auto routes = directory->GetInputRoutes(targets);
    if (routes.size() != 1 || routes.front().id != event.viewId) {
        return {
            false,
            false,
            false,
            HostErrorCode::RequestRejected,
            "Host input view was not found." };
    }
    if (routes.front().inputMode != HostInputMode::HostInjected) {
        return {
            false,
            false,
            false,
            HostErrorCode::RequestRejected,
            "Host input view uses native interactor input." };
    }
    const auto context = routes.front().context.lock();
    if (!context) {
        return {
            false,
            false,
            false,
            HostErrorCode::SessionNotReady,
            "Host input view context is not available." };
    }

    InteractionEvent routed;
    routed.viewId = event.viewId;
    routed.eventKind = kind;
    routed.x = event.x;
    routed.y = event.y;
    routed.isShiftDown = event.isShiftDown;
    routed.isCtrlDown = event.isCtrlDown;
    routed.isAltDown = event.isAltDown;
    routed.keyCode = event.keyCode;
    routed.keySym = event.keySym;
    try {
        const auto result = context->SendInput(routed);
        return {
            result.isSucceeded,
            result.isHandled,
            result.isPropagationStopped,
            result.isSucceeded ? HostErrorCode::None
                : HostErrorCode::OperationFailed,
            result.isSucceeded ? std::string{}
                : "Host input handler rejected the operation." };
    }
    catch (...) {
        return {
            false,
            false,
            true,
            HostErrorCode::OperationFailed,
            "Host input dispatch raised an exception." };
    }
}

bool HostInputRegistry::Impl::Stop()
{
    if (m_dispatchDepth != 0 || !m_featureBindings.empty()
        || m_hostBinding) {
        return false;
    }

    bool isStopped = true;
    auto route = m_installedRoutes.begin();
    while (route != m_installedRoutes.end()) {
        const auto context = route->context.lock();
        if (!context || context->ClearInputHandler()) {
            route = m_installedRoutes.erase(route);
        }
        else {
            isStopped = false;
            ++route;
        }
    }
    if (m_installedRoutes.empty()) {
        m_isStarted = false;
    }
    return isStopped && m_installedRoutes.empty();
}

HostInputRegistry::HostInputRegistry(
    std::weak_ptr<IHostViewDirectory> directory)
    : m_impl(std::make_shared<Impl>(std::move(directory)))
{
}

HostInputRegistry::~HostInputRegistry()
{
    if (m_impl && !m_impl->Stop()) {
        std::cerr
            << "[Host] Input registry destroyed before owner-thread cleanup.\n";
    }
}

bool HostInputRegistry::Start(HostViewTargets allViews)
{
    return m_impl && m_impl->Start(allViews);
}

HostInputPort& HostInputRegistry::GetFeaturePort()
{
    return m_impl->GetFeaturePort();
}

bool HostInputRegistry::SetHostInput(
    HostViewTargets targets,
    HostCallback onInput)
{
    return m_impl
        && m_impl->SetHostInput(std::move(targets), std::move(onInput));
}

bool HostInputRegistry::ClearHostInput()
{
    return m_impl && m_impl->ClearHostInput();
}

HostInputResult HostInputRegistry::SendInput(const HostInputEvent& event)
{
    return m_impl
        ? m_impl->SendInput(event)
        : HostInputResult{
            false,
            false,
            false,
            HostErrorCode::SessionNotReady,
            "Host input registry is not available." };
}

bool HostInputRegistry::Stop()
{
    return m_impl && m_impl->Stop();
}
