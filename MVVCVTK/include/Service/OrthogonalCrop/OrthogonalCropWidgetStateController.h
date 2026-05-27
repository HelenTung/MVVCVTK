#pragma once

#include "OrthogonalCrop/OrthogonalCropTypes.h"

#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCommand.h>
#include <vtkPlanes.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <array>
#include <functional>

class OrthogonalCropWidgetStateController;

class OrthogonalCropWidgetStateCallback : public vtkCommand {
public:
    static OrthogonalCropWidgetStateCallback* New();

    void SetOwner(OrthogonalCropWidgetStateController* owner);
    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override;

private:
    OrthogonalCropWidgetStateController* m_owner = nullptr;
};

class OrthogonalCropWidgetStateController {
public:
    using BoundsChangedCallback = std::function<void(const std::array<double, 6>& bounds, CropInteractionPhase phase)>;

    OrthogonalCropWidgetStateController();

    void SetInteractor(vtkRenderWindowInteractor* interactor);
    void SetReferenceBounds(const std::array<double, 6>& bounds);
    void SetWidgetBounds(const std::array<double, 6>& bounds);
    const std::array<double, 6>& GetCurrentBounds() const;
    bool GetPlanes(vtkPlanes* planes) const;
    void SetBoundsChangedCallback(BoundsChangedCallback callback);
    bool SetEnabled(bool enabled);
    bool GetEnabled() const;

private:
    friend class OrthogonalCropWidgetStateCallback;

    static bool GetBoundsAreValid(const std::array<double, 6>& bounds);
    static CropInteractionPhase GetInteractionPhaseFromEvent(unsigned long eventId);

    void EnsureObserversAdded();
    void HandleWidgetEvent(unsigned long eventId);

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
