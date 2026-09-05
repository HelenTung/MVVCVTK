#include "Viewer3DHandler.h"
#include <vtkActor.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

Viewer3DHandler::Viewer3DHandler(
    InteractionStatePort* statePort,
    SliceInputPort* slicePort,
    ModelInputPort* modelPort,
    RenderUpdatePort* updatePort,
    vtkPropPicker* picker,
    vtkRenderer* renderer)
    : m_statePort(statePort)
    , m_slicePort(slicePort)
    , m_modelPort(modelPort)
    , m_updatePort(updatePort)
    , m_picker(picker)
    , m_renderer(renderer)
{
    m_source.ownerId = "Viewer3D";
    m_source.channelId =
        std::to_string(reinterpret_cast<std::uintptr_t>(this));
}

Viewer3DHandler::~Viewer3DHandler()
{
    if (m_statePort) {
        (void)m_statePort->SetInteracting(m_source, false);
    }
}

InteractionResult Viewer3DHandler::Send(const InteractionEvent& eve)
{
    const auto getResult = [](
        const bool isSucceeded,
        const InteractionFailureReason failureReason,
        const bool isPropagationStopped = true) {
        InteractionResult result{ true, isPropagationStopped };
        result.isSucceeded = isSucceeded;
        result.failureReason = isSucceeded
            ? InteractionFailureReason::None : failureReason;
        return result;
    };

    // 模式切换不能吞掉已开始拖拽的 Release/Cancel。
    const bool isCleanup =
        eve.eventKind == InteractionEventKind::PrimaryRelease
        || eve.eventKind == InteractionEventKind::Cancel;
    if (isCleanup && m_isDragging) {
        if (!m_statePort
            || !m_statePort->SetInteracting(m_source, false)) {
            return getResult(
                false, InteractionFailureReason::CleanupRejected);
        }
        if (!m_updatePort || !m_updatePort->SetRenderNeeded()) {
            return getResult(
                false, InteractionFailureReason::RenderRejected);
        }
        m_isDragging = false;
        m_dragAxis = -1;
        return getResult(true, InteractionFailureReason::None);
    }
    if (eve.eventKind == InteractionEventKind::Cancel) {
        return {};
    }

    if (!m_statePort
        || !m_slicePort
        || !m_modelPort
        || !m_updatePort) {
        return {};
    }

    if (eve.toolMode == ToolMode::ModelTransform
        && eve.eventKind == InteractionEventKind::ViewInteraction) {
        vtkProp3D* prop = m_modelPort->GetMainProp();
        if (prop && prop->GetMatrix()) {
            const auto oldModelToWorld =
                m_modelPort->GetModelMatrix();
            std::array<double, 16> modelToWorld{};
            const double* matrixData = prop->GetMatrix()->GetData();
            std::copy(
                matrixData,
                matrixData + modelToWorld.size(),
                modelToWorld.begin());
            if (!m_modelPort->SetModelMatrix(modelToWorld)) {
                return getResult(
                    false,
                    InteractionFailureReason::StateRejected,
                    false);
            }
            if (!m_updatePort->SetRenderNeeded()) {
                (void)m_modelPort->SetModelMatrix(oldModelToWorld);
                return getResult(
                    false,
                    InteractionFailureReason::RenderRejected,
                    false);
            }
            return getResult(
                true, InteractionFailureReason::None, false);
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
            const int axis = m_slicePort->GetPlaneAxis(actor);

            if (axis != -1) {
                m_lastMouseX = eve.x;   // 记录起始点，供 MouseMove 计算增量
                m_lastMouseY = eve.y;
                const bool isStarted =
                    m_statePort->SetInteracting(m_source, true);
                m_isDragging = isStarted;
                m_dragAxis = isStarted ? axis : -1;
                return getResult(
                    isStarted, InteractionFailureReason::StateRejected);
            }
        }
        // 点到主模型或空白处：不消费，让相机交互继续
        return {};
    }

    // ── 左键抬起：结束拖拽 ────────────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::PrimaryRelease)
    {
        if (m_isDragging) {
            const bool isInteractionSet =
                m_statePort->SetInteracting(m_source, false);
            if (!isInteractionSet) {
                return getResult(
                    false, InteractionFailureReason::CleanupRejected);
            }
            const bool isRenderSet = m_updatePort->SetRenderNeeded();
            if (isRenderSet) {
                m_isDragging = false;
                m_dragAxis = -1;
            }
            return getResult(
                isRenderSet, InteractionFailureReason::RenderRejected);
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
        auto lastWorldPos = m_slicePort->GetCursorWorld();
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
        std::array<double, 3> finalWorld = {
            lastWorldPos[0],
            lastWorldPos[1],
            lastWorldPos[2]
        };
        finalWorld[m_dragAxis] += deltaAxis;

        // 全量更新
        if (!m_slicePort->SetCursorWorld(finalWorld, -1)) {
            return getResult(
                false, InteractionFailureReason::StateRejected);
        }
        if (!m_updatePort->SetRenderNeeded()) {
            (void)m_slicePort->SetCursorWorld(lastWorldPos, -1);
            return getResult(
                false, InteractionFailureReason::RenderRejected);
        }
        return getResult(true, InteractionFailureReason::None);
    }

    return {};
}
