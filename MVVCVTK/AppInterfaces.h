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
//   IDataLoaderService         — 数据加载
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
#include <mutex>

// ─────────────────────────────────────────────────────────────────────
// AbstractDataManager
// ─────────────────────────────────────────────────────────────────────
class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;
    virtual bool LoadData(const std::string& filePath) = 0;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;
    LoadState GetLoadState() const {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        return m_loadState;
    }
    bool IsLoading() const { return m_isLoading.load(); }

protected:
    mutable std::mutex m_stateMutex;
    LoadState          m_loadState{ LoadState::Idle };
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

    // Strategy 切换,实现在 AppService.cpp
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
    virtual void PreInit_SetBackground(const BackgroundColor& bg) = 0;
    virtual void PreInit_SetWindowLevel(double ww, double wc) = 0;
    virtual void PreInit_CommitConfig(const PreInitConfig& cfg) = 0;
};

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService — 数据加载接口
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

    // 查询当前加载状态（可从主线程轮询）
    virtual LoadState GetLoadState() const = 0;

    // 尽力取消加载（若实现支持，则设标记由加载线程自检退出）
    // 默认空实现，派生类按需覆盖
    virtual void CancelLoad() {}
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

    // ── 渲染外观配置（运行时，与事件驱动类同级）────────────────────────
    // 写 SharedState → Observer 广播 Visibility → ProcessPendingUpdates 执行
    virtual void SetElementVisible(uint32_t flagBit, bool show) {}

    // ── 运行时窗宽/窗位调整（鼠标交互驱动）───────────────────
    // 写 SharedState → Observer 广播 WindowLevel → ProcessPendingUpdates 执行
    virtual void AdjustWindowLevel(double deltaWW, double deltaWC) {}
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
    virtual void InitInteractor() = 0;  // 显式分离，避免 Start() 混乱
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
    virtual void SetElementVisible(uint32_t flagBit, bool show) {}
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
        if (ctx) ctx->HandleVTKEvent(caller, eventId, callData);
    }

    virtual void HandleVTKEvent(vtkObject* caller,
        long unsigned int eventId,
        void* callData) {
    }
};