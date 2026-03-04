// =====================================================================
// AppService.cpp
// =====================================================================
#include "AppService.h"
#include "DataManager.h"
#include "DataConverters.h"
#include "StrategyFactory.h"
#include <iostream>

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService::SwitchStrategy
//
// 主线程专属：旧策略下台 → 新策略上台 → 相机配置 → 标记脏数据。
// 不在此处调用 Render()，由 Context 的 Timer 心跳统一发起渲染。
// ─────────────────────────────────────────────────────────────────────
void AbstractAppService::SwitchStrategy(
    std::shared_ptr<AbstractVisualStrategy> newStrategy)
{
    if (!m_renderer || !m_renderWindow) return;

    if (m_currentStrategy) {
        m_currentStrategy->Detach(m_renderer);
    }
    m_currentStrategy = newStrategy;
    if (m_currentStrategy) {
        m_currentStrategy->Attach(m_renderer);
        m_currentStrategy->SetupCamera(m_renderer);
    }
    m_renderer->ResetCamera();
    m_isDirty = true;
}

MedicalVizService::MedicalVizService(
    std::shared_ptr<AbstractDataManager>    dataMgr,
    std::shared_ptr<SharedInteractionState> state)
    : m_sharedState(state)
    , m_transformService(std::make_unique<VolumeTransformService>(state))
{
    m_dataManager = std::move(dataMgr);
}

// ─────────────────────────────────────────────────────────────────────
// MedicalVizService::Initialize
//
// 由 BindService 触发，此时 shared_ptr 已完全构建，shared_from_this() 安全。
//
// 【Observer 注册策略】
//   Lambda 捕获 weak_ptr 而非 shared_ptr，避免循环引用：
//     Service → m_sharedState → ObserverEntry → Service（循环！）
//   weak_ptr 不增加引用计数，lock() 失败时自动忽略，安全析构。
//
// 【DataReady 路径与普通路径严格分离】
//   DataReady → 仅设置 m_needsDataRefresh=true，不调 MarkNeedsSync()
//   普通事件 → 位或 m_pendingFlags，调用 MarkNeedsSync()
//   两条路径的 ProcessPendingUpdates 判断逻辑是互斥的，不能混用。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::Initialize(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer>     ren)
{
    AbstractAppService::Initialize(win, ren);
    if (!m_sharedState) return;

    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

    m_sharedState->AddObserver(shared_from_this(),
        [weakSelf](UpdateFlags flags)
        {
            auto self = weakSelf.lock();
            if (!self) return; // Service 已销毁，忽略

            if (static_cast<int>(flags) & static_cast<int>(UpdateFlags::DataReady)) {
                // ── DataReady 路径：只标记，不执行任何 VTK 操作 ──────
                // VTK 管线操作必须在主线程执行，此 Lambda 可能在后台线程调用
                self->ClearStrategyCache();
                self->ResetCursorToCenter();
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
                self->m_needsDataRefresh = true;
                //  不调 MarkNeedsSync()，DataReady 走独立路径
                return;
            }

            // ── 普通事件路径：CAS 位或更新待处理标记 ─────────────────
            // 【为什么用 CAS 循环而不是直接 fetch_or】
            //   fetch_or 在 C++11 memory_order_seq_cst 下已经是原子的，
            //   这里使用 CAS 仅为了代码意图更清晰，实际 fetch_or 也可以。
            int oldVal = self->m_pendingFlags.load();
            while (!self->m_pendingFlags.compare_exchange_weak(
                oldVal, oldVal | static_cast<int>(flags)));
            self->MarkNeedsSync();
        }
    );
}

// ─────────────────────────────────────────────────────────────────────
// 前处理接口实现（IPreInitService）
// ─────────────────────────────────────────────────────────────────────

void MedicalVizService::PreInit_SetVizMode(VizMode mode)
{
    // 只记录意图，不执行任何 VTK 操作
    // DataReady 后 PostData_RebuildPipeline 会按此模式重建管线
    m_pendingVizModeInt.store(static_cast<int>(mode));
}

void MedicalVizService::PreInit_SetLuxParams(
    double ambient, double diffuse, double specular,
    double power, bool shadeOn)
{
    // 读-改-写 MaterialParams（SetMaterial 内部加锁，线程安全）
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
    // SetIsoValue 内部有微小变化检测（阈值 > 0.0001），避免无效广播
    m_sharedState->SetIsoValue(val);
}

