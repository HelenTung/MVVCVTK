#include "InputCallbackHandler.h"
#include "InteractionRouter.h"
#include "Interaction/AbstractViewContext.h"
#include "Interaction/DefaultNavigationPolicy.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/ViewContextFactory.h"
#include "Viewer2DHandler.h"
#include "AppStateTests.h"
#include "HostCommandRouterTests.h"
#include "HostHotkeyRouterTests.h"
#include "HostInputRegistryTests.h"

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <vtkCommand.h>
#include <vtkInteractorStyle.h>
#include <vtkRenderWindowInteractor.h>

namespace {

class TestUpdatePort final : public RenderUpdatePort {
public:
    bool SendUpdates() override { return true; }
    bool SendPendingUpdates() override { return true; }
    void SendCompletions() override {}
    bool SetRenderNeeded() override
    {
        ++renderNeededCount;
        return isRenderNeededAccepted;
    }
    bool ResetRenderNeeded() override { return false; }

    bool isRenderNeededAccepted = true;
    int renderNeededCount = 0;
};

class TestStatePort final : public InteractionStatePort {
public:
    bool SetInteracting(
        const InteractionSource&, bool isInteracting) override
    {
        if (isInteracting) {
            ++startCount;
            return isStartAccepted;
        }
        ++stopCount;
        return isStopAccepted;
    }

    bool isStartAccepted = true;
    bool isStopAccepted = true;
    int startCount = 0;
    int stopCount = 0;
};

class TestSlicePort final : public SliceInputPort {
public:
    bool SetSliceScroll(int) override
    {
        ++scrollCount;
        return isScrollAccepted;
    }
    int GetPlaneAxis(vtkActor*) const override { return -1; }
    bool SetCursorWorld(
        const std::array<double, 3>&, int) override
    {
        return true;
    }
    std::array<double, 3> GetCursorWorld() const override
    {
        return {};
    }
    WindowLevelParams GetWindowLevel() const override
    {
        return {};
    }
    bool SetWindowLevelDrag(
        int, int, int, int, double, double) override
    {
        return true;
    }

    bool isScrollAccepted = true;
    int scrollCount = 0;
};

class TestModelPort final : public ModelInputPort {
public:
    vtkProp3D* GetMainProp() const override { return nullptr; }
    std::array<double, 16> GetModelMatrix() const override
    {
        return {};
    }
    bool SetModelMatrix(
        const std::array<double, 16>&) override
    {
        return true;
    }
};

class TestCapture final : public IInteractionCapture {
public:
    using Callback =
        std::function<InteractionResult(const InteractionEvent&)>;

    TestCapture(
        InteractionCaptureKey key,
        InteractionEventKind releaseKind,
        Callback callback,
        int* destroyCount = nullptr)
        : m_key(key)
        , m_releaseKind(releaseKind)
        , m_callback(std::move(callback))
        , m_destroyCount(destroyCount)
    {
    }

    ~TestCapture() override
    {
        if (m_destroyCount) ++*m_destroyCount;
    }

    InteractionCaptureKey GetKey() const noexcept override
    {
        return m_key;
    }

    InteractionEventKind GetReleaseKind() const noexcept override
    {
        return m_releaseKind;
    }

    InteractionResult Send(const InteractionEvent& event) override
    {
        return m_callback ? m_callback(event) : InteractionResult{};
    }

private:
    InteractionCaptureKey m_key;
    InteractionEventKind m_releaseKind =
        InteractionEventKind::PrimaryRelease;
    Callback m_callback;
    int* m_destroyCount = nullptr;
};

} // namespace

class InteractionCases final {
public:

void SetExpect(bool isExpected, const char* message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

InteractionEvent BuildEvent(InteractionEventKind eventKind)
{
    InteractionEvent event;
    event.eventKind = eventKind;
    return event;
}

void StartFirstMatchCase(int& failureCount)
{
    InteractionRouter router;
    int firstCount = 0;
    int secondCount = 0;

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&firstCount](const InteractionEvent&) {
            ++firstCount;
            return InteractionResult{ true, true };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&secondCount](const InteractionEvent&) {
            ++secondCount;
            return InteractionResult{ true, false };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));

