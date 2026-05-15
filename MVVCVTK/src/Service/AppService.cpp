#include "AppService.h"
#include "DataManager.h"
#include "StrategyFactory.h"
#include <vtkSmartPointer.h>
#include <iostream>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService::SetCurrentStrategy（主线程专属）
// ─────────────────────────────────────────────────────────────────────
void AbstractAppService::SetCurrentStrategy(
    std::shared_ptr<AbstractVisualStrategy> newStrategy)
{
    if (m_currentStrategy == newStrategy) {
        if (m_currentStrategy && m_renderer) {
            m_currentStrategy->SetCameraConfigured(m_renderer);
            m_isDirty = true;
        }
        return;
    }

    if (m_currentStrategy && m_renderer)
        m_currentStrategy->SetRendererDetached(m_renderer);

    m_currentStrategy = newStrategy;

    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->SetRendererAttached(m_renderer);
        m_currentStrategy->SetCameraConfigured(m_renderer);
    }
    if (m_renderer)
        m_renderer->ResetCamera();
    m_isDirty = true;
}

void AbstractAppService::SetOverlayStrategyAdded(std::shared_ptr<AbstractVisualStrategy> strategy) {
    if (!strategy) return;

    const auto sameStrategy = std::find_if(m_overlayStrategies.begin(), m_overlayStrategies.end(),
        [strategy](const std::shared_ptr<AbstractVisualStrategy>& current) {
            return current.get() == strategy.get();
        });
    if (sameStrategy != m_overlayStrategies.end()) {
        return;
    }

    m_overlayStrategies.push_back(strategy);
    if (m_renderer) {
        strategy->SetRendererAttached(m_renderer);
    }

    m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
    m_needsSync = true;
    m_isDirty = true;
}

void AbstractAppService::SetOverlayStrategyRemoved(std::shared_ptr<AbstractVisualStrategy> strategy) {
    if (!strategy) return;

    const auto it = std::find_if(m_overlayStrategies.begin(), m_overlayStrategies.end(),
        [strategy](const std::shared_ptr<AbstractVisualStrategy>& current) {
            return current.get() == strategy.get();
        });
    if (it == m_overlayStrategies.end()) {
        return;
    }

    if (m_renderer) {
        strategy->SetRendererDetached(m_renderer);
    }
    m_overlayStrategies.erase(it);
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
    std::shared_ptr<SharedInteractionState> state,
    std::shared_ptr<IStateEventSource> stateEventSource)
    : m_sharedState(std::move(state))
    , m_stateEventSource(std::move(stateEventSource))
{
    m_dataManager = std::move(dataMgr);
}

MedicalVizService::~MedicalVizService()
{
    std::lock_guard<std::mutex> lk(m_ActiveLoadMutex);
    if (m_ActiveLoadFuture.valid())
        m_ActiveLoadFuture.wait();
}

