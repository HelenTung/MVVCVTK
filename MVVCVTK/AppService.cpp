#include "AppService.h"
#include "DataManager.h"
#include "DataConverters.h"
#include "StrategyFactory.h"

// --- 基类方法实现 ---
void AbstractAppService::SwitchStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy) {
    if (!m_renderer || !m_renderWindow) return;

    // 旧策略下台
    if (m_currentStrategy) {
        m_currentStrategy->Detach(m_renderer);
    }

    // 新策略上台
    m_currentStrategy = newStrategy;
    if (m_currentStrategy) {
        m_currentStrategy->Attach(m_renderer);
        // 让策略自己决定相机的行为 (2D平行 vs 3D透视)
        m_currentStrategy->SetupCamera(m_renderer);
    }

    m_renderer->ResetCamera();
    // m_renderWindow->Render();
    m_isDirty = true;
}

// --- 具体服务实现 ---
MedicalVizService::MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> state){
    // 实例化具体的 DataManager
    m_dataManager = dataMgr;
    m_sharedState = state; // 保存引用
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
}

void MedicalVizService::LoadFile(const std::string& path) {
    if (m_dataManager->LoadData(path)) {
		ClearCache(); // 数据变更，清空缓存
        ResetCursorCenter(); // 加载新数据时，重置坐标到中心
        UpdateAxes();
        
        if (m_dataManager->GetVtkImage()) {
            double range[2];
            m_dataManager->GetVtkImage()->GetScalarRange(range);
            m_sharedState->SetScalarRange(range[0], range[1]);
        }

        ShowIsoSurface(); // 默认显示
    }
}

void MedicalVizService::ShowVolume() {
    if (!m_dataManager->GetVtkImage()) return;
    auto strategy = GetStrategy(VizMode::Volume);
    SwitchStrategy(strategy);
    UpdateAxes();
	OnStateChanged();
}

void MedicalVizService::ShowIsoSurface() {
    if (!m_dataManager->GetVtkImage()) return;

    // 使用 Converter 进行数据处理 (Model -> Logic -> New Model)
    auto strategy = GetStrategy(VizMode::IsoSurface);
    SwitchStrategy(strategy);
    UpdateAxes();
    OnStateChanged();
}

void MedicalVizService::ShowSlice(VizMode sliceMode) {
    if (!m_dataManager->GetVtkImage()) return;
    auto strategy = GetStrategy(sliceMode);
    SwitchStrategy(strategy);
    // 2D 模式 隐藏 3D 坐标轴
    if (m_renderer) m_renderer->RemoveActor(m_cubeAxes);
    OnStateChanged();
}

void MedicalVizService::Show3DPlanes(VizMode renderMode) 
{
    if (!m_dataManager->GetVtkImage()) return;

    // 使用 Converter 进行数据处理 (Model -> Logic -> New Model)
    auto strategy = GetStrategy(renderMode);
    SwitchStrategy(strategy);
    UpdateAxes();
    OnStateChanged();
}

void MedicalVizService::UpdateInteraction(int value)
{
    if (!m_currentStrategy) return;

    // 获取当前图像维度用于边界检查
    int dims[3];
    m_dataManager->GetVtkImage()->GetDimensions(dims);

    auto sliceStrategy = std::dynamic_pointer_cast<SliceStrategy>(m_currentStrategy);
    if (sliceStrategy) {
        Orientation orient = sliceStrategy->GetOrientation();
        int axisIndex = (int)orient;

        // 调用共享状态的更新方法
        // 这里更新 state 会触发 NotifyObservers，
        // 从而导致所有窗口（包括自己）重绘
        m_sharedState->UpdateAxis(axisIndex, value, dims[axisIndex]);
    }
}

void MedicalVizService::UpdateAxes() {
    if (m_renderer && m_dataManager->GetVtkImage()) {
        m_cubeAxes->SetBounds(m_dataManager->GetVtkImage()->GetBounds());
        m_cubeAxes->SetCamera(m_renderer->GetActiveCamera());
        m_renderer->AddActor(m_cubeAxes);
    }
}