// ─────────────────────────────────────────────────────────────────────
// LoadFileAsync：发起异步加载
//
// 【线程安全分析】
//   1. 加载在后台线程执行（std::thread）
//   2. DataManager::LoadData 内部无需加锁（只有一个线程写数据）
//   3. 加载完成后调用 SharedState::NotifyDataReady（内部有 mutex）
//   4. onComplete 回调在后台线程执行，只允许操作 SharedState
//
// 【为什么 onComplete 用值捕获 shared_ptr 而不是引用捕获】
//   后台线程执行时，main 函数的栈帧可能已退出（理论上）。
//   引用捕获（& 或 std::ref）引用栈上局部变量，存在 UB 风险。
//   值捕获 shared_ptr 拷贝增加引用计数，保证对象生命周期覆盖回调执行。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::LoadFileAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    // 值捕获 shared_ptr 保证 DataManager 和 SharedState 在后台线程存活
    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::thread([dataMgr, sharedState, path, onComplete]() {
        bool ok = dataMgr->LoadData(path);

        if (ok) {
            auto img = dataMgr->GetVtkImage();
            if (img) {
                double range[2];
                img->GetScalarRange(range);
                // 通知所有 Service：数据已就绪，携带标量范围
                sharedState->NotifyDataReady(range[0], range[1]);
            }
        }
        else {
            std::cerr << "[LoadFileAsync] Failed to load: " << path << std::endl;
        }

        // 用户回调（在后台线程执行，只允许操作 SharedState）
        if (onComplete) onComplete(ok);

        }).detach();
}

// ─────────────────────────────────────────────────────────────────────
// ProcessPendingUpdates：主线程心跳定时器驱动的更新入口
//
// 【路由逻辑】
//   路径 A（DataReady 重建）优先级高于路径 B（增量同步），
//   同一帧若两者都触发，先做重建，下一帧再做同步。
//   这样避免在同一帧内既重建管线又同步参数，产生双重重绘。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ProcessPendingUpdates()
{
    // ── 路径 A：DataReady 到达，重建渲染管线（优先执行）──────────
    if (m_needsDataRefresh.exchange(false)) {
        PostData_RebuildPipeline();
        return; // 本帧只做重建，增量同步留到下帧
    }

    // ── 路径 B：普通状态增量同步 ─────────────────────────────────
    PostData_SyncStateToStrategy();
}

// ─────────────────────────────────────────────────────────────────────
// PostData_RebuildPipeline（后处理路径 A）
//
// 仅在主线程（Timer 心跳）调用，VTK 管线操作安全。
// 流程：获取/创建 Strategy → 注入数据 → SwitchStrategy → 标记需要同步
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::PostData_RebuildPipeline()
{
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto strategy = GetOrCreateStrategy(mode);
    if (!strategy) return;

    // 注入最新数据（Strategy 内部 pipeline 持有引用，不发生拷贝）
    auto img = m_dataManager->GetVtkImage();
    if (img) {
        int dims[3] = { 0, 0, 0 };
        img->GetDimensions(dims);
        // 三个维度都大于 0 才是有效数据
        if (dims[0] > 0 && dims[1] > 0 && dims[2] > 0) {
            strategy->SetInputData(img);
        }
    }

    SwitchStrategy(strategy);

    // 重建完成后标记"需要做一次全量参数同步"，下帧路径 B 会处理
    MarkNeedsSync();
}

// ─────────────────────────────────────────────────────────────────────
// PostData_SyncStateToStrategy（后处理路径 B）
//
// 增量同步：只更新 m_pendingFlags 中指定的参数类型，
// 避免每帧全量重建带来的性能浪费。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::PostData_SyncStateToStrategy()
{
    if (!m_needsSync.load() || !m_currentStrategy) return;

    // 原子取出并清空待处理标记（防止在 UpdateVisuals 期间丢失新标记）
    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    if (flags == UpdateFlags::None) {
        m_needsSync = false;
        return;
    }

    // 交互状态影响帧率：拖拽中降采样保流畅，停止后恢复高质量
    if (static_cast<int>(flags) & static_cast<int>(UpdateFlags::Interaction)) {
        if (m_renderWindow) {
            bool interacting = m_sharedState->IsInteracting();
            // 15 FPS = 拖拽降采样；0.001 = 停止时强制高质量渲染
            m_renderWindow->SetDesiredUpdateRate(interacting ? 15.0 : 0.001);
        }
    }

    RenderParams params = BuildRenderParams(flags);
    m_currentStrategy->UpdateVisuals(params, flags);

    m_isDirty = true;   // 通知 Context 本帧需要 Render()
    m_needsSync = false;
}

// ─────────────────────────────────────────────────────────────────────
// BuildRenderParams（私有辅助）
//
// 只读 SharedState，构造纯数据对象 RenderParams。
// 按 flags 按需拷贝，减少不必要的 vector 拷贝开销。
// ─────────────────────────────────────────────────────────────────────
RenderParams MedicalVizService::BuildRenderParams(UpdateFlags flags) const
{
    RenderParams params;

    // 光标位置（每帧都需要，切片策略依赖 cursor 定位平面）
    auto pos = m_sharedState->GetCursorPosition();
    params.cursor = { pos[0], pos[1], pos[2] };

    // 标量范围（TF 归一化映射必须）
    auto range = m_sharedState->GetDataRange();
    params.scalarRange[0] = range[0];
    params.scalarRange[1] = range[1];

    // 材质参数（光照/透明度）
    params.material = m_sharedState->GetMaterial();

    // 传输函数（仅 TF 标记时拷贝，vector 拷贝有成本）
    if (static_cast<int>(flags) & static_cast<int>(UpdateFlags::TF))
        m_sharedState->GetTFNodes(params.tfNodes);

    // 等值面阈值
    if (static_cast<int>(flags) & static_cast<int>(UpdateFlags::IsoValue))
        params.isoValue = m_sharedState->GetIsoValue();

    // 模型变换矩阵
    if (static_cast<int>(flags) & static_cast<int>(UpdateFlags::Transform))
        params.modelMatrix = m_sharedState->GetModelMatrix();

    return params;
}

