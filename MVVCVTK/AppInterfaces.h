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

// --- 可视化模式枚举 ---
enum class VizMode { 
    Volume, 
    IsoSurface, 
    SliceAxial,
    SliceCoronal,
    SliceSagittal,
	CompositeVolume, // 3D 体渲染 + 切片平面
	CompositeIsoSurface  // 3D 等值面 + 切片平面
};

// --- 交互工具枚举 ---
enum class ToolMode {
    Navigation,         // 默认漫游/切片浏览
    DistanceMeasure,    // 距离测量
    AngleMeasure        // 角度测量
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
    // 漫反射系数 (0.0~1.0): 决定物体接受光照后的固有颜色亮度，主要受光照角度影响
    double diffuse = 0.7;
    // 镜面反射系数 (0.0~1.0): 决定高光的亮度，值越大物体越像金属/塑料
    double specular = 0.2;
    // 高光强度 (1.0~100.0): 决定高光点的聚焦程度。值越大，光斑越小越锋利；值越小，光斑越散
    double specularPower = 10.0;
    // 全局透明度 (0.0~1.0): 0.0为全透，1.0为不透。主要用于等值面(皮肤/骨骼)的半透明效果
    double opacity = 1.0;
    // 阴影开关: true 开启
    bool shadeOn = false;
};

// 定义更新类型枚举
enum class UpdateFlags : int {
    None = 0,
    Cursor = 1 << 0,  // 仅位置改变 (0x01) 1 
    TF = 1 << 1,      // 仅颜色/透明度改变 (0x02) 2
    IsoValue = 1 << 2, // 仅阈值改变 0x04 4 
	Material = 1 << 3, // 仅材质参数改变 (0x08) 8
    Interaction = 1 << 4, // 仅交互状态改变 0x16 16
	All = Cursor | TF | IsoValue | Material // 全部改变  1 2 4 8 16 = 31
};

// --- 渲染参数结构体 ---
struct RenderParams {
    std::array<int, 3> cursor; // x, y, z
    // 传输函数纯数据
    std::vector<TFNode> tfNodes;
    // 数据标量范围 (用于将相对位置映射为真实值)
    double scalarRange[2];
    // 材质
    MaterialParams material;
    // 阈值
	double isoValue;
};

// 	AXIAL(0, 0, 1)  CORONAL(0, 1, 0)  SAGITTAL(1, 0, 0)
enum class Orientation { AXIAL = 2, CORONAL = 1, SAGITTAL = 0 };

// --- 数据管理抽象类 ---
class AbstractDataManager {
public:
    virtual ~AbstractDataManager() = default;
    virtual bool LoadData(const std::string& filePath) = 0;
    virtual vtkSmartPointer<vtkImageData> GetVtkImage() const = 0;
};

// --- 数据转换抽象类 (Template) ---
template <typename InputT, typename OutputT>
class AbstractDataConverter {
public:
    virtual ~AbstractDataConverter() = default;
    virtual vtkSmartPointer<OutputT> Process(vtkSmartPointer<InputT> input) = 0;
    virtual void SetParameter(const std::string& key, double value) {}
};

// --- 视图原子操作抽象类 ---
class AbstractVisualStrategy {
public:
    virtual ~AbstractVisualStrategy() = default;

    // 注入数据 (通用接口)
    virtual void SetInputData(vtkSmartPointer<vtkDataObject> data) = 0;
    // 原子操作：上台 (挂载到 Renderer)
    virtual void Attach(vtkSmartPointer<vtkRenderer> renderer) = 0;
    // 原子操作：下台 (从 Renderer 移除)
    virtual void Detach(vtkSmartPointer<vtkRenderer> renderer) = 0;
    // 视图专属的相机配置 (不做改变)
    virtual void SetupCamera(vtkSmartPointer<vtkRenderer> renderer) {}

    // --- 通用更新接口 ---
    // 策略根据 Params 自行决定是否更新、更新哪里
    virtual void UpdateVisuals(const RenderParams& params, UpdateFlags flags = UpdateFlags::All) {}
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; };
    virtual int GetNavigationAxis() const { return -1; }
};

