#pragma once

#include "Interaction/AbstractViewContext.h"
#include "Interaction/InputCallbackHandler.h"
#include "Interaction/InteractionRouter.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

class ViewContextStub final : public AbstractViewContext {
public:
    explicit ViewContextStub(
        const bool isHostInjected = false) noexcept
        : m_isHostInjected(isHostInjected)
    {
    }

    bool SetCameraStyle(const VizMode mode) override
    {
        ++m_cameraStyleSetCount;
        if (m_cameraFailCount > 0) {
            --m_cameraFailCount;
            return false;
        }
        m_vizMode = mode;
        return true;
    }

    VizMode GetVizMode() const { return m_vizMode; }
    int GetCameraStyleSetCount() const { return m_cameraStyleSetCount; }
    void SetCameraFailCount(const int failCount)
    {
        m_cameraFailCount = failCount;
    }

    bool ResetCamera() override
    {
        ++m_cameraResetCount;
        return true;
    }
    int GetCameraResetCount() const { return m_cameraResetCount; }
    bool SetCameraState(const ViewCameraState& state) override
    {
        ++m_cameraRestoreCount;
        return AbstractViewContext::SetCameraState(state);
    }
    int GetCameraRestoreCount() const { return m_cameraRestoreCount; }

    bool SetOrientationAxesVisible(const bool isVisible) override
    {
        ++m_axesSetCount;
        if (m_axesFailCount > 0) {
            --m_axesFailCount;
            return false;
        }
        m_isAxesVisible = isVisible;
        return true;
    }

    bool GetOrientationAxesVisible() const override
    {
        return m_isAxesVisible;
    }

    bool GetAxesVisible() const { return m_isAxesVisible; }
    int GetAxesSetCount() const { return m_axesSetCount; }
    void SetAxesFailCount(const int failCount)
    {
        m_axesFailCount = failCount;
    }

    bool SetToolMode(const ToolMode mode) override
    {
        m_toolMode = mode;
        ++m_toolModeSetCount;
        return true;
    }

    ToolMode GetToolMode() const override { return m_toolMode; }
    int GetToolModeSetCount() const { return m_toolModeSetCount; }

    bool SetInputHandler(
        InteractionRouteCallback handler,
        std::vector<InteractionEventKind> eventKinds) override
    {
        ++m_inputSetCount;
        if (m_inputSetFailCount > 0) {
            --m_inputSetFailCount;
            return false;
        }
        if (!handler || !m_inputRouter.SendCancel({}).isSucceeded
            || !m_inputRouter.ClearHandlers()) {
            return false;
        }
        return m_inputRouter.AttachHandler(
            std::make_unique<InputCallbackHandler>(
                std::move(handler), std::move(eventKinds)));
    }

    bool ClearInputHandler() override
    {
        ++m_inputClearCount;
        if (m_inputClearFailCount > 0) {
            --m_inputClearFailCount;
            return false;
        }
        return m_inputRouter.SendCancel({}).isSucceeded
            && m_inputRouter.ClearHandlers();
    }

    InteractionResult CancelInput(
        const InteractionCaptureKey& key) override
    {
        return m_inputRouter.CancelCapture(key, {});
    }

    InteractionResult SendInput(
        const InteractionEvent& event) override
    {
        if (!m_isHostInjected) {
            return {
                true,
                true,
                false,
                InteractionFailureReason::StateRejected };
        }
        return OnInput(event);
    }

    InteractionResult OnInput(const InteractionEvent& event)
    {
        return m_inputRouter.Dispatch(event);
    }

    void SetInputSetFailCount(int count) { m_inputSetFailCount = count; }
    void SetInputClearFailCount(int count) { m_inputClearFailCount = count; }
    int GetInputSetCount() const { return m_inputSetCount; }
    int GetInputClearCount() const { return m_inputClearCount; }

    bool SetInteractorReady() override { return true; }
    bool SetInputEnabled(const bool isEnabled) override
    {
        m_isInputEnabled = isEnabled;
        return true;
    }
    bool Start() override { return true; }
    bool StopInput() override
    {
        return ClearInputHandler() && ClearTimerHandler();
    }

    bool SetTimerHandler(std::function<void()> handler) override
    {
        m_timerHandler = std::move(handler);
        return true;
    }

    bool ClearTimerHandler() override
    {
        m_timerHandler = {};
        return true;
    }
    vtkRenderWindowInteractor* GetInteractor() const override
    {
        return nullptr;
    }

private:
    VizMode m_vizMode = VizMode::Volume;
    int m_cameraStyleSetCount = 0;
    int m_cameraFailCount = 0;
    int m_cameraResetCount = 0;
    int m_cameraRestoreCount = 0;
    bool m_isAxesVisible = false;
    int m_axesSetCount = 0;
    int m_axesFailCount = 0;
    ToolMode m_toolMode = ToolMode::Navigation;
    int m_toolModeSetCount = 0;
    InteractionRouter m_inputRouter;
    bool m_isHostInjected = false;
    int m_inputSetCount = 0;
    int m_inputClearCount = 0;
    int m_inputSetFailCount = 0;
    int m_inputClearFailCount = 0;
    std::function<void()> m_timerHandler;
    bool m_isInputEnabled = false;
};
