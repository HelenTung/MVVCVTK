// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/src/Service/PlanarCropWidgetStateController.cpp
// 分类: Service / Widget Controller Implementation
// 说明: 封装 vtkImplicitPlaneWidget2 的构造、启停、observer 绑定与平面回调翻译。
// =====================================================================

#include "PlanarCropWidgetStateController.h"

#include <vtkMath.h>
#include <vtkProperty.h>

#include <utility>

static constexpr double PlaneWidgetNormalEpsilon = 1e-8;

PlanarCropWidgetStateCallback* PlanarCropWidgetStateCallback::New()
{
    return new PlanarCropWidgetStateCallback();
}

void PlanarCropWidgetStateCallback::SetOwner(PlanarCropWidgetStateController* owner)
{
    m_owner = owner;
}

void PlanarCropWidgetStateCallback::Execute(vtkObject* caller, unsigned long eventId, void* callData)
{
    (void)caller;
    (void)callData;
    if (m_owner) {
        m_owner->HandleWidgetEvent(eventId);
    }
}

PlanarCropWidgetStateController::PlanarCropWidgetStateController()
{
    // 平面 widget 外观固定在控制器内，上层 bridge 只消费 world 中心点/法线。
    m_widget = vtkSmartPointer<vtkImplicitPlaneWidget2>::New();
    m_representation = vtkSmartPointer<vtkImplicitPlaneRepresentation>::New();
    m_representation->SetPlaceFactor(1.0);
    m_representation->DrawPlaneOn();
    m_representation->SetCropPlaneToBoundingBox(false);
    m_representation->DrawOutlineOff();
    m_representation->OutlineTranslationOff();
    m_representation->ScaleEnabledOff();
    m_representation->TubingOff();
    m_representation->GetPlaneProperty()->SetColor(0.1, 0.85, 0.5);
    m_representation->GetPlaneProperty()->SetOpacity(0.28);
    m_representation->GetOutlineProperty()->SetColor(0.1, 0.85, 0.5);
    m_representation->GetOutlineProperty()->SetLineWidth(2.0);
    m_representation->GetSelectedPlaneProperty()->SetColor(1.0, 0.55, 0.12);
    m_representation->GetSelectedPlaneProperty()->SetOpacity(0.36);
    m_widget->SetRepresentation(m_representation);

    m_callbackCommand = vtkSmartPointer<PlanarCropWidgetStateCallback>::New();
    m_callbackCommand->SetOwner(this);
}

void PlanarCropWidgetStateController::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    m_interactor = interactor;
    m_widget->SetInteractor(interactor);
}

void PlanarCropWidgetStateController::SetReferenceWorldBounds(const std::array<double, 6>& worldBounds)
{
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    m_referenceWorldBounds = worldBounds;
    m_currentWorldOrigin = {
        (worldBounds[0] + worldBounds[1]) * 0.5,
        (worldBounds[2] + worldBounds[3]) * 0.5,
        (worldBounds[4] + worldBounds[5]) * 0.5
    };
    ApplyCurrentPlaneToRepresentation();
}

void PlanarCropWidgetStateController::SetWidgetWorldPlane(
    const CropVectorDouble3Array& worldOrigin,
    const CropVectorDouble3Array& worldNormal)
{
    auto normalizedNormal = worldNormal;
    if (!NormalizePlaneNormal(normalizedNormal)) {
        return;
    }

    m_currentWorldOrigin = worldOrigin;
    m_currentWorldNormal = normalizedNormal;
    ApplyCurrentPlaneToRepresentation();
}

bool PlanarCropWidgetStateController::GetCurrentWorldPlane(
    CropVectorDouble3Array& worldOrigin,
    CropVectorDouble3Array& worldNormal) const
{
    worldOrigin = m_currentWorldOrigin;
    worldNormal = m_currentWorldNormal;
    return vtkMath::Norm(worldNormal.data()) > PlaneWidgetNormalEpsilon;
}

void PlanarCropWidgetStateController::SetWorldPlaneChangedCallback(WorldPlaneChangedCallback callback)
{
    m_worldPlaneChangedCallback = std::move(callback);
}

bool PlanarCropWidgetStateController::SetEnabled(bool enabled)
{
    if (enabled && !m_interactor) {
        return false;
    }

    EnsureObserversAdded();

    if (enabled) {
        if (!GetBoundsAreValid(m_referenceWorldBounds)) {
            return false;
        }

        ApplyCurrentPlaneToRepresentation();
        m_widget->On();
    }
    else {
        m_widget->Off();
    }

    m_enabled = enabled;
    return true;
}

bool PlanarCropWidgetStateController::GetEnabled() const
{
    return m_enabled;
}

bool PlanarCropWidgetStateController::GetBoundsAreValid(const std::array<double, 6>& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

bool PlanarCropWidgetStateController::NormalizePlaneNormal(CropVectorDouble3Array& worldNormal)
{
    const double length = vtkMath::Norm(worldNormal.data());
    if (length <= PlaneWidgetNormalEpsilon) {
        return false;
    }

    for (double& component : worldNormal) {
        component /= length;
    }
    return true;
}

CropInteractionPhase PlanarCropWidgetStateController::GetInteractionPhaseFromEvent(unsigned long eventId)
{
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

void PlanarCropWidgetStateController::EnsureObserversAdded()
{
    if (m_observersAdded) {
        return;
    }

    m_widget->AddObserver(vtkCommand::StartInteractionEvent, m_callbackCommand);
    m_widget->AddObserver(vtkCommand::InteractionEvent, m_callbackCommand);
    m_widget->AddObserver(vtkCommand::EndInteractionEvent, m_callbackCommand);
    m_observersAdded = true;
}

void PlanarCropWidgetStateController::ApplyCurrentPlaneToRepresentation()
{
    if (!m_representation || !GetBoundsAreValid(m_referenceWorldBounds)) {
        return;
    }

    m_representation->PlaceWidget(m_referenceWorldBounds.data());
    m_representation->SetOrigin(m_currentWorldOrigin.data());
    m_representation->SetNormal(m_currentWorldNormal.data());
}

void PlanarCropWidgetStateController::HandleWidgetEvent(unsigned long eventId)
{
    if (!m_representation) {
        return;
    }

    double rawOrigin[3] = { 0.0, 0.0, 0.0 };
    double rawNormal[3] = { 0.0, 0.0, 1.0 };
    m_representation->GetOrigin(rawOrigin);
    m_representation->GetNormal(rawNormal);

    CropVectorDouble3Array worldOrigin = { rawOrigin[0], rawOrigin[1], rawOrigin[2] };
    CropVectorDouble3Array worldNormal = { rawNormal[0], rawNormal[1], rawNormal[2] };
    if (!NormalizePlaneNormal(worldNormal)) {
        return;
    }

    m_currentWorldOrigin = worldOrigin;
    m_currentWorldNormal = worldNormal;
    if (m_worldPlaneChangedCallback) {
        m_worldPlaneChangedCallback(
            m_currentWorldOrigin,
            m_currentWorldNormal,
            GetInteractionPhaseFromEvent(eventId));
    }
}
