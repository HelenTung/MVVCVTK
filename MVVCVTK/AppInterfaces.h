#pragma once

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <array>
#include <thread>
#include <functional>

// --- 可视化模式枚举 ---
enum class VizMode {
    Volume,
    IsoSurface,
    SliceAxial,
    SliceCoronal,
    SliceSagittal,
    CompositeVolume,        // 3D 体渲染 + 切片平面
    CompositeIsoSurface     // 3D 等值面 + 切片平面
};

// --- 交互工具枚举 ---
enum class ToolMode {
    Navigation,         // 默认漫游/切片浏览
    DistanceMeasure,    // 距离测量
    AngleMeasure,       // 角度测量
    ModelTransform      // 模型变换（旋转/缩放/平移）
};

// --- 传输函数节点结构体 ---
struct TFNode {
    double position; // 0.0 - 1.0 (相对位置)
    double opacity;  // 0.0 - 1.0
    double r, g, b;  // 颜色
};

struct MaterialParams {
    // 环境光系数 (0.0~1.0): 决定阴影区域的最低亮度，值越大阴影越亮
    double ambient = 0.1;
    // 漫反射系数 (0.0~1.0): 决定物体接受光照后的固有颜色亮度
    double diffuse = 0.7;
    // 镜面反射系数 (0.0~1.0): 决定高光的亮度
    double specular = 0.2;
    // 高光强度 (1.0~100.0): 决定高光点的聚焦程度
    double specularPower = 10.0;
    // 全局透明度 (0.0~1.0): 0.0 为全透，1.0 为不透
    double opacity = 1.0;
    // 阴影开关
    bool   shadeOn = false;
};

// --- 更新类型位掩码（可组合）---
enum class UpdateFlags : int {
    None = 0,
    Cursor = 1 << 0,  // 仅位置改变 (0x01)
    TF = 1 << 1,  // 仅颜色/透明度改变 (0x02)
    IsoValue = 1 << 2,  // 仅阈值改变 (0x04)
    Material = 1 << 3,  // 仅材质参数改变 (0x08)
    Interaction = 1 << 4,  // 仅交互状态改变 (0x10)
    Transform = 1 << 5,  // 变换矩阵改变 (0x20)
    DataReady = 1 << 6,  // 数据加载完成，需要重建渲染管线
    All = Cursor | TF | IsoValue | Material | Interaction | Transform
};

// --- 渲染参数结构体（Strategy 的唯一输入，不含 VTK 指针）---
struct RenderParams {
    std::array<int, 3>     cursor = { 0, 0, 0 };  // 光标体素坐标
    std::vector<TFNode>    tfNodes;                      // 传输函数节点
    double                 scalarRange[2] = { 0, 255 }; // 数据标量范围
    MaterialParams         material;                     // 材质参数
    double                 isoValue = 0.0;            // 等值面阈值
    std::array<double, 16> modelMatrix = {               // 模型变换矩阵
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
};

// 切片朝向枚举（AXIAL=2/Z, CORONAL=1/Y, SAGITTAL=0/X）
// AXIAL(0,0,1)  CORONAL(0,1,0)  SAGITTAL(1,0,0)
enum class Orientation { AXIAL = 2, CORONAL = 1, SAGITTAL = 0 };

class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;
    virtual bool LoadData(const std::string& filePath) = 0;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;

    // callback(success) 在后台线程回调，调用方需自行做线程安全处理
    virtual void AsyncLoadData(const std::string& filePath,
        std::function<void(bool success)> callback) {
        std::thread([this, filePath, callback]() {
            bool ok = this->LoadData(filePath);
            if (callback) callback(ok);
            }).detach();
    }

    // 查询是否正在加载
    bool IsLoading() const { return m_isLoading.load(); }

protected:
    std::atomic<bool> m_isLoading{ false };
};

template <typename InputT, typename OutputT>
class AbstractDataConverter {
public:
    virtual ~AbstractDataConverter() = default;
    virtual vtkSmartPointer<OutputT> Process(vtkSmartPointer<InputT> input) = 0;
    virtual void SetParameter(const std::string& key, double value) {}
};

class AbstractVisualStrategy {
public:
    virtual ~AbstractVisualStrategy() = default;

    // 注入数据（通用接口）
    virtual void SetInputData(vtkSmartPointer<vtkDataObject> data) = 0;
    // 原子操作：上台（挂载到 Renderer）
    virtual void Attach(vtkSmartPointer<vtkRenderer> renderer) = 0;
    // 原子操作：下台（从 Renderer 移除）
    virtual void Detach(vtkSmartPointer<vtkRenderer> renderer) = 0;
    // 视图专属的相机配置（默认空实现）
    virtual void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) {}

    // 策略根据 Params 自行决定是否更新、更新哪里
    virtual void UpdateVisuals(const RenderParams& params,
        UpdateFlags flags = UpdateFlags::All) {
    }
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; }
    virtual int GetNavigationAxis() const { return -1; }
    // 获取当前策略的主渲染对象（用于模型变换）
    virtual vtkProp3D* GetMainProp() { return nullptr; }
};


class AbstractAppService {
protected:
    std::shared_ptr<AbstractDataManager>    m_dataManager;
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    vtkSmartPointer<vtkRenderer>            m_renderer;
    vtkSmartPointer<vtkRenderWindow>        m_renderWindow;
    std::atomic<bool> m_isDirty{ false };                              // 渲染脏标记
    std::atomic<bool> m_needsSync{ false };                              // 逻辑脏标记
    std::atomic<int>  m_pendingFlags{ static_cast<int>(UpdateFlags::All) }; // 待处理更新类型

public:
    virtual ~AbstractAppService() = default;

    virtual void Initialize(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren)
    {
        m_renderWindow = win;
        m_renderer = ren;
    }

