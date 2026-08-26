#pragma once
#include "IInteractionHandler.h"
#include "Interaction/InteractionPorts.h"

class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer3DHandler — 处理 CompositeVolume / CompositeIsoSurface 模式下的交互
//
// 支持的交互：
//   左键拾取切片平面 → 在 world 单轴约束下反投影鼠标增量，再调用 SetCursorWorldPosition
//   拖拽期间降低渲染更新率（15 fps），释放后恢复静态高精度（0.001）
// ─────────────────────────────────────────────────────────────────────
class Viewer3DHandler : public IInteractionHandler
{
public:
    Viewer3DHandler(
        InteractionStatePort* statePort,
        SliceInputPort* slicePort,
        ModelInputPort* modelPort,
        RenderUpdatePort* updatePort,
        vtkPropPicker* picker,
        vtkRenderer* renderer);
    ~Viewer3DHandler() override;

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

    bool m_isDragging = false; // 命中参考平面后置位，左键 release 清零
    int  m_dragAxis = -1;    // 单轴约束：0/1/2 为 world X/Y/Z，-1 表示未拖拽
    // 上一帧 VTK display 坐标，单位像素、左下角为原点；用于反投影鼠标增量。
    int  m_lastMouseX = 0;
    int  m_lastMouseY = 0;
};
