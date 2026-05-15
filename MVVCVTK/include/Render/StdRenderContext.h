#pragma once
#include "AppInterfaces.h"
#include "InteractionRouter.h"
#include <memory>
#include <vtkRenderWindowInteractor.h>
#include <vtkPropPicker.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkAxesActor.h>

class StdRenderContext : public AbstractRenderContext {
private:
    std::shared_ptr<AbstractInteractiveService> m_interactiveService;
    vtkSmartPointer<vtkRenderWindowInteractor>  m_interactor;
    vtkSmartPointer<vtkCallbackCommand>         m_eventCallback;
    vtkSmartPointer<vtkPropPicker>              m_picker;

    VizMode  m_currentMode = VizMode::Volume;
    ToolMode m_toolMode = ToolMode::Navigation;

    double m_angle = 0.0;   // 旋转角度
    // ── 路由器（替代原来 HandleVTKEvent 里的手写 if-else） ──────────
    InteractionRouter m_interactionRouter;

    // ── 坐标轴组件（与路由无关，保留） ───────────────────────────────
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;

    void SetInteractionObserversAdded();
    // 构建/重建路由表（BindService 和 InitInteractor 后调用）
    void SetInteractionRouter();
    bool SetKeyEventHandled(vtkRenderWindowInteractor* interactor);
    void SetInteractionEventBuilt(InteractionEvent& eve,
        vtkRenderWindowInteractor* interactor,
        long unsigned int eventId) const;

public:
    void SetInteractorInitialized() override;
    StdRenderContext();
    void SetStarted() override;
    void SetCameraStyleByVizMode(VizMode mode) override;
    void SetServiceBound(std::shared_ptr<AbstractAppService> service) override;
    void SetOrientationAxesVisible(bool show) override;
    void SetToolMode(ToolMode mode);
    void SetAngle(double angle = 0) { m_angle = angle; };
    vtkRenderWindowInteractor* GetInteractor() const { return m_interactor.GetPointer(); }
protected:
    void SetVTKEventHandled(vtkObject* caller,
        long unsigned int eventId,
        void* callData) override;
};
