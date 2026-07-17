#include "Viewer3DHandler.h"
#include "AppInterfaces.h"
#include <vtkActor.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>
#include <vtkMath.h>

Viewer3DHandler::Viewer3DHandler(InteractiveService* service,
    vtkPropPicker* picker,
    vtkRenderer* renderer)
    : m_service(service)
    , m_picker(picker)
    , m_renderer(renderer)
{
}

InteractionResult Viewer3DHandler::Send(const InteractionEvent& eve)
{
    if (!m_service) {
        return {};
    }

    if (eve.toolMode == ToolMode::ModelTransform
        && eve.eventKind == InteractionEventKind::ViewInteraction) {
        vtkProp3D* prop = m_service->GetMainProp();
        if (prop && prop->GetMatrix()) {
            m_service->SetModelMatrix(prop->GetMatrix());
            m_service->SetDirty();
            return { true, false };
        }
        return {};
    }

    const bool isCompositeMode =
        eve.vizMode == VizMode::CompositeVolume
        || eve.vizMode == VizMode::CompositeIsoSurface;
    if (!isCompositeMode) {
        return {};
    }

    // 3D 模式只处理“参考切片平面拖拽”这一类交互；
    // 其他鼠标事件继续交给 VTK 默认相机控制，避免把 3D 浏览手感全部吞掉。

    // ── 左键按下：拾取切片平面 ────────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::PrimaryPress)
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
                return { true, true };  // 停止传播，阻止相机转动
            }
        }
        // 点到主模型或空白处：不消费，让相机交互继续
        return {};
    }

    // ── 左键抬起：结束拖拽 ────────────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::PrimaryRelease)
    {
        if (m_isDragging) {
            // 恢复静态高精度渲染（VTK 推荐静态更新率 0.001）
            if (vtkRenderWindow* rw = m_renderer->GetRenderWindow()) {
                rw->SetDesiredUpdateRate(0.001);
            }
            m_service->SetInteracting(false);
            m_service->SetDirty();
            m_isDragging = false;
            m_dragAxis = -1;
            return { true, false };
        }
        return {};
    }

    // ── 鼠标移动：平面拖拽 ────────────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::PointerMove)
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
        auto lastWorldPos = m_service->GetCursorWorld();
        m_renderer->SetWorldPoint(lastWorldPos[0], lastWorldPos[1], lastWorldPos[2], 1.0);
        m_renderer->WorldToDisplay();
        auto lastDisplay = m_renderer->GetDisplayPoint();

        // 新的屏幕二维坐标，深度保持不变，确保深度关系正常
        double curDisplay[3] =
        {
            lastDisplay[0] + static_cast<double>(dx),
            lastDisplay[1] + static_cast<double>(dy),
            lastDisplay[2]
        };

        // 将新的屏幕坐标反投影回世界空间
        m_renderer->SetDisplayPoint(curDisplay);
        m_renderer->DisplayToWorld();
        auto curWorldPos = m_renderer->GetWorldPoint();

        // 齐次坐标除法，还原为 3D 世界坐标
        double invW = (curWorldPos[3] != 0.0) ? (1.0 / curWorldPos[3]) : 1.0;
        double newWorldPos[3] = {
            curWorldPos[0] * invW,
            curWorldPos[1] * invW,
            curWorldPos[2] * invW
        };

        // 增量约束更新：只放开当前拖拽轴，其余轴保持不变，
        // 这样 2D 参考平面在 3D 视图中的拖动仍然遵守单轴切片语义。
        auto deltaAxis = newWorldPos[m_dragAxis] - lastWorldPos[m_dragAxis];
        double finalWorld[3] = {
            lastWorldPos[0],
            lastWorldPos[1],
            lastWorldPos[2]
        };
        finalWorld[m_dragAxis] += deltaAxis;

        // 全量更新
        m_service->SetCursorWorldPosition(finalWorld, -1);

        return { true, true };
    }

    return {};
}
