#pragma once
#include "IInteractionHandler.h"

class AbstractInteractiveService;
class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer2DHandler — 处理 SliceTop_down / SliceFront_back / SliceLeft_right 模式下的交互
//
// 支持的交互：
//   滚轮前/后         → 切片步进
//   Shift + 左键拖拽  → 拖拽十字线定位
//   Ctrl + 左键拖拽   → 定轴旋转
//   左键拖拽          → 调窗
//   右键拖拽          → 缩放
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
    bool m_enableDragSlice = false;      // 是否正在拖拽切片
    bool m_enableDragWindowLevel = false; // 左键：拖拽调窗
    bool m_enableRightZoom = false;       // 右键：2D 图像缩放
    int  m_lastRotateX = 0;               // 上一帧旋转角度（用于计算 delta，单位度）
    int  m_lastRotateY = 0;               // 上一帧旋转角度（用于计算 delta，单位度）
    int  m_lastDragX = 0;                 // 上一帧鼠标 X（用于计算 delta）
    int  m_lastDragY = 0;                 // 上一帧鼠标 Y
    
    int m_startDragX = 0;
    int m_startDragY = 0;
    double m_startWW = 0.0;
    double m_startWC = 0.0;

    int m_zoomStartY = 0;
    double m_startOriginValue = 1.0;
};