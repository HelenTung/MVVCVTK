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
        // Ctrl 修饰：步进 ×5，便于快速浏览
        const int step = eve.ctrl ? 5 : 1;
        const int delta = (eve.vtkEventId == vtkCommand::MouseWheelForwardEvent)
            ? step : -step;
        m_service->UpdateInteraction(delta);
        m_service->MarkDirty();
        return { true, true };  // abortVtk=true：阻止 VTK 默认滚轮相机缩放
    }

    // ── Shift + 左键按下：开始拖拽十字线 ─────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonPressEvent)
    {
        if (eve.shift) {
            m_enableDragCrosshair = true;
            m_service->SetInteracting(true);
            return { true, true };  // abortVtk=true：阻止 VTK 默认 Window/Level
        }
        return {};
    }

    // ── 左键抬起：结束拖拽 ────────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonReleaseEvent)
    {
        if (m_enableDragCrosshair) {
            m_enableDragCrosshair = false;
            m_service->SetInteracting(false);
            return { true, false };
        }
        return {};
    }

    // ── 鼠标移动：拖拽十字线 ──────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::MouseMoveEvent)
    {
        if (!m_enableDragCrosshair || !m_picker || !m_renderer) {
            return {};
        }

        m_picker->Pick(eve.x, eve.y, 0, m_renderer);
        double* worldPos = m_picker->GetPickPosition();
        if (worldPos) {
            m_service->SyncCursorToWorldPosition(worldPos);
            m_service->MarkDirty();
        }
        return { true, true };
    }

    return {};
}