#pragma once
#include "AppInterfaces.h"
#include "InteractionRouter.h"
#include <functional>
#include <memory>
#include <vector>
#include <vtkRenderWindowInteractor.h>
#include <vtkPropPicker.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkAxesActor.h>

class StdRenderContext : public AbstractRenderContext {
private:
    std::shared_ptr<AbstractInteractiveService> m_interactiveService; // 交互服务入口，Router 最终把事件委托给它更新状态
    vtkSmartPointer<vtkRenderWindowInteractor>  m_interactor; // VTK 事件源与主循环载体
    vtkSmartPointer<vtkCallbackCommand>         m_eventCallback; // 统一转发 VTK 事件到当前 RenderContext
    vtkSmartPointer<vtkPropPicker>              m_picker; // 2D/3D 交互命中检测共用的拾取器

    VizMode  m_currentMode = VizMode::Volume; // 当前视图模式，决定相机风格与事件路由目标
    ToolMode m_toolMode = ToolMode::Navigation; // 当前工具模式，决定事件是否进入模型变换专用路径

    // ── 路由器（替代原来 HandleVTKEvent 里的手写 if-else） ──────────
    InteractionRouter m_interactionRouter; // 把 Timer / 2D / 3D 交互处理器组织成统一派发链
    std::vector<unsigned long> m_observerTags; // 当前 interactor 上由本 context 注册的业务 observer
    unsigned long m_timerObserverTag = 0; // 当前 TimerEvent observer tag，替换 interactor 时精确移除
    int m_timerId = 0; // CreateRepeatingTimer 返回的 timer id，用于避免重复创建和精确销毁
    std::function<InteractionResult(const InteractionEvent&)> m_keyHandler; // host 调试输入 hook，不携带 host 语义
    std::function<void()> m_timerHandler; // host 主线程 tick hook，TimerEvent 生命周期仍归 RenderContext

    // ── 坐标轴组件（与路由无关，保留） ───────────────────────────────
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;

    // 外部宿主窗口可能已经带有自己的 interactor；集中接管可以复用同一套路由和 observer 生命周期。
    void AttachInteractor(vtkSmartPointer<vtkRenderWindowInteractor> interactor);
    void EnsureObservers();
    void RemoveObservers();
    void EnsureTimer();
    void RemoveTimer();
    // 构建/重建路由表（BindService 和 InitInteractor 后调用）
    void BuildInteractionRouter();
    void BuildInteractionEvent(InteractionEvent& eve,
        vtkRenderWindowInteractor* interactor,
        long unsigned int eventId) const; // 把 VTK 原生事件压平成业务层统一使用的 InteractionEvent

public:
    void InitializeInteractor() override;
    StdRenderContext();
    ~StdRenderContext() override;
    void Start() override;
    void ApplyCameraStyle(VizMode mode) override;
    void SetServiceBound(std::shared_ptr<AbstractAppService> service) override;
    // 只替换 VTK render window，不引入 Qt 类型；Qt 生命周期由宿主层负责。
    void SetRenderWindow(vtkSmartPointer<vtkRenderWindow> renderWindow) override;
    void SetOrientationAxesVisible(bool show) override;
    void SetToolMode(ToolMode mode);
    ToolMode GetToolMode() const { return m_toolMode; }
    void SetKeyHandler(std::function<InteractionResult(const InteractionEvent&)> handler);
    void ClearKeyHandler();
    void SetTimerHandler(std::function<void()> handler);
    void ClearTimerHandler();
    vtkRenderWindowInteractor* GetInteractor() const { return m_interactor.GetPointer(); }
protected:
    void HandleVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) override;
};
