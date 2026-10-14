// 测试用途：汇总交互路由、命令、快捷键、输入注册和应用状态测试。
#include "InputCallbackHandler.h"
#include "InteractionRouter.h"
#include "Interaction/AbstractViewContext.h"
#include "Interaction/DefaultNavigationPolicy.h"
#include "Interaction/InputBindings.h"
#include "Interaction/NavigationBindings.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/ViewContextFactory.h"
#include "Viewer2DHandler.h"
#include "Viewer3DHandler.h"
#include "AppStateTests.h"
#include "HostCommandRouterTests.h"
#include "InteractionControllerTests.h"
#include "HostInputRegistryTests.h"
#include "Host/HostInputRegistry.h"
#include "Host/HostViewRuntimeRegistry.h"
#include "FeatureSupport/RoutedWidgets.h"
#include "FeatureSupport/WidgetBindings.h"
#include <vtkBoxRepresentation.h>
#include <vtkTransform.h>
#include <vtkCallbackCommand.h>

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <vtkCommand.h>
#include <vtkCamera.h>
#include <vtkRenderWindow.h>
#include <cmath>
#include <limits>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
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
    int GetPlaneAxis(vtkActor*) const override { return planeAxis; }
    bool SetCursorWorld(
        const std::array<double, 3>&, int) override
    {
        ++cursorCount;return true;
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
    int scrollCount = 0,cursorCount=0,planeAxis=-1;
};

class TestModelPort final : public ModelInputPort {
public:
    bool isPointVisible=true;
    mutable int pointQueries=0;
    bool GetPointVisible(const std::array<double,3>&) const override {++pointQueries;return isPointVisible;}
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


}

class PickPointProbe final : public vtkPropPicker {
public:
    static PickPointProbe* New(){return new PickPointProbe;}
    vtkTypeMacro(PickPointProbe,vtkPropPicker);
    int Pick(double,double,double,vtkRenderer*) override {
        PickPosition[0]=1;PickPosition[1]=2;PickPosition[2]=3;return 1;
    }
};

void StartHiddenPickCase(int& failureCount)
{
    TestStatePort state;TestSlicePort slice;TestModelPort model;TestUpdatePort update;
    auto picker=vtkSmartPointer<PickPointProbe>::New();auto renderer=vtkSmartPointer<vtkRenderer>::New();
    Viewer2DHandler viewer(&state,&slice,&model,&update,picker,renderer);
    auto event=BuildEvent(InteractionEventKind::PrimaryPress);event.vizMode=VizMode::SliceTop_down;event.isShiftDown=true;
    const auto started=viewer.Send(event);model.isPointVisible=false;event.eventKind=InteractionEventKind::PointerMove;
    const auto rejected=viewer.Send(event);
    SetExpect(started.isSucceeded&&rejected.isHandled&&!rejected.isSucceeded&&slice.cursorCount==0&&model.pointQueries==1,
        "Hidden 2D pick must not update the business cursor.",failureCount);
    model.isPointVisible=true;const auto accepted=viewer.Send(event);
    event.eventKind=InteractionEventKind::PrimaryRelease;const auto ended=viewer.Send(event);
    SetExpect(accepted.isSucceeded&&ended.isSucceeded&&slice.cursorCount==1&&state.stopCount==1,
        "Visible 2D pick and cleanup must remain available.",failureCount);
    TestStatePort state3;TestSlicePort slice3;TestModelPort model3;TestUpdatePort update3;slice3.planeAxis=1;
    Viewer3DHandler viewer3(&state3,&slice3,&model3,&update3,picker,renderer);
    event=BuildEvent(InteractionEventKind::PrimaryPress);event.vizMode=VizMode::CompositeVolume;
    model3.isPointVisible=false;viewer3.Send(event);
    SetExpect(state3.startCount==0&&model3.pointQueries==1,"Hidden 3D reference-plane pick must not start interaction.",failureCount);
    model3.isPointVisible=true;const auto started3=viewer3.Send(event);
    event.eventKind=InteractionEventKind::PrimaryRelease;const auto ended3=viewer3.Send(event);
    SetExpect(started3.isSucceeded&&ended3.isSucceeded&&state3.startCount==1&&state3.stopCount==1,
        "Visible 3D reference-plane pick must remain interactive.",failureCount);
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
            && capturedPointerCount == 2 && fallbackMoveCount == 1,
        "捕获移动和原按钮释放；额外按钮按下只消费，不重新开始手势。",
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
    press.isShiftDown=true;
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


}