// ─────────────────────────────────────────────────────────────────────
// GetOrCreateStrategy（Strategy 缓存管理）
//
// 缓存命中 → 直接返回，避免重建 VTK pipeline（开销大）。
// 缓存未命中 → 通过 StrategyFactory 创建并存入缓���。
// ─────────────────────────────────────────────────────────────────────
std::shared_ptr<AbstractVisualStrategy>
MedicalVizService::GetOrCreateStrategy(VizMode mode)
{
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end()) return it->second;

    auto strategy = StrategyFactory::CreateStrategy(mode);
    if (strategy) m_strategyCache[mode] = strategy;
    return strategy;
}

void MedicalVizService::ClearStrategyCache()
{
    // DataReady 时调用：旧数据的 VTK 管线引用全部释放
    // 先 Detach 当前策略，再清空缓存，避免悬空 VTK 对象
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->Detach(m_renderer);
        m_currentStrategy = nullptr;
    }
    m_strategyCache.clear();
}

void MedicalVizService::ResetCursorToCenter()
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    auto img = m_dataManager->GetVtkImage();
    int dims[3];
    img->GetDimensions(dims);
    // 注意：SetCursorPosition 内部有 mutex，线程安全
    m_sharedState->SetCursorPosition(dims[0] / 2, dims[1] / 2, dims[2] / 2);
}

void MedicalVizService::MarkNeedsSync()
{
    m_needsSync = true;
    m_isDirty = true;
}

// ─────────────────────────────────────────────────────────────────────
// 交互接口实现
// ─────────────────────────────────────────────────────────────────────

void MedicalVizService::UpdateInteraction(int delta)
{
    if (!m_sharedState) return;
    auto pos = m_sharedState->GetCursorPosition();

    // 根据当前模式决定移动哪个轴
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    int axis = 2; // 默认 Z 轴（Axial）
    if (mode == VizMode::SliceCoronal)   axis = 1; // Y 轴
    if (mode == VizMode::SliceSagittal)  axis = 0; // X 轴

    pos[axis] += delta;

    // 夹位：确保坐标在体数据范围内
    if (m_dataManager && m_dataManager->GetVtkImage()) {
        int dims[3];
        m_dataManager->GetVtkImage()->GetDimensions(dims);
        pos[axis] = std::max(0, std::min(pos[axis], dims[axis] - 1));
    }
    m_sharedState->SetCursorPosition(pos[0], pos[1], pos[2]);
}

void MedicalVizService::SyncCursorToWorldPosition(double worldPos[3], int axis)
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    auto img = m_dataManager->GetVtkImage();
    double spacing[3], origin[3];
    img->GetSpacing(spacing);
    img->GetOrigin(origin);
    int dims[3];
    img->GetDimensions(dims);

    // 世界坐标 → 体素坐标（整数）
    auto clamp = [](int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); };

    auto pos = m_sharedState->GetCursorPosition();
    if (axis == -1 || axis == 0)
        pos[0] = clamp(static_cast<int>((worldPos[0] - origin[0]) / spacing[0]), 0, dims[0] - 1);
    if (axis == -1 || axis == 1)
        pos[1] = clamp(static_cast<int>((worldPos[1] - origin[1]) / spacing[1]), 0, dims[1] - 1);
    if (axis == -1 || axis == 2)
        pos[2] = clamp(static_cast<int>((worldPos[2] - origin[2]) / spacing[2]), 0, dims[2] - 1);

    m_sharedState->SetCursorPosition(pos[0], pos[1], pos[2]);
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
    if (!m_currentStrategy) return -1;
    return m_currentStrategy->GetPlaneAxis(actor);
}

// ── 模型变换（委托给 VolumeTransformService）──────────────────────

vtkProp3D* MedicalVizService::GetMainProp()
{
    if (m_currentStrategy) return m_currentStrategy->GetMainProp();
    return nullptr;
}

void MedicalVizService::SyncModelMatrix(vtkMatrix4x4* mat)
{
    m_transformService->SyncModelMatrix(mat);
}

void MedicalVizService::TransformModel(
    double translate[3], double rotate[3], double scale[3])
{
    m_transformService->TransformModel(translate, rotate, scale);
}

void MedicalVizService::ResetModelTransform()
{
    m_transformService->ResetModelTransform();
}

void MedicalVizService::WorldToModel(
    const double worldPos[3], double modelPos[3])
{
    m_transformService->WorldToModel(worldPos, modelPos);
}

void MedicalVizService::ModelToWorld(
    const double modelPos[3], double worldPos[3])
{
    m_transformService->ModelToWorld(modelPos, worldPos);
}