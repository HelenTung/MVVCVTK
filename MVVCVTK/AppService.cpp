// =====================================================================
// AppService.cpp
// =====================================================================
#include "AppService.h"
#include "DataManager.h"
#include "DataConverters.h"
#include "StrategyFactory.h"

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService 基类方法（保持原样）
// ───────────────────���─────────────────────────────────────────────────
void AbstractAppService::SwitchStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy)
{
    if (!m_renderer || !m_renderWindow) return;

    if (m_currentStrategy)
        m_currentStrategy->Detach(m_renderer);

    m_currentStrategy = newStrategy;
    if (m_currentStrategy) {
        m_currentStrategy->Attach(m_renderer);
        m_currentStrategy->SetupCamera(m_renderer);
    }

    m_renderer->ResetCamera();
    m_isDirty = true;
}

// ─────────────────────────────────────────────────────────────────────
// MedicalVizService：构造
//
// 【为什么在构造里创建 VolumeTransformService 而不是 Initialize】
//   构造完成后对象即可使用坐标变换（不依赖 Renderer），
//   而 Initialize 在 BindService 时触发，晚于构造。
//   让构造尽可能完整，减少"半初始化"对象的风险。
// ──���──────────────────────────────────────────────────────────────────
MedicalVizService::MedicalVizService(
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> state)
{
    m_dataManager = dataMgr;
    m_sharedState = state;

    // 创建坐标变换子服务
    // 【为什么用 unique_ptr 而不是 shared_ptr】
    //   VolumeTransformService 的生命周期完全由本类管理，
    //   没有其他对象需要共同持有它，unique_ptr 语义更准确，
    //   也避免了不必要的引用计数开销。
    m_transformService = std::make_unique<VolumeTransformService>(state);
}

// ─────────────────────────────────────────────────────────────────────
// Initialize：绑定 VTK 渲染管线，注册 SharedState 观察者
//
// 【为什么这里才注册 Observer 而不是在构造里】
//   shared_from_this() 只有在 shared_ptr 完全构建后才能使用。
//   构造函数执行期间 shared_ptr 尚未创建，调用 shared_from_this()
//   会抛出 bad_weak_ptr。Initialize 由 BindService 调用，
//   此时 shared_ptr 已经存在，可以安全使用。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::Initialize(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    AbstractAppService::Initialize(win, ren);

    if (!m_sharedState) return;

    // 获取 weak_ptr：Lambda 捕获 weak_ptr 而非 shared_ptr，
    // 【为什么用 weak_ptr】
    //   如果 Lambda 捕获 shared_ptr，则 SharedState 的 observer 列表
    //   会持有 Service 的引用，形成循环引用（Service -> State -> Service），
    //   导致两者都无法被析构，内存泄漏。
    //   weak_ptr 不增加引用计数，lock() 时如果对象已销毁返回 nullptr，
    //   自然实现安全的生命周期管理。
    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

    m_sharedState->AddObserver(shared_from_this(), [weakSelf](UpdateFlags flags) {
        auto self = weakSelf.lock();
        if (!self) return; // Service 已销毁，忽略

        if ((int)flags & (int)UpdateFlags::DataReady) {
            // ── 数据就绪：标记需要重建管线 ──────────────────────────
            // 【为什么只标记，不直接调 RebuildPipeline】
            //   此 Lambda 在后台线程（或任何 NotifyObservers 调用方的线程）执行，
            //   VTK 渲染管线操作（Attach/Detach/SetInputData 等）不是线程安全的，
            //   必须在主线程（渲染循环的心跳 Timer 里）执行。
            //   标记 m_needsDataRefresh = true，由主线程的 ProcessPendingUpdates 检测并执行。
            self->ClearStrategyCache();
            self->ResetCursorToCenter();
            self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
            self->m_needsDataRefresh = true;
            // 注意：不调 MarkNeedsSync()，DataReady 走独立的重建路径，
            // 两条路径不能混用，否则 ProcessPendingUpdates 里的条件判断会乱。
            return;
        }

        // ── 普通事件：位或更新待处理标记，标记需要同步 ────────────
        // 【为什么用 CAS 循环而不是直接赋值】
        //   多个 Service 可能同时收到不同 flags 的通知，用 compare_exchange_weak
        //   的 CAS 循环保证原子性地把新 flag 合并进去，不会互相覆盖。
        int oldVal = self->m_pendingFlags.load();
        while (!self->m_pendingFlags.compare_exchange_weak(
            oldVal, oldVal | static_cast<int>(flags)));
        self->MarkNeedsSync();
        });
}

