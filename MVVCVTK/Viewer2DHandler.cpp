#include "Viewer2DHandler.h"
#include "AppInterfaces.h"
#include <vtkCommand.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkCamera.h>

namespace {

    bool IsSliceMode(VizMode mode)
    {
        return mode == VizMode::SliceTop_down
            || mode == VizMode::SliceFront_back
            || mode == VizMode::SliceLeft_right;
    }

    // 窗宽/窗位灵敏度系数（每像素对应的 WW/WC 变化量）
    // 可根据数据范围动态缩放，此处使用固定系数作为合理默认值
    constexpr double kWWSensitivity = 2.0;   // 水平拖动 → ΔWW（对比度）
    constexpr double kWCSensitivity = 2.0;   // 垂直拖动 → ΔWC（亮度/窗位）

} // namespace

Viewer2DHandler::Viewer2DHandler(AbstractInteractiveService* service,
    vtkPropPicker* picker,
    vtkRenderer* renderer)
    : m_service(service)
    , m_picker(picker)
    , m_renderer(renderer)
{
}

InteractionResult Viewer2DHandler::Handle(const InteractionEvent& eve)
{
    if (!m_service || !IsSliceMode(eve.vizMode)) {
        return {};
    }

    // ── 滚轮切片 ──────────────────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::MouseWheelForwardEvent
        || eve.vtkEventId == vtkCommand::MouseWheelBackwardEvent)
    {
        const int step = eve.ctrl ? 5 : 1;
        const int delta = (eve.vtkEventId == vtkCommand::MouseWheelForwardEvent)
            ? step : -step;
        m_service->ScrollSlice(delta);
        // m_service->MarkDirty();
        return { true, true };  // abortVtk=true：阻止 VTK 默认滚轮相机缩放
    }

    // ── 左键按下：Shift → 开始拖拽十字线 ────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonPressEvent)
    {
        if (eve.shift) {
            m_enableDragCrosshair = true;
            m_service->SetInteracting(true);
            return { true, true };  // abortVtk=true：阻止 VTK 默认 Window/Level
        }
        return { true, true };
    }

    // ── 左键抬起：结束十字线拖拽 ─────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonReleaseEvent)
    {
        if (m_enableDragCrosshair) {
            m_enableDragCrosshair = false;
            m_service->SetInteracting(false);
            return { true, true };
        }
        return { true, true };
    }

    // ── 右键按下：开始拖拽调窗 ─────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::RightButtonPressEvent)
    {
        m_enableDragWindowLevel = true;
        m_lastDragX = eve.x;
        m_lastDragY = eve.y;

        m_startDragX = eve.x;
        m_startDragY = eve.y;

        // 获取当前状态作为静态常数 W_start
        auto wl = m_service->GetWindowLevel();
        m_startWW = wl.windowWidth;
        m_startWC = wl.windowCenter;

        m_service->SetInteracting(true);
        return { true, true };  // abortVtk=true：阻止 VTK 默认右键菜单/操作
    }

    // ── 右键抬起：结束调窗 ─────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::RightButtonReleaseEvent)
    {
        if (m_enableDragWindowLevel) {
            m_enableDragWindowLevel = false;
            m_service->SetInteracting(false);
            return { true, false };
        }
        return {};
    }

    // ── 鼠标移动：十字线拖拽 / 调窗拖拽（二选一）────────────────────
    if (eve.vtkEventId == vtkCommand::MouseMoveEvent)
    {
        // 路径 A：十字线拖拽（Shift+左键）
        if (m_enableDragCrosshair)
        {
            if (!m_picker || !m_renderer) return {};
            m_picker->Pick(eve.x, eve.y, 0, m_renderer);
            double* worldPos = m_picker->GetPickPosition();
            if (worldPos) {
                m_service->UpdateCursorFromWorldPosition(worldPos);
                m_service->MarkDirty();
            }
            return { true, true };
        }

        // 路径 B：调窗拖拽（右键）
        //   水平方向（ΔX > 0 → 向右拖）→ 增大 WW（对比度变强）
        //   垂直方向（ΔY > 0 → 向上拖）→ 增大 WC（图像变暗/窗位升高）
        if (m_enableDragWindowLevel)
        {
            const int dx = eve.x - m_lastDragX;
            const int dy = eve.y - m_lastDragY;
            m_lastDragX = eve.x;
            m_lastDragY = eve.y;
            const int totalDx = eve.x - m_startDragX;
            const int totalDy = eve.y - m_startDragY;

            // 提取视口分辨率以执行归一化
            int viewWidth = 600, viewHeight = 600;
            if (m_renderer && m_renderer->GetRenderWindow()) {
                int* size = m_renderer->GetRenderWindow()->GetSize();
                if (size && size[0] > 0 && size[1] > 0) {
                    viewWidth = size[0];
                    viewHeight = size[1];
                }   
            }

            m_service->AdjustWindowLevel(totalDx, totalDy, viewWidth, viewHeight, m_startWW, m_startWC);
            return { true, true };
        }

        return {};
    }

    return {};
}