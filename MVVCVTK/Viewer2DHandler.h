#pragma once
#include "IInteractionHandler.h"

class AbstractInteractiveService;
class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer2DHandler — 处理 SliceAxial / SliceCoronal / SliceSagittal 模式下的交互
//
// 支持的交互：
//   滚轮前/后         → 切片步进 ±1（Ctrl 修饰时 ±5）
//   Shift + 左键拖拽  → 拖拽十字线定位（SyncCursorToWorldPosition）
// ─────────────────────────────────────────────────────────────────────
class Viewer2DHandler : public IInteractionHandler
{
public:
    Viewer2DHandler(AbstractInteractiveService* service,
        vtkPropPicker* picker,
        vtkRenderer* renderer);

    InteractionResult Handle(const InteractionEvent& eve) override;

private:
    AbstractInteractiveService* m_service = nullptr;
    vtkPropPicker* m_picker = nullptr;
    vtkRenderer* m_renderer = nullptr;

    bool m_enableDragCrosshair = false;  // 是否正在拖拽十字线
    bool m_enableDragWindowLevel = false; // 右键：拖拽调窗
    int  m_lastDragX = 0;                 // 上一帧鼠标 X（用于计算 delta）
    int  m_lastDragY = 0;                 // 上一帧鼠标 Y
};