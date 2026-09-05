#include "HostInputRegistryTests.h"

#include "Host/HostInputRegistry.h"
#include "Host/HostRoutes.h"
#include "Host/Types/HostRequestTypes.h"
#include "Host/Types/HostSessionTypes.h"
#include "ViewContext.h"

#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class TestInputEndpoint final : public HostInputEndpoint {
public:
    HostInputResult SendInput(const HostInputEvent& event) override
    {
        return {
            event.kind != HostInputKind::None,
            true,
            true,
            HostErrorCode::None,
            {} };
    }
};

static_assert(std::is_enum_v<HostInputMode>);
static_assert(std::is_enum_v<HostInputKind>);
static_assert(std::is_base_of_v<HostInputEndpoint, TestInputEndpoint>);

void SetExpect(bool isExpected, const char* message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

InteractionEvent BuildEvent(InteractionEventKind kind)
{
    InteractionEvent event;
    event.eventKind = kind;
    return event;
}

HostViewTargets GetAllViews()
{
    HostViewTargets targets;
    targets.viewIds = { "primary", "slice" };
    return targets;
}

void StartPublicContractCase(int& failureCount)
{
    HostInputEvent event;
    event.viewId = "slice";
    event.kind = HostInputKind::PrimaryPress;
    event.x = 12;
    event.y = 24;

    HostRenderViewConfig view;
    view.inputMode = HostInputMode::HostInjected;

    TestInputEndpoint endpoint;
    const auto result = endpoint.SendInput(event);
    SetExpect(
        result.isSucceeded && result.isHandled
            && result.isDefaultSuppressed
            && result.errorCode == HostErrorCode::None
            && view.inputMode == HostInputMode::HostInjected,
        "Host input public contract should expose stable result fields.",
        failureCount);
}

void StartBindingAndPhaseCase(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto primary = std::make_shared<ViewContextStub>();
    auto slice = std::make_shared<ViewContextStub>();
    views->CreateView("primary", HostRenderViewRole::Primary3D, primary);
    views->CreateView("slice", HostRenderViewRole::FrontBackSlice, slice);
    HostInputRegistry registry(views->GetViewDirectory());
    SetExpect(
        registry.Start(GetAllViews()),
        "Registry should install all stable view callbacks.",
        failureCount);

    HostInputBinding invalid;
    invalid.featureId = "invalid";
    SetExpect(
        !registry.GetFeaturePort().AttachInput(std::move(invalid)),
        "Registry should reject an incomplete binding.",
        failureCount);

    std::vector<std::string> order;
    std::vector<InteractionEventKind> observedKinds;
    bool hasExactViewId = true;
    HostInputBinding primaryBinding;
    primaryBinding.featureId = "feature.primary";
    primaryBinding.targetViews.viewIds = { "primary" };
    primaryBinding.onInput = [&](const InteractionEvent& event) {
        order.push_back("feature");
        observedKinds.push_back(event.eventKind);
        hasExactViewId = hasExactViewId && event.viewId == "primary";
        return InteractionResult{};
    };
    SetExpect(
        registry.GetFeaturePort().AttachInput(std::move(primaryBinding)),
        "Registry should attach a feature binding.",
        failureCount);

    HostInputBinding duplicate;
    duplicate.featureId = "feature.primary";
    duplicate.targetViews.viewIds = { "primary" };
    duplicate.onInput = [](const InteractionEvent&) {
        return InteractionResult{};
    };
    SetExpect(
        !registry.GetFeaturePort().AttachInput(std::move(duplicate)),
        "Registry should reject a duplicate feature id.",
        failureCount);

    int sliceCount = 0;
    HostInputBinding sliceBinding;
    sliceBinding.featureId = "feature.slice";
    sliceBinding.targetViews.viewIds = { "slice" };
    sliceBinding.onInput = [&](const InteractionEvent& event) {
        ++sliceCount;
        hasExactViewId = hasExactViewId && event.viewId == "slice";
        return InteractionResult{};
    };
    SetExpect(
        registry.GetFeaturePort().AttachInput(std::move(sliceBinding)),
        "Registry should attach an independent target binding.",
        failureCount);

    HostViewTargets hostTargets;
    hostTargets.viewIds = { "primary", "slice" };
    int hostCount = 0;
    SetExpect(
        registry.SetHostInput(
            std::move(hostTargets),
            [&](const InteractionEvent& event,
                const std::string& viewId,
                HostRenderViewRole) {
                order.push_back("host");
                ++hostCount;
                hasExactViewId = hasExactViewId
                    && event.viewId == viewId;
                return InteractionResult{};
            }),
        "Registry should attach one HostExtension binding.",
        failureCount);

    const std::vector<InteractionEventKind> kinds{
        InteractionEventKind::WheelForward,
        InteractionEventKind::WheelBackward,
        InteractionEventKind::PrimaryPress,
        InteractionEventKind::PrimaryRelease,
        InteractionEventKind::SecondaryPress,
        InteractionEventKind::SecondaryRelease,
        InteractionEventKind::PointerMove,
        InteractionEventKind::KeyPress,
        InteractionEventKind::KeyRelease,
        InteractionEventKind::TextInput
    };
    for (const auto kind : kinds) {
        primary->OnInput(BuildEvent(kind));
    }
    slice->OnInput(BuildEvent(InteractionEventKind::KeyPress));

    bool isPhaseOrdered = order.size() == kinds.size() * 2 + 1;
    for (std::size_t index = 0;
        isPhaseOrdered && index < kinds.size(); ++index) {
        isPhaseOrdered = order[index * 2] == "feature"
            && order[index * 2 + 1] == "host";
    }
    SetExpect(
        isPhaseOrdered && hasExactViewId
            && observedKinds == kinds && hostCount == 11
            && sliceCount == 1,
        "Registry should route all input kinds by Feature then Host phase.",
        failureCount);

    SetExpect(
        registry.GetFeaturePort().DetachInput("feature.primary")
            && registry.GetFeaturePort().DetachInput("feature.slice")
            && registry.ClearHostInput()
            && registry.Stop(),
        "Registry bindings and stable callbacks should detach symmetrically.",
        failureCount);
}

void StartExceptionAndReentrancyCase(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto context = std::make_shared<ViewContextStub>();
    views->CreateView("primary", HostRenderViewRole::Primary3D, context);
    HostInputRegistry registry(views->GetViewDirectory());
    HostViewTargets all;
    all.viewIds = { "primary" };
    SetExpect(registry.Start(all),
        "Exception fixture should start registry.", failureCount);

    HostInputBinding throwing;
    throwing.featureId = "feature.throw";
    throwing.targetViews = all;
    throwing.onInput = [](const InteractionEvent&) -> InteractionResult {
        throw 1;
    };
    registry.GetFeaturePort().AttachInput(std::move(throwing));

    int nextCount = 0;
    HostInputBinding next;
    next.featureId = "feature.next";
    next.targetViews = all;
    next.onInput = [&](const InteractionEvent&) {
        ++nextCount;
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(next));
    const auto exceptionResult =
        context->OnInput(BuildEvent(InteractionEventKind::KeyPress));
    SetExpect(
        !exceptionResult.isSucceeded && nextCount == 1,
        "A throwing binding should report failure and allow the next binding.",
        failureCount);

    bool wasAttachAccepted = true;
    bool wasDetachAccepted = true;
    HostInputBinding reentrant;
    reentrant.featureId = "feature.reentrant";
    reentrant.targetViews = all;
    reentrant.onInput = [&](const InteractionEvent&) {
        HostInputBinding late;
        late.featureId = "feature.late";
        late.targetViews = all;
        late.onInput = [](const InteractionEvent&) {
            return InteractionResult{};
        };
        wasAttachAccepted =
            registry.GetFeaturePort().AttachInput(std::move(late));
        wasDetachAccepted =
            registry.GetFeaturePort().DetachInput("feature.reentrant");
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(reentrant));
    context->OnInput(BuildEvent(InteractionEventKind::KeyRelease));
    SetExpect(
        !wasAttachAccepted && !wasDetachAccepted,
        "Registry should reject structural changes from a callback stack.",
        failureCount);

    registry.GetFeaturePort().DetachInput("feature.throw");
    registry.GetFeaturePort().DetachInput("feature.next");
    registry.GetFeaturePort().DetachInput("feature.reentrant");
    SetExpect(registry.Stop(),
        "Exception fixture should stop cleanly.", failureCount);
}

void StartCaptureAndDetachCase(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto context = std::make_shared<ViewContextStub>();
    views->CreateView("primary", HostRenderViewRole::Primary3D, context);
    HostInputRegistry registry(views->GetViewDirectory());
    HostViewTargets all;
    all.viewIds = { "primary" };
    registry.Start(all);

    int ownerMoveCount = 0;
    int fallbackMoveCount = 0;
    int releaseCount = 0;
    int cancelCount = 0;
    bool isCancelFailing = true;
    HostInputBinding owner;
    owner.featureId = "feature.owner";
    owner.targetViews = all;
    owner.onInput = [&](const InteractionEvent& event) {
        if (event.eventKind == InteractionEventKind::PrimaryPress) {
            return InteractionResult{ true, true };
        }
        if (event.eventKind == InteractionEventKind::PointerMove) {
            ++ownerMoveCount;
            return InteractionResult{
                false,
                false,
                false,
                InteractionFailureReason::StateRejected };
        }
        if (event.eventKind == InteractionEventKind::PrimaryRelease) {
            ++releaseCount;
            return InteractionResult{
                true,
                true,
                releaseCount > 1,
                releaseCount > 1
                    ? InteractionFailureReason::None
                    : InteractionFailureReason::CleanupRejected };
        }
        if (event.eventKind == InteractionEventKind::Cancel) {
            ++cancelCount;
            return InteractionResult{
                true,
                true,
                !isCancelFailing,
                isCancelFailing
                    ? InteractionFailureReason::CleanupRejected
                    : InteractionFailureReason::None };
        }
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(owner));

    HostInputBinding fallback;
    fallback.featureId = "feature.fallback";
    fallback.targetViews = all;
    fallback.onInput = [&](const InteractionEvent& event) {
        if (event.eventKind == InteractionEventKind::PointerMove) {
            ++fallbackMoveCount;
            return InteractionResult{ true, false };
        }
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(fallback));

    context->OnInput(BuildEvent(InteractionEventKind::PrimaryPress));
    context->OnInput(BuildEvent(InteractionEventKind::PointerMove));
    const auto failedRelease =
        context->OnInput(BuildEvent(InteractionEventKind::PrimaryRelease));
    const auto retryRelease =
        context->OnInput(BuildEvent(InteractionEventKind::PrimaryRelease));
    context->OnInput(BuildEvent(InteractionEventKind::PointerMove));
    SetExpect(
        ownerMoveCount == 2 && fallbackMoveCount == 1
            && !failedRelease.isSucceeded && retryRelease.isSucceeded
            && releaseCount == 2,
        "Registry continuation should retain a failed release without fallback.",
        failureCount);

    context->OnInput(BuildEvent(InteractionEventKind::PrimaryPress));
    const bool firstDetach =
        registry.GetFeaturePort().DetachInput("feature.owner");
    isCancelFailing = false;
    const bool retryDetach =
        registry.GetFeaturePort().DetachInput("feature.owner");
    SetExpect(
        !firstDetach && retryDetach && cancelCount == 2,
        "Detach should retry the exact Router-owned capture before removing a binding.",
        failureCount);

    registry.GetFeaturePort().DetachInput("feature.fallback");
    SetExpect(registry.Stop(),
        "Capture fixture should stop cleanly.", failureCount);
}

void StartAtomicPressCase(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto context = std::make_shared<ViewContextStub>();
    views->CreateView("primary", HostRenderViewRole::Primary3D, context);
    HostInputRegistry registry(views->GetViewDirectory());
    HostViewTargets all;
    all.viewIds = { "primary" };
    registry.Start(all);

    int followerMoveCount = 0;
    HostInputBinding failed;
    failed.featureId = "feature.failed";
    failed.targetViews = all;
    failed.onInput = [](const InteractionEvent& event) {
        if (event.eventKind == InteractionEventKind::PrimaryPress) {
            return InteractionResult{
                true,
                true,
                false,
                InteractionFailureReason::StateRejected };
        }
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(failed));
    HostInputBinding follower;
    follower.featureId = "feature.follower";
    follower.targetViews = all;
    follower.onInput = [&](const InteractionEvent& event) {
        if (event.eventKind == InteractionEventKind::PointerMove) {
            ++followerMoveCount;
        }
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(follower));

    context->OnInput(BuildEvent(InteractionEventKind::PrimaryPress));
    context->OnInput(BuildEvent(InteractionEventKind::PointerMove));
    SetExpect(
        followerMoveCount == 1,
        "A handled but failed press should not leave an orphan capture.",
        failureCount);

    registry.GetFeaturePort().DetachInput("feature.failed");
    registry.GetFeaturePort().DetachInput("feature.follower");
    registry.Stop();
}

void StartStableCallbackLifecycleCase(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto primary = std::make_shared<ViewContextStub>();
    auto slice = std::make_shared<ViewContextStub>();
    views->CreateView("primary", HostRenderViewRole::Primary3D, primary);
    views->CreateView("slice", HostRenderViewRole::FrontBackSlice, slice);

    int oldCallbackCount = 0;
    slice->SetInputHandler(
        [&](const InteractionEvent&) {
            ++oldCallbackCount;
            return InteractionDispatch{
                InteractionResult{ true, false }, nullptr };
        },
        { InteractionEventKind::KeyPress });
    slice->SetInputSetFailCount(1);

    HostInputRegistry registry(views->GetViewDirectory());
    const bool firstStart = registry.Start(GetAllViews());
    slice->OnInput(BuildEvent(InteractionEventKind::KeyPress));
    SetExpect(
        !firstStart && oldCallbackCount == 1
            && primary->GetInputClearCount() == 1,
        "A partial Start failure should restore installed views and preserve the rejected view callback.",
        failureCount);

    SetExpect(
        registry.Start(GetAllViews()),
        "Registry Start should succeed after the transient install failure.",
        failureCount);
    primary->SetInputClearFailCount(1);
    const int sliceClearBefore = slice->GetInputClearCount();
    const bool firstStop = registry.Stop();
    const int sliceClearAfter = slice->GetInputClearCount();
    const bool retryStop = registry.Stop();
    SetExpect(
        !firstStop && retryStop
            && sliceClearAfter == sliceClearBefore + 1
            && slice->GetInputClearCount() == sliceClearAfter,
        "Registry Stop should retry only installed routes whose clear failed.",
        failureCount);
}

void StartEndpointMappingCase(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto native = std::make_shared<ViewContextStub>(false);
    auto injected = std::make_shared<ViewContextStub>(true);
    views->CreateView(
        "native", HostRenderViewRole::Primary3D, native,
        HostInputMode::NativeInteractor);
    views->CreateView(
        "injected", HostRenderViewRole::Composite3D, injected,
        HostInputMode::HostInjected);
    HostInputRegistry registry(views->GetViewDirectory());
    HostViewTargets all;
    all.viewIds = { "native", "injected" };
    registry.Start(all);

    std::vector<InteractionEventKind> observedKinds;
    InteractionEvent observed;
    HostInputBinding binding;
    binding.featureId = "feature.endpoint";
    binding.targetViews.viewIds = { "injected" };
    binding.onInput = [&](const InteractionEvent& event) {
        observed = event;
        observedKinds.push_back(event.eventKind);
        return InteractionResult{};
    };
    registry.GetFeaturePort().AttachInput(std::move(binding));

    const std::vector<std::pair<HostInputKind, InteractionEventKind>> kinds{
        { HostInputKind::WheelForward, InteractionEventKind::WheelForward },
        { HostInputKind::WheelBackward, InteractionEventKind::WheelBackward },
        { HostInputKind::PrimaryPress, InteractionEventKind::PrimaryPress },
        { HostInputKind::PrimaryRelease, InteractionEventKind::PrimaryRelease },
        { HostInputKind::SecondaryPress, InteractionEventKind::SecondaryPress },
        { HostInputKind::SecondaryRelease, InteractionEventKind::SecondaryRelease },
        { HostInputKind::PointerMove, InteractionEventKind::PointerMove },
        { HostInputKind::KeyPress, InteractionEventKind::KeyPress },
        { HostInputKind::KeyRelease, InteractionEventKind::KeyRelease },
        { HostInputKind::TextInput, InteractionEventKind::TextInput }
    };
    bool areMapped = true;
    for (const auto& kind : kinds) {
        HostInputEvent event;
        event.viewId = "injected";
        event.kind = kind.first;
        event.x = 41;
        event.y = 52;
        event.isShiftDown = true;
        event.isCtrlDown = true;
        event.isAltDown = true;
        event.keyCode = 'E';
        event.keySym = "Endpoint";
        const auto result = registry.SendInput(event);
        areMapped = areMapped && result.isSucceeded
            && !observedKinds.empty()
            && observedKinds.back() == kind.second
            && observed.viewId == event.viewId
            && observed.x == event.x && observed.y == event.y
            && observed.isShiftDown && observed.isCtrlDown
            && observed.isAltDown && observed.keyCode == event.keyCode
            && observed.keySym == event.keySym;
    }

    HostInputEvent invalid;
    const auto invalidResult = registry.SendInput(invalid);
    invalid.viewId = "missing";
    invalid.kind = HostInputKind::KeyPress;
    const auto missingResult = registry.SendInput(invalid);
    invalid.viewId = "native";
    const auto nativeResult = registry.SendInput(invalid);
    SetExpect(
        areMapped && observedKinds.size() == kinds.size()
            && !invalidResult.isSucceeded
            && invalidResult.errorCode == HostErrorCode::RequestRejected
            && !missingResult.isSucceeded
            && missingResult.errorCode == HostErrorCode::RequestRejected
            && !nativeResult.isSucceeded
            && nativeResult.errorCode == HostErrorCode::RequestRejected,
        "Endpoint should map all kinds and reject invalid or native routes.",
        failureCount);

    registry.GetFeaturePort().DetachInput("feature.endpoint");
    registry.Stop();
}

} // namespace

int HostInputRegistrySuite::GetFailCount() const
{
    int failureCount = 0;
    StartPublicContractCase(failureCount);
    StartBindingAndPhaseCase(failureCount);
    StartExceptionAndReentrancyCase(failureCount);
    StartCaptureAndDetachCase(failureCount);
    StartAtomicPressCase(failureCount);
    StartStableCallbackLifecycleCase(failureCount);
    StartEndpointMappingCase(failureCount);
    return failureCount;
}