// ─────────────────────────────────────────────────────────────────────
// RenderContext 绑定
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    AbstractAppService::SetRenderContext(win, ren);
    if (!m_stateEventSource) return;

    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

    m_stateEventSource->SetObserver(shared_from_this(),
        [weakSelf](UpdateFlags flags)
        {
            auto self = weakSelf.lock();
            if (!self) return;

            self->m_stateSyncStrategy.SetFlagsHandled(flags, *self);
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
LoadState MedicalVizService::GetFileLoadState() const
{
    return m_sharedState ? m_sharedState->GetFileLoadState() : LoadState::Idle;
}

LoadState MedicalVizService::GetReloadLoadState() const
{
    return m_sharedState ? m_sharedState->GetReloadLoadState() : LoadState::Idle;
}

bool MedicalVizService::SetFileLoadStarted(
    std::function<void(bool)> callback)
{
    if (!m_sharedState) {
        SetFileLoadCallbackReady(false, std::move(callback));
        return false;
    }

    if (m_sharedState->GetFileLoadState() == LoadState::Loading
        || m_sharedState->GetReloadLoadState() == LoadState::Loading)
    {
        std::cerr << "[SetFileLoadStarted] Already loading, ignoring duplicate call.\n";
        SetFileLoadCallbackReady(false, std::move(callback));
        return false;
    }

    SetFileLoadCallback(std::move(callback));
    m_sharedState->SetFileLoadStarted();
    return true;
}

bool MedicalVizService::SetReloadLoadStarted(
    std::function<void(bool)> callback)
{
    if (!m_sharedState) {
        SetReloadLoadCallbackReady(false, std::move(callback));
        return false;
    }

    if (m_sharedState->GetFileLoadState() == LoadState::Loading
        || m_sharedState->GetReloadLoadState() == LoadState::Loading)
    {
        std::cerr << "[SetReloadLoadStarted] Already loading, ignoring duplicate call.\n";
        SetReloadLoadCallbackReady(false, std::move(callback));
        return false;
    }

    SetReloadLoadCallback(std::move(callback));
    m_sharedState->SetReloadLoadStarted();
    return true;
}

void MedicalVizService::SetTaskStarted(
    std::packaged_task<void()> task,
    bool keepActiveLoadFuture)
{
    if (keepActiveLoadFuture) {
        std::lock_guard<std::mutex> lk(m_ActiveLoadMutex);
        m_ActiveLoadFuture = task.get_future(); // 当前活动加载任务的 future，用于析构时等待后台线程结束
    }
    std::thread(std::move(task)).detach();
}

void MedicalVizService::SetFileLoadedAsync(
    const std::string& path,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool success)> onComplete)
{
    if (!SetFileLoadStarted(std::move(onComplete))) return;

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::packaged_task<void()> task([dataMgr, sharedState, path, spacing, origin]() mutable
        {
            bool ok = dataMgr->SetDataLoaded(path,spacing,origin);

            if (ok) {
                auto img = dataMgr->GetVtkImage();
                if (img) {
                    const auto range = dataMgr->GetScalarRange();
                    const auto spacing = dataMgr->GetSpacing();
                    sharedState->SetFileDataReady(range[0], range[1], spacing);
                }
                else {
                    std::cerr << "[SetFileLoadedAsync] GetVtkImage() returned null after load.\n";
                    sharedState->SetFileLoadFailed();
                }
            }
            else {
                std::cerr << "[SetFileLoadedAsync] Failed to load: " << path << "\n";
                sharedState->SetFileLoadFailed();
            }
        });

    SetTaskStarted(std::move(task), true);
}

bool MedicalVizService::SetReloadFromBufferAsync(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool success)> onComplete)
{
    if (!SetReloadLoadStarted(std::move(onComplete))) {
        return false;
    }

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::packaged_task<void()> task(
        [dataMgr, sharedState, data, dims, spacing, origin]() mutable
        {
            // 在后台线程,注意此时状态还是loading，禁止任何VTK操作
            bool ok = dataMgr->SetFromBuffer(data, dims, spacing, origin);

            if (!ok)
            {
                sharedState->SetReloadLoadFailed();
            }
        });

    SetTaskStarted(std::move(task), true);
    return true;
}

void MedicalVizService::SetTransformedDataSavedAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    SetSaveCompletionCallback(std::move(onComplete));

    // 捕获必要资源，确保在后台线程中的生命周期安全
    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;
    const std::string resolvedPath = path.empty() && dataMgr
        ? dataMgr->GetDefaultTransformedDataPath()
        : path;

    if (!dataMgr || !sharedState) {
        SetSaveCompletionCallbackReady(false);
        return;
    }

    if (resolvedPath.empty()) {
        SetSaveCompletionCallbackReady(false);
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
            self->SetSaveCompletionCallbackReady(ok);
        }
    });

    SetTaskStarted(std::move(task), false);
}