    const auto result = router.Dispatch(
        BuildEvent(InteractionEventKind::PrimaryPress), RouterDispatchMode::FirstMatch);
    SetExpect(result.isHandled, "FirstMatch should report handled.", failureCount);
    SetExpect(result.isPropagationStopped,
        "FirstMatch should keep propagation stop request.", failureCount);
    SetExpect(firstCount == 1, "First callback should run once.", failureCount);
    SetExpect(secondCount == 0, "Second callback should not run after FirstMatch handled.", failureCount);
}

void StartBroadcastCase(int& failureCount)
{
    InteractionRouter router;
    int firstCount = 0;
    int secondCount = 0;

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&firstCount](const InteractionEvent&) {
            ++firstCount;
            return InteractionResult{ true, false };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::Timer }));

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&secondCount](const InteractionEvent&) {
            ++secondCount;
            return InteractionResult{ true, true };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::Timer }));

    const auto result = router.Dispatch(
        BuildEvent(InteractionEventKind::Timer), RouterDispatchMode::Broadcast);
    SetExpect(result.isHandled, "Broadcast should aggregate handled state.", failureCount);
    SetExpect(result.isPropagationStopped,
        "Broadcast should aggregate propagation state.", failureCount);
    SetExpect(firstCount == 1, "First broadcast callback should run once.", failureCount);
    SetExpect(secondCount == 1, "Second broadcast callback should run once.", failureCount);
}

void StartFilterCase(int& failureCount)
{
    InteractionRouter router;
    int count = 0;

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&count](const InteractionEvent&) {
            ++count;
            return InteractionResult{ true, true };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::KeyPress }));

    const auto result = router.Dispatch(
        BuildEvent(InteractionEventKind::KeyRelease), RouterDispatchMode::FirstMatch);
    SetExpect(!result.isHandled, "Filtered event should not be handled.", failureCount);
    SetExpect(!result.isPropagationStopped,
        "Filtered event should not stop propagation.", failureCount);
    SetExpect(count == 0, "Filtered callback should not run.", failureCount);
}

void StartEmptyFilterCase(int& failureCount)
{
    InteractionRouter router;
    int count = 0;

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&count](const InteractionEvent&) {
            ++count;
            return InteractionResult{ true, true };
        },
        std::vector<InteractionEventKind>{}));

    const auto firstResult = router.Dispatch(BuildEvent(InteractionEventKind::KeyPress));
    const auto secondResult = router.Dispatch(BuildEvent(InteractionEventKind::PointerMove));

    SetExpect(firstResult.isHandled, "Empty event filter should handle the first event.", failureCount);
    SetExpect(firstResult.isPropagationStopped,
        "Empty event filter should stop the first event.", failureCount);
    SetExpect(secondResult.isHandled, "Empty event filter should handle the second event.", failureCount);
    SetExpect(secondResult.isPropagationStopped,
        "Empty event filter should stop the second event.", failureCount);
    SetExpect(count == 2, "Empty event filter callback should run for both events.", failureCount);
}

void StartIndependentResultCase(int& failureCount)
{
    InteractionRouter router;
    int secondCount = 0;

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [](const InteractionEvent&) {
            return InteractionResult{ false, true };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&secondCount](const InteractionEvent&) {
            ++secondCount;
            return InteractionResult{ true, false };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));

    const auto result = router.Dispatch(
        BuildEvent(InteractionEventKind::PrimaryPress), RouterDispatchMode::FirstMatch);
    SetExpect(result.isHandled,
        "Unhandled propagation stop should not block a later handled result.", failureCount);
    SetExpect(result.isPropagationStopped,
        "Propagation stop should be retained independently of handled state.", failureCount);
    SetExpect(secondCount == 1,
        "FirstMatch should continue after an unhandled propagation stop.", failureCount);
}

