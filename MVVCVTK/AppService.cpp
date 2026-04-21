#include "AppService.h"
#include "DataManager.h"
#include "DataConverters.h"
#include "StrategyFactory.h"
#include <iostream>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService::SetCurrentStrategy（主线程专属）
// ─────────────────────────────────────────────────────────────────────
void AbstractAppService::SetCurrentStrategy(
    std::shared_ptr<AbstractVisualStrategy> newStrategy)
{
    if (!m_renderer || !m_renderWindow) return;

    if (m_currentStrategy)
        m_currentStrategy->SetRendererDetached(m_renderer);

    m_currentStrategy = newStrategy;

    if (m_currentStrategy) {
        m_currentStrategy->SetRendererAttached(m_renderer);
        m_currentStrategy->SetCameraConfigured(m_renderer);
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
// SetRenderContext（由 SetServiceBound 触发，shared_from_this() 已安全）
//
// Observer 回调中不直接调用有 VTK 操作的函数；
// 所有 VTK 操作通过标记延迟到主线程 SetPendingUpdatesProcessed 执行。
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer>     ren)
{
    AbstractAppService::SetRenderContext(win, ren);
    if (!m_sharedState) return;

    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

	// 注册 SharedState 观察者，响应数据和配置变化事件
    m_sharedState->SetObserver(shared_from_this(),
        [weakSelf](UpdateFlags flags)
        {
            auto self = weakSelf.lock();
            if (!self) return;

            // ── DataReady：后台线程，只设标记，禁止任何 VTK 操作 ──
            if (HasFlag(flags, UpdateFlags::DataReady)) {
                self->SetStrategyCacheClearRequested();
                self->SetCursorCentered();
                self->m_pendingFlags.fetch_or(static_cast< int>(UpdateFlags::All));
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
                self->SetSyncRequested();
                return;
            }

            // ── 普通事件：原子位或 + 标记需要同步 ────────────────
            int old = self->m_pendingFlags.load();
            while (!self->m_pendingFlags.compare_exchange_weak(
                old, old | static_cast<int>(flags)));
            self->SetSyncRequested();
        }
    );
}

// ─────────────────────────────────────────────────────────────────────
// IVisualConfigService — 前处理：逐项设置（向后兼容）
// ─────────────────────────────────────────────────────────────────────

void MedicalVizService::SetVizMode(VizMode mode)
{
    m_pendingVizModeInt.store(static_cast<int>(mode));
}

void MedicalVizService::SetMaterial(const MaterialParams& mat)
{
    m_sharedState->SetMaterial(mat);
}

void MedicalVizService::SetOpacity(double opacity)
{
    auto mat = m_sharedState->GetMaterial();
    mat.opacity = opacity;
    m_sharedState->SetMaterial(mat);
}

void MedicalVizService::SetTransferFunction(const std::vector<TFNode>& nodes)
{
    m_sharedState->SetTFNodes(nodes);
}

void MedicalVizService::SetIsoThreshold(double val)
{
    m_sharedState->SetIsoValue(val);
}

// 【前处理：数据无关】背景色直接写渲染器（可在加载前随时调用）
void MedicalVizService::SetBackground(const BackgroundColor& bg)
{
    // 同步写 SharedState（供后续 RenderParams 填充）
    m_sharedState->SetBackground(bg);
}

void MedicalVizService::SetWindowLevel(double ww, double wc)
{
    m_sharedState->SetWindowLevel(ww, wc);
}

// ─────────────────────────────────────────────────────────────────────
// IVisualConfigService::SetVisualConfig（批量提交）
//
// 一次锁 + 一次广播；VizMode 仅写原子变量（无需进 SharedState）
// 背景色在此同步应用到渲染器（前处理阶段，主线程）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetVisualConfig(const PreInitConfig& cfg)
{
    // VizMode：只记录意图，无需写 SharedState，也不触发广播
    m_pendingVizModeInt.store(static_cast<int>(cfg.vizMode));

    // 其余参数批量写入 SharedState（内部通过 SetPreInitConfig 做精确 diff + 一次锁 + 一次广播）
    m_sharedState->SetPreInitConfig(cfg);
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::GetLoadState
// ─────────────────────────────────────────────────────────────────────
LoadState MedicalVizService::GetLoadState() const
{
    return m_sharedState->GetLoadState();
}

void MedicalVizService::SetTransformedDataSavedAsync(const std::string& path, std::function<void(bool success)> onComplete)
{
    SetSaveCallback(std::move(onComplete));

    // 捕获必要资源，确保在后台线程中的生命周期安全
    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;
    const std::string resolvedPath = path.empty() && dataMgr
        ? dataMgr->GetDefaultTransformedDataPath()
        : path;

    if (!dataMgr || !sharedState) {
        SetSaveCallbackReady(false);
        return;
    }

    if (resolvedPath.empty()) {
        SetSaveCallbackReady(false);
        return;
    }

    // 获取当前模型矩阵（主线程/调度线程同步读取，不再传入后台）
    std::array<double, 16> currentMatrix = sharedState->GetModelMatrix();
    std::weak_ptr<MedicalVizService> weakSelf = shared_from_this();
    
    // 封装异步任务
    std::packaged_task<void()> task([dataMgr, resolvedPath, currentMatrix, weakSelf]() mutable {
        // 进入后台线程，执行重采样和保存操作
        bool ok = dataMgr->SetTransformedDataSaved(resolvedPath, currentMatrix);

        auto self = weakSelf.lock();
        if (self) {
            self->SetSaveCallbackReady(ok);
        }
    });

    std::thread(std::move(task)).detach();
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::SetLoadCanceled（尽力取消）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetLoadCanceled()
{
    m_cancelFlag->store(true);
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::SetFileLoadedAsync
//
// 【线程安全】
//   - 加载在独立线程执行；通过 std::packaged_task + future 管理生命周期
//   - m_cancelFlag 是 share 类型的 atomic<bool>，加载函数内部可自检退出
//   - dataMgr / sharedState 值捕获保证生命周期（shared_ptr 引用计数）
//   - onComplete 统一延迟到主线程 SetPendingUpdatesProcessed 调用
//   - 加载成功 → SetDataReady；失败 → SetLoadFailed
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetFileLoadedAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    // 防止重复加载（加载中状态直接返回）
    if (m_sharedState->GetLoadState() == LoadState::Loading) {
        std::cerr << "[SetFileLoadedAsync] Already loading, ignoring duplicate call.\n";
        SetLoadCallbackReady(false, std::move(onComplete));
        return;
    }

    SetLoadCallback(std::move(onComplete));

    m_cancelFlag->store(false);
    m_sharedState->SetLoadState(LoadState::Loading);

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;
    auto cancelFlag = m_cancelFlag;

    // 用 packaged_task 包装，future 用于析构等待
    std::packaged_task<void()> task([dataMgr, sharedState, path,
        cancelFlag]() mutable
        {
            // 加载前检查取消标记
            if (cancelFlag->load()) {
                sharedState->SetLoadFailed();
                return;
            }

            bool ok = dataMgr->SetDataLoaded(path);

            // 加载后再次检查取消标记
            if (cancelFlag->load()) {
                sharedState->SetLoadFailed();
                return;
            }

            if (ok) {
                auto img = dataMgr->GetVtkImage();
                if (img) {
                    double range[2];
                    img->GetScalarRange(range);
                    sharedState->SetDataReady(range[0], range[1]);  // 设 Succeeded
                }
                else {
                    std::cerr << "[SetFileLoadedAsync] GetVtkImage() returned null after load.\n";
                    sharedState->SetLoadFailed();  
                }
            }
            else {
                std::cerr << "[SetFileLoadedAsync] Failed to load: " << path << "\n";
                sharedState->SetLoadFailed();
            }
        });

    // 保存 future 用于析构 join
    {
        std::lock_guard<std::mutex> lk(m_loadMutex);
        m_loadFuture = task.get_future();
    }

    std::thread(std::move(task)).detach();
}

bool MedicalVizService::SetFromBufferAsync(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool success)> onComplete)
{
    if (m_sharedState->GetLoadState() == LoadState::Loading) {
        std::cerr << "[SetFromBufferAsync] Already loading, ignoring.\n";
        SetLoadCallbackReady(false, std::move(onComplete));
        return false;
    }

    SetLoadCallback(std::move(onComplete));

    m_sharedState->SetLoadState(LoadState::Loading);

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::packaged_task<void()> task(
        [dataMgr, sharedState, data, dims, spacing, origin]() mutable
        {
			// 在后台线程,注意此时状态还是loading，禁止任何VTK操作
            bool ok = dataMgr->SetFromBuffer(data, dims, spacing, origin);
            
            if (!ok)
            {
                sharedState->SetLoadFailed();
            }
        });

    {
        std::lock_guard<std::mutex> lk(m_loadMutex);
        m_loadFuture = task.get_future();
    }

    std::thread(std::move(task)).detach();
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// SetPendingUpdatesProcessed（主线程 Timer 驱动）
//
// 路由优先级：
//   1. m_needsCacheClear  → SetStrategyCacheCleared（SetRendererDetached 在主线程）
//   2. m_needsLoadFailed  → SetLoadFailedHandled
//   3. m_needsDataRefresh → SetPipelineRebuilt
//   4. m_needsSync        → SetStrategyStateSynced
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetPendingUpdatesProcessed()
{
    // 消费来自第三方重建的 ReconBuffer
    // 必须在 m_needsDataRefresh 检查前，因为 SetReconImageConsumed 成功后会设 LoadState::Succeeded，下面的逻辑再据此驱动管线重建。
    if (auto* rawMgr = dynamic_cast<RawVolumeDataManager*>(m_dataManager.get())) {
        if (rawMgr->SetReconImageConsumed()) {
            // 走和文件加载成功相同的后处理路径
            auto img = m_dataManager->GetVtkImage();
            if (img) {
                double range[2];
                img->GetScalarRange(range);
                m_sharedState->SetDataReady(range[0], range[1]);
            }
            else {
                m_sharedState->SetLoadFailed();
            }
        }
    }

    // 导出异步任务回调触发
    if (m_HasPendingSaveCallback.exchange(false)) {
        SetPendingSaveCallbackExecuted();
    }

    // 延迟缓存清理（Detach 必须在主线程）
    if (m_needsCacheClear.exchange(false))
        SetStrategyCacheCleared();

    bool shouldReturnAfterLoadFailure = false;

    // 加载失败处理
    if (m_needsLoadFailed.exchange(false)) {
        SetLoadFailedHandled();
        SetLoadCallbackReady(false);
        shouldReturnAfterLoadFailure = true;
    }

    // DataReady → 重建管线（优先于增量同步）
    if (!shouldReturnAfterLoadFailure && m_needsDataRefresh.exchange(false)) {
        SetPipelineRebuilt();
        SetLoadCallbackReady(true);
        // return; // 本帧只做重建，同步留到下帧
    }

	// 加载成功但未重建（如 SetFromBufferAsync 失败），也要执行回调以通知 UI
    if (m_HasPendingLoadCallback.exchange(false)) {
        SetPendingLoadCallbackExecuted();
    }

    if (shouldReturnAfterLoadFailure) {
        return;
    }

    // 普通事件增量同步
    SetStrategyStateSynced();
}

// ─────────────────────────────────────────────────────────────────────
// SetPipelineRebuilt（后处理路径 A，主线程）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetPipelineRebuilt()
{
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto    strategy = GetStrategy(mode);
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

    SetCurrentStrategy(strategy);
    SetSyncRequested(); // 重建后触发一次全量参数同步
}

// ────────────────────────────────────────────��────────────────────────
// SetStrategyStateSynced（后处理路径 B，主线程）
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetStrategyStateSynced()
{
    bool expected = true;
    if (!m_needsSync.compare_exchange_strong(expected, false)) return;
    if (!m_currentStrategy) return;

    int flagsInt = m_pendingFlags.exchange(0);
    UpdateFlags flags = static_cast<UpdateFlags>(flagsInt);

    if (flags == UpdateFlags::None) {
        m_needsSync = false;
        return;
    }

    // 交互状态控制帧率
    if (HasFlag(flags, UpdateFlags::Interaction) && m_renderWindow) {
        bool interacting = m_sharedState->GetIsInteracting();
        m_renderWindow->SetDesiredUpdateRate(interacting ? 15.0 : 0.001);
    }

    // 背景色同步（数据无关，直接写渲染器）
    if (HasFlag(flags, UpdateFlags::Background) && m_renderer) {
        auto bg = m_sharedState->GetBackground();
        m_renderer->SetBackground(bg.r, bg.g, bg.b);
    }

    RenderParams params = GetRenderParams(flags);
    m_currentStrategy->SetVisualState(params, flags);

    m_isDirty = true;
    // m_needsSync = false;
}

void MedicalVizService::SetLoadFailedHandled()
{
    std::cerr << "[SetLoadFailedHandled] Load failed; clearing pipeline state.\n";

    // 清理策略缓存（无有效数据，不应保留旧 Strategy）
    SetStrategyCacheCleared();

    // 重置刷新标记，防止残留 DataReady 触发错误重建
    m_needsDataRefresh = false;

	// 重置交互状态
    m_pendingFlags = 0;

    // 标脏使渲染器刷新空场景
    m_isDirty = true;
}

// ─────────────────────────────────────────────────────────────────────
// GetRenderParams（私有，按 flags 精确读取 SharedState，减少锁调用）
// ─────────────────────────────────────────────────────────────────────
RenderParams MedicalVizService::GetRenderParams(UpdateFlags flags) const
{
    RenderParams p;

    if (HasFlag(flags, UpdateFlags::Cursor) || HasFlag(flags,UpdateFlags::Transform)) {
        auto pos = m_sharedState->GetCursorWorld();
        auto rawPos = m_sharedState->GetCursorRawWorld();
        p.cursor = { pos[0], pos[1], pos[2] };
        p.cursorRaw = { rawPos[0], rawPos[1], rawPos[2] };
        p.cursorAxis = m_sharedState->GetCursorAxis();
        p.modelMatrix = m_sharedState->GetModelMatrix();
    }
    if (HasFlag(flags, UpdateFlags::TF)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (HasFlag(flags, UpdateFlags::WindowLevel)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.windowLevel = m_sharedState->GetWindowLevel();
    }

    if (HasFlag(flags, UpdateFlags::Material)) {
        auto range = m_sharedState->GetDataRange();
        p.scalarRange[0] = range[0];
        p.scalarRange[1] = range[1];
        p.material = m_sharedState->GetMaterial();
    }

    if (HasFlag(flags, UpdateFlags::IsoValue))
        p.isoValue = m_sharedState->GetIsoValue();

    if (HasFlag(flags, UpdateFlags::Background))
        p.background = m_sharedState->GetBackground();

    if (HasFlag(flags, UpdateFlags::Visibility))
		p.visibilityMask = m_sharedState->GetVisibilityMask();
    return p;
}

// ─────────────────────────────────────────────────────────────────────
// 可视化策略缓存管理
// ─────────────────────────────────────────────────────────────────────
std::shared_ptr<AbstractVisualStrategy>
MedicalVizService::GetStrategy(VizMode mode)
{
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end()) return it->second;

    auto s = StrategyFactory::GetStrategy(mode);
    if (s) m_strategyCache[mode] = s;
    return s;
}

void MedicalVizService::SetStrategyCacheClearRequested()
{
    m_needsCacheClear = true;
}

void MedicalVizService::SetStrategyCacheCleared()
{
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->SetRendererDetached(m_renderer);
        m_currentStrategy = nullptr;
    }
    m_strategyCache.clear();
}

// ─────────────────────────────────────────────────────────────────────
// 辅助
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetCursorCentered()
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    double imgcenter[3] = {0.0};
	auto img = m_dataManager->GetVtkImage();
	img->GetCenter(imgcenter);

	double imgcenterWorld[3];
    GetWorldPositionFromModel(imgcenter, imgcenterWorld);
 m_sharedState->SetCursorRawWorld(imgcenterWorld[0], imgcenterWorld[1], imgcenterWorld[2]);
    m_sharedState->SetCursorAxis(-1);
	m_sharedState->SetCursorWorld(imgcenterWorld[0], imgcenterWorld[1], imgcenterWorld[2]);
}

void MedicalVizService::SetSyncRequested()
{
    m_needsSync = true;
    m_isDirty = true;
}

// ─────────────────────────────────────────────────────────────────────
// 交互接口实现
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetSliceScrolled(int delta)
{
    if (!m_sharedState) return;
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    int axis = 2; // 默认 Z（Top_down）
    if (mode == VizMode::SliceFront_back)  axis = 1;
    if (mode == VizMode::SliceLeft_right) axis = 0;
    
    // 取间距
    double space[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
	img->GetSpacing(space);

    auto cursorWorld = m_sharedState->GetCursorWorld();
	double cursorModel[3] = { 0.0 };
    GetModelPositionFromWorld(cursorWorld.data(), cursorModel);
    cursorModel[axis] += static_cast<double>(delta)* space[axis];

	// 边界检查
    double bounds[6] = { 0.0 };
    img->GetBounds(bounds);
    cursorModel[0] = std::max(bounds[0], std::min(cursorModel[0], bounds[1]));
    cursorModel[1] = std::max(bounds[2], std::min(cursorModel[1], bounds[3]));
    cursorModel[2] = std::max(bounds[4], std::min(cursorModel[2], bounds[5]));
    
	// 模型坐标转世界坐标，更新 SharedState
    double newCursorWorld[3] = { 0.0, 0.0, 0.0 };
    GetWorldPositionFromModel(cursorModel, newCursorWorld);
    m_sharedState->SetCursorRawWorld(newCursorWorld[0], newCursorWorld[1], newCursorWorld[2]);
    m_sharedState->SetCursorAxis(axis);
    m_sharedState->SetCursorWorld(newCursorWorld[0], newCursorWorld[1], newCursorWorld[2]);
    SetSyncRequested();
}

void MedicalVizService::SetCursorWorldPosition(double worldpos[3], int axis)
{
    if (!m_dataManager || !m_dataManager->GetVtkImage() || !m_sharedState) return;
    auto currentPos = m_sharedState->GetCursorWorld(); // 上一帧的位置索引
    m_sharedState->SetCursorRawWorld(worldpos[0], worldpos[1], worldpos[2]);
    m_sharedState->SetCursorAxis(axis);

    double newPos[3] = { currentPos[0], currentPos[1], currentPos[2] };
    if (axis == -1 || axis == 0) newPos[0] = worldpos[0];
    if (axis == -1 || axis == 1) newPos[1] = worldpos[1];
    if (axis == -1 || axis == 2) newPos[2] = worldpos[2];

    m_sharedState->SetCursorWorld(newPos[0], newPos[1], newPos[2]);
}

std::array<double, 3> MedicalVizService::GetCursorWorld()
{
    return m_sharedState->GetCursorWorld();
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

void MedicalVizService::SetModelMatrixSynced(vtkMatrix4x4* mat)
{
    m_transformService->SetModelMatrix(mat);
}

void MedicalVizService::SetElementVisible(uint32_t flagBit, bool show)
{
    m_sharedState->SetElementVisible(flagBit, show);
}

// 运行时交互兼容层
void MedicalVizService::SetWindowLevelAdjusted(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
{

    double dx = 3.0 * totalDx / static_cast<double>(viewWidth);
    double dy = 3.0 * totalDy / static_cast<double>(viewHeight);

    // 2. 独立的基准缩放 (VTK 核心：X用窗宽缩放，Y用窗位缩放！)
    if (std::abs(startWW) > 0.01) {
        dx = dx * startWW;
    }
    else {
        dx = dx * (startWW < 0.0 ? -0.01 : 0.01);
    }

    if (std::abs(startWC) > 0.01) {
        dy = dy * startWC;
    }
    else {
        dy = dy * (startWC < 0.0 ? -0.01 : 0.01);
    }

    // 绝对值保护 (防止当参数为负时，拖拽方向反转)
    if (startWW < 0.0) {
        dx = -1.0 * dx;
    }
    if (startWC < 0.0) {
        dy = -1.0 * dy;
    }

    // 计算新值 (注意 VTK 源码中 Y 是减法)
    double newWW = startWW + dx;
    double newWC = startWC - dy;

    // 极小值钳制
    if (newWW < 0.01) {
        newWW = 0.01;
    }

    // 写入 SharedState 触发单向数据流更新
    m_sharedState->SetWindowLevel(newWW, newWC);
    SetSyncRequested();
}

void MedicalVizService::SetModelTransform(
    double translate[3], double rotate[3], double scale[3])
{
    m_transformService->SetModelTransform(translate, rotate, scale);
}

void MedicalVizService::SetModelTransformReset()
{
    m_transformService->SetModelTransformReset();
}

void MedicalVizService::GetModelPositionFromWorld(const double w[3], double m[3]) const
{
    m_transformService->GetModelPositionFromWorld(w, m);
}

void MedicalVizService::GetWorldPositionFromModel(const double m[3], double w[3]) const
{
    m_transformService->GetWorldPositionFromModel(m, w);
}