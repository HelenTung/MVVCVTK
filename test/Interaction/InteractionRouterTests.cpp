#include "InputCallbackHandler.h"
#include "InteractionRouter.h"
#include "AppStateTests.h"
#include "HostCommandRouterTests.h"
#include "HostHotkeyRouterTests.h"

#include <iostream>
#include <memory>
#include <vector>

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

    int GetFailCount()
    {
        int failureCount = 0;
        StartFirstMatchCase(failureCount);
        StartBroadcastCase(failureCount);
        StartFilterCase(failureCount);
        StartEmptyFilterCase(failureCount);
        StartIndependentResultCase(failureCount);
        StartHandledNoStopCase(failureCount);
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
