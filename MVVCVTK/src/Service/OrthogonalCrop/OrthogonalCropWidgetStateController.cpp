// =====================================================================
// Path: MVVCVTK/src/Service/OrthogonalCrop/OrthogonalCropWidgetStateController.cpp
// 分类: Service / Widget Controller Implementation
// 说明: 封装 vtkBoxWidget2 的构造、启停、observer 绑定与 bounds 回调翻译。
// =====================================================================
// 这个控制器刻意保持“薄”：
// - 下层只接触 VTK widget 细节
// - 上层只拿到 bounds 与 phase
// - 中间不掺入任何 preview 或算法逻辑，便于保持交互链路职责清晰

#include "OrthogonalCrop/OrthogonalCropWidgetStateController.h"
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
    // ═══ 初始化 vtkBoxWidget2 外观 ═══
    m_widget = vtkSmartPointer<vtkBoxWidget2>::New();
    m_representation = vtkSmartPointer<vtkBoxRepresentation>::New();
    m_representation->SetPlaceFactor(1.0);
    // 默认轮廓：暗红色 2.0px 线宽
    m_representation->GetOutlineProperty()->SetColor(1.0, 0.15, 0.10);
    m_representation->GetOutlineProperty()->SetLineWidth(2.0);
    // 选中轮廓：亮橙色 2.5px 线宽
    m_representation->GetSelectedOutlineProperty()->SetColor(1.0, 0.55, 0.20);
    m_representation->GetSelectedOutlineProperty()->SetLineWidth(2.5);
    m_widget->SetRepresentation(m_representation);

    // VTK 回调适配器：把原始 VTK 事件转交到 HandleWidgetEvent
    m_callbackCommand = vtkSmartPointer<OrthogonalCropWidgetStateCallback>::New();
    m_callbackCommand->SetOwner(this);
}

void OrthogonalCropWidgetStateController::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    // widget 与 interactor 的绑定始终在这里完成，避免上层直接碰 vtkBoxWidget2。
    m_interactor = interactor;
    m_widget->SetInteractor(interactor);
}

void OrthogonalCropWidgetStateController::SetReferenceBounds(const std::array<double, 6>& bounds)
{
    if (!GetBoundsAreValid(bounds)) {
        return;
    }

    // referenceBounds 是 Enable 时的回退基准，同时也是第一次 PlaceWidget 的边界来源。
    m_referenceBounds = bounds;
    if (!GetBoundsAreValid(m_currentBounds)) {
        m_currentBounds = bounds;
    }
    m_representation->PlaceWidget(m_currentBounds.data());
}

void OrthogonalCropWidgetStateController::SetWidgetBounds(const std::array<double, 6>& bounds)
{
    if (!GetBoundsAreValid(bounds)) {
        return;
    }

    m_currentBounds = bounds;
    m_representation->PlaceWidget(m_currentBounds.data());
}

const std::array<double, 6>& OrthogonalCropWidgetStateController::GetCurrentBounds() const
{
    return m_currentBounds;
}

bool OrthogonalCropWidgetStateController::GetPlanes(vtkPlanes* planes) const
{
    if (!planes || !m_representation) {
        return false;
    }

    m_representation->GetPlanes(planes);

    // 这里返回的是 widget 当前几何状态的即时快照，
    // 供交互桥在需要 3D 主模型临时 clip 时读取，不持久缓存到控制器外部。
    return planes->GetPoints() && planes->GetNormals();
}

void OrthogonalCropWidgetStateController::SetBoundsChangedCallback(BoundsChangedCallback callback)
{
    m_boundsChangedCallback = std::move(callback);
}

bool OrthogonalCropWidgetStateController::SetEnabled(bool enabled)
{
    // 启用前提：必须已绑定 interactor
    if (enabled && !m_interactor) {
        return false;
    }

    // ═══ 懒绑定 observer（整个生命周期只做一次，避免重复回调）═══
    EnsureObserversAdded();

    if (enabled) {
        // 启用：先确保有合法 bounds，再 Place + On
        if (!GetBoundsAreValid(m_currentBounds)) {
            m_currentBounds = m_referenceBounds;
        }

        if (!GetBoundsAreValid(m_currentBounds)) {
            return false;
        }

        m_representation->PlaceWidget(m_currentBounds.data());
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

    const std::array<double, 6> bounds = {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
    if (!GetBoundsAreValid(bounds)) {
        return;
    }

    m_currentBounds = bounds;
    if (m_boundsChangedCallback) {
        // 这里不做任何 preview / 算法调用，只把纯状态变化上抛给交互桥。
        // 因此 dragging 与 released 的不同处理策略，完全由桥接层决定。
        m_boundsChangedCallback(m_currentBounds, GetInteractionPhaseFromEvent(eventId));
    }
}
