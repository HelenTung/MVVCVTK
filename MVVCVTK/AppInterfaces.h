#pragma once
// =====================================================================
// AppInterfaces.h — 纯虚接口定义
//
// 依赖：AppTypes.h（数据结构）+ VTK 智能指针
// 不包含任何实现，不包含 AppState.h
//
// 接口层次（从底到顶）：
//   AbstractDataManager        — 数据 I/O
//   AbstractDataConverter<I,O> — 数据变换
//   AbstractVisualStrategy     — 渲染策略
//   AbstractAppService         — 基础服务
//   IPreInitService            — 前处理配置
//   IDataLoaderService         — 数据加载（NEW: 解耦加载职责）
//   AbstractInteractiveService — 交互服务
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
#include <atomic>
#include <thread>
#include <functional>

// ─────────────────────────────────────────────────────────────────────
// AbstractDataManager
// ─────────────────────────────────────────────────────────────────────
class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;
    virtual bool LoadData(const std::string& filePath) = 0;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;

    bool IsLoading() const { return m_isLoading.load(); }

protected:
    std::atomic<bool> m_isLoading{ false };
};

// ─────────────────────────────────────────────────────────────────────
// AbstractDataConverter<InputT, OutputT>
// ───────────────────────────────────────────────��─────────────────────
template <typename InputT, typename OutputT>
class AbstractDataConverter {
public:
    virtual ~AbstractDataConverter() = default;
    virtual vtkSmartPointer<OutputT> Process(vtkSmartPointer<InputT> input) = 0;
    virtual void SetParameter(const std::string& key, double value) {}
};

// ─────────────────────────────────────────────────────────────────────
// AbstractVisualStrategy
// ─────────────────────────────────────────────────────────────────────
class AbstractVisualStrategy {
public:
    virtual ~AbstractVisualStrategy() = default;

    virtual void SetInputData(vtkSmartPointer<vtkDataObject> data) = 0;
    virtual void Attach(vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void Detach(vtkSmartPointer<vtkRenderer> renderer) = 0;
    virtual void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) {}
    virtual void UpdateVisuals(const RenderParams& params,
        UpdateFlags flags = UpdateFlags::All) {
    }
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual int GetNavigationAxis() const { return -1; }
    virtual vtkProp3D* GetMainProp() { return nullptr; }
};

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 基础服务（渲染脏标记 + 更新入口）
// ─────────────────────────────────────────────────────────────────────
class AbstractAppService {
protected:
    std::shared_ptr<AbstractDataManager>    m_dataManager;
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    vtkSmartPointer<vtkRenderer>            m_renderer;
    vtkSmartPointer<vtkRenderWindow>        m_renderWindow;
    std::atomic<bool> m_isDirty{ false };
    std::atomic<bool> m_needsSync{ false };
    std::atomic<int>  m_pendingFlags{ static_cast<int>(UpdateFlags::All) };

public:
    virtual ~AbstractAppService() = default;

    virtual void Initialize(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer>     ren)
    {
        m_renderWindow = win;
        m_renderer = ren;
    }

    // 主线程 Timer 心跳驱动的更新入口
    virtual void ProcessPendingUpdates() {}

    bool IsDirty()      const { return m_isDirty; }
    void SetDirty(bool val) { m_isDirty = val; }
    void MarkDirty() { m_isDirty = true; }

    std::shared_ptr<AbstractDataManager> GetDataManager() { return m_dataManager; }

    // Strategy 切换（主线程专属）—— 实现在 AppService.cpp
    void SwitchStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy);
};

// ─────────────────────────────────────────────────────────────────────
// IPreInitService — 前处理接口
//
// 职责：数据到达之前，登记所有与数据无关的配置意图。
//   • 所有方法只写 SharedState（内部有 mutex），零 VTK 操作
//   • 可在 BindService 之后、异步加载之前的任意时刻调用
//   • 支持逐项设置 OR 批量提交（CommitConfig）
// ─────────────────────────────────────────────────────────────────────
class IPreInitService {
public:
    virtual ~IPreInitService() = default;

    // 逐项设置（向后兼容）
    virtual void PreInit_SetVizMode(VizMode mode) = 0;
    virtual void PreInit_SetMaterial(const MaterialParams& mat) = 0;
    virtual void PreInit_SetOpacity(double opacity) = 0;
    virtual void PreInit_SetTransferFunction(const std::vector<TFNode>& nodes) = 0;
    virtual void PreInit_SetIsoThreshold(double val) = 0;

    // 【新增】批量提交：一次性写入所有配置，只加一次锁 + 只广播一次
    // 推荐在前处理阶段使用此接口，减少锁争用
    virtual void PreInit_CommitConfig(const PreInitConfig& cfg) = 0;
};

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService — 数据加载接口（NEW）
//
// 将加载职责从 MedicalVizService 中分离，加载与渲染解耦。
// 任意持有此接口的调用方均可发起加载，无需关心渲染细节。
// ─────────────────────────────────────────────────────────────────────
class IDataLoaderService {
public:
    virtual ~IDataLoaderService() = default;

    // 异步加载：onComplete 在后台线程回调，只允许操作 SharedState
    virtual void LoadFileAsync(const std::string& path,
        std::function<void(bool success)> onComplete = nullptr) = 0;
};

// ─────────────────────────────────────────────────────────────────────
// AbstractInteractiveService — 交互服务接口
// ─────────────────────────────────────────────────────────────────────
class AbstractInteractiveService
    : public AbstractAppService
{
public:
    virtual ~AbstractInteractiveService() = default;

    virtual void UpdateInteraction(int delta) {}
    virtual int  GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual void SyncCursorToWorldPosition(double worldPos[3], int axis = -1) {}
    virtual std::array<int, 3> GetCursorPosition() { return { 0, 0, 0 }; }
    virtual void SetInteracting(bool val) {}
    virtual vtkProp3D* GetMainProp() { return nullptr; }
    virtual void SyncModelMatrix(vtkMatrix4x4* mat) {}
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

    virtual void BindService(std::shared_ptr<AbstractAppService> service) {
        m_service = service;
        if (m_service)
            m_service->Initialize(m_renderWindow, m_renderer);
    }

    virtual void Render() {
        if (m_renderWindow) m_renderWindow->Render();
    }

    virtual void ResetCamera() {
        if (m_renderer) m_renderer->ResetCamera();
    }

    virtual void SetInteractionMode(VizMode mode) = 0;
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
    virtual void ToggleOrientationAxes(bool show) {}

protected:
    static void DispatchVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* clientData,
        void* callData)
    {
        auto* ctx = static_cast<AbstractRenderContext*>(clientData);
        if (ctx) ctx->HandleVTKEvent(caller, eventId, callData);
    }

    virtual void HandleVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) {
    }
};