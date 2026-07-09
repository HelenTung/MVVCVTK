#include "InputCallbackHandler.h"
#include "InteractionRouter.h"

#include <iostream>
#include <memory>
#include <vector>

namespace {

void SetExpect(bool isExpected, const char* message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

InteractionEvent BuildEvent(unsigned long eventId)
{
    InteractionEvent event;
    event.vtkEventId = eventId;
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
        std::vector<unsigned long>{ 101 }));

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&secondCount](const InteractionEvent&) {
            ++secondCount;
            return InteractionResult{ true, false };
        },
        std::vector<unsigned long>{ 101 }));

    const auto result = router.Dispatch(BuildEvent(101), RouterDispatchMode::FirstMatch);
    SetExpect(result.isHandled, "FirstMatch should report handled.", failureCount);
    SetExpect(result.hasVtkAbort, "FirstMatch should keep abort request.", failureCount);
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
        std::vector<unsigned long>{ 201 }));

    router.AttachHandler(std::make_unique<InputCallbackHandler>(
        [&secondCount](const InteractionEvent&) {
            ++secondCount;
            return InteractionResult{ true, true };
        },
        std::vector<unsigned long>{ 201 }));

    const auto result = router.Dispatch(BuildEvent(201), RouterDispatchMode::Broadcast);
    SetExpect(result.isHandled, "Broadcast should aggregate handled state.", failureCount);
    SetExpect(result.hasVtkAbort, "Broadcast should aggregate abort state.", failureCount);
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
        std::vector<unsigned long>{ 301 }));

    const auto result = router.Dispatch(BuildEvent(302), RouterDispatchMode::FirstMatch);
    SetExpect(!result.isHandled, "Filtered event should not be handled.", failureCount);
    SetExpect(!result.hasVtkAbort, "Filtered event should not request abort.", failureCount);
    SetExpect(count == 0, "Filtered callback should not run.", failureCount);
}

} // namespace

int main()
{
    int failureCount = 0;
    StartFirstMatchCase(failureCount);
    StartBroadcastCase(failureCount);
    StartFilterCase(failureCount);

    if (failureCount == 0) {
        std::cout << "Interaction router tests passed.\n";
        return 0;
    }

    std::cerr << "Interaction router tests failed: " << failureCount << '\n';
    return 1;
}