void MedicalVizService::SetSliceImagesSavedAsync(
    const std::string& path,
    const double angle,
    std::function<void(bool success)> onComplete)
{
    SetSaveCompletionCallback(std::move(onComplete));

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;
    const std::string resolvedPath = path;

    if (!dataMgr || !sharedState) {
        SetSaveCompletionCallbackReady(false);
        return;
    }

    const WindowLevelParams currentWindowLevel = sharedState->GetWindowLevel();
    const std::array<double, 16> currentMatrix = sharedState->GetModelMatrix(); // 当前姿态矩阵，导出切片前需要先应用
    const VizMode currentMode = static_cast<VizMode>(m_pendingVizModeInt.load());
    const SliceExportData exportData = InteractionComputeService::GetSliceExportData(
        currentMatrix,
        currentMode,
        sharedState->GetCursorWorld(),
        angle); // 当前切片导出任务的朝向与姿态矩阵

    std::weak_ptr<MedicalVizService> weakSelf = shared_from_this();
    std::packaged_task<void()> task([dataMgr, resolvedPath, exportData, currentWindowLevel, weakSelf]() mutable {
        bool ok = dataMgr->SetSliceImagesSaved(resolvedPath, exportData.orientation, currentWindowLevel, exportData.matrix);

        auto self = weakSelf.lock();
        if (self) {
            self->SetSaveCompletionCallbackReady(ok);
        }
    });

    SetTaskStarted(std::move(task), false);
}

void MedicalVizService::SetFileLoadCanceled()
{
    // 当前未实现可中断加载，接口保留仅为兼容 IFileLoadControlService。
}

// ─────────────────────────────────────────────────────────────────────
// AbstractInteractiveService — 交互接口
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetSliceScrolled(int delta)
{
    if (!m_sharedState || !m_dataManager || !m_dataManager->GetVtkImage()) return;
    const VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    const int axis = InteractionComputeService::GetSliceAxis(mode); // 当前切片滚动应推进的模型坐标轴

    double space[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
    img->GetSpacing(space);

    auto cursorWorld = m_sharedState->GetCursorWorld();
    double cursorModel[3] = { 0.0 };
    GetModelPositionFromWorld(cursorWorld.data(), cursorModel);

    double bounds[6] = { 0.0 };
    img->GetBounds(bounds);
    double nextModel[3] = { 0.0, 0.0, 0.0 }; // 滚轮推进并钳制后的模型坐标
    InteractionComputeService::GetScrolledModelPosition(cursorModel, axis, delta, space, bounds, nextModel);

    double newCursorWorld[3] = { 0.0, 0.0, 0.0 };
    GetWorldPositionFromModel(nextModel, newCursorWorld);
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
    if (!mat) return;

    std::array<double, 16> matData = { 0 }; // 当前模型矩阵快照，回写 SharedState 使用
    std::memcpy(matData.data(), mat->GetData(), 16 * sizeof(double));
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(matData);
    }
}

void MedicalVizService::SetElementVisible(uint32_t flagBit, bool show)
{
    m_sharedState->SetElementVisible(flagBit, show);
}

void MedicalVizService::SetWindowLevelAdjusted(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
{
    if (!m_sharedState) return;

    const WindowLevelParams windowLevel = InteractionComputeService::GetWindowLevel(
        totalDx,
        totalDy,
        viewWidth,
        viewHeight,
        startWW,
        startWC); // 当前拖拽结束后应写回状态的窗宽窗位

    m_sharedState->SetWindowLevel(windowLevel.windowWidth, windowLevel.windowCenter);
    SetSyncRequested();
}

void MedicalVizService::SetModelTransform(
    double translate[3], double rotate[3], double scale[3])
{
    auto currentMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，作为 TRS 叠加基准
    currentMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        currentMatrix->DeepCopy(matrixData.data());
    }

    auto matrix = InteractionComputeService::GetModelMatrix(
        currentMatrix, translate, rotate, scale);
    std::array<double, 16> matData = { 0 }; // TRS 叠加后的模型矩阵快照，供状态同步使用
    std::memcpy(matData.data(), matrix->GetData(), 16 * sizeof(double));
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(matData);
    }
}

