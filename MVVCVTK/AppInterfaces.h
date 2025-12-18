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
enum class VizMode { Volume, IsoSurface, AxialSlice };

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
    // 扩展：处理交互 (如切片索引)
    virtual void SetInteractionValue(int value) {}
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

    // 核心调度逻辑 (在 .cpp 中实现)
    void SwitchStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy);
};