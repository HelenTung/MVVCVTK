#include "Viewer2DHandler.h"
#include <vtkMath.h>
#include <vtkPropPicker.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkCamera.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>

Viewer2DHandler::Viewer2DHandler(
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
    m_source.ownerId = "Viewer2D";
    m_source.channelId =
        std::to_string(reinterpret_cast<std::uintptr_t>(this));
}

Viewer2DHandler::~Viewer2DHandler()
{
    if (m_statePort) {
        (void)m_statePort->SetInteracting(m_source, false);
    }
}

InteractionResult Viewer2DHandler::Send(const InteractionEvent& eve)
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

    // 模式可在按下与释放之间切换；Release/Cancel 必须先于模式门控清理 source。
    const bool isPrimaryActive = m_isDragCrosshair || m_isDragWindowLevel;
    const bool isPrimaryCleanup =
        eve.eventKind == InteractionEventKind::PrimaryRelease
        || eve.eventKind == InteractionEventKind::Cancel;
    if (isPrimaryCleanup && isPrimaryActive) {
        if (!m_statePort
            || !m_statePort->SetInteracting(m_source, false)) {
            return getResult(
                false, InteractionFailureReason::CleanupRejected);
        }
        m_isDragCrosshair = false;
        m_isDragWindowLevel = false;
        return getResult(true, InteractionFailureReason::None);
    }
    const bool isSecondaryCleanup =
        eve.eventKind == InteractionEventKind::SecondaryRelease
        || eve.eventKind == InteractionEventKind::Cancel;
    if (isSecondaryCleanup && m_isRightZoom) {
        if (!m_statePort
            || !m_statePort->SetInteracting(m_source, false)) {
            return getResult(
                false, InteractionFailureReason::CleanupRejected);
        }
        m_isRightZoom = false;
        return getResult(true, InteractionFailureReason::None);
    }
    if (eve.eventKind == InteractionEventKind::Cancel) {
        return {};
    }

    const bool isSliceMode =
        eve.vizMode == VizMode::SliceTop_down
        || eve.vizMode == VizMode::SliceFront_back
        || eve.vizMode == VizMode::SliceLeft_right;
    if (!m_statePort
        || !m_slicePort
        || !m_modelPort
        || !m_updatePort
        || !isSliceMode) {
        return {};
    }

    // 2D 模式下同一时刻只允许一种主交互语义成立：
    // 旋转由独立 Feature 接管；默认处理滚轮、十字线、调窗和相机缩放。

    // ── 滚轮切片 ──────────────────────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::WheelForward
        || eve.eventKind == InteractionEventKind::WheelBackward)
    {
        const int step = eve.isCtrlDown ? 10 : 5;
        const int delta = (eve.eventKind == InteractionEventKind::WheelForward)
            ? step : -step;
        return getResult(
            m_slicePort->SetSliceScroll(delta),
            InteractionFailureReason::StateRejected);
    }

    // ── 左键按下：isShiftDown → 开始拖拽十字线 ────────────────────────────
    if (eve.eventKind == InteractionEventKind::PrimaryPress)
    {
        if (eve.isCtrlDown)
        {
            // 未装配旋转 Feature 时也不允许底层 style 修改模型。
            return getResult(false, InteractionFailureReason::StateRejected);
        }
        if (eve.isShiftDown) {
            const bool isStarted =
                m_statePort->SetInteracting(m_source, true);
            m_isDragCrosshair = isStarted;
            return getResult(
                isStarted, InteractionFailureReason::StateRejected);
        }

        m_lastDragX = eve.x;
        m_lastDragY = eve.y;
        m_startDragX = eve.x;
        m_startDragY = eve.y;

        const auto wl = m_slicePort->GetWindowLevel();
        m_startWW = wl.windowWidth;
        m_startWC = wl.windowCenter;

        const bool isStarted =
            m_statePort->SetInteracting(m_source, true);
        m_isDragWindowLevel = isStarted;
        return getResult(
            isStarted, InteractionFailureReason::StateRejected);
    }

    // ── 左键抬起：结束交互 ─────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::PrimaryRelease)
    {
        if (m_isDragCrosshair) {
            const bool isStopped =
                m_statePort->SetInteracting(m_source, false);
            if (isStopped) m_isDragCrosshair = false;
            return getResult(
                isStopped, InteractionFailureReason::CleanupRejected);
        }
        if (m_isDragWindowLevel) {
            const bool isStopped =
                m_statePort->SetInteracting(m_source, false);
            if (isStopped) m_isDragWindowLevel = false;
            return getResult(
                isStopped, InteractionFailureReason::CleanupRejected);
        }
        return { true, true };
    }

    // ── 右键按下：开始缩放 ─────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::SecondaryPress)
    {
        m_zoomStartY = eve.y;

        if (m_renderer && m_renderer->GetActiveCamera()) {
            m_startOriginValue = m_renderer->GetActiveCamera()->GetParallelScale();
        }
        else {
            m_startOriginValue = 1.0;
        }

        const bool isStarted =
            m_statePort->SetInteracting(m_source, true);
        m_isRightZoom = isStarted;
        return getResult(
            isStarted, InteractionFailureReason::StateRejected);
    }

    // ── 右键抬起：结束缩放 ─────────────────────────────────────────
    if (eve.eventKind == InteractionEventKind::SecondaryRelease)
    {
        if (m_isRightZoom) {
            const bool isStopped =
                m_statePort->SetInteracting(m_source, false);
            if (isStopped) m_isRightZoom = false;
            return getResult(
                isStopped, InteractionFailureReason::CleanupRejected);
        }
        return {};
    }

    // ── 鼠标移动：十字线拖拽 / 调窗拖拽 / 缩放 / 定轴旋转 ─────────────
    if (eve.eventKind == InteractionEventKind::PointerMove)
    {
        // 路径 A：十字线拖拽
        if (m_isDragCrosshair)
        {
            if (!m_picker || !m_renderer
                || !m_picker->Pick(eve.x, eve.y, 0, m_renderer)) {
                return getResult(
                    false, InteractionFailureReason::StateRejected);
            }
            double* worldPos = m_picker->GetPickPosition();

            if (worldPos) {
                if(!m_modelPort->GetPointVisible({worldPos[0],worldPos[1],worldPos[2]}))
                    return getResult(false,InteractionFailureReason::StateRejected);
                // 这里直接写 CursorWorldPosition，让服务层统一完成轴约束、状态广播和后续渲染刷新。
                bool isCursorSet = false;
                if (eve.vizMode == VizMode::SliceTop_down) {
                    isCursorSet = m_slicePort->SetCursorWorld(
                        { worldPos[0], worldPos[1], worldPos[2] }, 2);
                }
                else if (eve.vizMode == VizMode::SliceFront_back) {
                    isCursorSet = m_slicePort->SetCursorWorld(
                        { worldPos[0], worldPos[1], worldPos[2] }, 1);
                }
                else if (eve.vizMode == VizMode::SliceLeft_right) {
                    isCursorSet = m_slicePort->SetCursorWorld(
                        { worldPos[0], worldPos[1], worldPos[2] }, 0);
                }
                if (!isCursorSet) {
                    return getResult(
                        false, InteractionFailureReason::StateRejected);
                }
                return getResult(
                    m_updatePort->SetRenderNeeded(),
                    InteractionFailureReason::RenderRejected);
            }
            return getResult(
                false, InteractionFailureReason::StateRejected);
        }

        // 路径 B：调窗拖拽
        //   水平方向（ΔX > 0，向右拖）增大 WW，使窗内灰度映射斜率降低。
        //   垂直方向（ΔY > 0，向上拖）从起始 WC 减去正增量，使窗位降低。
        if (m_isDragWindowLevel)
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

            return getResult(
                m_slicePort->SetWindowLevelDrag(
                    totalDx,
                    totalDy,
                    viewWidth,
                    viewHeight,
                    m_startWW,
                    m_startWC),
                InteractionFailureReason::StateRejected);
        }
        // 正 totalDy 增大 parallelScale，视图缩小；负 totalDy 减小 scale，视图放大。
        if (m_isRightZoom)
        {
            if (!m_renderer || !m_renderer->GetActiveCamera()) {
                return { true, true };
            }

            const int totalDy = eve.y - m_zoomStartY;
            const double factor = std::pow(1.01, totalDy);
            const double nextScale = std::max(1e-6, m_startOriginValue * factor);

            auto* cam = m_renderer->GetActiveCamera();
            const double oldScale = cam->GetParallelScale();
            cam->SetParallelScale(nextScale);
            m_renderer->ResetCameraClippingRange();

            // 连续 MouseMove 只更新最终相机状态；可见帧由统一 Timer 消费 dirty，
            // 避免每个输入事件绕过 33 ms 合并窗口直接渲染。
            if (!m_updatePort->SetRenderNeeded()) {
                cam->SetParallelScale(oldScale);
                m_renderer->ResetCameraClippingRange();
                return getResult(
                    false, InteractionFailureReason::RenderRejected);
            }
            return getResult(
                true, InteractionFailureReason::None);
        }

        return {};
    }

    return {};
}
