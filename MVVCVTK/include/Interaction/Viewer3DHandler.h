#pragma once
#include "IInteractionHandler.h"

class InteractiveService;
class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer3DHandler — 处理 CompositeVolume / CompositeIsoSurface 模式下的交互
//
// 支持的交互：
//   左键拾取切片平面 → 拖拽平面（UpdateCursorFromWorldPosition + dragAxis）
//   拖拽期间降低渲染更新率（15 fps），释放后恢复静态高精度（0.001）
// ─────────────────────────────────────────────────────────────────────
class Viewer3DHandler : public IInteractionHandler
{
public:
    Viewer3DHandler(InteractiveService* service,
        vtkPropPicker* picker,
        vtkRenderer* renderer);

    InteractionResult Send(const InteractionEvent& eve) override;

private:
    // 非拥有观察指针；StdRenderContext 持有 service 与 VTK 对象，Router 重建会先销毁本 Handler。
    InteractiveService* m_service = nullptr;
    vtkPropPicker* m_picker = nullptr;
    vtkRenderer* m_renderer = nullptr;

    bool m_isDragging = false; // 命中参考平面后置位，左键 release 清零
    int  m_dragAxis = -1;    // 单轴约束：0/1/2 为 world X/Y/Z，-1 表示未拖拽
    // 上一帧 VTK display 坐标，单位像素、左下角为原点；用于反投影鼠标增量。
    int  m_lastMouseX = 0;
    int  m_lastMouseY = 0;
};
