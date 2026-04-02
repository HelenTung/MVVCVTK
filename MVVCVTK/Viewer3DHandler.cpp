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
        vtkMath::Cross(forward, up, right);

        const auto distance = cam->GetDistance();
        const auto angle = cam->GetViewAngle() * (3.14159265358979 / 180.0);;
        const auto winsize = m_renderer->GetRenderWindow()->GetSize();
        const auto vph = (winsize && winsize[1] > 0) ? winsize[1] : 1;
        auto piepx = 2 * distance * std::tan(angle * 0.5) / vph;

        double dx_W = dx * piepx;
        double dy_W = dy * piepx;

        // 构造世界空间位移向量 (w=0)
        double delta_W[4] = {
            right[0] * dx_W + up[0] * dy_W,
            right[1] * dx_W + up[1] * dy_W,
            right[2] * dx_W + up[2] * dy_W,
            0.0
        };

        // 获取 World -> Model 的逆变换矩阵
        auto matData = m_service->GetModelMatrix();
        auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
        mat->DeepCopy(matData.data());
        mat->Invert(); // 反转矩阵，用于将世界位移映射回模型空间

        // 将世界空间的位移向量转换到模型空间
        double delta_M[4] = { 0.0, 0.0, 0.0, 0.0 };
        mat->MultiplyPoint(delta_W, delta_M);

        // 提取对应目标轴的增量（模型空间中，切片平面的移动刚好就是坐标轴方向）
        double axisDelta = delta_M[m_dragAxis];

        // 构造目标物理坐标：SyncCursorToWorldPosition 实际上接收的是 Model Space 坐标
        double fakeModelPos[3] = { 0.0, 0.0, 0.0 };
        auto curIdx = m_service->GetCursorPosition();

        if (auto* img = m_service->GetDataManager()
            ? m_service->GetDataManager()->GetVtkImage().Get()
            : nullptr)
        {
            double sp[3], orig[3];
            img->GetSpacing(sp);
            img->GetOrigin(orig);

            // 当前切片所在的模型空间物理坐标 + 鼠标拖拽对应的模型空间增量
            fakeModelPos[m_dragAxis] =
                orig[m_dragAxis] +
                curIdx[m_dragAxis] * sp[m_dragAxis] +
                axisDelta;
        }

        m_service->SyncCursorToWorldPosition(fakeModelPos, m_dragAxis);

        return { true, true };
    }

    return {};
}