// ─────────────────────────────────────────────────────────────────────
// 前处理接口实现
// ─────────────────────────────────────────────────────────────────────

void MedicalVizService::PreInit_SetVizMode(VizMode mode)
{
    // 只记录意图，不执行任何 VTK 操作
    // 数据到达后 PostData_RebuildPipeline 会按此模式重建管线
    m_pendingVizMode = mode;
}

void MedicalVizService::PreInit_SetLuxParams(
    double ambient, double diffuse, double specular, double power, bool shadeOn)
{
    // 读-改-写 SharedState（SetMaterial 内部加锁，线程安全）
    // 触发 UpdateFlags::Material，所有订阅者收到通知后更新光照
    auto mat = m_sharedState->GetMaterial();
    mat.ambient = ambient;
    mat.diffuse = diffuse;
    mat.specular = specular;
    mat.specularPower = power;
    mat.shadeOn = shadeOn;
    m_sharedState->SetMaterial(mat);
}

void MedicalVizService::PreInit_SetOpacity(double opacity)
{
    auto mat = m_sharedState->GetMaterial();
    mat.opacity = opacity;
    m_sharedState->SetMaterial(mat);
}

void MedicalVizService::PreInit_SetTransferFunction(const std::vector<TFNode>& nodes)
{
    // SetTFNodes 内部加锁，触发 UpdateFlags::TF 广播
    m_sharedState->SetTFNodes(nodes);
}

void MedicalVizService::PreInit_SetIsoThreshold(double val)
{
    // SetIsoValue 内部有微小变化检测（> 0.0001），避免无效广播
    m_sharedState->SetIsoValue(val);
}

// ─────────────────────────────────────────────────────────────────────
// LoadFileAsync：发起异步加载
//
// 【线程安全分析】
//   1. AsyncLoadData 在独立线程执行 LoadData（阻塞 I/O）
//   2. 完成后 Lambda 在该后台线程执行：
//      a. weakSelf.lock() 检查 Service 是否存活
//      b. GetVtkImage() 内部有 mutex（RawVolumeDataManager::m_mutex），安全
//      c. GetScalarRange() 是 VTK 纯读操作，安全
//      d. NotifyDataReady() 内部有 mutex，安全
//      e. onComplete 在 weakSelf 存活保证下执行，外部只允许写 SharedState
//
// 【为什么 onComplete 在 lock 内部调用】
//   onComplete 在 lock 外调用，若 Service 已销毁则 onComplete
//   捕获的外部引用（如 &sharedState）可能悬空。
//   移入 lock 内部后，self 存活保证了整个回调执行期间对象有效。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::LoadFileAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

    m_dataManager->AsyncLoadData(path, [weakSelf, onComplete](bool success) {
        // 后台线程：只做线程安全操作
        if (auto self = weakSelf.lock()) {
            if (success) {
                if (auto img = self->m_dataManager->GetVtkImage()) {
                    double range[2];
                    img->GetScalarRange(range);
                    // 广播 DataReady，所有订阅了 SharedState 的 Service 都会收到
                    self->m_sharedState->NotifyDataReady(range[0], range[1]);
                }
            }
            // onComplete 在 self 存活期间执行，避免悬空引用
            if (onComplete) onComplete(success);
        }
        // self 已销毁时：不调 onComplete，安全退出
        });
}

