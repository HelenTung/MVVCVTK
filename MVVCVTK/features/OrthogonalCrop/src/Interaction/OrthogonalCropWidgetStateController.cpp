// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Interaction/OrthogonalCropWidgetStateController.cpp
// 分类: Service / Widget Controller Implementation
// 说明: 封装 vtkBoxWidget2 的构造、启停、observer 绑定与 bounds 回调翻译。
// =====================================================================
// 这个控制器刻意保持“薄”：
// - 下层只接触 VTK widget 细节
// - 上层只拿到 bounds 与 phase
// - 中间不掺入任何 preview 或算法逻辑，便于保持交互链路职责清晰

#include "Interaction/OrthogonalCropWidgetStateController.h"
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <utility>

OrthogonalCropWidgetStateCallback* OrthogonalCropWidgetStateCallback::New()
{
    return new OrthogonalCropWidgetStateCallback();
}

void OrthogonalCropWidgetStateCallback::SetOwner(OrthogonalCropWidgetStateController* owner)
{
    m_owner = owner;
}

void OrthogonalCropWidgetStateCallback::Execute(vtkObject* caller, unsigned long eventId, void* callData)
{
    (void)caller;
    (void)callData;
    if (m_owner) {
        m_owner->HandleWidgetEvent(eventId);
    }
}

OrthogonalCropWidgetStateController::OrthogonalCropWidgetStateController()
{
    // 初始化 widget 和 representation；
    // 普通态使用中性色，是因为裁切盒只是空间手柄；只有被 VTK 选中/拖拽时才用高亮色提示交互焦点。
    m_widget = vtkSmartPointer<vtkBoxWidget2>::New();
    m_representation = vtkSmartPointer<vtkBoxRepresentation>::New();
    m_representation->SetPlaceFactor(1.0);
    m_representation->GetOutlineProperty()->SetColor(0.82, 0.86, 0.90);
    m_representation->GetOutlineProperty()->SetLineWidth(2.0);
    m_representation->GetSelectedOutlineProperty()->SetColor(1.0, 0.55, 0.20);
    m_representation->GetSelectedOutlineProperty()->SetLineWidth(2.5);
    m_widget->SetRepresentation(m_representation);

    // VTK callback 只做事件转发；
    // 交互相位和 preview 触发策略统一留给控制器与 bridge。
    m_callbackCommand = vtkSmartPointer<OrthogonalCropWidgetStateCallback>::New();
    m_callbackCommand->SetOwner(this);
}

void OrthogonalCropWidgetStateController::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    // widget 与 interactor 的绑定始终在这里完成，避免上层直接碰 vtkBoxWidget2。
    m_interactor = interactor;
    m_widget->SetInteractor(interactor);
}

void OrthogonalCropWidgetStateController::SetReferenceWorldBounds(const std::array<double, 6>& worldBounds)
{
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    // referenceWorldBounds 是 Enable 时的回退基准，同时也是第一次 PlaceWidget 的边界来源。
    m_referenceWorldBounds = worldBounds;
    if (!GetBoundsAreValid(m_currentWorldBounds)) {
        m_currentWorldBounds = worldBounds;
    }
    m_widgetInitialWorldBounds = m_currentWorldBounds;
    m_representation->PlaceWidget(m_currentWorldBounds.data());
}

void OrthogonalCropWidgetStateController::SetWidgetWorldBounds(const std::array<double, 6>& worldBounds)
{
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    m_currentWorldBounds = worldBounds;
    m_widgetInitialWorldBounds = worldBounds;
    m_representation->PlaceWidget(m_currentWorldBounds.data());
}

const std::array<double, 6>& OrthogonalCropWidgetStateController::GetCurrentWorldBounds() const
{
    return m_currentWorldBounds;
}

