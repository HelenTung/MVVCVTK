#pragma once

#include "IInteractionHandler.h"
#include "Interaction/InteractionPorts.h"
#include "Viewer2DHandler.h"
#include "Viewer3DHandler.h"

class vtkPropPicker;
class vtkRenderer;

// Host 私有的默认导航阶段；普通事件 FirstMatch，Cancel 广播。
class DefaultNavigationPolicy final : public IInteractionHandler
{
public:
    DefaultNavigationPolicy(
        InteractionStatePort* statePort,
        SliceInputPort* slicePort,
        ModelInputPort* modelPort,
        RenderUpdatePort* updatePort,
        vtkPropPicker* picker,
        vtkRenderer* renderer);

    InteractionResult Send(const InteractionEvent& event) override;
    InteractionDispatch Route(const InteractionEvent& event) override;

private:
    Viewer2DHandler m_viewer2D;
    Viewer3DHandler m_viewer3D;
};
