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
    std::shared_ptr<IMeasurementService> m_measurementFacade;      // 业务层持有的统一测量服务接口
    std::shared_ptr<class MeasurementService> m_measurementService; // 当前渲染上下文实际绑定的测量服务实现

    // ── 坐标轴组件（与路由无关，保留） ───────────────────────────────
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;

    // 构建/重建路由表（BindService 和 InitInteractor 后调用）
    void SetInteractionRouter();

public:
    void SetInteractorInitialized() override;
    StdRenderContext();
    void SetStarted() override;
    void SetCameraStyleByVizMode(VizMode mode) override;
    void SetServiceBound(std::shared_ptr<AbstractAppService> service) override;
    void SetOrientationAxesVisible(bool show) override;
    void SetToolMode(ToolMode mode);
    void SetMeasurementService(std::shared_ptr<IMeasurementService> service);
    void SetAngle(double angle = 0) { m_angle = angle; };
    std::shared_ptr<IMeasurementService> GetMeasurementService() const { return m_measurementFacade; }
    vtkRenderWindowInteractor* GetInteractor() const { return m_interactor.GetPointer(); }
protected:
    void SetVTKEventHandled(vtkObject* caller,
        long unsigned int eventId,
        void* callData) override;
};