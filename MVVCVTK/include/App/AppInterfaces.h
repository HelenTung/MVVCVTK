#pragma once
// =====================================================================
// AppInterfaces.h — 纯虚接口定义
//
// 依赖：AppTypes.h（数据结构）+ VTK 智能指针
// 不包含任何实现，不包含 AppState.h
//
// 接口层次（从底到顶）：
//   AbstractDataManager        — 数据 I/O
//   AbstractVisualStrategy     — 渲染策略
//   OverlayService             — 图层叠加服务
//   AbstractAppService         — 基础渲染服务
//   InteractiveService — 交互服务
//   AbstractRenderContext      — 渲染上下文
// =====================================================================

#include "AppTypes.h"
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkProp3D.h>
#include <vtkActor.h>
#include <vtkMatrix4x4.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <utility>

// ─────────────────────────────────────────────────────────────────────
// AbstractDataManager
// ─────────────────────────────────────────────────────────────────────
class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;
    virtual std::array<double, 2> GetScalarRange() const { return { 0.0, 0.0 }; }
    virtual std::array<double, 3> GetSpacing() const { return { 1.0, 1.0, 1.0 }; }
    virtual DataVersion GetDataVersion() const = 0;
    virtual bool SetDataLoaded(const std::string& filePath,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) = 0;
    virtual bool SetFromBuffer(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin) {
        return false;
    }
    // 后台线程把新体数据准备好后，只通过这个入口交给主线程正式接管并提交到当前 vtkImage。
    virtual bool SetCurrentFromPending() {
        return false;
    }
    virtual bool ExportData(const std::string& filePath, const std::array<double, 16>& modelToWorldMatrix) { return false; }
    virtual bool ExportSlices(const std::string& dirPath, Orientation orientation, const WindowLevelParams& windowLevel, const std::array<double, 16>& modelToWorldMatrix) { return false; }
};

// ─────────────────────────────────────────────────────────────────────
// AbstractVisualStrategy
// ─────────────────────────────────────────────────────────────────────
class AbstractVisualStrategy {
public:
    virtual ~AbstractVisualStrategy() = default;

    virtual void SetInputData(vtkSmartPointer<vtkDataObject> data) = 0;
    virtual void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void SetCamera(vtkSmartPointer<vtkRenderer> renderer) {}
    virtual void SetVisualState(const RenderParams& params,
        UpdateFlags flags = UpdateFlags::All) {
    }
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual int GetNavigationAxis() const { return -1; }
    virtual vtkProp3D* GetMainProp() { return nullptr; }
};

// ─────────────────────────────────────────────────────────────────────
// OverlayService — 图层叠加策略生命周期
// ─────────────────────────────────────────────────────────────────────
class OverlayService {
public:
    virtual ~OverlayService() = default;

    // 图层叠加管理接口：Overlay 与主 Strategy 共享同一套状态同步节奏，但生命周期可独立增删。
    virtual void AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy) = 0;
    virtual void RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy) = 0;
    virtual void ClearOverlayStrategies() = 0;
};

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 基础渲染服务
// ─────────────────────────────────────────────────────────────────────
class AbstractAppService
    : public OverlayService {
public:
    ~AbstractAppService() override = default;

    virtual void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren) = 0;

    // 主线程 Timer 心跳驱动的更新入口。
    virtual void SendUpdates() = 0;

    virtual bool GetDirty() const = 0;
    virtual void SetDirty() = 0;
    // 取走并清空当前渲染脏位，让 Timer 线程能以“消费一次渲染请求”的语义推进渲染循环。
    virtual bool ResetDirty() = 0;

    virtual void SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy) = 0;
};

