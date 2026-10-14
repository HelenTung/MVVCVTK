#pragma once
#include "IInteractionHandler.h"
#include "Interaction/InteractionPorts.h"
#include "Interaction/NavigationBindings.h"

class vtkPropPicker;
class vtkRenderer;

// 切片十字线适配；相机、翻页和调窗由 ViewNavigationExecutor 统一执行。
class Viewer2DHandler : public IInteractionHandler
{
public:
    Viewer2DHandler(
        InteractionStatePort* statePort,
        SliceInputPort* slicePort,
        ModelInputPort* modelPort,
        RenderUpdatePort* updatePort,
        vtkPropPicker* picker,
        vtkRenderer* renderer,
        const NavigationBindings* bindings = nullptr);
    ~Viewer2DHandler() override;

    bool GetIsCaptureActive() const noexcept override { return m_isDragCrosshair; }
    InteractionResult Send(const InteractionEvent& eve) override;

private:
    // 非拥有观察指针；StdViewContext 持有 ports 与 VTK 对象，Router 重建会先销毁本 Handler。
    InteractionStatePort* m_statePort = nullptr;
    SliceInputPort* m_slicePort = nullptr;
    ModelInputPort* m_modelPort = nullptr;
    RenderUpdatePort* m_updatePort = nullptr;
    vtkPropPicker* m_picker = nullptr;
    vtkRenderer* m_renderer = nullptr;
    InteractionSource m_source;
    NavigationBindings m_defaultBindings;
    const NavigationBindings* m_bindings = nullptr;
    InteractionEventKind m_dragReleaseKind = InteractionEventKind::None;

    bool m_isDragCrosshair=false;
};