void StartHandledNoStopCase(int& failureCount)
{
    InteractionRouter router;
    int secondCount = 0;

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [](const InteractionEvent&) {
            return InteractionResult{ true, false };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&secondCount](const InteractionEvent&) {
            ++secondCount;
            return InteractionResult{ false, true };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));

    const auto result = router.Dispatch(
        BuildEvent(InteractionEventKind::PrimaryPress), RouterDispatchMode::FirstMatch);
    SetExpect(result.isHandled, "Handled result should stop FirstMatch.", failureCount);
    SetExpect(!result.isPropagationStopped,
        "Handled state should not imply propagation stop.", failureCount);
    SetExpect(secondCount == 0,
        "FirstMatch should not run handlers after a handled result.", failureCount);
}

void StartFailureCase(int& failureCount)
{
    InteractionRouter router;
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [](const InteractionEvent&) {
            return InteractionResult{
                true,
                true,
                false,
                InteractionFailureReason::StateRejected };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::PrimaryPress }));
    const auto routed = router.Dispatch(
        BuildEvent(InteractionEventKind::PrimaryPress),
        RouterDispatchMode::FirstMatch);
    SetExpect(routed.isHandled
            && routed.isPropagationStopped
            && !routed.isSucceeded
            && routed.failureReason
                == InteractionFailureReason::StateRejected,
        "Router should preserve handled ownership and the first failure reason.",
        failureCount);

    TestStatePort state;
    TestSlicePort slice;
    TestModelPort model;
    TestUpdatePort update;
    slice.isScrollAccepted = false;
    Viewer2DHandler viewer(
        &state, &slice, &model, &update, nullptr, nullptr);
    auto wheel = BuildEvent(InteractionEventKind::WheelForward);
    wheel.vizMode = VizMode::SliceTop_down;
    const auto wheelResult = viewer.Send(wheel);
    SetExpect(wheelResult.isHandled
            && wheelResult.isPropagationStopped
            && !wheelResult.isSucceeded
            && wheelResult.failureReason
                == InteractionFailureReason::StateRejected
            && slice.scrollCount == 1,
        "Viewer2D should consume wheel input while reporting rejected slice state.",
        failureCount);
}

void StartCaptureCase(int& failureCount)
{
    InteractionRouter router;
    int pressCount = 0;
    int capturedPointerCount = 0;
    int fallbackMoveCount = 0;
    const InteractionCaptureKey key{ 7, 11 };

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        InputCallbackHandler::RoutedCallback{
            [&](const InteractionEvent& event) {
                InteractionDispatch routed;
                if (event.eventKind != InteractionEventKind::PrimaryPress) {
                    return routed;
                }
                auto capture = std::make_unique<TestCapture>(
                    key,
                    InteractionEventKind::PrimaryRelease,
                    [&](const InteractionEvent&) {
                        ++capturedPointerCount;
                        return InteractionResult{};
                    });
                ++pressCount;
                routed.result = { true, false };
                routed.capture = std::move(capture);
                return routed;
            } },
        std::vector<InteractionEventKind>{}));
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&fallbackMoveCount](const InteractionEvent& event) {
            if (event.eventKind == InteractionEventKind::PointerMove) {
                ++fallbackMoveCount;
                return InteractionResult{ true, false };
            }
            return InteractionResult{};
        },
        std::vector<InteractionEventKind>{}));

    const auto press = router.Dispatch(
        BuildEvent(InteractionEventKind::PrimaryPress));
    const bool wasAttachAccepted = router.AttachHandler(
        std::make_unique<InputCallbackHandler>(
            [](const InteractionEvent&) {
                return InteractionResult{};
            },
            std::vector<InteractionEventKind>{}));
    const bool wasClearAccepted = router.ClearHandlers();
    router.Dispatch(BuildEvent(InteractionEventKind::SecondaryPress));
    router.Dispatch(BuildEvent(InteractionEventKind::PointerMove));
    router.Dispatch(BuildEvent(InteractionEventKind::PrimaryRelease));
    router.Dispatch(BuildEvent(InteractionEventKind::PointerMove));

    SetExpect(
        press.isHandled && pressCount == 1
            && !wasAttachAccepted && !wasClearAccepted
            && capturedPointerCount == 3 && fallbackMoveCount == 1,
        "Pointer capture should exclusively route pointer events until release.",
        failureCount);
}

