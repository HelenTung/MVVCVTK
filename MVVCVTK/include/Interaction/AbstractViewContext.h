#pragma once

#include "App/AppTypes.h"
#include "Interaction/InteractionTypes.h"

#include <vtkSmartPointer.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class vtkObject;
class vtkRenderer;
class vtkRenderWindow;
class vtkRenderWindowInteractor;

struct ViewCameraState final {
    std::array<double, 3> position{};
    std::array<double, 3> focalPoint{};
    std::array<double, 3> viewUp{};
    std::array<double, 2> clippingRange{};
    double parallelScale = 1.0;
    double viewAngle = 30.0;
    bool isParallel = false;
};

// 单视图 VTK 生命周期契约；不保存 App service，也不执行跨端口 RTTI。
class AbstractViewContext {
public:
    AbstractViewContext();
    virtual ~AbstractViewContext();

    virtual bool SetRenderWindow(
        vtkSmartPointer<vtkRenderWindow> renderWindow);
    virtual bool SendRender();
    virtual bool ResetCamera();
    virtual std::optional<ViewCameraState> GetCameraState() const;
    virtual bool SetCameraState(const ViewCameraState& state);
    vtkRenderer* GetRenderer() const;
    vtkRenderWindow* GetRenderWindow() const;

    virtual bool SetCameraStyle(VizMode mode) = 0;
    virtual bool SetInteractorReady() = 0;
    virtual bool SetInputEnabled(bool isEnabled) = 0;
    virtual bool Start() = 0;
    virtual bool StopInput() = 0;
    virtual bool SetWindowSize(int width, int height);
    virtual bool SetWindowPosition(int x, int y);
    virtual bool SetWindowTitle(const std::string& title);
    virtual bool SetOrientationAxesVisible(bool isVisible) = 0;
    virtual bool GetOrientationAxesVisible() const = 0;
    virtual bool SetRendererBackground(
        const BackgroundColor& background);
    virtual bool SetToolMode(ToolMode mode) = 0;
    virtual ToolMode GetToolMode() const = 0;
    virtual bool SetInputHandler(
        std::function<InteractionResult(const InteractionEvent&)> handler,
        std::vector<InteractionEventKind> eventKinds) = 0;
    virtual bool ClearInputHandler() = 0;
    virtual bool SetTimerHandler(std::function<void()> handler) = 0;
    virtual bool ClearTimerHandler() = 0;
    virtual vtkRenderWindowInteractor* GetInteractor() const = 0;

protected:
    bool GetIsOwnerThread() const noexcept;

    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;

    static void DispatchVTKEvent(
        vtkObject* caller,
        unsigned long eventId,
        void* clientData,
        void* callData);
    virtual void OnVTKEvent(
        vtkObject* caller,
        unsigned long eventId,
        void* callData);

private:
    // Context 创建线程是唯一 VTK owner；生命周期与析构均不迁移到其他线程。
    const std::thread::id m_ownerThread =
        std::this_thread::get_id();
};
