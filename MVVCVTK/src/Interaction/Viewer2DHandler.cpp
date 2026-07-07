#include "Viewer2DHandler.h"
#include "AppInterfaces.h"
#include <vtkCommand.h>
#include <vtkMath.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>
#include <vtkTransform.h>
#include <vtkMatrix4x4.h>
#include <cmath>
#include <algorithm>

namespace {

    bool GetIsSliceMode(VizMode mode)
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

InteractionResult Viewer2DHandler::Send(const InteractionEvent& eve)
{
    if (!m_service || !GetIsSliceMode(eve.vizMode)) {
        return {};
    }

    // 2D 模式下同一时刻只允许一种主交互语义成立：
    // 滚轮切片、Shift 左键拖十字线、Ctrl 左键定轴旋转、右键调窗彼此互斥。

    // ── 滚轮切片 ──────────────────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::MouseWheelForwardEvent
        || eve.vtkEventId == vtkCommand::MouseWheelBackwardEvent)
    {
        const int step = eve.ctrl ? 10 : 5;
        const int delta = (eve.vtkEventId == vtkCommand::MouseWheelForwardEvent)
            ? step : -step;
        m_service->ScrollSlice(delta);
        // m_service->MarkDirty();
        return { true, true };  // abortVtk=true：阻止 VTK 默认滚轮相机缩放
    }

    // ── 左键按下：Shift → 开始拖拽十字线 ────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonPressEvent)
    {
        if (eve.ctrl)
        {
            m_enableDragSlice = true;
            m_lastRotateX = eve.x;
            m_lastRotateY = eve.y;
			m_service->SetInteracting(true);
			return { true, true };  // abortVtk=true：阻止 VTK 默认 Window/Level
        }
        if (eve.shift) {
            m_enableDragCrosshair = true;
            m_service->SetInteracting(true);
            return { true, true };  // abortVtk=true：阻止 VTK 默认 Window/Level
        }

        m_enableDragWindowLevel = true;
        m_lastDragX = eve.x;
        m_lastDragY = eve.y;
        m_startDragX = eve.x;
        m_startDragY = eve.y;

        const auto wl = m_service->GetWindowLevel();
        m_startWW = wl.windowWidth;
        m_startWC = wl.windowCenter;

        m_service->SetInteracting(true);
        return { true, true };
    }

    // ── 左键抬起：结束交互 ─────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::LeftButtonReleaseEvent)
    {
        if (m_enableDragCrosshair) {
            m_enableDragCrosshair = false;
            m_service->SetInteracting(false);
            return { true, true };
        }
        if (m_enableDragSlice)
        {
            m_enableDragSlice = false;
			m_service->SetInteracting(false);
			return { true, true };
        }
        if (m_enableDragWindowLevel) {
            m_enableDragWindowLevel = false;
            m_service->SetInteracting(false);
            return { true, true };
        }
        return { true, true };
    }

    // ── 右键按下：开始缩放 ─────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::RightButtonPressEvent)
    {
        m_enableRightZoom = true;
        m_zoomStartY = eve.y;

        if (m_renderer && m_renderer->GetActiveCamera()) {
            m_startOriginValue = m_renderer->GetActiveCamera()->GetParallelScale();
        }
        else {
            m_startOriginValue = 1.0;
        }

        m_service->SetInteracting(true);
        return { true, true };
    }

    // ── 右键抬起：结束缩放 ─────────────────────────────────────────
    if (eve.vtkEventId == vtkCommand::RightButtonReleaseEvent)
    {
        if (m_enableRightZoom) {
            m_enableRightZoom = false;
            m_service->SetInteracting(false);
            return { true, true };
        }
        return {};
    }

