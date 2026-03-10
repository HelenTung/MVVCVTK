#include "Viewer2DHandler.h"
#include "AppInterfaces.h"
#include <vtkCommand.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>

namespace {

    bool IsSliceMode(VizMode mode)
    {
        return mode == VizMode::SliceAxial
            || mode == VizMode::SliceCoronal
            || mode == VizMode::SliceSagittal;
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
        m_service->UpdateInteraction(delta);
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
        return {};
    }

    // ── 左键抬起：结束十字线拖拽 ─────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonReleaseEvent)
    {
        if (m_enableDragCrosshair) {
            m_enableDragCrosshair = false;
            m_service->SetInteracting(false);
            return { true, false };
        }
        return {};
    }

    // ── 右键按下：开始拖拽调窗 ─────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::RightButtonPressEvent)
    {
        m_enableDragWindowLevel = true;
        m_lastDragX = eve.x;
        m_lastDragY = eve.y;
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
                m_service->SyncCursorToWorldPosition(worldPos);
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

            const double deltaWW = dx * kWWSensitivity;
            const double deltaWC = dy * kWCSensitivity;  // VTK Y 轴向上，不取反

            m_service->AdjustWindowLevel(deltaWW, deltaWC);
            // AdjustWindowLevel 内部已调 MarkNeedsSync，此处无需重复 MarkDirty
            return { true, true };
        }

        return {};
    }

    return {};
}