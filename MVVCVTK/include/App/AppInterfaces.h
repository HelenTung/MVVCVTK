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
//   IVisualConfigService       — 前处理配置
//   IFileLoadControlService    — 文件加载状态/控制
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
#include <map>

// ─────────────────────────────────────────────────────────────────────
// AbstractDataManager
// ─────────────────────────────────────────────────────────────────────
class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;
    virtual std::array<double, 2> GetScalarRange() const { return { 0.0, 0.0 }; }
    virtual std::array<double, 3> GetSpacing() const { return { 1.0, 1.0, 1.0 }; }
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
    virtual bool ConsumePendingImage() {
        return false;
    }
    virtual bool SaveTransformedData(const std::string& filePath, const std::array<double, 16>& modelToWorldMatrix) { return false; }
    virtual bool SaveSliceImages(const std::string& dirPath, Orientation orientation, const WindowLevelParams& windowLevel, const std::array<double, 16>& modelToWorldMatrix) { return false; }
    virtual std::string GetDefaultTransformedDataPath() const { return {}; }
};

// ─────────────────────────────────────────────────────────────────────
// AbstractDataConverter<InputT, OutputT>
// ─────────────────────────────────────────────────────────────────────
template <typename InputT, typename OutputT>
class AbstractDataConverter {
public:
    virtual ~AbstractDataConverter() = default;
    virtual vtkSmartPointer<OutputT> GetOutputData(vtkSmartPointer<InputT> input) = 0;
    virtual void SetParameter(const std::string& key, double value) {}
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
    virtual void ConfigureCamera(vtkSmartPointer<vtkRenderer> renderer) {}
    virtual void SetVisualState(const RenderParams& params,
        UpdateFlags flags = UpdateFlags::All) {
    }
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual int GetNavigationAxis() const { return -1; }
    virtual vtkProp3D* GetMainProp() { return nullptr; }
};

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 基础服务（渲染脏标记 + ProcessPendingUpdates 更新入口）
// ─────────────────────────────────────────────────────────────────────
class AbstractAppService {
protected:
    std::shared_ptr<AbstractDataManager>    m_dataManager;
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    vtkSmartPointer<vtkRenderer>            m_renderer;
    vtkSmartPointer<vtkRenderWindow>        m_renderWindow;
    std::atomic<bool> m_isDirty{ false }; // 渲染脏位：只负责驱动下一帧 Render，不承载具体业务语义
    std::atomic<bool> m_needsSync{ false }; // 同步闸门：后台/回调线程只置位，真正的策略同步统一留给主线程 Timer 心跳执行
    std::atomic<int>  m_pendingFlags{ static_cast<int>(UpdateFlags::All) }; // 增量更新位图：把多次状态改动折叠成一次主线程消费

    // 图层叠加策略列表（泛型支持任意算法）
    std::vector<std::shared_ptr<AbstractVisualStrategy>> m_overlayStrategies;
public:
    virtual ~AbstractAppService() = default;

    virtual void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer>     ren)
    {
        auto oldRenderer = m_renderer;
        if (oldRenderer && oldRenderer != ren) {
            if (m_currentStrategy) {
                m_currentStrategy->DetachRenderer(oldRenderer);
            }
            for (auto& overlay : m_overlayStrategies) {
                if (overlay) overlay->DetachRenderer(oldRenderer);
            }
        }

        m_renderWindow = win;
        m_renderer = ren;

        if (!m_renderer) return;

        if (m_currentStrategy) {
            m_currentStrategy->AttachRenderer(m_renderer);
            m_currentStrategy->ConfigureCamera(m_renderer);
        }
        for (auto& overlay : m_overlayStrategies) {
            if (overlay) overlay->AttachRenderer(m_renderer);
        }
        m_isDirty = true;
    }

    // 主线程 Timer 心跳驱动的更新入口
    virtual void ProcessPendingUpdates() {}

    bool IsDirty()      const { return m_isDirty; }
    void MarkDirty() { m_isDirty = true; }
    // 取走并清空当前渲染脏位，让 Timer 线程能以“消费一次渲染请求”的语义推进渲染循环。
    bool ConsumeDirty() { return m_isDirty.exchange(false); }

    // Strategy 切换，实现在 AppService.cpp
    void SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy);

	// 图层叠加管理接口：Overlay 与主 Strategy 共享同一套状态同步节奏，但生命周期可独立增删。
    virtual void AddOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy);
    virtual void RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy);
    virtual void ClearOverlayStrategies();
};

// ─────────────────────────────────────────────────────────────────────
// IVisualConfigService — 前处理接口
//
// 职责：数据到达之前，登记所有与数据无关的配置意图。
//   • 所有方法只写 SharedState（内部有 mutex），零 VTK 操作
//   • 可在 SetServiceBound 之后、异步加载之前的任意时刻调用
//   • 支持逐项设置 OR 批量提交（SetVisualConfig）
// ─────────────────────────────────────────────────────────────────────
class IVisualConfigService {
public:
    virtual ~IVisualConfigService() = default;