void StartCaptureRetryCase(int& failureCount)
{
    InteractionRouter router;
    int releaseCount = 0;
    int capturedMoveCount = 0;
    int fallbackMoveCount = 0;
    const InteractionCaptureKey key{ 8, 12 };

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        InputCallbackHandler::RoutedCallback{
            [&](const InteractionEvent& event) {
                if (event.eventKind != InteractionEventKind::SecondaryPress) {
                    return InteractionDispatch{};
                }
                InteractionDispatch routed;
                routed.result = { true, true };
                routed.capture = std::make_unique<TestCapture>(
                    key,
                    InteractionEventKind::SecondaryRelease,
                    [&](const InteractionEvent& captured) {
                        if (captured.eventKind
                            == InteractionEventKind::PointerMove) {
                            ++capturedMoveCount;
                            return InteractionResult{
                                false,
                                false,
                                false,
                                InteractionFailureReason::StateRejected };
                        }
                        if (captured.eventKind
                            == InteractionEventKind::SecondaryRelease) {
                            ++releaseCount;
                            if (releaseCount == 1) {
                                return InteractionResult{
                                    true,
                                    true,
                                    false,
                                    InteractionFailureReason::CleanupRejected };
                            }
                        }
                        return InteractionResult{ true, true };
                    });
                return routed;
            } },
        std::vector<InteractionEventKind>{}));
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&fallbackMoveCount](const InteractionEvent& event) {
            if (event.eventKind == InteractionEventKind::PointerMove) {
                ++fallbackMoveCount;
                return InteractionResult{ true, false };
            }
            return InteractionResult{};
        },
        std::vector<InteractionEventKind>{}));

    router.Dispatch(BuildEvent(InteractionEventKind::SecondaryPress));
    router.Dispatch(BuildEvent(InteractionEventKind::PointerMove));
    router.Dispatch(BuildEvent(InteractionEventKind::PrimaryRelease));
    const auto firstRelease = router.Dispatch(
        BuildEvent(InteractionEventKind::SecondaryRelease));
    const auto retryRelease = router.Dispatch(
        BuildEvent(InteractionEventKind::SecondaryRelease));
    router.Dispatch(BuildEvent(InteractionEventKind::PointerMove));

    SetExpect(
        capturedMoveCount == 1 && fallbackMoveCount == 1
            && releaseCount == 2 && !firstRelease.isSucceeded
            && retryRelease.isSucceeded,
        "A failed matching release should retain capture for one retry.",
        failureCount);
}