void StartViewNavigationCase(int& failureCount)
{
    auto update=std::make_shared<TestUpdatePort>();auto state=std::make_shared<TestStatePort>();
    auto slice=std::make_shared<TestSlicePort>();auto model=std::make_shared<TestModelPort>();
    auto context=CreateViewContext({update,state,slice,model},true,true);
    SetExpect(context&&context->SetInteractorReady()&&context->SetInputEnabled(true),"导航真实 Context 初始化失败",failureCount);
    if(!context)return;
    auto controller=std::make_shared<InteractionController>();
    InteractionControllerAccess::Start(*controller,{{"slice",InteractionViewClass::Slice}},[context](const std::string&){return context->GetNavigation();},[context](const std::string&,std::shared_ptr<ViewNavigation> n){return context->SetNavigation(std::move(n));});
    NavigationBindings::RegisterActions(*controller);
    SetExpect(context->SetInteractionController(controller,"slice"),"导航绑定关联失败",failureCount);
    SetExpect(context->SetInputHandler([](const InteractionEvent&){return InteractionDispatch{};},{InteractionEventKind::KeyPress}),"重建输入测试关联失败",failureCount);
    SetExpect(context->SetCameraStyle(VizMode::SliceFront_back),"切片模式失败",failureCount);
    context->SetWindowSize(400,300);
    auto* renderer=context->GetRenderer();auto* camera=renderer->GetActiveCamera();
    camera->SetParallelProjection(true);camera->SetPosition(0,-100,0);camera->SetFocalPoint(0,0,0);camera->SetViewUp(0,0,1);camera->SetParallelScale(20);
    const auto navigation=context->GetNavigation();
    const double tolerance=std::numeric_limits<double>::epsilon()*400*1024;
    SetExpect(navigation&&navigation->GetState()&&navigation->GetState()->isSliceView,"默认导航应关联实际切片",failureCount);
    const auto initial=*navigation->GetState();
    SetExpect(navigation->SetZoomRatio(2)==NavigationStatus::Applied&&std::abs(camera->GetParallelScale()-10)<tolerance,"绝对倍率必须直接作用于参考状态",failureCount);
    SetExpect(navigation->SetZoomRatio(1)==NavigationStatus::Applied&&std::abs(camera->GetParallelScale()-20)<tolerance,"绝对倍率反向恢复失败",failureCount);
    renderer->SetWorldPoint(3,0,7,1);renderer->WorldToDisplay();std::array<double,2> before{renderer->GetDisplayPoint()[0],renderer->GetDisplayPoint()[1]};
    SetExpect(navigation->ZoomBy(1.5,ZoomAnchor::WorldPoint({3,0,7}))==NavigationStatus::Applied,"世界点缩放失败",failureCount);
    renderer->SetWorldPoint(3,0,7,1);renderer->WorldToDisplay();const auto* display=renderer->GetDisplayPoint();
    SetExpect(std::abs(display[0]-before[0])<tolerance&&std::abs(display[1]-before[1])<tolerance,"缩放锚点应保持原屏幕位置",failureCount);
    auto options=navigation->GetOptions();options.zoom.dragSensitivity=0.01;navigation->SetOptions(options);
    InteractionEvent input;input.viewId="slice";input.eventKind=InteractionEventKind::SecondaryPress;input.displayPosition=std::array<double,2>{100.25,100.5};input.displayScale=2;
    const double startScale=camera->GetParallelScale();
    SetExpect(context->SendInput(input).isHandled,"导航拖动未获得捕获",failureCount);
    options.zoom.dragSensitivity=0.1;navigation->SetOptions(options);
    BindingPatch disabled;disabled.Disable(std::string(NavigationBindingKeys::ZoomDrag));
    SetExpect(controller->ApplyBindings(disabled).status==InputBindingStatus::Applied,"捕获期间修改绑定不应重建路由器",failureCount);
    input.eventKind=InteractionEventKind::PointerMove;input.displayPosition=std::array<double,2>{100.25,120.5};
    SetExpect(context->SendInput(input).isSucceeded&&std::abs(camera->GetParallelScale()-startScale*std::exp(0.1))<tolerance,"高 DPI 增量与开始时参数应完整保留",failureCount);
    input.eventKind=InteractionEventKind::SecondaryRelease;SetExpect(context->SendInput(input).isSucceeded&&context->GetIsInputIdle(),"拖动释放应保持原捕获归属",failureCount);
    input.eventKind=InteractionEventKind::SecondaryPress;SetExpect(!context->SendInput(input).isHandled,"禁用后不得回落到 VTK 默认缩放",failureCount);
    SetExpect(context->SetNavigation(nullptr)==NavigationStatus::Applied&&!navigation->GetState(),"替换后旧导航句柄必须解绑",failureCount);
    const auto next=context->GetNavigation();SetExpect(next&&next!=navigation&&std::abs(next->GetState()->zoomRatio-initial.visibleWorldHeight/(2*camera->GetParallelScale()))<tolerance,"替换策略不应重置视图倍率基准",failureCount);
    SetExpect(context->StopInput(),"导航停止清理失败",failureCount);
    SetExpect(next->ZoomBy(1.1)==NavigationStatus::Unbound,"停止后旧导航不得继续修改相机",failureCount);
    InteractionControllerAccess::Stop(*controller);
}