// --- 服务集成抽象类 ---
class AbstractAppService {
protected:
    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;
    std::atomic<bool> m_isDirty{ false }; // 脏数据
	std::atomic<bool> m_needsSync{ false }; //  逻辑脏标记：表示 sharedState 变了，但还没同步给 strategy
	std::atomic<int> m_pendingFlags{ static_cast<int>(UpdateFlags::All) }; // 待处理的更新类型

public:
    virtual ~AbstractAppService() = default;

    virtual void Initialize(vtkSmartPointer<vtkRenderWindow> win, vtkSmartPointer<vtkRenderer> ren) {
        m_renderWindow = win;
        m_renderer = ren;
    }
    // 允许Context在渲染循环中调用此方法来同步业务逻辑,处理挂起的逻辑更新 (Lazy Update 接口)
    virtual void ProcessPendingUpdates() {};

    // 供 Context 查询状态
    bool IsDirty() const { return m_isDirty; }

    // 供 Context 重置状态
    void SetDirty(bool val) { m_isDirty = val; }

	// 供 context 标记脏数据
    void MarkDirty() { m_isDirty = true; }

	// 访问数据管理器
    std::shared_ptr<AbstractDataManager> GetDataManager() {
        return m_dataManager;
    }
    // 核心调度逻辑 (在 .cpp 中实现)
    void SwitchStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy);
};
    
// --- 抽象控制层接口 ---
class AbstractRenderContext {
protected:
    // VTK 核心渲染管线
    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;

    // 持有业务服务的基类指针 (多态)
    std::shared_ptr<AbstractAppService> m_service;

public:
    virtual ~AbstractRenderContext() = default;
    AbstractRenderContext() {
        m_renderer = vtkSmartPointer<vtkRenderer>::New();
        m_renderWindow = vtkSmartPointer<vtkRenderWindow>::New();
        m_renderWindow->AddRenderer(m_renderer);
    }

    // 绑定业务服务
    virtual void BindService(std::shared_ptr<AbstractAppService> service) {
        m_service = service;
        // 初始化 Service 内部的 VTK 对象
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

    // 抽象交互接口 mode: 告知 Context 当前进入了什么模式，Context 决定切换什么动作
    virtual void SetInteractionMode(VizMode mode) = 0;
    // 启动视窗 (Qt 模式下可能为空实现，因为 Qt 主循环接管)
    virtual void Start() = 0;

public:
    // 设置窗口大小 (像素)
    virtual void SetWindowSize(int width, int height) {
        if (m_renderWindow) m_renderWindow->SetSize(width, height);
    }

    // 设置窗口屏幕坐标 (左上角为原点)
    virtual void SetWindowPosition(int x, int y) {
        if (m_renderWindow) m_renderWindow->SetPosition(x, y);
    }

    // 设置窗口标题
    virtual void SetWindowTitle(const std::string& title) {
        if (m_renderWindow) m_renderWindow->SetWindowName(title.c_str());
    }

protected:
    // 静态回调函数转发器 (用于 VTK C-Style 回调)
    // clientData 就是在构造函数里 SetClientData(this) 传进去的指针
    static void DispatchVTKEvent(vtkObject* caller, long unsigned int eventId,
        void* clientData, void* callData) {
        auto* context = static_cast<AbstractRenderContext*>(clientData);
        if (context) {
            context->HandleVTKEvent(caller, eventId, callData);
        }
    }

    // 子类重写此方法处理具体事件
    virtual void HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData) {}
};

// --- 抽象交互服务接口 (继承自 AbstractAppService) ---
class AbstractInteractiveService : public AbstractAppService {
public:
    virtual ~AbstractInteractiveService() = default;

    // 这里放那些交互接口，默认空实现
    virtual void UpdateInteraction(int value) {}
    virtual int GetPlaneAxis(vtkActor* actor) { return -1; }

    // Context 不需要自己算坐标，直接把拾取到的世界坐标扔给 Service
    virtual void SyncCursorToWorldPosition(double worldPos[3]) {}

    // 返回标准类型，而不是具体 State 对象
    virtual std::array<int, 3> GetCursorPosition() { return { 0,0,0 }; }

    // 状态交互接口
    virtual void SetInteracting(bool val) {};
};