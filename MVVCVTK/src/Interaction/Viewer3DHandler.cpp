#include "Viewer3DHandler.h"
#include "Host/FeatureModelTransformPort.h"
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
#include <cmath>

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
    if (m_modelToken) {
        if (auto* port = dynamic_cast<FeatureModelTransformPort*>(m_modelPort))
            (void)port->StopTransform(m_modelToken);
    }
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

    if (m_isModelDrag || (!m_isDragging && eve.toolMode == ToolMode::ModelTransform))
        return SetModelDrag(eve);

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
            const auto* point=m_picker->GetPickPosition();
            if(!point||!m_modelPort->GetPointVisible({point[0],point[1],point[2]}))return {};
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

InteractionResult Viewer3DHandler::SetModelDrag(const InteractionEvent& event)
{
    // 空闲时的广播清理无需编辑能力；例如无数据视图的 StopInput。
    if (!m_isModelDrag
        && event.eventKind != InteractionEventKind::PrimaryPress
        && event.eventKind != InteractionEventKind::SecondaryPress) return {};
    const auto result = [](bool succeeded) {
        return InteractionResult{true,true,succeeded, succeeded
            ? InteractionFailureReason::None : InteractionFailureReason::StateRejected};
    };
    if (!m_modelPort || !m_statePort || !m_updatePort || !m_renderer) return result(false);
    auto* port = dynamic_cast<FeatureModelTransformPort*>(m_modelPort);
    const auto state = port ? port->GetTransformState() : std::nullopt;
    if (!state) return result(false);
    if (m_isModelDrag && state->editToken != m_modelToken) {
        // 换数据会原子退休旧编辑；迟到鼠标事件只清理自身来源。
        if (!m_statePort->SetInteracting(m_source,false)) return result(false);
        m_isModelDrag = false;
        m_modelToken = 0;
        return result(true);
    }
    const bool isCancel = event.eventKind == InteractionEventKind::Cancel
        || event.eventKind == InteractionEventKind::Exit
        || event.toolMode != ToolMode::ModelTransform;
    if (m_isModelDrag && isCancel) {
        if (!port->StopTransform(m_modelToken)
            || !m_statePort->SetInteracting(m_source,false)) return result(false);
        m_isModelDrag = false;
        m_modelToken = 0;
        return result(m_updatePort->SetRenderNeeded());
    }
    const bool isPrimary = event.eventKind == InteractionEventKind::PrimaryPress;
    const bool isSecondary = event.eventKind == InteractionEventKind::SecondaryPress;
    if (!m_isModelDrag && (isPrimary || isSecondary)) {
        // 旋转已由上游窄输入能力消费；本层只保留 Shift 平移和 Ctrl+Shift 缩放。
        if (isPrimary && !event.isShiftDown) return result(false);
        auto* prop = m_modelPort->GetMainProp();
        if (!prop || !m_renderer->GetRenderWindow()) return result(false);
        std::copy_n(prop->GetCenter(), 3, m_modelCenter.begin());
        m_renderer->SetWorldPoint(m_modelCenter[0],m_modelCenter[1],m_modelCenter[2],1);
        m_renderer->WorldToDisplay();
        m_modelDepth = m_renderer->GetDisplayPoint()[2];
        m_renderer->SetDisplayPoint(event.x,event.y,m_modelDepth);
        m_renderer->DisplayToWorld();
        const auto* point = m_renderer->GetWorldPoint();
        if (!std::isfinite(point[3]) || std::abs(point[3]) < 1e-12) return result(false);
        for (int axis=0; axis<3; ++axis) m_panStart[axis] = point[axis]/point[3];
        const auto token = port->StartTransform(*state);
        if (!token) return result(false);
        if (!m_statePort->SetInteracting(m_source,true)) {
            (void)port->StopTransform(*token);
            return result(false);
        }
        m_modelToken = *token;
        m_modelSequence = 0;
        m_modelStart = state->modelToWorld;
        m_isModelScale = isPrimary && event.isCtrlDown;
        m_isModelSecondary = isSecondary;
        m_modelStartY = event.y;
        m_isModelDrag = true;
        return result(true);
    }
    const bool isRelease = event.eventKind == (m_isModelSecondary
        ? InteractionEventKind::SecondaryRelease : InteractionEventKind::PrimaryRelease);
    if (m_isModelDrag && (event.eventKind == InteractionEventKind::PointerMove || isRelease)) {
        auto matrix = m_modelStart;
        if (m_isModelScale) {
            const double factor = std::exp(std::clamp(
                (static_cast<double>(event.y)-m_modelStartY)*0.01,-20.0,20.0));
            for (int row=0; row<3; ++row) {
                for (int column=0; column<3; ++column) matrix[row*4+column] *= factor;
                matrix[row*4+3] = m_modelCenter[row]
                    + factor*(m_modelStart[row*4+3]-m_modelCenter[row]);
            }
        }
        else {
            m_renderer->SetDisplayPoint(event.x,event.y,m_modelDepth);
            m_renderer->DisplayToWorld();
            const auto* point = m_renderer->GetWorldPoint();
            if (!std::isfinite(point[3]) || std::abs(point[3]) < 1e-12) return result(false);
            for (int axis=0; axis<3; ++axis)
                matrix[axis*4+3] += point[axis]/point[3] - m_panStart[axis];
        }
        const bool isSet = isRelease
            ? port->SetTransformCommit(m_modelToken,m_modelSequence+1,matrix)
            : port->SetTransformPreview(m_modelToken,m_modelSequence+1,matrix);
        if (!isSet) return result(false);
        ++m_modelSequence;
        if (isRelease) {
            if (!m_statePort->SetInteracting(m_source,false)) return result(false);
            m_isModelDrag = false;
            m_modelToken = 0;
        }
        return result(m_updatePort->SetRenderNeeded());
    }
    return m_isModelDrag ? result(true) : InteractionResult{};
}