bool OrthogonalCropWidgetStateController::GetCurrentWorldBox(
    CropVectorDouble3Array& initialWorldCenter,
    CropVectorDouble3Array& initialWorldDimensions,
    CropMatrixDouble16Array& initialWorldToCurrentWorldMatrix) const
{
    if (!m_representation || !GetBoundsAreValid(m_widgetInitialWorldBounds)) {
        return false;
    }

    // 先把最近一次 PlaceWidget 的 world AABB 拆成中心和尺寸；
    // 这两个变量描述“未交互前的基准盒”，后续用它把标准盒放回同一个 world 体积。
    initialWorldCenter = {
        (m_widgetInitialWorldBounds[0] + m_widgetInitialWorldBounds[1]) * 0.5,
        (m_widgetInitialWorldBounds[2] + m_widgetInitialWorldBounds[3]) * 0.5,
        (m_widgetInitialWorldBounds[4] + m_widgetInitialWorldBounds[5]) * 0.5
    };
    initialWorldDimensions = {
        m_widgetInitialWorldBounds[1] - m_widgetInitialWorldBounds[0],
        m_widgetInitialWorldBounds[3] - m_widgetInitialWorldBounds[2],
        m_widgetInitialWorldBounds[5] - m_widgetInitialWorldBounds[4]
    };

    // 再从当前 widget polydata 读取交互后的 world 角点；
    // polydata 保留旋转姿态，GetBounds 只给外接 AABB，不能表达真实有向盒。
    auto boxPolyData = vtkSmartPointer<vtkPolyData>::New();
    m_representation->GetPolyData(boxPolyData);
    if (!boxPolyData->GetPoints() || boxPolyData->GetNumberOfPoints() < 8) {
        return false;
    }

    double currentP0[3] = { 0.0, 0.0, 0.0 };
    double currentP1[3] = { 0.0, 0.0, 0.0 };
    double currentP3[3] = { 0.0, 0.0, 0.0 };
    double currentP4[3] = { 0.0, 0.0, 0.0 };
    boxPolyData->GetPoint(0, currentP0);
    boxPolyData->GetPoint(1, currentP1);
    boxPolyData->GetPoint(3, currentP3);
    boxPolyData->GetPoint(4, currentP4);

    // 用 P0/P1/P3/P4 反解 initialWorld -> currentWorld affine：
    // currentP0 是基准盒 minX/minY/minZ 角点交互后的 world 位置，
    // currentP1/currentP3/currentP4 分别提供基准盒 X/Y/Z 三条边的当前 world 方向和长度，
    // 因此矩阵能表达用户拖拽后的旋转、缩放和平移，而不是只表达外接框大小。
    auto initialWorldToCurrentWorldVtkMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    initialWorldToCurrentWorldVtkMatrix->Identity();
    for (int row = 0; row < 3; ++row) {
        // 每条当前 world 边除以基准盒原始 world 尺寸，得到 initialWorld 单位长度在 currentWorld 中的步长。
        const double initialWorldToCurrentWorldX =
            (currentP1[row] - currentP0[row]) / initialWorldDimensions[0];
        const double initialWorldToCurrentWorldY =
            (currentP3[row] - currentP0[row]) / initialWorldDimensions[1];
        const double initialWorldToCurrentWorldZ =
            (currentP4[row] - currentP0[row]) / initialWorldDimensions[2];

        // 反解 offset，让基准盒 min 角点精确映射到 currentP0；
        // 这样矩阵同时包含原始 min 坐标补偿，不会把基准盒错误地当成从原点开始。
        const double initialWorldToCurrentWorldOffset =
            currentP0[row]
            - initialWorldToCurrentWorldX * m_widgetInitialWorldBounds[0]
            - initialWorldToCurrentWorldY * m_widgetInitialWorldBounds[2]
            - initialWorldToCurrentWorldZ * m_widgetInitialWorldBounds[4];

        initialWorldToCurrentWorldVtkMatrix->SetElement(row, 0, initialWorldToCurrentWorldX);
        initialWorldToCurrentWorldVtkMatrix->SetElement(row, 1, initialWorldToCurrentWorldY);
        initialWorldToCurrentWorldVtkMatrix->SetElement(row, 2, initialWorldToCurrentWorldZ);
        initialWorldToCurrentWorldVtkMatrix->SetElement(row, 3, initialWorldToCurrentWorldOffset);
    }

    vtkMatrix4x4::DeepCopy(initialWorldToCurrentWorldMatrix.data(), initialWorldToCurrentWorldVtkMatrix);
    return true;
}