// ─────────────────────────────────────────────────────────────────────
// InteractiveService — 交互服务接口
// ─────────────────────────────────────────────────────────────────────
class InteractiveService
    : public AbstractAppService
{
public:
    ~InteractiveService() override = default;

    virtual void SetSliceScroll(int delta) {}
    virtual int  GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual void SetCursorWorldPosition(double worldPos[3], int axis = -1) {}
    virtual std::array<double, 3> GetCursorWorld() { return { 0, 0, 0 }; }
    virtual void SetInteracting(bool isInteracting) {}
    virtual vtkProp3D* GetMainProp() { return nullptr; }
    virtual void SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix) {}
    virtual std::array<double, 16> GetModelMatrix() {
        return { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    }
    virtual int GetNavigationAxis() const { return -1; }
    virtual WindowLevelParams GetWindowLevel() const { return {}; }
    virtual void SetElementVisible(uint32_t flagBit, bool isVisible) {}
    virtual void SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC) {}
    virtual void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const = 0;
    virtual void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const = 0;
};

// ─────────────────────────────────────────────────────────────────────
// AbstractRenderContext — 渲染上下文
// ─────────────────────────────────────────────────────────────────────
class AbstractRenderContext {
protected:
    vtkSmartPointer<vtkRenderer>        m_renderer;
    vtkSmartPointer<vtkRenderWindow>    m_renderWindow;
    std::shared_ptr<AbstractAppService> m_service;

public:
    virtual ~AbstractRenderContext() = default;

    AbstractRenderContext() {
        m_renderer = vtkSmartPointer<vtkRenderer>::New();
        m_renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
        m_renderWindow->AddRenderer(m_renderer);
    }

    virtual void SetServiceBound(std::shared_ptr<AbstractAppService> service) {
        m_service = service;
        if (m_service)
            m_service->SetRenderContext(m_renderWindow, m_renderer);
    }

    // 宿主层可能先创建平台相关窗口，例如 Qt 的 vtkGenericOpenGLRenderWindow。
    // 接口只接受 VTK 基类，是为了让 core / feature 继续与具体 UI 框架解耦。
    virtual void SetRenderWindow(vtkSmartPointer<vtkRenderWindow> renderWindow) {
        if (!renderWindow || renderWindow.GetPointer() == m_renderWindow.GetPointer()) {
            return;
        }

        // renderer 是业务视图的稳定真源，替换窗口时只能迁移 renderer，
        // 不能让 service 持有旧 window，否则后续 overlay / Render 会落到错误宿主窗口。
        if (m_renderWindow && m_renderer) {
            m_renderWindow->RemoveRenderer(m_renderer);
        }
        m_renderWindow = std::move(renderWindow);
        if (m_renderWindow && m_renderer) {
            m_renderWindow->AddRenderer(m_renderer);
        }
        if (m_service) {
            m_service->SetRenderContext(m_renderWindow, m_renderer);
        }
    }

    virtual void SendRender() {
        if (m_renderWindow) m_renderWindow->Render();
    }

    virtual void ResetCamera() {
        if (m_renderer) m_renderer->ResetCamera();
    }
    vtkRenderer* GetRenderer() const { return m_renderer.GetPointer(); }
    vtkRenderWindow* GetRenderWindow() const { return m_renderWindow.GetPointer(); }

    virtual void SetCameraStyle(VizMode mode) = 0;
    virtual void SetInteractorReady() = 0;  // 显式分离，避免 Start() 混乱
    virtual void Start() = 0;

    virtual void SetWindowSize(int w, int h) {
        if (m_renderWindow) m_renderWindow->SetSize(w, h);
    }
    virtual void SetWindowPosition(int x, int y) {
        if (m_renderWindow) m_renderWindow->SetPosition(x, y);
    }
    virtual void SetWindowTitle(const std::string& title) {
        if (m_renderWindow) m_renderWindow->SetWindowName(title.c_str());
    }
    virtual void SetOrientationAxesVisible(bool isVisible) {}

    virtual void SetRendererBackground(const BackgroundColor& bg) {
        if (m_renderer)
            m_renderer->SetBackground(bg.r, bg.g, bg.b);
    }
protected:
    static void DispatchVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* clientData,
        void* callData)
    {
        auto* ctx = static_cast<AbstractRenderContext*>(clientData);
        if (ctx) ctx->OnVTKEvent(caller, eventId, callData);
    }

    virtual void OnVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) {
    }
};
