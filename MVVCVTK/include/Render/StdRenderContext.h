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
    // 共享拥有交互服务；Router 内 Handler 只观察裸指针，换绑 service 时立即重建。
    std::shared_ptr<InteractiveService> m_interactiveService;
    // 持有当前 interactor 的 VTK 引用；可接管宿主注入对象，替换前移除旧 observer/timer。
    vtkSmartPointer<vtkRenderWindowInteractor>  m_interactor;
    // 持有统一 VTK callback；ClientData 非拥有指向 this，析构前先解除全部 observer。
    vtkSmartPointer<vtkCallbackCommand>         m_eventCallback;
    // Context 独占引用的共享拾取器；2D/3D Handler 仅持有其裸观察指针。
    vtkSmartPointer<vtkPropPicker>              m_picker;

    VizMode  m_currentMode = VizMode::Volume; // SetCameraStyle 写入，并复制到后续事件决定 2D/3D 路由
    ToolMode m_toolMode = ToolMode::Navigation; // SetToolMode 写入，并复制到事件决定模型变换路径

    // ── 路由器（替代原来 OnVTKEvent 里的手写 if-else） ──────────
    // 独占按优先级排列的 Handler；service/window/input hook 变化时清空并重建，避免悬空观察指针。
    InteractionRouter m_interactionRouter;
    // 当前 interactor 生成的业务 observer tag；替换 interactor 或析构时逐项移除并清空。
    std::vector<unsigned long> m_observerTags;
    unsigned long m_timerObserverTag = 0; // TimerEvent observer tag；RemoveTimer 后清零
    int m_timerId = 0; // CreateRepeatingTimer 返回值；0 表示未创建，RemoveTimer 负责销毁并清零
    // Context 按值持有 host 输入 hook；空 eventIds 匹配全部事件，设置/清除后重建对应 Handler。
    std::function<InteractionResult(const InteractionEvent&)> m_inputHandler;
    std::vector<unsigned long> m_inputEventIds;
    // Context 按值持有主线程 tick hook；每个 TimerEvent 在 Router 分发后同步调用，Clear 时释放。
    std::function<void()> m_timerHandler;

    // ── 坐标轴组件（与路由无关，保留） ───────────────────────────────
    // 延迟创建并持有方向轴 widget；始终绑定当前 interactor，Context 析构时释放 VTK 引用。
    vtkSmartPointer<vtkOrientationMarkerWidget> m_axesWidget;

    // 外部宿主窗口可能已经带有自己的 interactor；集中接管可以复用同一套路由和 observer 生命周期。
    void AttachInteractor(vtkSmartPointer<vtkRenderWindowInteractor> interactor);
    void AttachObservers();
    void RemoveObservers();
    void AttachTimer();
    void RemoveTimer();
    // service、interactor、renderWindow 或输入 hook 变化后重建路由表，刷新 Handler 的观察依赖。
    void BuildInteractionRouter();
    void BuildInteractionEvent(InteractionEvent& eve,
        vtkRenderWindowInteractor* interactor,
        long unsigned int eventId) const; // 把 VTK 原生事件压平成业务层统一使用的 InteractionEvent

public:
    void SetInteractorReady() override;
    StdRenderContext();
    ~StdRenderContext() override;
    void Start() override;
    void SetCameraStyle(VizMode mode) override;
    void SetServiceBound(std::shared_ptr<AbstractAppService> service) override;
    // 只替换 VTK render window，不引入 Qt 类型；Qt 生命周期由宿主层负责。
    void SetRenderWindow(vtkSmartPointer<vtkRenderWindow> renderWindow) override;
    void SetOrientationAxesVisible(bool isVisible) override;
    void SetToolMode(ToolMode mode);
    ToolMode GetToolMode() const { return m_toolMode; }
    void SetInputHandler(
        std::function<InteractionResult(const InteractionEvent&)> handler,
        std::vector<unsigned long> eventIds);
    void ClearInputHandler();
    void SetTimerHandler(std::function<void()> handler);
    void ClearTimerHandler();
    vtkRenderWindowInteractor* GetInteractor() const { return m_interactor.GetPointer(); }
protected:
    void OnVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) override;
};
