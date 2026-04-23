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

void AbstractAppService::SetOverlayStrategyAdded(std::shared_ptr<AbstractVisualStrategy> strategy) {
    if (!strategy) return;
    m_overlayStrategies.push_back(strategy);
    if (m_renderer) {
        strategy->SetRendererAttached(m_renderer);
    }

    m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
    m_needsSync = true;
    m_isDirty = true;
}

void AbstractAppService::SetOverlayStrategiesCleared() {
    if (m_renderer) {
        for (auto& s : m_overlayStrategies) {
            s->SetRendererDetached(m_renderer);
        }
    }
    m_overlayStrategies.clear();
    m_isDirty = true;
}

// ─────────────────────────────────────────────────────────────────────
// 构造 / 析构
// ─────────────────────────────────────────────────────────────────────
MedicalVizService::MedicalVizService(
    std::shared_ptr<AbstractDataManager> dataMgr,
    std::shared_ptr<SharedInteractionState> state)
    : m_sharedState(std::move(state))
    , m_transformService(std::make_unique<VolumeTransformService>(m_sharedState))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
{
    m_dataManager = std::move(dataMgr);
}

MedicalVizService::~MedicalVizService()
{
    if (m_cancelFlag)
        m_cancelFlag->store(true);

    std::lock_guard<std::mutex> lk(m_loadMutex);
    if (m_loadFuture.valid())
        m_loadFuture.wait();
}

// ─────────────────────────────────────────────────────────────────────
// RenderContext 绑定
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    AbstractAppService::SetRenderContext(win, ren);
    if (!m_sharedState) return;

    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

    m_sharedState->SetObserver(shared_from_this(),
        [weakSelf](UpdateFlags flags)
        {
            auto self = weakSelf.lock();
            if (!self) return;

            if (HasFlag(flags, UpdateFlags::DataReady)) {
                self->SetStrategyCacheClearRequested();
                self->SetCursorCentered();
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
                self->m_needsDataRefresh = true;
                return;
            }

            if (HasFlag(flags, UpdateFlags::Spacing)) {
                self->SetStrategyCacheClearRequested();
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
                self->m_needsDataRefresh = true;
                return;
            }

            if (HasFlag(flags, UpdateFlags::LoadFailed)) {
                self->m_needsLoadFailed = true;
                self->m_isDirty = true;
                return;
            }

            if (HasFlag(flags, UpdateFlags::Background)) {
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::Background));
                self->SetSyncRequested();
                return;
            }

            int old = self->m_pendingFlags.load();
            while (!self->m_pendingFlags.compare_exchange_weak(
                old, old | static_cast<int>(flags)));
            self->SetSyncRequested();
        }
    );
}

// ─────────────────────────────────────────────────────────────────────
// IVisualConfigService — 前处理配置
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

void MedicalVizService::SetBackground(const BackgroundColor& bg)
{
    m_sharedState->SetBackground(bg);
}

void MedicalVizService::SetSpacing(double sx, double sy, double sz)
{
    m_sharedState->SetSpacing(sx, sy, sz);
}

void MedicalVizService::SetWindowLevel(double ww, double wc)
{
    m_sharedState->SetWindowLevel(ww, wc);
}

void MedicalVizService::SetVisualConfig(const PreInitConfig& cfg)
{
    m_pendingVizModeInt.store(static_cast<int>(cfg.vizMode));
    m_sharedState->SetPreInitConfig(cfg);
}

