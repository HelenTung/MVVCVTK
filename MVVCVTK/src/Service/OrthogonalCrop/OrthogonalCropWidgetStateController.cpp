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
    m_widget = vtkSmartPointer<vtkBoxWidget2>::New();
    m_representation = vtkSmartPointer<vtkBoxRepresentation>::New();
    m_representation->SetPlaceFactor(1.0);
    m_representation->GetOutlineProperty()->SetColor(1.0, 0.15, 0.10);
    m_representation->GetOutlineProperty()->SetLineWidth(2.0);
    m_representation->GetSelectedOutlineProperty()->SetColor(1.0, 0.55, 0.20);
    m_representation->GetSelectedOutlineProperty()->SetLineWidth(2.5);
    m_widget->SetRepresentation(m_representation);

    m_callbackCommand = vtkSmartPointer<OrthogonalCropWidgetStateCallback>::New();
    m_callbackCommand->SetOwner(this);
}

void OrthogonalCropWidgetStateController::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    m_interactor = interactor;
    m_widget->SetInteractor(interactor);
}

void OrthogonalCropWidgetStateController::SetReferenceBounds(const std::array<double, 6>& bounds)
{
    if (!GetBoundsAreValid(bounds)) {
        return;
    }

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
    return planes->GetPoints() && planes->GetNormals();
}

void OrthogonalCropWidgetStateController::SetBoundsChangedCallback(BoundsChangedCallback callback)
{
    m_boundsChangedCallback = std::move(callback);
}

bool OrthogonalCropWidgetStateController::SetEnabled(bool enabled)
{
    if (enabled && !m_interactor) {
        return false;
    }

    EnsureObserversAdded();

    if (enabled) {
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
        m_boundsChangedCallback(m_currentBounds, GetInteractionPhaseFromEvent(eventId));
    }
}