void StartNativeWidgetCase(int& failureCount)
{
    auto update=std::make_shared<TestUpdatePort>();auto state=std::make_shared<TestStatePort>();
    auto context=CreateViewContext({update,state,std::make_shared<TestSlicePort>(),std::make_shared<TestModelPort>()},true,true);
    if(!context||!context->SetInteractorReady()||!context->SetInputEnabled(true)){SetExpect(false,"原生控件 Context 初始化失败",failureCount);return;}
    class Directory final:public IHostViewDirectory {
    public:
        std::shared_ptr<AbstractViewContext> context;
        std::optional<HostDataRoute> GetDataRoute(const HostViewTarget&) const override{return {};}
        std::optional<HostViewRoute> GetViewRoute(const HostViewTarget& t) const override{if(t.viewId!="native")return {};return HostViewRoute{"native",HostRenderViewRole::Primary3D,{},{},context,{}};}
        std::weak_ptr<AppSessionPort> GetSessionPort() const override{return {};}
        std::vector<HostInputRoute> GetInputRoutes(const HostViewTargets&) const override{return {{"native",HostRenderViewRole::Primary3D,HostInputMode::HostInjected,context}};}
        bool StopView(std::string_view) override{return true;}
    };
    auto directory=std::make_shared<Directory>();directory->context=context;
    HostInputRegistry registry(directory);HostViewTargets targets;targets.viewIds={"native"};
    SetExpect(registry.Start(targets),"原生控件输入注册失败",failureCount);
    auto* renderer=context->GetRenderer();context->SetWindowSize(300,300);
    auto* camera=renderer->GetActiveCamera();camera->SetParallelProjection(true);camera->SetParallelScale(3);camera->SetPosition(0,0,10);camera->SetFocalPoint(0,0,0);camera->SetViewUp(0,1,0);
    auto widget=vtkSmartPointer<FeatureSupport::RoutedBoxWidget>::New();auto representation=vtkSmartPointer<vtkBoxRepresentation>::New();
    double bounds[6]{-1,1,-1,1,-1,1};representation->SetPlaceFactor(1);representation->PlaceWidget(bounds);
    widget->SetRepresentation(representation);widget->SetInteractor(context->GetInteractor());widget->SetCurrentRenderer(renderer);widget->SetDefaultRenderer(renderer);widget->On();
    struct State {bool active=false;bool failCleanup=false;int completed=0;} input;
    auto callback=vtkSmartPointer<vtkCallbackCommand>::New();callback->SetClientData(&input);
    callback->SetCallback([](vtkObject* caller,unsigned long event,void* data,void*){
        auto& state=*static_cast<State*>(data);if(event==vtkCommand::StartInteractionEvent){state.active=true;static_cast<FeatureSupport::RoutedBoxWidget*>(caller)->ReleaseInputFocus();}
        if(event==vtkCommand::EndInteractionEvent){if(!state.failCleanup)state.active=false;++state.completed;}
    });
    widget->AddObserver(vtkCommand::StartInteractionEvent,callback);widget->AddObserver(vtkCommand::EndInteractionEvent,callback);
    const auto controller=registry.GetController();
    HostInputBinding binding;binding.featureId="native-widget";binding.targetViews=targets;
    InteractionActionInfo action;action.actionId="test.widget.edit";action.ownerId=binding.featureId;
    action.bindings.push_back({"test.widget.drag",{InputTriggerKind::Drag,InputMouseButton::Primary},true,{},"widget:test",{InputTriggerKind::Drag}});binding.actions.push_back(action);
    binding.getIsActionAvailable=[&](std::string_view,const InteractionEvent& e){return representation->ComputeInteractionState(e.x,e.y)!=vtkBoxRepresentation::Outside;};
    binding.getIsCaptureActive=[&](std::string_view){return input.active;};
    binding.onBindingsChanged=[&](const std::vector<EffectiveBinding>& values){if(!input.active)FeatureSupport::SetWidgetBindings(widget,values,{{"test.widget.drag",vtkWidgetEvent::Translate}});};
    binding.onAction=[&](std::string_view,const InteractionEvent& e,const std::optional<HostSemanticTarget>&){
        if(e.eventKind==InteractionEventKind::Cancel){if(input.failCleanup)return InteractionResult{true,true,false,InteractionFailureReason::CleanupRejected};widget->StopGesture();input.active=false;return InteractionResult{true,true,true};}
        if(e.eventKind==InteractionEventKind::PrimaryPress)input.active=true;return InteractionResult{true,false,true};
    };
    SetExpect(registry.GetFeaturePort().AttachInput(std::move(binding)),"原生控件动作装配失败",failureCount);
    context->GetRenderWindow()->SetOffScreenRendering(1);context->GetRenderWindow()->Render();
    HostInputEvent event;event.viewId="native";event.x=150;event.y=150;event.kind=HostInputKind::PrimaryPress;
    const auto pressed=registry.SendInput(event);SetExpect(pressed.isSucceeded&&pressed.isHandled&&!context->GetIsInputIdle(),"原生控件按下须同时建立 SDK 捕获",failureCount);
    SetExpect(context->GetNavigation()->ZoomBy(1.1)==NavigationStatus::Busy,"业务捕获期间相机写入应拒绝",failureCount);
    BindingPatch patch;patch.Disable("test.widget.drag");SetExpect(controller->ApplyBindings(patch).status==InputBindingStatus::Applied,"控件拖动期间可改绑定",failureCount);
    event.kind=HostInputKind::PointerMove;event.x=175;SetExpect(registry.SendInput(event).isSucceeded,"控件抓取焦点后仍须经过 SDK 输入桥",failureCount);
    vtkNew<vtkTransform> transform;representation->GetTransform(transform);
    SetExpect(std::abs(transform->GetMatrix()->GetElement(0,3))>0&&camera->GetParallelScale()==3,"控件只执行一次且不重复修改相机",failureCount);
    event.kind=HostInputKind::PrimaryRelease;SetExpect(registry.SendInput(event).isSucceeded&&input.completed==1&&context->GetIsInputIdle(),"原生释放必须结束同一捕获且完成一次",failureCount);
    FeatureSupport::SetWidgetBindings(widget,controller->GetBindings(BindingScope::View("native")),{{"test.widget.drag",vtkWidgetEvent::Translate}});
    event.kind=HostInputKind::PrimaryPress;const auto before=input.completed;(void)registry.SendInput(event);
    SetExpect(!input.active&&input.completed==before,"禁用后不得进入旧原生控件映射",failureCount);
    // widget 禁用后仍可能由 SDK 导航接管按下，先完成该物理按键周期。
    event.kind=HostInputKind::PrimaryRelease;(void)registry.SendInput(event);
    BindingPatch restore;restore.UseDefault("test.widget.drag");SetExpect(controller->ApplyBindings(restore).status==InputBindingStatus::Applied,"控件恢复绑定失败",failureCount);
    event.kind=HostInputKind::PrimaryPress;(void)registry.SendInput(event);
    SetExpect(input.active&&!context->GetIsInputIdle(),"恢复后须建立原生控件捕获",failureCount);input.failCleanup=true;
    event.kind=HostInputKind::PrimaryRelease;(void)registry.SendInput(event);
    SetExpect(!context->GetIsInputIdle(),"原生 release 后清理未完成须保留 SDK 捕获",failureCount);
    event.kind=HostInputKind::Cancel;SetExpect(!registry.SendInput(event).isSucceeded&&!context->GetIsInputIdle(),"失败取消不能丢弃原生捕获",failureCount);
    input.failCleanup=false;SetExpect(registry.SendInput(event).isSucceeded&&context->GetIsInputIdle(),"原生清理应可以重试",failureCount);
    widget->Off();SetExpect(registry.GetFeaturePort().DetachInput("native-widget")&&registry.Stop()&&context->StopInput(),"原生控件退出清理失败",failureCount);
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
        StartHiddenPickCase(failureCount);
        StartCaptureCase(failureCount);
        StartCaptureRetryCase(failureCount);
        StartCancelCase(failureCount);
        StartCaptureNonPointerCase(failureCount);
        StartCaptureReentrancyCase(failureCount);
        StartAtomicCaptureCase(failureCount);
        StartDefaultNavigationPolicyCase(failureCount);

        StartViewNavigationCase(failureCount);
    StartNativeWidgetCase(failureCount);
    StartContextThreadCase(failureCount);
        StartContextStyleCancelRetryCase(failureCount);
        return failureCount;
    }
};

int main()
{
    int failureCount = InteractionCases().GetFailCount();
    failureCount += HostRouterSuite().GetFailCount();
    failureCount += InteractionControllerSuite().GetFailCount();
    failureCount += HostInputRegistrySuite().GetFailCount();
    failureCount += AppStateSuite().GetFailCount();

    if (failureCount == 0) {
        std::cout << "Interaction tests passed.\n";
        return 0;
    }

    std::cerr << "Interaction tests failed: " << failureCount << '\n';
    return 1;
}