    // ── 鼠标移动：十字线拖拽 / 调窗拖拽 / 缩放 / 定轴旋转 ─────────────
    if (eve.vtkEventId == vtkCommand::MouseMoveEvent)
    {
        // 路径 A：十字线拖拽
        if (m_enableDragCrosshair)
        {
            if (!m_picker || !m_renderer) return {};
            m_picker->Pick(eve.x, eve.y, 0, m_renderer);
            double* worldPos = m_picker->GetPickPosition();

            if (worldPos) {
                // 这里直接写 CursorWorldPosition，让服务层统一完成轴约束、状态广播和后续渲染刷新。
                if (eve.vizMode == VizMode::SliceTop_down) {
                    m_service->SetCursorWorldPosition(worldPos, 2);
                }
                else if (eve.vizMode == VizMode::SliceFront_back) {
                    m_service->SetCursorWorldPosition(worldPos, 1);
                }
                else if (eve.vizMode == VizMode::SliceLeft_right) {
                    m_service->SetCursorWorldPosition(worldPos, 0);
                }
                m_service->MarkDirty();
            }
            return { true, true };
        }

        // 路径 B：调窗拖拽
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
        // zoom放大
        if (m_enableRightZoom)
        {
            if (!m_renderer || !m_renderer->GetActiveCamera()) {
                return { true, true };
            }

            const int totalDy = eve.y - m_zoomStartY;
            const double factor = std::pow(1.01, totalDy);
            const double nextScale = std::max(1e-6, m_startOriginValue * factor);

            auto* cam = m_renderer->GetActiveCamera();
            cam->SetParallelScale(nextScale);
            m_renderer->ResetCameraClippingRange();

            if (m_renderer->GetRenderWindow()) {
                m_renderer->GetRenderWindow()->Render();
            }

            return { true, true };
        }

        // 路径C：定轴旋转
        if (m_enableDragSlice)
        {
            if (!m_renderer)
                return{true,true};
			auto cursor = m_service->GetCursorWorld();
			// 旋转中心固定取当前十字线位置：先从世界坐标投到屏幕坐标，
			// 后续鼠标轨迹就能在同一显示平面内稳定计算角度增量。
			m_renderer->SetWorldPoint(cursor[0], cursor[1], cursor[2], 1.0);
            m_renderer->WorldToDisplay();
            auto pos = m_renderer->GetDisplayPoint();
			double cx = pos[0];
			double cy = pos[1];

			// 当鼠标太靠近中心点时忽略以免发生抖动跳跃
            double distSq = (eve.x - cx) * (eve.x - cx) + (eve.y - cy) * (eve.y - cy);
            if (distSq < 25)
				return { true, true };

			// 计算旋转增量（单位度）
			double v2x = eve.x - cx;
			double v2y = eve.y - cy;
			double v1x = m_lastRotateX - cx;
			double v1y = m_lastRotateY - cy;
			double cross = v1x * v2y - v1y * v2x; // 叉积（判断旋转方向）v1v2sinθ
			double proj = v1x * v2x + v1y * v2y;   // 点积（计算旋转幅度）v1v2cosθ

            //所有 sin/cos/tan 的输入参数必须是弧度。
			//所有 asin / acos / atan / atan2 的输出结果必须是弧度，atan2 比 atan 更适合计算两向量之间的夹角，因为它考虑了象限信息，可以正确处理所有情况（包括 v1 或 v2 之一为零的情况）。
			double deltaAngleRadians = std::atan2(cross, proj);
            double deltaAngleDeg = vtkMath::DegreesFromRadians(deltaAngleRadians);

			auto cam = m_renderer->GetActiveCamera();
            double focous[3], campos[3];
			cam->GetPosition(campos);
			cam->GetFocalPoint(focous);

            // 是为了提取出正确的旋转轴（Rotation Axis）
			double viewDir[3] = { focous[0] - campos[0], focous[1] - campos[1], focous[2] - campos[2] };
			double RotationAxis[3] = { -viewDir[0], -viewDir[1], -viewDir[2] };
			vtkMath::Normalize(viewDir);
			vtkMath::Normalize(RotationAxis);

            // 构建“绕十字线中心、沿当前视线法向”的增量旋转，
            // 然后把结果矩阵整包回写给服务层，由服务层继续触发 Transform 链路。
			auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
            auto previousModelToWorldMatrix = m_service->GetModelMatrix();
			modelToWorldMatrix->DeepCopy(previousModelToWorldMatrix.data());

			auto transform = vtkSmartPointer<vtkTransform>::New();
			transform->SetMatrix(modelToWorldMatrix);
			transform->PostMultiply();
			transform->Translate(-cursor[0], -cursor[1], -cursor[2]); // 平移到旋转中心
			transform->RotateWXYZ(deltaAngleDeg, RotationAxis); // 旋转
            transform->Translate(cursor[0], cursor[1], cursor[2]); // 平移回原点

			// 将旋转后的矩阵同步回服务，触发模型更新
			m_service->SyncModelMatrix(transform->GetMatrix());
            m_lastRotateX = eve.x;
            m_lastRotateY = eve.y;
			return { true, true };
        }

        return {};
    }

    return {};
}
