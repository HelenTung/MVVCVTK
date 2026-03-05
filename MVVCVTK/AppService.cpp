#include "AppService.h"
#include "DataManager.h"
#include "DataConverters.h"
#include "StrategyFactory.h"
#include <iostream>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService::SwitchStrategy（主线程专属）
// ─────────────────────────────────────────────────────────────────────
void AbstractAppService::SwitchStrategy(
    std::shared_ptr<AbstractVisualStrategy> newStrategy)
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
// 构造 / 析构
// ─────────────────────────────────────────────────────────────────────
MedicalVizService::MedicalVizService(
    std::shared_ptr<AbstractDataManager>    dataMgr,
    std::shared_ptr<SharedInteractionState> state)
    : m_sharedState(std::move(state))
    , m_transformService(std::make_unique<VolumeTransformService>(m_sharedState))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
{
    m_dataManager = std::move(dataMgr);
}

MedicalVizService::~MedicalVizService()
{
    // 通知加载线程尽快退出
    if (m_cancelFlag)
        m_cancelFlag->store(true);

    // 等待加载线程完成，避免 detach 导致的 UB
    std::lock_guard<std::mutex> lk(m_loadMutex);
    if (m_loadFuture.valid())
        m_loadFuture.wait();
}

// ─────────────────────────────────────────────────────────────────────
// Initialize（由 BindService 触发，shared_from_this() 已安全）
//
// Observer 回调中不直接调用有 VTK 操作的函数；
// 所有 VTK 操作通过标记延迟到主线程 ProcessPendingUpdates 执行。
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
            if (!self) return;

            // ── DataReady：后台线程，只设标记，禁止任何 VTK 操作 ──
            if (HasFlag(flags, UpdateFlags::DataReady)) {
                self->RequestClearStrategyCache();
                self->ResetCursorToCenter();
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
                self->m_needsDataRefresh = true;
                return;
            }

            // ── LoadFailed：后台线程，只设标记 ──────────────
            if (HasFlag(flags, UpdateFlags::LoadFailed)) {
                self->m_needsLoadFailed = true;
                self->m_isDirty = true;
                return;
            }

            // ── Background：直接写渲染器（无 pipeline 操作）──
            // 背景色写渲染器本身是线程安全的（VTK 渲染器内部有锁），
            // 但为一致性仍通过标记延迟到主线程
            if (HasFlag(flags, UpdateFlags::Background)) {
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::Background));
                self->MarkNeedsSync();
                return;
            }

            // ── 普通事件：原子位或 + 标记需要同步 ────────────────
            int old = self->m_pendingFlags.load();
            while (!self->m_pendingFlags.compare_exchange_weak(
                old, old | static_cast<int>(flags)));
            self->MarkNeedsSync();
        }
    );
}

// ─────────────────────────────────────────────────────────────────────
// IPreInitService — 前处理：逐项设置（向后兼容）
// ─────────────────────────────────────────────────────────────────────

void MedicalVizService::PreInit_SetVizMode(VizMode mode)
{
    m_pendingVizModeInt.store(static_cast<int>(mode));
}

void MedicalVizService::PreInit_SetMaterial(const MaterialParams& mat)
{
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
    m_sharedState->SetTFNodes(nodes);
}

void MedicalVizService::PreInit_SetIsoThreshold(double val)
{
    m_sharedState->SetIsoValue(val);
}

// 【前处理：数据无关】背景色直接写渲染器（可在加载前随时调用）
void MedicalVizService::PreInit_SetBackground(const BackgroundColor& bg)
{
    // 同步写 SharedState（供后续 RenderParams 填充）
    m_sharedState->SetBackground(bg);

    // 直接更新渲染器（前处理阶段，主线程调用，无 VTK 线程问题）
    if (m_renderer)
        m_renderer->SetBackground(bg.r, bg.g, bg.b);
}

void MedicalVizService::PreInit_SetWindowLevel(double ww, double wc)
{
    m_sharedState->SetWindowLevel(ww, wc);
}

