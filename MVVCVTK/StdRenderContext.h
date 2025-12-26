#pragma once
#include "AppInterfaces.h"
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkInteractorStyleImage.h>
#include <vtkPropPicker.h>
#include <vtkDistanceWidget.h>
#include <vtkAngleWidget.h>
#include <vtkDistanceRepresentation2D.h>
#include <vtkAngleRepresentation2D.h>

class StdRenderContext : public AbstractRenderContext {
private:
    std::shared_ptr<AbstractInteractiveService> m_interactiveService;
    vtkSmartPointer<vtkRenderWindowInteractor> m_interactor;
    vtkSmartPointer<vtkCallbackCommand> m_eventCallback;
    // 当前上下文记录的模式，用于决定滚轮行为
    VizMode m_currentMode = VizMode::Volume;
    
    vtkSmartPointer<vtkPropPicker> m_picker;
	// 记录当前拖拽的轴向
    int m_dragAxis = -1;
    // 标记是否正在拖拽
    bool m_isDragging = false;
    // 十字线拖拽
	bool m_enableDragCrosshair = false;

    // --- 测量组件 ---
    vtkSmartPointer<vtkDistanceWidget> m_distanceWidget;
    vtkSmartPointer<vtkAngleWidget> m_angleWidget;
    ToolMode m_toolMode = ToolMode::Navigation;

public:
    void InitInteractor();
    StdRenderContext();
    void Start() override;
    void SetInteractionMode(VizMode mode) override;
    void BindService(std::shared_ptr<AbstractAppService> service) override;

	// 测量工具模式切换
    void SetToolMode(ToolMode mode);
protected:
    // 处理从基类转发过来的 VTK 事件
    void HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData) override;
};