    // 逐项设置（向后兼容）
    virtual void SetVizMode(VizMode mode) = 0;
    virtual void SetMaterial(const MaterialParams& mat) = 0;
    virtual void SetOpacity(double opacity) = 0;
    virtual void SetTransferFunction(const std::vector<TFNode>& nodes) = 0;
    virtual void SetIsoThreshold(double val) = 0;
    virtual void SetBackground(const BackgroundColor& bg) = 0;
    virtual void SetSpacing(double sx, double sy, double sz) = 0;
    virtual void SetWindowLevel(double ww, double wc) = 0;
    virtual void SetVisualConfig(const PreInitConfig& cfg) = 0;
};

// ─────────────────────────────────────────────────────────────────────
// IFileLoadControlService — 文件加载状态/控制接口
//
// 只暴露文件流/重载流程的状态读取与取消控制。
// 文件流加载入口与回调广播由独立流程对象负责，不再耦合到该接口。
// ─────────────────────────────────────────────────────────────────────
class IFileLoadControlService {
public:
    virtual ~IFileLoadControlService() = default;

    // 查询文件流加载状态（推荐 UI / 上层按需读取）
    virtual LoadState GetFileLoadState() const = 0;

    // 查询重载加载状态（推荐 UI / 上层按需读取）
    virtual LoadState GetReloadLoadState() const = 0;

    // 尽力取消加载（若实现支持，则设标记由加载线程自检退出）
    // 默认空实现，派生类按需覆盖
    virtual void CancelFileLoad() {}
};

class IDataExportService {
public:
    virtual ~IDataExportService() = default;

    // 异步保存：在后台线程进行重采样和 I/O，onComplete 由主线程延迟回调
    virtual void SaveTransformedDataAsync(
        const std::string& path = {},
        std::function<void(bool success)> onComplete = nullptr) = 0;

    // 异步保存：按当前切片方向导出原始体数据全部切片图，窗宽窗位沿用当前状态
    virtual void SaveSliceImagesAsync(
        const std::string& path = {},
        const double angle = 0.0,
        std::function<void(bool success)> onComplete = nullptr) = 0;
};

class AbstractInteractiveService;

// 数据算法后处理入口与注册表：MedicalVizService 只负责在 reload 重建完成后分发本次算法后处理，
// 具体相机保存/恢复等策略由各算法入口内部执行。注册表按算法类型聚合多个入口。
class IAlgorithmPost {
public:
    virtual ~IAlgorithmPost() = default;
    virtual DataAlgorithmKind GetDataAlgorithmKind() const = 0;
    virtual void OnPipelineSynced(vtkRenderer* renderer) = 0;
};

class AlgorithmPostRegistry {
public:
    void Register(const std::shared_ptr<IAlgorithmPost>& entry) {
        if (!entry || entry->GetDataAlgorithmKind() == DataAlgorithmKind::None) {
            return;
        }

        m_posts[entry->GetDataAlgorithmKind()].push_back(entry);
    }

    void Sync(DataAlgorithmKind algorithmKind, vtkRenderer* renderer) const {
        const auto it = m_posts.find(algorithmKind);
        if (it == m_posts.end()) {
            return;
        }

        for (const auto& weakEntry : it->second) {
            if (const auto entry = weakEntry.lock()) {
                entry->OnPipelineSynced(renderer);
            }
        }
    }

private:
    std::map<DataAlgorithmKind, std::vector<std::weak_ptr<IAlgorithmPost>>> m_posts;
};

// ─────────────────────────────────────────────────────────────────────
// AbstractInteractiveService — 交互服务接口
// ─────────────────────────────────────────────────────────────────────
class AbstractInteractiveService
    : public AbstractAppService
{
public:
    virtual ~AbstractInteractiveService() = default;

    virtual void ScrollSlice(int delta) {}
    virtual int  GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual void SetCursorWorldPosition(double worldPos[3], int axis = -1) {}
    virtual std::array<double, 3> GetCursorWorld() { return { 0, 0, 0 }; }
    virtual void SetInteracting(bool val) {}
    virtual vtkProp3D* GetMainProp() { return nullptr; }
    virtual void SyncModelMatrix(vtkMatrix4x4* modelToWorldMatrix) {}
    virtual std::array<double, 16> GetModelMatrix() {
        return { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    }
    virtual int GetNavigationAxis() const { return -1; }
    virtual WindowLevelParams GetWindowLevel() const { return {}; }
    virtual void SetElementVisible(uint32_t flagBit, bool show) {}
    virtual void AdjustWindowLevel(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC) {}
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

    virtual void Render() {
        if (m_renderWindow) m_renderWindow->Render();
    }

    virtual void ResetCamera() {
        if (m_renderer) m_renderer->ResetCamera();
    }
    vtkRenderer* GetRenderer() const { return m_renderer.GetPointer(); }

    virtual void ApplyCameraStyle(VizMode mode) = 0;
    virtual void InitializeInteractor() = 0;  // 显式分离，避免 Start() 混乱
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
    virtual void SetOrientationAxesVisible(bool show) {}

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
