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
        
        // 获取屏幕二维坐标
        auto lastworldpos = m_service->GetCursorWorld();
        m_renderer->SetWorldPoint(lastworldpos[0], lastworldpos[1], lastworldpos[2],1.0);
        m_renderer->WorldToDisplay();
        auto lastdisplay = m_renderer->GetDisplayPoint();

        // 新的屏幕二维坐标，深度保持不变，确保深度关系正常
        double curdisplay[3] =
        {
            lastdisplay[0] + static_cast<double>(dx),
            lastdisplay[1] + static_cast<double>(dy),
            lastdisplay[2]
        };

        // 将新的屏幕坐标反投影回世界空间
        m_renderer->SetDisplayPoint(curdisplay);
        m_renderer->DisplayToWorld();
        auto curworldpos = m_renderer->GetWorldPoint();
        
        // 齐次坐标除法，还原为 3D 世界坐标
        double invW = (curworldpos[3] != 0.0) ? (1.0 / curworldpos[3]) : 1.0;
        double newWorldPos[3] = {
            curworldpos[0] * invW,
            curworldpos[1] * invW,
            curworldpos[2] * invW
        };
        
        // 增量约束更新
        auto deltaAxis = newWorldPos[m_dragAxis] - lastworldpos[m_dragAxis];
        double finalyworld[3] = {
            lastworldpos[0],
            lastworldpos[1],
            lastworldpos[2]
        };
        finalyworld[m_dragAxis] += deltaAxis;
        
        // 全量更新
        m_service->SetCursorWorldPosition(finalyworld, -1);

        return { true, true };
    }

    return {};
}
