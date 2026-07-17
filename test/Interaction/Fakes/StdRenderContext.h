#pragma once

#include "AppTypes.h"
#include "Interaction/InteractionTypes.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

class StdRenderContext {
public:
    void SetCameraStyle(VizMode mode)
    {
        m_vizMode = mode;
        ++m_cameraStyleSetCount;
    }

    VizMode GetVizMode() const
    {
        return m_vizMode;
    }

    int GetCameraStyleSetCount() const
    {
        return m_cameraStyleSetCount;
    }

    void SetToolMode(ToolMode mode)
    {
        m_toolMode = mode;
        ++m_toolModeSetCount;
    }

    ToolMode GetToolMode() const
    {
        return m_toolMode;
    }

    int GetToolModeSetCount() const
    {
        return m_toolModeSetCount;
    }

    void SetInputHandler(
        std::function<InteractionResult(const InteractionEvent&)> handler,
        std::vector<InteractionEventKind> eventKinds)
    {
        m_inputHandler = std::move(handler);
        m_inputEventKinds = std::move(eventKinds);
    }

    void ClearInputHandler()
    {
        m_inputHandler = nullptr;
        m_inputEventKinds.clear();
    }

    InteractionResult OnInput(const InteractionEvent& event)
    {
        if (!m_inputHandler
            || (!m_inputEventKinds.empty()
                && std::find(m_inputEventKinds.begin(), m_inputEventKinds.end(), event.eventKind)
                    == m_inputEventKinds.end())) {
            return {};
        }
        return m_inputHandler(event);
    }

private:
    VizMode m_vizMode = VizMode::Volume;
    int m_cameraStyleSetCount = 0;
    ToolMode m_toolMode = ToolMode::Navigation;
    int m_toolModeSetCount = 0;
    std::function<InteractionResult(const InteractionEvent&)> m_inputHandler;
    std::vector<InteractionEventKind> m_inputEventKinds;
};