void MedicalVizService::SetModelTransformReset()
{
    std::array<double, 16> identity = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    }; // 模型重置后的单位矩阵
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(identity);
    }
}

void MedicalVizService::GetModelPositionFromWorld(const double w[3], double m[3]) const
{
    auto inverseMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // 由 SharedState 现算的逆矩阵，避免维护第二份真相
    inverseMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        inverseMatrix->DeepCopy(matrixData.data());
        inverseMatrix->Invert();
    }

    InteractionComputeService::GetModelPositionFromWorld(inverseMatrix, w, m);
}

void MedicalVizService::GetWorldPositionFromModel(const double m[3], double w[3]) const
{
    auto modelMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，保证 world/model 换算只依赖单一真源
    modelMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        modelMatrix->DeepCopy(matrixData.data());
    }

    InteractionComputeService::GetWorldPositionFromModel(modelMatrix, m, w);
}

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 主线程后处理入口
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetPendingUpdatesProcessed()
{
    // 消费来自第三方重建的 ReconBuffer
    // 必须在 m_needsDataRefresh 检查前，因为 SetPendingImageConsumed 成功后会更新 SharedState 的重载状态与数据可信状态，下面的逻辑再据此驱动管线重建。
    if (m_dataManager && m_dataManager->SetPendingImageConsumed()) {
        // 走和文件加载成功相同的后处理路径
        auto img = m_dataManager->GetVtkImage();
        if (img) {
            const auto range = m_dataManager->GetScalarRange();
            const auto spacing = m_dataManager->GetSpacing();
            m_sharedState->SetReloadDataReady(range[0], range[1], spacing);
        }
        else {
            m_sharedState->SetReloadLoadFailed();
        }
    }

    // 导出异步任务回调触发
    if (GetPendingSaveCompletionCallbackConsumed()) {
        SetPendingSaveCompletionCallbackExecuted();
    }

    // 延迟缓存清理（Detach 必须在主线程）
    if (m_needsCacheClear.exchange(false))
        SetStrategyCacheCleared();

    bool shouldReturnAfterLoadFailure = false;

    // 加载失败处理
    if (m_needsLoadFailed.exchange(false)) {
        SetLoadFailedHandled();
        if (GetFileLoadCallbackBound()) {
            SetFileLoadCallbackReady(false);
        }
        else {
            SetReloadLoadCallbackReady(false);
        }
        shouldReturnAfterLoadFailure = true;
    }

    // DataReady → 重建管线（优先于增量同步）
    if (!shouldReturnAfterLoadFailure && m_needsDataRefresh.exchange(false)) {
        SetPipelineRebuilt();
        if (GetFileLoadCallbackBound()) {
            SetFileLoadCallbackReady(true);
        }
        else {
            SetReloadLoadCallbackReady(true);
        }
        // return; // 本帧只做重建，同步留到下帧
    }

   // 文件加载/重载完成后，都要执行对应回调以通知 UI
    if (GetPendingFileLoadCallbackConsumed()) {
        SetPendingFileLoadCallbackExecuted();
    }
    if (GetPendingReloadLoadCallbackConsumed()) {
        SetPendingReloadLoadCallbackExecuted();
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

void MedicalVizService::SetPendingFlagsMerged(UpdateFlags flags)
{
    int old = m_pendingFlags.load();
    while (!m_pendingFlags.compare_exchange_weak(
        old, old | static_cast<int>(flags))) {
    }
}

void MedicalVizService::SetDataRefreshRequested()
{
    m_needsDataRefresh = true;
}

void MedicalVizService::SetLoadFailedRequested()
{
    m_needsLoadFailed = true;
    m_isDirty = true;
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

    SetOverlayStrategiesCleared();
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
