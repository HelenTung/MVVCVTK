#pragma once
#include "IInteractionHandler.h"

class InteractiveService;
class vtkPropPicker;
class vtkRenderer;

// ─────────────────────────────────────────────────────────────────────
// Viewer2DHandler — 处理 SliceTop_down / SliceFront_back / SliceLeft_right 模式下的交互
//
// 支持的交互：
//   滚轮前/后         → 切片步进
//   isShiftDown + 左键拖拽  → 拖拽十字线定位
//   isCtrlDown + 左键拖拽   → 定轴旋转
//   左键拖拽          → 调窗
//   右键拖拽          → 缩放
// ─────────────────────────────────────────────────────────────────────
class Viewer2DHandler : public IInteractionHandler
{
public:
    Viewer2DHandler(InteractiveService* service,
        vtkPropPicker* picker,
        vtkRenderer* renderer);

    InteractionResult Send(const InteractionEvent& eve) override;

private:
    // 非拥有观察指针；StdRenderContext 持有 service 与 VTK 对象，Router 重建会先销毁本 Handler。
    InteractiveService* m_service = nullptr;
    vtkPropPicker* m_picker = nullptr;
    vtkRenderer* m_renderer = nullptr;

    // 四种拖拽状态分别在对应 press 置位、release 清零，并驱动 SharedState 的 interacting 状态。
    bool m_isDragCrosshair = false;  // Shift+左键：拖拽十字线
    bool m_isDragSlice = false;      // Ctrl+左键：绕当前十字线定轴旋转
    bool m_isDragWindowLevel = false; // 普通左键：调窗
    bool m_isRightZoom = false;       // 右键：修改平行投影缩放
    // VTK display 坐标，单位像素、左下角为原点；每次旋转或调窗 MouseMove 后更新。
    int  m_lastRotateX = 0;
    int  m_lastRotateY = 0;
    int  m_lastDragX = 0;
    int  m_lastDragY = 0;
    
    // 左键调窗 press 时的 display 像素与 WW/WC 快照；拖动全程相对该基线计算累计量。
    int m_startDragX = 0;
    int m_startDragY = 0;
    double m_startWW = 0.0;
    double m_startWC = 0.0;

    int m_zoomStartY = 0; // 右键 press 的 VTK display Y 像素
    double m_startOriginValue = 1.0; // press 时 camera parallelScale，单位为 world 视口半高
};
