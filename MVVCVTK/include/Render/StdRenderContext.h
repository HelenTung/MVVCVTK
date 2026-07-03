#pragma once
#include "AppInterfaces.h"
#include "InteractionRouter.h"
#include <functional>
#include <memory>
#include <vtkRenderWindowInteractor.h>
#include <vtkPropPicker.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkAxesActor.h>

class StdRenderContext : public AbstractRenderContext {
public:
    using HostKeyEventHandler = std::function<bool(vtkRenderWindowInteractor*, StdRenderContext&)>;

private:
    std::shared_ptr<AbstractInteractiveService> m_interactiveService; // 交互服务入口，Router 最终把事件委托给它更新状态
    vtkSmartPointer<vtkRenderWindowInteractor>  m_interactor; // VTK 事件源与主循环载体
    vtkSmartPointer<vtkCallbackCommand>         m_eventCallback; // 统一转发 VTK 事件到当前 RenderContext
    vtkSmartPointer<vtkPropPicker>              m_picker; // 2D/3D 交互命中检测共用的拾取器

    VizMode  m_currentMode = VizMode::Volume; // 当前视图模式，决定相机风格与事件路由目标
    ToolMode m_toolMode = ToolMode::Navigation; // 当前工具模式，决定事件是否进入模型变换专用路径

    double m_angle = 0.0;   // 切片导出等场景复用的业务角度参数
    HostKeyEventHandler m_hostKeyEventHandler; // 宿主层按键映射；RenderContext 只负责转交，不写死功能键
    // ── 路由器（替代原来 HandleVTKEvent 里的手写 if-else） ──────────
    InteractionRouter m_interactionRouter; // 把 Timer / 2D / 3D 交互处理器组织成统一派发链

    // ── 坐标轴组件（与路由无关，保留） ───────────────────────────────
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;

    // 外部宿主窗口可能已经带有自己的 interactor；集中接管可以复用同一套路由和 observer 生命周期。
    void AttachInteractor(vtkSmartPointer<vtkRenderWindowInteractor> interactor);
    void AddInteractionObservers();
    // 构建/重建路由表（BindService 和 InitInteractor 后调用）
    void BuildInteractionRouter();
    bool HandleKeyEvent(vtkRenderWindowInteractor* interactor); // 宿主按键入口；具体键位不写入 RenderContext
    void BuildInteractionEvent(InteractionEvent& eve,
        vtkRenderWindowInteractor* interactor,
        long unsigned int eventId) const; // 把 VTK 原生事件压平成业务层统一使用的 InteractionEvent

public:
    void InitializeInteractor() override;
    StdRenderContext();
    void Start() override;
    void ApplyCameraStyle(VizMode mode) override;
    void SetServiceBound(std::shared_ptr<AbstractAppService> service) override;
    // 只替换 VTK render window，不引入 Qt 类型；Qt 生命周期由宿主层负责。
    void SetRenderWindow(vtkSmartPointer<vtkRenderWindow> renderWindow) override;
    void SetOrientationAxesVisible(bool show) override;
    void SetHostKeyEventHandler(HostKeyEventHandler handler);
    void SetToolMode(ToolMode mode);
    ToolMode GetToolMode() const { return m_toolMode; }
    double GetAngle() const { return m_angle; }
    void SetAngle(double angle = 0) { m_angle = angle; };
    vtkRenderWindowInteractor* GetInteractor() const { return m_interactor.GetPointer(); }
protected:
    void HandleVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) override;
};