// ─────────────────────────────────────────────────────────────────────
// 数据加载 / 数据导出
// ─────────────────────────────────────────────────────────────────────
LoadState MedicalVizService::GetLoadState() const
{
    return m_sharedState->GetLoadState();
}

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

    std::packaged_task<void()> task([dataMgr, sharedState, path, cancelFlag]() mutable
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
                    const auto range = dataMgr->GetScalarRange();
                    const auto spacing = dataMgr->GetSpacing();
                    sharedState->SetDataReady(range[0], range[1], spacing);
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

void MedicalVizService::SetTransformedDataSavedAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
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

void MedicalVizService::SetLoadCanceled()
{
    m_cancelFlag->store(true);
}

// ─────────────────────────────────────────────────────────────────────
// AbstractInteractiveService — 交互接口
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetSliceScrolled(int delta)
{
    if (!m_sharedState) return;
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    int axis = 2;
    if (mode == VizMode::SliceFront_back) axis = 1;
    if (mode == VizMode::SliceLeft_right) axis = 0;

    double space[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
    img->GetSpacing(space);

    auto cursorWorld = m_sharedState->GetCursorWorld();
    double cursorModel[3] = { 0.0 };
    GetModelPositionFromWorld(cursorWorld.data(), cursorModel);
    cursorModel[axis] += static_cast<double>(delta) * space[axis];

    double bounds[6] = { 0.0 };
    img->GetBounds(bounds);
    cursorModel[0] = std::max(bounds[0], std::min(cursorModel[0], bounds[1]));
    cursorModel[1] = std::max(bounds[2], std::min(cursorModel[1], bounds[3]));
    cursorModel[2] = std::max(bounds[4], std::min(cursorModel[2], bounds[5]));

    double newCursorWorld[3] = { 0.0, 0.0, 0.0 };
    GetWorldPositionFromModel(cursorModel, newCursorWorld);
    m_sharedState->SetCursorRawWorld(newCursorWorld[0], newCursorWorld[1], newCursorWorld[2]);
    m_sharedState->SetCursorAxis(axis);
    m_sharedState->SetCursorWorld(newCursorWorld[0], newCursorWorld[1], newCursorWorld[2]);
    SetSyncRequested();
}

void MedicalVizService::SetCursorWorldPosition(double worldPos[3], int axis)
{
    if (!m_dataManager || !m_dataManager->GetVtkImage() || !m_sharedState) return;
    auto currentPos = m_sharedState->GetCursorWorld();
    m_sharedState->SetCursorRawWorld(worldPos[0], worldPos[1], worldPos[2]);
    m_sharedState->SetCursorAxis(axis);

    double newPos[3] = { currentPos[0], currentPos[1], currentPos[2] };
    if (axis == -1 || axis == 0) newPos[0] = worldPos[0];
    if (axis == -1 || axis == 1) newPos[1] = worldPos[1];
    if (axis == -1 || axis == 2) newPos[2] = worldPos[2];

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

void MedicalVizService::SetWindowLevelAdjusted(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
{
    double dx = 3.0 * totalDx / static_cast<double>(viewWidth);
    double dy = 3.0 * totalDy / static_cast<double>(viewHeight);

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

    if (startWW < 0.0) {
        dx = -1.0 * dx;
    }
    if (startWC < 0.0) {
        dy = -1.0 * dy;
    }

    double newWW = startWW + dx;
    double newWC = startWC - dy;

    if (newWW < 0.01) {
        newWW = 0.01;
    }

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

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 主线程后处理入口
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
                const auto range = m_dataManager->GetScalarRange();
                const auto spacing = m_dataManager->GetSpacing();
                m_sharedState->SetDataReady(range[0], range[1], spacing);
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
// 私有辅助
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetPipelineRebuilt()
{
    VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    auto strategy = GetStrategy(mode);
    if (!strategy) {
        std::cerr << "[RebuildPipeline] StrategyFactory returned null for mode "
            << static_cast<int>(mode) << "\n";
        return;
    }

    auto img = m_dataManager ? m_dataManager->GetVtkImage() : nullptr;
    if (img) {
        const auto spacing = m_sharedState->GetSpacing();
        double currentSpacing[3] = { 1.0, 1.0, 1.0 };
        img->GetSpacing(currentSpacing);
        if (std::abs(currentSpacing[0] - spacing[0]) > 1e-6 ||
            std::abs(currentSpacing[1] - spacing[1]) > 1e-6 ||
            std::abs(currentSpacing[2] - spacing[2]) > 1e-6)
        {
            img->SetSpacing(spacing[0], spacing[1], spacing[2]);
        }

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

    for (auto& overlay : m_overlayStrategies) {
        overlay->SetVisualState(params, flags);
    }

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

RenderParams MedicalVizService::GetRenderParams(UpdateFlags flags) const
{
    RenderParams p;

    if (HasFlag(flags, UpdateFlags::Cursor) || HasFlag(flags, UpdateFlags::Transform)) {
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
        m_sharedState->GetTFNodes(p.tfNodes);
    }

    if (HasFlag(flags, UpdateFlags::Interaction))
        p.isInteracting = m_sharedState->GetIsInteracting();

    if (HasFlag(flags, UpdateFlags::IsoValue))
        p.isoValue = m_sharedState->GetIsoValue();

    if (HasFlag(flags, UpdateFlags::Background))
        p.background = m_sharedState->GetBackground();

    if (HasFlag(flags, UpdateFlags::Visibility))
		p.visibilityMask = m_sharedState->GetVisibilityMask();

    return p;
}

std::shared_ptr<AbstractVisualStrategy> MedicalVizService::GetStrategy(VizMode mode)
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

void MedicalVizService::SetCursorCentered()
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    double imgCenter[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
    img->GetCenter(imgCenter);

    double imgCenterWorld[3];
    GetWorldPositionFromModel(imgCenter, imgCenterWorld);
    m_sharedState->SetCursorRawWorld(imgCenterWorld[0], imgCenterWorld[1], imgCenterWorld[2]);
    m_sharedState->SetCursorAxis(-1);
    m_sharedState->SetCursorWorld(imgCenterWorld[0], imgCenterWorld[1], imgCenterWorld[2]);
}

void MedicalVizService::SetSyncRequested()
{
    m_needsSync = true;
    m_isDirty = true;
}