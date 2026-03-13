#include "Viewer3DHandler.h"
#include "AppInterfaces.h"
#include <vtkActor.h>
#include <vtkCommand.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>

namespace {

    bool IsCompositeMode(VizMode mode)
    {
        return mode == VizMode::CompositeVolume
            || mode == VizMode::CompositeIsoSurface;
    }

} // namespace

Viewer3DHandler::Viewer3DHandler(AbstractInteractiveService* service,
    vtkPropPicker* picker,
    vtkRenderer* renderer)
    : m_service(service)
    , m_picker(picker)
    , m_renderer(renderer)
{
}

InteractionResult Viewer3DHandler::Handle(const InteractionEvent& eve)
{
    if (!m_service || !IsCompositeMode(eve.vizMode)) {
        return {};
    }

    // ── 左键按下：拾取切片平面 ────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonPressEvent)
    {
        if (!m_picker || !m_renderer) {
            return {};
        }

        if (m_picker->Pick(eve.x, eve.y, 0, m_renderer)) {
            vtkActor* actor = m_picker->GetActor();
            const int axis = m_service->GetPlaneAxis(actor);

            if (axis != -1) {
                m_isDragging = true;
                m_dragAxis = axis;
                m_lastMouseX = eve.x;   // 记录起始点，供 MouseMove 计算增量
                m_lastMouseY = eve.y;
                m_service->SetInteracting(true);

                // 拖拽期间降低更新率，保证流畅
                if (vtkRenderWindow* rw = m_renderer->GetRenderWindow()) {
                    rw->SetDesiredUpdateRate(15.0);
                }
                return { true, true };  // abortVtk=true：阻止相机转动
            }
        }
        // 点到主模型或空白处：不消费，让相机交互继续
        return {};
    }

    // ── 左键抬起：结束拖拽 ────────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonReleaseEvent)
    {
        if (m_isDragging) {
            // 恢复静态高精度渲染（VTK 推荐静态更新率 0.001）
            if (vtkRenderWindow* rw = m_renderer->GetRenderWindow()) {
                rw->SetDesiredUpdateRate(0.001);
            }
            m_service->SetInteracting(false);
            m_service->MarkDirty();
            m_isDragging = false;
            m_dragAxis = -1;
            return { true, false };
        }
        return {};
    }

    // ── 鼠标移动：平面拖拽 ────────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::MouseMoveEvent)
    {
        if (!m_isDragging || m_dragAxis == -1 || !m_renderer) {
            return {};
        }

        const int dx = eve.x - m_lastMouseX;
        const int dy = eve.y - m_lastMouseY;
        m_lastMouseX = eve.x;
        m_lastMouseY = eve.y;

        if (dx == 0 && dy == 0) {
            return { true, true };
        }

        vtkCamera* cam = m_renderer->GetActiveCamera();
        if (!cam) return { true, true };

        // 相机的 up 和 right 方向（世界空间，已归一化）
        double up[3], forward[3], right[3];
        cam->GetViewUp(up);
        cam->GetDirectionOfProjection(forward);

        // right = forward × up
        right[0] = forward[1] * up[2] - forward[2] * up[1];
        right[1] = forward[2] * up[0] - forward[0] * up[2];
        right[2] = forward[0] * up[1] - forward[1] * up[0];

        // 目标轴单位向量（0→X, 1→Y, 2→Z）
        double axisVec[3] = { 0.0, 0.0, 0.0 };
        axisVec[m_dragAxis] = 1.0;

        // 屏幕位移在目标轴上的投影（dx 对应 right，dy 对应 up）
        const double proj =
            (right[0] * axisVec[0] + right[1] * axisVec[1] + right[2] * axisVec[2]) * dx +
            (up[0] * axisVec[0] + up[1] * axisVec[1] + up[2] * axisVec[2]) * dy;

        // 每像素对应的世界单位：camDist × 2tan(fov/2) / viewportHeight
        const double camDist = cam->GetDistance();
        const double viewAngle = cam->GetViewAngle() * (3.14159265358979 / 180.0);
        int* winSize = m_renderer->GetRenderWindow()->GetSize();
        const int    vpH = (winSize && winSize[1] > 0) ? winSize[1] : 1;
        const double worldPerPx = 2.0 * camDist * std::tan(viewAngle * 0.5) / vpH;

        // 构造目标世界坐标：只修改目标轴分量，其余轴取当前光标对应物理坐标
        // 这样 SyncCursorToWorldPosition(pos, axis) 只更新一个轴，其余不变
        const double worldDelta = proj * worldPerPx;

        auto curIdx = m_service->GetCursorPosition();
        // 取当前光标的世界坐标作为基础，只叠加目标轴增量
        // 注意：SyncCursorToWorldPosition 内部用 axis 过滤，只更新 m_dragAxis 轴
        // 因此其他两轴传什么值无所谓，直接传 0 即可
        double fakeWorldPos[3] = { 0.0, 0.0, 0.0 };

        // 用当前光标索引换算回当前物理坐标，再叠加增量
        // Service 层 SyncCursorToWorldPosition 会把世界坐标转回索引，只写 m_dragAxis 轴
        if (auto* img = m_service->GetDataManager()
            ? m_service->GetDataManager()->GetVtkImage().Get()
            : nullptr)
        {
            double sp[3], orig[3];
            img->GetSpacing(sp);
            img->GetOrigin(orig);
            fakeWorldPos[m_dragAxis] =
                orig[m_dragAxis] +
                curIdx[m_dragAxis] * sp[m_dragAxis] +
                worldDelta;
        }

        m_service->SyncCursorToWorldPosition(fakeWorldPos, m_dragAxis);

        return { true, true };
    }

    return {};
}