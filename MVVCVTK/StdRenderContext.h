#pragma once
#include "AppInterfaces.h"
#include "InteractionRouter.h"   
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkInteractorStyleImage.h>
#include <vtkDistanceWidget.h>
#include <vtkAngleWidget.h>
#include <vtkDistanceRepresentation2D.h>
#include <vtkAngleRepresentation2D.h>
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

    // ── 路由器（替代原来 HandleVTKEvent 里的手写 if-else） ──────────
    InteractionRouter m_interactionRouter;

    // ── 测量 & 坐标轴组件（与路由无关，保留） ─────────────────────────
    vtkSmartPointer<vtkDistanceWidget>          m_distanceWidget;
    vtkSmartPointer<vtkAngleWidget>             m_angleWidget;
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;

    // 构建/重建路由表（BindService 和 InitInteractor 后调用）
    void BuildInteractionRouter();

public:
    void InitInteractor() override;
    StdRenderContext();
    void Start() override;
    void SetInteractionMode(VizMode mode) override;
    void BindService(std::shared_ptr<AbstractAppService> service) override;
    void ToggleOrientationAxes(bool show) override;
    void SetToolMode(ToolMode mode);
	void SetElementVisible(uint32_t flagBit, bool show) override;
    vtkRenderWindowInteractor* GetInteractor() const { return m_interactor.GetPointer(); }
protected:
    void HandleVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) override;
};