void StartCancelCase(int& failureCount)
{
    InteractionRouter router;
    int captureCancelCount = 0;
    int parentCancelCount = 0;
    int trailingCancelCount = 0;
    bool isCaptureCancelFailing = true;
    bool isTrailingCancelFailing = true;
    const InteractionCaptureKey key{ 9, 13 };

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        InputCallbackHandler::RoutedCallback{
            [&](const InteractionEvent& event) {
                if (event.eventKind == InteractionEventKind::Cancel) {
                    ++parentCancelCount;
                    return InteractionDispatch{
                        InteractionResult{},
                        nullptr };
                }
                if (event.eventKind != InteractionEventKind::PrimaryPress) {
                    return InteractionDispatch{};
                }
                InteractionDispatch routed;
                routed.result = { true, true };
                routed.capture = std::make_unique<TestCapture>(
                    key,
                    InteractionEventKind::PrimaryRelease,
                    [&](const InteractionEvent& captured) {
                        if (captured.eventKind == InteractionEventKind::Cancel) {
                            ++captureCancelCount;
                        }
                        return InteractionResult{
                            true,
                            true,
                            !isCaptureCancelFailing,
                            isCaptureCancelFailing
                                ? InteractionFailureReason::CleanupRejected
                                : InteractionFailureReason::None };
                    });
                return routed;
            } },
        std::vector<InteractionEventKind>{}));
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&trailingCancelCount, &isTrailingCancelFailing](
            const InteractionEvent& event) {
            if (event.eventKind != InteractionEventKind::Cancel) {
                return InteractionResult{};
            }
            ++trailingCancelCount;
            return InteractionResult{
                false,
                false,
                !isTrailingCancelFailing,
                isTrailingCancelFailing
                    ? InteractionFailureReason::CleanupRejected
                    : InteractionFailureReason::None };
        },
        std::vector<InteractionEventKind>{}));

    router.Dispatch(BuildEvent(InteractionEventKind::PrimaryPress));
    const auto missing = router.CancelCapture(
        InteractionCaptureKey{ key.domain, key.generation + 1 },
        BuildEvent(InteractionEventKind::Cancel));
    const auto first = router.CancelCapture(
        key, BuildEvent(InteractionEventKind::Cancel));
    isCaptureCancelFailing = false;
    const auto second = router.SendCancel(
        BuildEvent(InteractionEventKind::Cancel));
    isTrailingCancelFailing = false;
    const auto third = router.SendCancel(
        BuildEvent(InteractionEventKind::Cancel));

    SetExpect(
        missing.isSucceeded && !missing.isHandled
            && !first.isSucceeded && second.isHandled
            && !second.isSucceeded && third.isSucceeded
            && captureCancelCount == 2 && parentCancelCount == 1
            && trailingCancelCount == 2,
        "Cancel should retain only failed work and never replay a cleared capture.",
        failureCount);
}

void StartCaptureNonPointerCase(int& failureCount)
{
    InteractionRouter router;
    int keyCount = 0;
    int timerCount = 0;
    const InteractionCaptureKey key{ 10, 14 };

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        InputCallbackHandler::RoutedCallback{
            [&](const InteractionEvent& event) {
                if (event.eventKind == InteractionEventKind::KeyPress) {
                    ++keyCount;
                    return InteractionDispatch{
                        InteractionResult{ true, false }, nullptr };
                }
                if (event.eventKind == InteractionEventKind::Timer) {
                    ++timerCount;
                    return InteractionDispatch{};
                }
                if (event.eventKind != InteractionEventKind::PrimaryPress) {
                    return InteractionDispatch{};
                }
                InteractionDispatch routed;
                routed.result = { true, true };
                routed.capture = std::make_unique<TestCapture>(
                    key,
                    InteractionEventKind::PrimaryRelease,
                    [](const InteractionEvent&) {
                        return InteractionResult{ true, true };
                    });
                return routed;
            } },
        std::vector<InteractionEventKind>{}));

    router.Dispatch(BuildEvent(InteractionEventKind::PrimaryPress));
    router.Dispatch(BuildEvent(InteractionEventKind::KeyPress));
    router.Dispatch(
        BuildEvent(InteractionEventKind::Timer),
        RouterDispatchMode::Broadcast);
    router.Dispatch(BuildEvent(InteractionEventKind::PrimaryRelease));

    SetExpect(
        keyCount == 1 && timerCount == 1,
        "Keyboard and timer events should bypass pointer capture.",
        failureCount);
}

void StartCaptureReentrancyCase(int& failureCount)
{
    InteractionRouter router;
    InteractionResult nested;
    bool wasAttachAccepted = true;
    bool wasClearAccepted = true;
    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&](const InteractionEvent&) {
            nested = router.Dispatch(
                BuildEvent(InteractionEventKind::PointerMove));
            wasAttachAccepted = router.AttachHandler(
                std::make_unique<InputCallbackHandler>(
                    [](const InteractionEvent&) {
                        return InteractionResult{};
                    },
                    std::vector<InteractionEventKind>{}));
            wasClearAccepted = router.ClearHandlers();
            return InteractionResult{ true, true };
        },
        std::vector<InteractionEventKind>{ InteractionEventKind::KeyPress }));

    const auto outer = router.Dispatch(
        BuildEvent(InteractionEventKind::KeyPress));
    SetExpect(
        outer.isHandled && !nested.isSucceeded
            && nested.failureReason == InteractionFailureReason::StateRejected
            && !wasAttachAccepted && !wasClearAccepted,
        "Router mutations and nested dispatch should be rejected in callbacks.",
        failureCount);
}

