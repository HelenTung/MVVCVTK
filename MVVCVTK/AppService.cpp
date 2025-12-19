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
    m_renderWindow->Render();
}

// --- 具体服务实现 ---
MedicalVizService::MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr) {
    // 实例化具体的 DataManager
    m_dataManager = dataMgr;
    m_cubeAxes = vtkSmartPointer<vtkCubeAxesActor>::New();
}

void MedicalVizService::LoadFile(const std::string& path) {
    if (m_dataManager->LoadData(path)) {
		ClearCache(); // 数据变更，清空缓存
        UpdateAxes();
        ShowIsoSurface(); // 默认显示
    }
}

void MedicalVizService::ShowVolume() {
    if (!m_dataManager->GetVtkImage()) return;
    auto strategy = GetStrategy(VizMode::Volume);
    SwitchStrategy(strategy);
    UpdateAxes();
}

void MedicalVizService::ShowIsoSurface() {
    if (!m_dataManager->GetVtkImage()) return;

    // 使用 Converter 进行数据处理 (Model -> Logic -> New Model)
    auto strategy = GetStrategy(VizMode::IsoSurface);
    SwitchStrategy(strategy);
    UpdateAxes();
}

void MedicalVizService::ShowSliceAxial() {
    if (!m_dataManager->GetVtkImage()) return;
    auto strategy = GetStrategy(VizMode::AxialSlice);
    SwitchStrategy(strategy);
    // 2D 模式 隐藏 3D 坐标轴
    if (m_renderer) m_renderer->RemoveActor(m_cubeAxes);
}

void MedicalVizService::UpdateInteraction(int value)
{
	auto it = m_strategyCache.find(VizMode::AxialSlice);
    if (it != m_strategyCache.end()) {
        auto sliceStrategy = std::dynamic_pointer_cast<SliceStrategy>(it->second);
        if (sliceStrategy) {
            sliceStrategy->SetInteractionValue(value);
            SwitchStrategy(sliceStrategy);
        }
	}
}

void MedicalVizService::UpdateSliceOrientation(Orientation orient)
{
    auto strategy = GetStrategy(VizMode::AxialSlice);
    auto sliceStrategy = std::dynamic_pointer_cast<SliceStrategy>(strategy);
    if (sliceStrategy) {
        sliceStrategy->SetOrientation(orient);
		SwitchStrategy(sliceStrategy);
    }
}

void MedicalVizService::UpdateAxes() {
    if (m_renderer && m_dataManager->GetVtkImage()) {
        m_cubeAxes->SetBounds(m_dataManager->GetVtkImage()->GetBounds());
        m_cubeAxes->SetCamera(m_renderer->GetActiveCamera());
        m_renderer->AddActor(m_cubeAxes);
    }
}

std::shared_ptr<AbstractVisualStrategy> MedicalVizService::GetStrategy(VizMode mode)
{
    // 检查cache
	auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end())
		return it->second;

	auto strategy = StrategyFactory::CreateStrategy(mode);
    if (mode == VizMode::IsoSurface) {
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
        // Volume 和 Slice 直接吃 vtkImage
        if (m_dataManager->GetVtkImage()) {
            strategy->SetInputData(m_dataManager->GetVtkImage());
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

// vtk vtkren