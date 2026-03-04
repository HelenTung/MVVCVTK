#pragma once
#include "IInteractionHandler.h"

class AbstractInteractiveService;
class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer3DHandler — 处理 CompositeVolume / CompositeIsoSurface 模式下的交互
//
// 支持的交互：
//   左键拾取切片平面 → 拖拽平面（SyncCursorToWorldPosition + dragAxis）
//   拖拽期间降低渲染更新率（15 fps），释放后恢复静态高精度（0.001）
// ─────────────────────────────────────────────────────────────────────
class Viewer3DHandler : public IInteractionHandler
{
public:
    Viewer3DHandler(AbstractInteractiveService* service,
        vtkPropPicker* picker,
        vtkRenderer* renderer);

    InteractionResult Handle(const InteractionEvent& eve) override;

private:
    AbstractInteractiveService* m_service = nullptr;
    vtkPropPicker* m_picker = nullptr;
    vtkRenderer* m_renderer = nullptr;

    bool m_isDragging = false;  // 是否正在拖拽平面
    int  m_dragAxis = -1;     // 当前拖拽的轴向（0/1/2 = X/Y/Z）
};