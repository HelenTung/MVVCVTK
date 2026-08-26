#pragma once

#include "Interaction/AbstractViewContext.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

class ViewContextStub final : public AbstractViewContext {
public:
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
        std::function<InteractionResult(const InteractionEvent&)> handler,
        std::vector<InteractionEventKind> eventKinds) override
    {
        m_inputHandler = std::move(handler);
        m_inputEventKinds = std::move(eventKinds);
        return true;
    }

    bool ClearInputHandler() override
    {
        m_inputHandler = nullptr;
        m_inputEventKinds.clear();
        return true;
    }

    InteractionResult OnInput(const InteractionEvent& event)
    {
        if (!m_inputHandler
            || (!m_inputEventKinds.empty()
                && std::find(
                    m_inputEventKinds.begin(),
                    m_inputEventKinds.end(),
                    event.eventKind) == m_inputEventKinds.end())) {
            return {};
        }
        return m_inputHandler(event);
    }

    bool SetInteractorReady() override { return true; }
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
    std::function<InteractionResult(const InteractionEvent&)> m_inputHandler;
    std::vector<InteractionEventKind> m_inputEventKinds;
    std::function<void()> m_timerHandler;
};
