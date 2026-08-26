#include "InputCallbackHandler.h"
#include "InteractionRouter.h"
#include "Interaction/AbstractViewContext.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/ViewContextFactory.h"
#include "Viewer2DHandler.h"
#include "AppStateTests.h"
#include "HostCommandRouterTests.h"
#include "HostHotkeyRouterTests.h"

#include <array>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

class TestUpdatePort final : public RenderUpdatePort {
public:
    bool SendUpdates() override { return true; }
    bool SetRenderNeeded() override { return true; }
    bool ResetRenderNeeded() override { return false; }
};

class TestStatePort final : public InteractionStatePort {
public:
    bool SetInteracting(
        const InteractionSource&, bool) override
    {
        return true;
    }
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
        StartContextThreadCase(failureCount);
        return failureCount;
    }
};

int main()
{
    int failureCount = InteractionCases().GetFailCount();
    failureCount += HostRouterSuite().GetFailCount();
    failureCount += HostHotkeySuite().GetFailCount();
    failureCount += AppStateSuite().GetFailCount();

    if (failureCount == 0) {
        std::cout << "Interaction tests passed.\n";
        return 0;
    }

    std::cerr << "Interaction tests failed: " << failureCount << '\n';
    return 1;
}
