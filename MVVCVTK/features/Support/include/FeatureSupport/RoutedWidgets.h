#pragma once
#include <vtkBoxWidget2.h>
#include <vtkImplicitPlaneWidget2.h>
#include <vtkHandleWidget.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkCallbackCommand.h>

namespace FeatureSupport {
// SDK Router 保存输入归属。VTK 只执行几何操作，不独占 interactor 事件流。
class RoutedBoxWidget final : public vtkBoxWidget2 {
public:
    static RoutedBoxWidget* New() { return new RoutedBoxWidget; }
    vtkTypeMacro(RoutedBoxWidget,vtkBoxWidget2);
    void ReleaseInputFocus() { ReleaseFocus(); }
    void StopGesture() { EndSelectAction(this); ReleaseFocus(); }
    void SetEnabled(int value) override {
        vtkBoxWidget2::SetEnabled(value);
        // 轴约束键不能绕过统一的绑定表。
        if(Interactor)Interactor->RemoveObserver(KeyEventCallbackCommand);
    }
protected:
    RoutedBoxWidget()=default;
    ~RoutedBoxWidget() override=default;
};
class RoutedPlaneWidget final : public vtkImplicitPlaneWidget2 {
public:
    static RoutedPlaneWidget* New() { return new RoutedPlaneWidget; }
    vtkTypeMacro(RoutedPlaneWidget,vtkImplicitPlaneWidget2);
    void ReleaseInputFocus() { ReleaseFocus(); }
    void StopGesture() { EndSelectAction(this); ReleaseFocus(); }
protected:
    RoutedPlaneWidget()=default;
    ~RoutedPlaneWidget() override=default;
};
class RoutedHandleWidget final : public vtkHandleWidget {
public:
    static RoutedHandleWidget* New() { return new RoutedHandleWidget; }
    vtkTypeMacro(RoutedHandleWidget,vtkHandleWidget);
    void ReleaseInputFocus() { ReleaseFocus(); }
    void StopGesture() { EndSelectAction(this); ReleaseFocus(); }
    void SetEnabled(int value) override {
        vtkHandleWidget::SetEnabled(value);
        if(Interactor)Interactor->RemoveObserver(KeyEventCallbackCommand);
    }
protected:
    RoutedHandleWidget()=default;
    ~RoutedHandleWidget() override=default;
};
}