// ─────────────────────────────────────────────────────────────────────
// IPreInitService::PreInit_CommitConfig（批量提交）
//
// 一次锁 + 一次广播；VizMode 仅写原子变量（无需进 SharedState）
// 背景色在此同步应用到渲染器（前处理阶段，主线程）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::PreInit_CommitConfig(const PreInitConfig& cfg)
{
    // VizMode：只记录意图，无需写 SharedState，也不触发广播
    m_pendingVizModeInt.store(static_cast<int>(cfg.vizMode));

    // 其余参数批量写入 SharedState（内部精确 diff + 一次锁 + 一次广播）
    m_sharedState->CommitPreInitConfig(cfg);

    // 背景色：前处理阶段直接应用到渲染器（数据无关，主线程安全）
    if (cfg.hasBgColor && m_renderer)
        m_renderer->SetBackground(cfg.bgColor.r, cfg.bgColor.g, cfg.bgColor.b);
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::GetLoadState
// ─────────────────────────────────────────────────────────────────────
LoadState MedicalVizService::GetLoadState() const
{
    return m_sharedState->GetLoadState();
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::CancelLoad（尽力取消）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::CancelLoad()
{
    m_cancelFlag->store(true);
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::LoadFileAsync
//
// 【线程安全】
//   - 加载在独立线程执行；通过 std::packaged_task + future 管理生命周期
//   - m_cancelFlag 是 share 类型的 atomic<bool>，加载函数内部可自检退出
//   - dataMgr / sharedState 值捕获保证生命周期（shared_ptr 引用计数）
//   - onComplete 在后台线程调用，只允许操作 SharedState
//   - 加载成功 → NotifyDataReady；失败 → NotifyLoadFailed
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::LoadFileAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    // 防止重复加载（加载中状态直接返回）
    if (m_sharedState->GetLoadState() == LoadState::Loading) {
        std::cerr << "[LoadFileAsync] Already loading, ignoring duplicate call.\n";
        return;
    }

    m_cancelFlag->store(false);
    m_sharedState->SetLoadState(LoadState::Loading);

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;
    auto cancelFlag = m_cancelFlag;

    // 用 packaged_task 包装，future 用于析构等待
    std::packaged_task<void()> task([dataMgr, sharedState, path, onComplete,
        cancelFlag]() mutable
        {
            // 加载前检查取消标记
            if (cancelFlag->load()) {
                sharedState->SetLoadState(LoadState::Idle);
                if (onComplete) onComplete(false);
                return;
            }

            bool ok = dataMgr->LoadData(path);

            // 加载后再次检查取消标记
            if (cancelFlag->load()) {
                sharedState->SetLoadState(LoadState::Idle);
                if (onComplete) onComplete(false);
                return;
            }

            if (ok) {
                auto img = dataMgr->GetVtkImage();
                if (img) {
                    double range[2];
                    img->GetScalarRange(range);
                    sharedState->NotifyDataReady(range[0], range[1]);  // 设 Succeeded
                }
                else {
                    std::cerr << "[LoadFileAsync] GetVtkImage() returned null after load.\n";
                    sharedState->NotifyLoadFailed();  
                }
            }
            else {
                std::cerr << "[LoadFileAsync] Failed to load: " << path << "\n";
                sharedState->NotifyLoadFailed();  
            }

            if (onComplete) onComplete(ok);
        });

    // 保存 future 用于析构 join
    {
        std::lock_guard<std::mutex> lk(m_loadMutex);
        m_loadFuture = task.get_future();
    }

    std::thread(std::move(task)).detach();
}

// ─────────────────────────────────────────────────────────────────────
// ProcessPendingUpdates（主线程 Timer 驱动）
//
// 路由优先级：
//   1. m_needsCacheClear  → ExecuteClearStrategyCache（Detach 在主线程）
//   2. m_needsLoadFailed  → PostData_HandleLoadFailed
//   3. m_needsDataRefresh → PostData_RebuildPipeline
//   4. m_needsSync        → PostData_SyncStateToStrategy
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ProcessPendingUpdates()
{
    // 延迟缓存清理（Detach 必须在主线程）
    if (m_needsCacheClear.exchange(false))
        ExecuteClearStrategyCache();

    // 加载失败处理
    if (m_needsLoadFailed.exchange(false)) {
        PostData_HandleLoadFailed();
        return;
    }

    // DataReady → 重建管线（优先于增量同步）
    if (m_needsDataRefresh.exchange(false)) {
        PostData_RebuildPipeline();
        return; // 本帧只做重建，同步留到下帧
    }

    // 普通事件增量同步
    PostData_SyncStateToStrategy();
}

// ─────────────────────────────────────────────────────────────────────
// PostData_RebuildPipeline（后处理路径 A，主线程）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::PostData_RebuildPipeline()
{
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto    strategy = GetOrCreateStrategy(mode);
    if (!strategy) {
        std::cerr << "[RebuildPipeline] StrategyFactory returned null for mode "
            << static_cast<int>(mode) << "\n";
        return;
    }

    auto img = m_dataManager ? m_dataManager->GetVtkImage() : nullptr;
    if (img) {
        int dims[3] = { 0, 0, 0 };
        img->GetDimensions(dims);
        if (dims[0] > 0 && dims[1] > 0 && dims[2] > 0) {
            strategy->SetInputData(img);
        }
        else {
            std::cerr << "[RebuildPipeline] Image has zero dimension, skipping SetInputData.\n";
            return;
        }
    }
    else {
        std::cerr << "[RebuildPipeline] DataManager has no valid image.\n";
        return;
    }

    SwitchStrategy(strategy);
    MarkNeedsSync(); // 重建后触发一次全量参数同步
}

// ────────────────────────────────────────────��────────────────────────
// PostData_SyncStateToStrategy（后处理路径 B，主线程）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::PostData_SyncStateToStrategy()
{
    if (!m_needsSync.load() || !m_currentStrategy) return;

    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    if (flags == UpdateFlags::None) {
        m_needsSync = false;
        return;
    }

    // 交互状态控制帧率
    if (HasFlag(flags, UpdateFlags::Interaction) && m_renderWindow) {
        bool interacting = m_sharedState->IsInteracting();
        m_renderWindow->SetDesiredUpdateRate(interacting ? 15.0 : 0.001);
    }

    // 背景色同步（数据无关，直接写渲染器）
    if (HasFlag(flags, UpdateFlags::Background) && m_renderer) {
        auto bg = m_sharedState->GetBackground();
        m_renderer->SetBackground(bg.r, bg.g, bg.b);
    }

    RenderParams params = BuildRenderParams(flags);
    m_currentStrategy->UpdateVisuals(params, flags);

    m_isDirty = true;
    m_needsSync = false;
}

void MedicalVizService::PostData_HandleLoadFailed()
{
    std::cerr << "[PostData_HandleLoadFailed] Load failed; clearing pipeline state.\n";

    // 清理策略缓存（无有效数据，不应保留旧 Strategy）
    ExecuteClearStrategyCache();

    // 重置刷新标记，防止残留 DataReady 触发错误重建
    m_needsDataRefresh = false;

	// 重置交互状态
    m_pendingFlags = 0;

    // 标脏使渲染器刷新空场景
    m_isDirty = true;
}

// ─────────────────────────────────────────────────────────────────────
// BuildRenderParams（私有，按 flags 精确读取 SharedState，减少锁调用）
// ─────────────────────────────────────────────────────────────────────
RenderParams MedicalVizService::BuildRenderParams(UpdateFlags flags) const
{
    RenderParams p;

    if (HasFlag(flags, UpdateFlags::Cursor)) {
        auto pos = m_sharedState->GetCursorPosition();
        p.cursor = { pos[0], pos[1], pos[2] };
    }
    if (HasFlag(flags, UpdateFlags::TF)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (HasFlag(flags, UpdateFlags::Material)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.material = m_sharedState->GetMaterial();
    }

    if (HasFlag(flags, UpdateFlags::IsoValue))
        p.isoValue = m_sharedState->GetIsoValue();

    if (HasFlag(flags, UpdateFlags::Transform))
        p.modelMatrix = m_sharedState->GetModelMatrix();

    if (HasFlag(flags, UpdateFlags::Background))
        p.background = m_sharedState->GetBackground();

    if (HasFlag(flags, UpdateFlags::WindowLevel)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.windowLevel = m_sharedState->GetWindowLevel();
    }

    if (HasFlag(flags, UpdateFlags::Visibility))
		p.visibilityMask = m_sharedState->GetVisibilityMask();
    return p;
}

// ─────────────────────────────────────────────────────────────────────
// Strategy 缓存管理
// ─────────────────────────────────────────────────────────────────────
std::shared_ptr<AbstractVisualStrategy>
MedicalVizService::GetOrCreateStrategy(VizMode mode)
{
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end()) return it->second;

    auto s = StrategyFactory::CreateStrategy(mode);
    if (s) m_strategyCache[mode] = s;
    return s;
}

void MedicalVizService::RequestClearStrategyCache()
{
    m_needsCacheClear = true;
}

void MedicalVizService::ExecuteClearStrategyCache()
{
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->Detach(m_renderer);
        m_currentStrategy = nullptr;
    }
    m_strategyCache.clear();
}

// ─────────────────────────────────────────────────────────────────────
// 辅助
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ResetCursorToCenter()
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    int dims[3];
    m_dataManager->GetVtkImage()->GetDimensions(dims);
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
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    int axis = 2; // 默认 Z（Axial）
    if (mode == VizMode::SliceCoronal)  axis = 1;
    if (mode == VizMode::SliceSagittal) axis = 0;

    auto pos = m_sharedState->GetCursorPosition();
    pos[axis] += delta;

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
    double sp[3], orig[3];
    int    dims[3];
    img->GetSpacing(sp);
    img->GetOrigin(orig);
    img->GetDimensions(dims);

    auto clamp = [](int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); };
    auto pos = m_sharedState->GetCursorPosition();

    if (axis == -1 || axis == 0)
        pos[0] = clamp(int((worldPos[0] - orig[0]) / sp[0]), 0, dims[0] - 1);
    if (axis == -1 || axis == 1)
        pos[1] = clamp(int((worldPos[1] - orig[1]) / sp[1]), 0, dims[1] - 1);
    if (axis == -1 || axis == 2)
        pos[2] = clamp(int((worldPos[2] - orig[2]) / sp[2]), 0, dims[2] - 1);

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
    return m_currentStrategy ? m_currentStrategy->GetPlaneAxis(actor) : -1;
}