void MedicalVizService::ResetCursorCenter()
{
    auto img = m_dataManager->GetVtkImage();
    if (!img) return;
    int dims[3];
    img->GetDimensions(dims);
    m_sharedState->SetCursorPosition(dims[0] / 2, dims[1] / 2, dims[2] / 2);
}

std::shared_ptr<AbstractVisualStrategy> MedicalVizService::GetStrategy(VizMode mode)
{
    // 检查cache
	auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end())
		return it->second;

	auto strategy = StrategyFactory::CreateStrategy(mode);
	// 原始数据接口
    vtkSmartPointer<vtkImageData> rawImage = m_dataManager->GetVtkImage();

    if (mode == VizMode::IsoSurface || mode == VizMode::CompositeIsoSurface) {
        if (m_dataManager->GetVtkImage()) {
            auto converter = std::make_shared<IsoSurfaceConverter>();
            double range[2];
            m_dataManager->GetVtkImage()->GetScalarRange(range);
            converter->SetParameter("IsoValue", range[0] + (range[1] - range[0]) * 0.4);
            auto polyMesh = converter->Process(m_dataManager->GetVtkImage());
            strategy->SetInputData(polyMesh);
        }
    }
    else {
        // CompositeVolume、Volume 和 Slice 直接吃 vtkImage
        if (m_dataManager->GetVtkImage()) {
            strategy->SetInputData(m_dataManager->GetVtkImage());
        }
    }
    
	// 如果是 CompositeStrategy，还需要设置原始image作为参考
    auto compositeStrategy = std::dynamic_pointer_cast<CompositeStrategy>(strategy);
    if (mode == VizMode::CompositeVolume || mode == VizMode::CompositeIsoSurface)
    {
        if (compositeStrategy && rawImage) {
            // 无论主视图显示什么，背景切片永远需要原始 Image
            compositeStrategy->SetReferenceData(rawImage);
        }
    }

    // 存入缓存
    m_strategyCache[mode] = strategy;
    return strategy;
}

void MedicalVizService::ClearCache()
{
    m_strategyCache.clear();
    m_currentStrategy = nullptr;
    if (m_renderer) {
        m_renderer->RemoveAllViewProps();
    }
}

void MedicalVizService::OnStateChanged() {
    // 只有当有策略时才执行
    if (!m_currentStrategy) return;

    // 将 SharedState (业务对象) 转换为 RenderParams (纯数据对象)
    RenderParams params;

    // 获取位置
    int* pos = m_sharedState->GetCursorPosition();
    params.cursor = { pos[0], pos[1], pos[2] }; // std::array 赋值

    // 获取 TF
    params.colorTF = m_sharedState->GetColorTF();
    params.opacityTF = m_sharedState->GetOpacityTF();

    // 泛型调用
    m_currentStrategy->UpdateVisuals(params);
    
    // 触发渲染
    //if (m_renderWindow) m_renderWindow->Render();
    m_isDirty = true;
}

int MedicalVizService::GetPlaneAxis(vtkActor* actor) {
    // 利用多态调用当前策略的接口
    auto compositeStrategy = std::dynamic_pointer_cast<CompositeStrategy>(m_currentStrategy);
    if (m_currentStrategy) {
        return compositeStrategy->GetPlaneAxis(actor);
    }
    return -1;
}

vtkSmartPointer<vtkTable> MedicalVizService::GetHistogramData(int binCount) {
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return nullptr;

    // 实例化转换器
    auto converter = std::make_shared<HistogramConverter>();

    // 设置参数
    converter->SetParameter("BinCount", (double)binCount);

    // 执行处理 (Model -> Logic -> Output Model)
    auto table = converter->Process(m_dataManager->GetVtkImage());

    return table;
}

void MedicalVizService::SaveHistogramImage(const std::string& filePath, int binCount)
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;

    // 实例化转换器
    auto converter = std::make_shared<HistogramConverter>();

    // 设置参数
    converter->SetParameter("BinCount", (double)binCount);

    // 设置参数
    converter->SaveHistogramImage(m_dataManager->GetVtkImage(),filePath);
}
