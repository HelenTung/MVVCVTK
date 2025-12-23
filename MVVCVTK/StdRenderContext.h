#pragma once

#include "AppInterfaces.h"
#include "AppService.h"
#include <vtkRenderWindowInteractor.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkInteractorStyleImage.h>
//#include <vtkCellPicker.h>
#include <vtkPropPicker.h>
class StdRenderContext : public AbstractRenderContext {
private:
    vtkSmartPointer<vtkRenderWindowInteractor> m_interactor;
    vtkSmartPointer<vtkCallbackCommand> m_eventCallback;
    // 当前上下文记录的模式，用于决定滚轮行为
    VizMode m_currentMode = VizMode::Volume;

    //// 拾取器
    //vtkSmartPointer<vtkCellPicker> m_picker;
    
    vtkSmartPointer<vtkPropPicker> m_picker; 
    int m_dragAxis = -1; //
    // 标记是否正在拖拽
    bool m_isDragging = false;

public:
    void InitInteractor();
    StdRenderContext();
    void Start() override;
    void SetInteractionMode(VizMode mode) override;

protected:
    // 处理从基类转发过来的 VTK 事件
    void HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData) override;
};
