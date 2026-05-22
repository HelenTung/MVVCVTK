#pragma once

#include "OrthogonalCrop/OrthogonalCropTypes.h"

#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCommand.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>
#include <utility>

class OrthogonalCropWidgetStateController;

class OrthogonalCropWidgetStateCallback : public vtkCommand {
public:
    static OrthogonalCropWidgetStateCallback* New() { return new OrthogonalCropWidgetStateCallback(); }

    void SetOwner(OrthogonalCropWidgetStateController* owner)
    {
        m_owner = owner;
    }

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override;

private:
    OrthogonalCropWidgetStateController* m_owner = nullptr;
};

class OrthogonalCropWidgetStateController {
public:
    using BoundsChangedCallback = std::function<void(const std::array<double, 6>& bounds, CropInteractionPhase phase)>;

    OrthogonalCropWidgetStateController()
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

    void SetInteractor(vtkRenderWindowInteractor* interactor)
    {
        m_interactor = interactor;
        m_widget->SetInteractor(interactor);
    }

    void SetReferenceBounds(const std::array<double, 6>& bounds)
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

    void SetWidgetBounds(const std::array<double, 6>& bounds)
    {
        if (!GetBoundsAreValid(bounds)) {
            return;
        }

        m_currentBounds = bounds;
        m_representation->PlaceWidget(m_currentBounds.data());
    }

    const std::array<double, 6>& GetCurrentBounds() const
    {
        return m_currentBounds;
    }

    void SetBoundsChangedCallback(BoundsChangedCallback callback)
    {
        m_boundsChangedCallback = std::move(callback);
    }

    bool SetEnabled(bool enabled)
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

    bool GetEnabled() const
    {
        return m_enabled;
    }

private:
    friend class OrthogonalCropWidgetStateCallback;

    static bool GetBoundsAreValid(const std::array<double, 6>& bounds)
    {
        return bounds[0] < bounds[1]
            && bounds[2] < bounds[3]
            && bounds[4] < bounds[5];
    }

    static CropInteractionPhase GetInteractionPhaseFromEvent(unsigned long eventId)
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

    void EnsureObserversAdded()
    {
        if (m_observersAdded) {
            return;
        }

        m_widget->AddObserver(vtkCommand::StartInteractionEvent, m_callbackCommand);
        m_widget->AddObserver(vtkCommand::InteractionEvent, m_callbackCommand);
        m_widget->AddObserver(vtkCommand::EndInteractionEvent, m_callbackCommand);
        m_observersAdded = true;
    }

    void HandleWidgetEvent(unsigned long eventId)
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

    vtkRenderWindowInteractor* m_interactor = nullptr;
    vtkSmartPointer<vtkBoxWidget2> m_widget;
    vtkSmartPointer<vtkBoxRepresentation> m_representation;
    vtkSmartPointer<OrthogonalCropWidgetStateCallback> m_callbackCommand;
    BoundsChangedCallback m_boundsChangedCallback;
    bool m_enabled = false;
    bool m_observersAdded = false;
    std::array<double, 6> m_currentBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 6> m_referenceBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};

inline void OrthogonalCropWidgetStateCallback::Execute(vtkObject* caller, unsigned long eventId, void* callData)
{
    (void)caller;
    (void)callData;
    if (m_owner) {
        m_owner->HandleWidgetEvent(eventId);
    }
}
