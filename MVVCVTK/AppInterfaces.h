#pragma once

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vector>
#include <memory>
#include <string>

// --- 可视化模式枚举 ---
enum class VizMode { 
    Volume, 
    IsoSurface, 
    AxialSlice, 
	CompositeVolume, // 3D 体渲染 + 切片平面
	CompositeIsoSurface  // 3D 等值面 + 切片平面
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
};

// --- 服务集成抽象类 ---
class AbstractAppService {
protected:
    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::shared_ptr<AbstractVisualStrategy> m_currentStrategy;
    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkRenderWindow> m_renderWindow;

public:
    virtual ~AbstractAppService() = default;

    virtual void Initialize(vtkSmartPointer<vtkRenderWindow> win, vtkSmartPointer<vtkRenderer> ren) {
        m_renderWindow = win;
        m_renderer = ren;
    }
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

    // 抽象交互接口 (子类需实现具体的交互器逻辑) mode: 告知 Context 当前进入了什么模式，Context 决定切换什么动作
    virtual void SetInteractionMode(VizMode mode) = 0;
    // 启动视窗 (Qt 模式下可能为空实现，因为 Qt 主循环接管)
    virtual void Start() = 0;

protected:
    // 静态回调函数转发器 (用于 VTK C-Style 回调)
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