    // 允许 Context 在渲染循环中调用此方法同步业务逻辑（Lazy Update 入口）
    virtual void ProcessPendingUpdates() {}

    // 供 Context 查询渲染脏标记
    bool IsDirty() const { return m_isDirty; }

    // 供 Context 重置渲染脏标记（与原版保持一致，StdRenderContext 使用 SetDirty(false)）
    void SetDirty(bool val) { m_isDirty = val; }

    // 供 Context / Service 内部标记需要渲染
    void MarkDirty() { m_isDirty = true; }

    // 供 Context 访问数据管理器
    std::shared_ptr<AbstractDataManager> GetDataManager() { return m_dataManager; }

    // 核心 Strategy 切换（在 AppService.cpp 中实现）
    void SwitchStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy);
};

// ─────────────────────────────────────────────────────────────────────
// IPreInitService — 前处理接口
//
// 职责：数据到达之前，登记所有与数据无关的配置意图。
//   • 所有方法只写 SharedState（内部有 mutex），零 VTK 操作
//   • 可在 BindService 之后、LoadFileAsync 之前的任意时刻调用
//   • MedicalVizService 同时继承此接口和 AbstractInteractiveService
//
// 设计原则：
//   • 纯虚接口，不引入任何成员变量，不影响现有继承链
//   • 调用方可通过 IPreInitService* 使用前处理接口，
//     与 AbstractInteractiveService* 的交互接口完全分离
// ─────────────────────────────────────────────────────────────────────
class IPreInitService {
public:
    virtual ~IPreInitService() = default;

    // 登记目标可视化模式（DataReady 后按此模式重建 VTK 管线）
    virtual void PreInit_SetVizMode(VizMode mode) = 0;

    // 写入光照/材质参数 → SharedState::Material（触发 UpdateFlags::Material）
    virtual void PreInit_SetLuxParams(double ambient, double diffuse,
        double specular, double power, bool shadeOn = false) = 0;

    // 写入全局透明度 → MaterialParams.opacity
    virtual void PreInit_SetOpacity(double opacity) = 0;

    // 写入体渲染传输函数节点（触发 UpdateFlags::TF）
    virtual void PreInit_SetTransferFunction(const std::vector<TFNode>& nodes) = 0;

    // 写入等值面阈值（触发 UpdateFlags::IsoValue，含微小变化检测）
    virtual void PreInit_SetIsoThreshold(double val) = 0;
};

class AbstractRenderContext {
protected:
    vtkSmartPointer<vtkRenderer>     m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;
    std::shared_ptr<AbstractAppService> m_service;

public:
    virtual ~AbstractRenderContext() = default;
    AbstractRenderContext() {
        m_renderer = vtkSmartPointer<vtkRenderer>::New();
        m_renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
        m_renderWindow->AddRenderer(m_renderer);
    }

    // 绑定业务服务，触发 Initialize
    virtual void BindService(std::shared_ptr<AbstractAppService> service) {
        m_service = service;
        if (m_service) {
            m_service->Initialize(m_renderWindow, m_renderer);
        }
    }

    // 核心渲染接口
    virtual void Render() {
        if (m_renderWindow) m_renderWindow->Render();
    }

    virtual void ResetCamera() {
        if (m_renderer) m_renderer->ResetCamera();
    }

    // 告知 Context 当前进入了什么模式，Context 决定切换什么交互器
    virtual void SetInteractionMode(VizMode mode) = 0;

    // 启动视窗消息循环
    virtual void Start() = 0;

    // 设置窗口大小（像素）
    virtual void SetWindowSize(int width, int height) {
        if (m_renderWindow) m_renderWindow->SetSize(width, height);
    }

    // 设置窗口屏幕坐标（左上角为原点）
    virtual void SetWindowPosition(int x, int y) {
        if (m_renderWindow) m_renderWindow->SetPosition(x, y);
    }

    // 设置窗口标题
    virtual void SetWindowTitle(const std::string& title) {
        if (m_renderWindow) m_renderWindow->SetWindowName(title.c_str());
    }

    // 开关世界坐标系显示
    virtual void ToggleOrientationAxes(bool show) {}

protected:
    // 静态 VTK 事件转发器（clientData = this 指针）
    // StdRenderContext 构造函数中用 SetClientData(this) 注册
    static void DispatchVTKEvent(vtkObject* caller, long unsigned int eventId,
        void* clientData, void* callData)
    {
        auto* context = static_cast<AbstractRenderContext*>(clientData);
        if (context) context->HandleVTKEvent(caller, eventId, callData);
    }

    // 子类重写此方法处理具体 VTK 事件（StdRenderContext 覆盖）
    virtual void HandleVTKEvent(vtkObject* caller,
        long unsigned int eventId, void* callData) {
    }
};

class AbstractInteractiveService : public AbstractAppService {
public:
    virtual ~AbstractInteractiveService() = default;

    // 滚轮/键盘切片导航
    virtual void UpdateInteraction(int value) {}

    // 获取切片平面轴（Context 拾取 Actor 后调用）
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; }

    // 将世界坐标同步为光标位置（axis=-1 全轴更新，0/1/2 单轴更新）
    virtual void SyncCursorToWorldPosition(double worldPos[3], int axis = -1) {}

    // 获取当前光标体素坐标
    virtual std::array<int, 3> GetCursorPosition() { return { 0, 0, 0 }; }

    // 设置交互状态（拖拽中 = true，停止 = false）
    virtual void SetInteracting(bool val) {}

    // 获取主渲染 Prop（供坐标变换使用）
    virtual vtkProp3D* GetMainProp() { return nullptr; }

    // 将 VTK 变换矩阵回写到 SharedState
    virtual void SyncModelMatrix(vtkMatrix4x4* mat) {}
};