void OrthogonalCropWidgetStateController::SetWorldBoundsChangedCallback(WorldBoundsChangedCallback callback)
{
    m_worldBoundsChangedCallback = std::move(callback);
}

bool OrthogonalCropWidgetStateController::SetEnabled(bool enabled)
{
    // 启用 widget 必须先绑定 interactor；
    // 没有窗口事件源时直接返回失败，避免创建半启用状态。
    if (enabled && !m_interactor) {
        return false;
    }

    // observer 懒绑定且只绑定一次；
    // 重复进入裁切模式时不应产生多重 VTK 回调。
    EnsureObserversAdded();

    if (enabled) {
        // 启用前确保有合法 world bounds；
        // 当前盒无效时回退到 reference world bounds，保证 PlaceWidget 有稳定输入。
        if (!GetBoundsAreValid(m_currentWorldBounds)) {
            m_currentWorldBounds = m_referenceWorldBounds;
        }

        if (!GetBoundsAreValid(m_currentWorldBounds)) {
            return false;
        }

        m_widgetInitialWorldBounds = m_currentWorldBounds;
        m_representation->PlaceWidget(m_currentWorldBounds.data());
        m_widget->On();
    }
    else {
        m_widget->Off();
    }

    m_enabled = enabled;
    return true;
}

bool OrthogonalCropWidgetStateController::GetEnabled() const
{
    return m_enabled;
}

bool OrthogonalCropWidgetStateController::GetBoundsAreValid(const std::array<double, 6>& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

CropInteractionPhase OrthogonalCropWidgetStateController::GetInteractionPhaseFromEvent(unsigned long eventId)
{
    // Start/Interaction 一律视为拖拽中，只有 End 才切换到 Released。
    switch (eventId) {
    case vtkCommand::StartInteractionEvent:
        return CropInteractionPhase::Dragging;
    case vtkCommand::InteractionEvent:
        return CropInteractionPhase::Dragging;
    case vtkCommand::EndInteractionEvent:
        return CropInteractionPhase::Released;
    default:
        return CropInteractionPhase::Idle;
    }
}

void OrthogonalCropWidgetStateController::EnsureObserversAdded()
{
    if (m_observersAdded) {
        return;
    }

    // observer 只绑定一次，避免重复进入模式时收到多重回调。
    m_widget->AddObserver(vtkCommand::StartInteractionEvent, m_callbackCommand);
    m_widget->AddObserver(vtkCommand::InteractionEvent, m_callbackCommand);
    m_widget->AddObserver(vtkCommand::EndInteractionEvent, m_callbackCommand);
    m_observersAdded = true;
}

void OrthogonalCropWidgetStateController::HandleWidgetEvent(unsigned long eventId)
{
    const auto rawBounds = m_representation->GetBounds();
    if (!rawBounds) {
        return;
    }

    // GetBounds 只返回当前 widget 的 world AABB；
    // 完整旋转姿态由 GetPolyData 重建 initialWorldToCurrentWorld，避免把外接框误当作有向盒。
    const std::array<double, 6> worldBounds = {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    m_currentWorldBounds = worldBounds;
    if (m_worldBoundsChangedCallback) {
        // 这里不做任何 preview / 算法调用，只把纯状态变化上抛给交互桥。
        // 因此 dragging 与 released 的不同处理策略，完全由桥接层决定。
        m_worldBoundsChangedCallback(m_currentWorldBounds, GetInteractionPhaseFromEvent(eventId));
    }
}