vtkProp3D* MedicalVizService::GetMainProp()
{
    return m_currentStrategy ? m_currentStrategy->GetMainProp() : nullptr;
}

void MedicalVizService::SyncModelMatrix(vtkMatrix4x4* mat)
{
    m_transformService->SyncModelMatrix(mat);
}

void MedicalVizService::SetElementVisible(uint32_t flagBit, bool show)
{
    m_sharedState->SetElementVisible(flagBit, show);
}

// 运行时交互兼容层
void MedicalVizService::AdjustWindowLevel(double deltaWW, double deltaWC)
{
    auto cur = m_sharedState->GetWindowLevel();

    // WW 不允许小于最小有效值（防止 LUT 除以零）
    constexpr double kMinWW = 1.0;
    const double newWW = std::max(kMinWW, cur.windowWidth + deltaWW);
    const double newWC = cur.windowCenter + deltaWC;

    // SetWindowLevel 内部有 diff 检测 + mutex + NotifyObservers
    m_sharedState->SetWindowLevel(newWW, newWC);
    MarkNeedsSync();  // 与其他交互（SyncCursorToWorldPosition 等）保持一致
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

void MedicalVizService::WorldToModel(const double w[3], double m[3])
{
    m_transformService->WorldToModel(w, m);
}

void MedicalVizService::ModelToWorld(const double m[3], double w[3])
{
    m_transformService->ModelToWorld(m, w);
}