void StartAtomicCaptureCase(int& failureCount)
{
    int destroyCount = 0;
    {
        InteractionRouter router;
        router.AttachHandler(std::make_unique<InputCallbackHandler>(
            InputCallbackHandler::RoutedCallback{
                [&](const InteractionEvent&) {
                    InteractionDispatch routed;
                    routed.result = {
                        true,
                        true,
                        false,
                        InteractionFailureReason::StateRejected };
                    routed.capture = std::make_unique<TestCapture>(
                        InteractionCaptureKey{ 11, 15 },
                        InteractionEventKind::PrimaryRelease,
                        [](const InteractionEvent&) {
                            return InteractionResult{};
                        },
                        &destroyCount);
                    return routed;
                } },
            std::vector<InteractionEventKind>{
                InteractionEventKind::PrimaryPress }));
        router.Dispatch(BuildEvent(InteractionEventKind::PrimaryPress));
        SetExpect(
            destroyCount == 1 && router.ClearHandlers(),
            "A failed handled press should destroy its uncommitted continuation.",
            failureCount);
    }
}

void StartDefaultNavigationPolicyCase(int& failureCount)
{
    TestStatePort state;
    TestSlicePort slice;
    TestModelPort model;
    TestUpdatePort update;
    InteractionRouter router;
    SetExpect(
        router.AttachHandler(std::make_unique<DefaultNavigationPolicy>(
            &state, &slice, &model, &update, nullptr, nullptr)),
        "Default navigation policy should attach as one router stage.",
        failureCount);

    auto press = BuildEvent(InteractionEventKind::PrimaryPress);
    press.vizMode = VizMode::SliceTop_down;
    const auto pressed = router.Dispatch(press);
    state.isStopAccepted = false;
    const auto firstCancel = router.SendCancel(
        BuildEvent(InteractionEventKind::Cancel));
    state.isStopAccepted = true;
    const auto retryCancel = router.SendCancel(
        BuildEvent(InteractionEventKind::Cancel));
    const int stoppedAfterRetry = state.stopCount;
    const auto inactiveCancel = router.SendCancel(
        BuildEvent(InteractionEventKind::Cancel));

    SetExpect(
        pressed.isHandled && state.startCount == 1
            && !firstCancel.isSucceeded && retryCancel.isSucceeded
            && retryCancel.isHandled && inactiveCancel.isSucceeded
            && state.stopCount == stoppedAfterRetry,
        "Default navigation cancel should retry once and become an inactive no-op.",
        failureCount);

    auto wheel = BuildEvent(InteractionEventKind::WheelForward);
    wheel.vizMode = VizMode::SliceFront_back;
    const auto wheelResult = router.Dispatch(wheel);
    SetExpect(
        wheelResult.isHandled && slice.scrollCount == 1,
        "Default navigation should preserve 2D FirstMatch behavior.",
        failureCount);
}