// ─────────────────────────────────────────────────────────────────────
// ProcessPendingUpdates：主线程心跳定时器驱动的更新入口
// 【为什么是"入口"而不是"实现"】
//   ProcessPendingUpdates 只负责"路由"：判断走哪条路径，
//   具体逻辑在 PostData_RebuildPipeline / PostData_SyncStateToStrategy。
//   这样每条路径的职责清晰，可以独立阅读和修改。
//
//   DataReady（重建管线）优先级高于普通同步，
//   同一帧内若两者都触发，先做重建，下一帧再做同步。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ProcessPendingUpdates()
{
    // ── 路径 A：数据就绪，重建渲染管线（优先执行）──────────────────
    if (m_needsDataRefresh.exchange(false)) {
        PostData_RebuildPipeline();
        return; // 本帧只做重建，增量同步留到下帧
    }

    // ── 路径 B：普通状态增量同步 ─────────────────────────────────
    PostData_SyncStateToStrategy();
}

void MedicalVizService::PostData_RebuildPipeline()
{
    // 此方法只在主线程（Timer 心跳）调用，VTK 管线操作安全
    auto strategy = GetOrCreateStrategy(m_pendingVizMode);
    if (!strategy) return;

    SwitchStrategy(strategy);
    MarkNeedsSync(); // 重建完成后，下一帧还需要做一次全量参数同步
}

void MedicalVizService::PostData_SyncStateToStrategy()
{
    if (!m_needsSync || !m_currentStrategy) return;

    // 取出并清空待处理标记（原子操作，防止丢标记）
    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    if (flags == UpdateFlags::None) {
        m_needsSync = false;
        return;
    }

    // 调整渲染帧率：交互中降采样提升流畅度，停止交互恢复高质量
    if ((int)flags & (int)UpdateFlags::Interaction) {
        if (m_renderWindow) {
            bool interacting = m_sharedState->IsInteracting();
            // 15 FPS = 拖拽时降采样保流畅；0.001 FPS = 停止时恢复高质量
            m_renderWindow->SetDesiredUpdateRate(interacting ? 15.0 : 0.001);
        }
    }

    RenderParams params = BuildRenderParams(flags);
    m_currentStrategy->UpdateVisuals(params, flags);

    m_isDirty = true;  // 通知 Context 本帧需要 Render()
    m_needsSync = false;
}

RenderParams MedicalVizService::BuildRenderParams(UpdateFlags flags) const
{
    RenderParams params;

    // 位置（每帧都需要，切片策略依赖 cursor 位置）
    auto pos = m_sharedState->GetCursorPosition();
    params.cursor = { pos[0], pos[1], pos[2] };

    // 标量范围（用于 TF 归一化映射）
    auto range = m_sharedState->GetDataRange();
    params.scalarRange[0] = range[0];
    params.scalarRange[1] = range[1];

    // 材质（光照、透明度）
    params.material = m_sharedState->GetMaterial();

    // 传输函数（仅在 TF 标记时才拷贝，减少不必要的 vector 拷贝）
    if ((int)flags & (int)UpdateFlags::TF)
        m_sharedState->GetTFNodes(params.tfNodes);

    // 等值面阈值
    if ((int)flags & (int)UpdateFlags::IsoValue)
        params.isoValue = m_sharedState->GetIsoValue();

    // 模型变换矩阵
    if ((int)flags & (int)UpdateFlags::Transform)
        params.modelMatrix = m_sharedState->GetModelMatrix();

    return params;
}

std::shared_ptr<AbstractVisualStrategy>
MedicalVizService::GetOrCreateStrategy(VizMode mode)
{
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end())
        return it->second;

    auto strategy = StrategyFactory::CreateStrategy(mode);
    if (!strategy) return nullptr;

    // 数据注入：此时数据一定存在（PostData_RebuildPipeline 调用时）
    auto rawImage = m_dataManager->GetVtkImage();
    if (rawImage)
        strategy->SetInputData(rawImage);

    m_strategyCache[mode] = strategy;
    return strategy;
}

