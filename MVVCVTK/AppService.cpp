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
}

void MedicalVizService::Initialize(vtkSmartPointer<vtkRenderWindow> win, vtkSmartPointer<vtkRenderer> ren) {
    // 初始化
    AbstractAppService::Initialize(win, ren);

    // 注册到 SharedState shared_from_this() 作为存活凭证 Lambda 回调
    if (m_sharedState) {
        // 获取 weak_ptr 供 Lambda 内部安全使用
        std::weak_ptr<MedicalVizService> weakSelf = std::static_pointer_cast<MedicalVizService>(shared_from_this());

        m_sharedState->AddObserver(shared_from_this(), [weakSelf](UpdateFlags flags) {
            // Lambda 内部标准写法：先 lock 再用
            if (auto self = weakSelf.lock()) {
                int oldVal = self->m_pendingFlags.load();
				while (!self->m_pendingFlags.compare_exchange_weak(oldVal, oldVal | static_cast<int>(flags))); // 位或更新待处理标记
                self->OnStateChanged();
            }
        });
    }
}

void MedicalVizService::LoadFile(const std::string& path) {
    if (m_dataManager->LoadData(path)) {
		ClearCache(); // 数据变更，清空缓存
        ResetCursorCenter(); // 加载新数据时，重置坐标到中心
        
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
	OnStateChanged();
}

void MedicalVizService::ShowIsoSurface() {
    if (!m_dataManager->GetVtkImage()) return;

    // 使用 Converter 进行数据处理 (Model -> Logic -> New Model)
    auto strategy = GetStrategy(VizMode::IsoSurface);
    SwitchStrategy(strategy);
    OnStateChanged();
}

void MedicalVizService::ShowSlice(VizMode sliceMode) {
    if (!m_dataManager->GetVtkImage()) return;
    auto strategy = GetStrategy(sliceMode);
    SwitchStrategy(strategy);
    OnStateChanged();
}

void MedicalVizService::Show3DPlanes(VizMode renderMode) 
{
    if (!m_dataManager->GetVtkImage()) return;

    // 使用 Converter 进行数据处理 (Model -> Logic -> New Model)
    auto strategy = GetStrategy(renderMode);
    SwitchStrategy(strategy);
    OnStateChanged();
}

void MedicalVizService::UpdateInteraction(int value)
{
    if (!m_currentStrategy) return;

    // 获取当前图像维度用于边界检查
    int dims[3];
    m_dataManager->GetVtkImage()->GetDimensions(dims);

    int axisIndex = m_currentStrategy->GetNavigationAxis();
    if (axisIndex != -1)
        m_sharedState->UpdateAxis(axisIndex, value, dims[axisIndex]);

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
    

    if (mode == VizMode::CompositeVolume || mode == VizMode::CompositeIsoSurface)
    {   
        // 多态方法
		strategy->SetInputData(rawImage);
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
	m_needsSync = true;
}

int MedicalVizService::GetPlaneAxis(vtkActor* actor) {
    if (m_currentStrategy) {
        return m_currentStrategy->GetPlaneAxis(actor);
    }
    return -1;
}

void MedicalVizService::SyncCursorToWorldPosition(double worldPos[3]) {
    // 获取数据元信息
    auto img = m_dataManager->GetVtkImage();
    if (!img) return;

    double origin[3], spacing[3];
    img->GetOrigin(origin);
    img->GetSpacing(spacing);
    int dims[3];
    img->GetDimensions(dims);

    // 执行坐标转换逻辑 (原本在 Context 里的代码挪到这里)
    int i = std::round((worldPos[0] - origin[0]) / spacing[0]);
    int j = std::round((worldPos[1] - origin[1]) / spacing[1]);
    int k = std::round((worldPos[2] - origin[2]) / spacing[2]);

    // 边界检查
    if (i < 0) i = 0; if (i >= dims[0]) i = dims[0] - 1;
    if (j < 0) j = 0; if (j >= dims[1]) j = dims[1] - 1;
    if (k < 0) k = 0; if (k >= dims[2]) k = dims[2] - 1;

    // 更新内部 State
    m_sharedState->SetCursorPosition(i, j, k);
}

void MedicalVizService::ProcessPendingUpdates()
{
    if (!m_needsSync || !m_currentStrategy) return;

	// 获取并清空待处理标记
    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    // 如果没有任何更新标志,直接返回
    if (flags == UpdateFlags::None) {
        m_needsSync = false;
        return;
    }

    // 将 SharedState业务对象转换为 RenderParams纯数据对象
    // 避免持有复杂的业务逻辑引用
    RenderParams params;

    // 获取位置
    int* pos = m_sharedState->GetCursorPosition();
    params.cursor = { pos[0], pos[1], pos[2] }; // std::array 赋值

    // 获取 TF
    params.tfNodes = m_sharedState->GetTFNodes();
    auto* range = m_sharedState->GetDataRange();
    params.scalarRange[0] = range[0];
    params.scalarRange[1] = range[1];

    // 泛型调用
    m_currentStrategy->UpdateVisuals(params, flags);

    // 逻辑同步完成，现在标记渲染层脏了
    m_isDirty = true;
	m_needsSync = false;
}

std::array<int, 3> MedicalVizService::GetCursorPosition() {
    int* pos = m_sharedState->GetCursorPosition();
    return { pos[0], pos[1], pos[2] };
}