void StartContextThreadCase(int& failureCount)
{
    InteractionPorts ports;
    ports.update = std::make_shared<TestUpdatePort>();
    ports.state = std::make_shared<TestStatePort>();
    ports.slice = std::make_shared<TestSlicePort>();
    ports.model = std::make_shared<TestModelPort>();
    const auto context = CreateViewContext(std::move(ports));
    SetExpect(context != nullptr,
        "View context factory should build a context.", failureCount);
    if (!context) return;

    SetExpect(context->SetTimerHandler([] {}),
        "Owner thread should set the timer handler.", failureCount);

    bool isTimerSet = true;
    bool isTimerCleared = true;
    bool isWindowSized = true;
    vtkRenderWindowInteractor* interactor = nullptr;
    std::thread worker([&]() {
        isTimerSet = context->SetTimerHandler([] {});
        isTimerCleared = context->ClearTimerHandler();
        isWindowSized = context->SetWindowSize(320, 240);
        interactor = context->GetInteractor();
    });
    worker.join();

    SetExpect(!isTimerSet && !isTimerCleared && !isWindowSized,
        "Context mutations should reject a non-owner thread.", failureCount);
    SetExpect(interactor == nullptr,
        "Context should not lend VTK objects to a non-owner thread.",
        failureCount);
    SetExpect(context->GetInteractor() != nullptr
            && context->ClearTimerHandler()
            && context->StopInput(),
        "Owner thread should retain context access after rejection.",
        failureCount);
}

void StartContextStyleCancelRetryCase(int& failureCount)
{
    auto update = std::make_shared<TestUpdatePort>();
    auto state = std::make_shared<TestStatePort>();
    InteractionPorts ports;
    ports.update = update;
    ports.state = state;
    ports.slice = std::make_shared<TestSlicePort>();
    ports.model = std::make_shared<TestModelPort>();
    const auto context = CreateViewContext(std::move(ports));
    if (!context || !context->SetInteractorReady()
        || !context->SetInputEnabled(true)) {
        SetExpect(false,
            "Style cancel retry fixture should initialize a context.",
            failureCount);
        return;
    }
    auto* style = vtkInteractorStyle::SafeDownCast(
        context->GetInteractor()->GetInteractorStyle());
    if (!style) {
        SetExpect(false,
            "Style cancel retry fixture should expose an interactor style.",
            failureCount);
        return;
    }
    const int baselineStopCount = state->stopCount;
    style->InvokeEvent(vtkCommand::StartInteractionEvent);
    update->isRenderNeededAccepted = false;
    const bool first = context->SetToolMode(ToolMode::ModelTransform);
    const int stopCountAfterFailure = state->stopCount;
    update->isRenderNeededAccepted = true;
    const bool retry = context->SetToolMode(ToolMode::ModelTransform);

    SetExpect(
        !first && retry
            && stopCountAfterFailure == baselineStopCount + 1
            && state->stopCount == stopCountAfterFailure
            && context->GetToolMode() == ToolMode::ModelTransform,
        "Style Cancel should retry only its failed render doorbell stage.",
        failureCount);
    SetExpect(context->StopInput(),
        "Style cancel retry context should stop cleanly.", failureCount);
}

    int GetFailCount()
    {
        int failureCount = 0;
        StartFirstMatchCase(failureCount);
        StartBroadcastCase(failureCount);
        StartFilterCase(failureCount);
        StartEmptyFilterCase(failureCount);
        StartIndependentResultCase(failureCount);
        StartHandledNoStopCase(failureCount);
        StartFailureCase(failureCount);
        StartCaptureCase(failureCount);
        StartCaptureRetryCase(failureCount);
        StartCancelCase(failureCount);
        StartCaptureNonPointerCase(failureCount);
        StartCaptureReentrancyCase(failureCount);
        StartAtomicCaptureCase(failureCount);
        StartDefaultNavigationPolicyCase(failureCount);
        StartContextThreadCase(failureCount);
        StartContextStyleCancelRetryCase(failureCount);
        return failureCount;
    }
};

int main()
{
    int failureCount = InteractionCases().GetFailCount();
    failureCount += HostRouterSuite().GetFailCount();
    failureCount += HostHotkeySuite().GetFailCount();
    failureCount += HostInputRegistrySuite().GetFailCount();
    failureCount += AppStateSuite().GetFailCount();

    if (failureCount == 0) {
        std::cout << "Interaction tests passed.\n";
        return 0;
    }

    std::cerr << "Interaction tests failed: " << failureCount << '\n';
    return 1;
}