void MedicalVizService::ClearStrategyCache()
{
    m_strategyCache.clear();
    m_currentStrategy = nullptr;
}


void MedicalVizService::ResetCursorToCenter()
{
    auto img = m_dataManager->GetVtkImage();
    if (!img) return;
    int dims[3];
    img->GetDimensions(dims);
    m_sharedState->SetCursorPosition(dims[0] / 2, dims[1] / 2, dims[2] / 2);
}

void MedicalVizService::MarkNeedsSync()
{
    m_needsSync = true;
}

// ─────────────────────────────────────────────────────────────────────
// 交互接口实现
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::UpdateInteraction(int value)
{
    if (!m_currentStrategy) return;

    auto img = m_dataManager->GetVtkImage();
    if (!img) return;

    int dims[3];
    img->GetDimensions(dims);

    int axisIndex = m_currentStrategy->GetNavigationAxis();
    if (axisIndex != -1)
        m_sharedState->UpdateAxis(axisIndex, value, dims[axisIndex]);
}

void MedicalVizService::SyncCursorToWorldPosition(double worldPos[3], int axis)
{
    auto img = m_dataManager->GetVtkImage();
    if (!img) return;

    // 世界坐标先逆变换回模型坐标（考虑模型变换矩阵）
    double modelPos[3];
    WorldToModel(worldPos, modelPos);

    double origin[3], spacing[3];
    img->GetOrigin(origin);
    img->GetSpacing(spacing);
    int dims[3];
    img->GetDimensions(dims);

    // 将物理坐标转换为体素索引
    // 【注意】这里用 worldPos 而不是 modelPos，因为 Origin/Spacing 在 World Space
    int i = static_cast<int>(std::round((worldPos[0] - origin[0]) / spacing[0]));
    int j = static_cast<int>(std::round((worldPos[1] - origin[1]) / spacing[1]));
    int k = static_cast<int>(std::round((worldPos[2] - origin[2]) / spacing[2]));

    // 边界裁剪
    i = std::max(0, std::min(i, dims[0] - 1));
    j = std::max(0, std::min(j, dims[1] - 1));
    k = std::max(0, std::min(k, dims[2] - 1));

    auto cur = m_sharedState->GetCursorPosition();

    if (axis == 0)       m_sharedState->SetCursorPosition(i, cur[1], cur[2]);
    else if (axis == 1)  m_sharedState->SetCursorPosition(cur[0], j, cur[2]);
    else if (axis == 2)  m_sharedState->SetCursorPosition(cur[0], cur[1], k);
    else                 m_sharedState->SetCursorPosition(i, j, k); // axis == -1：全部更新
}

std::array<int, 3> MedicalVizService::GetCursorPosition()
{
    return m_sharedState->GetCursorPosition();
}

void MedicalVizService::SetInteracting(bool val)
{
    m_sharedState->SetInteracting(val);
}

int MedicalVizService::GetPlaneAxis(vtkActor* actor)
{
    if (m_currentStrategy)
        return m_currentStrategy->GetPlaneAxis(actor);
    return -1;
}

void MedicalVizService::WorldToModel(const double worldPos[3], double modelPos[3])
{
    m_transformService->WorldToModel(worldPos, modelPos);
}

void MedicalVizService::ModelToWorld(const double modelPos[3], double worldPos[3])
{
    m_transformService->ModelToWorld(modelPos, worldPos);
}

void MedicalVizService::SyncModelMatrix(vtkMatrix4x4* mat)
{
    m_transformService->SyncModelMatrix(mat);
}

void MedicalVizService::TransformModel(double translate[3], double rotate[3], double scale[3])
{
    m_transformService->TransformModel(translate, rotate, scale);
}

void MedicalVizService::ResetModelTransform()
{
    m_transformService->ResetModelTransform();
}

vtkProp3D* MedicalVizService::GetMainProp()
{
    if (m_currentStrategy)
        return m_currentStrategy->GetMainProp();
    return nullptr;
}