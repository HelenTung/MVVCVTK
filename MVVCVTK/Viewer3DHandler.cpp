#include "Viewer3DHandler.h"
#include "AppInterfaces.h"
#include <vtkActor.h>
#include <vtkCommand.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>
#include <vtkMath.h>

namespace {

    bool GetIsCompositeMode(VizMode mode)
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

InteractionResult Viewer3DHandler::GetHandleResult(const InteractionEvent& eve)
{
    if (!m_service || !GetIsCompositeMode(eve.vizMode)) {
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
            m_service->SetDirtyMarked();
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
		vtkMath::Normalize(up);
		vtkMath::Normalize(forward);
        vtkMath::Cross(forward, up, right);
        vtkMath::Normalize(right);

        const auto distance = cam->GetDistance();
        const auto angle = cam->GetViewAngle() * (3.14159265358979 / 180.0);;
        const auto winsize = m_renderer->GetRenderWindow()->GetSize();
        const auto vph = (winsize && winsize[1] > 0) ? winsize[1] : 1;
        auto piepx = 2 * distance * std::tan(angle * 0.5) / vph;

        double dx_W = dx * piepx;
        double dy_W = dy * piepx;

        // 构造世界空间位移向量增量 (w=0)
        double delta_W[4] = {
            right[0] * dx_W + up[0] * dy_W,
            right[1] * dx_W + up[1] * dy_W,
            right[2] * dx_W + up[2] * dy_W,
            0
        };

        double normal_w[4] = { 0.0, 0.0, 0.0, 0.0 };
        normal_w[m_dragAxis] = 1.0;
        double dist = delta_W[0] * normal_w[0] + delta_W[1] * normal_w[1] + delta_W[2] * normal_w[2];

        // 构造真正有意义的受约束位移
        double pure_delta_W[4] = {
            dist * normal_w[0],
            dist * normal_w[1],
            dist * normal_w[2],
            0.0 // 向量 w=0
        };

		auto LastWorldPos = m_service->GetCursorWorld();
        double newWorldPos[3] = {
            LastWorldPos[0] + pure_delta_W[0],
            LastWorldPos[1] + pure_delta_W[1],
            LastWorldPos[2] + pure_delta_W[2],
		};

        // 全量更新
        m_service->SetCursorWorldPosition(newWorldPos, -1);

        return { true, true };
    }

    return {};
}
