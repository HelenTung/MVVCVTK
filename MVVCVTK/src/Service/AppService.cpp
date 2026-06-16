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
            m_currentStrategy->ConfigureCamera(m_renderer);
            m_isDirty = true;
        }
        return;
    }

    if (m_currentStrategy && m_renderer)
        m_currentStrategy->DetachRenderer(m_renderer);

    m_currentStrategy = newStrategy;

    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->AttachRenderer(m_renderer);
        m_currentStrategy->ConfigureCamera(m_renderer);
    }
    if (m_renderer)
        m_renderer->ResetCamera();
    m_isDirty = true;
}

void AbstractAppService::AddOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy) {
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
        strategy->AttachRenderer(m_renderer);
    }

    m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
    m_needsSync = true;
    m_isDirty = true;
}

void AbstractAppService::RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy) {
    if (!strategy) return;

    const auto it = std::find_if(m_overlayStrategies.begin(), m_overlayStrategies.end(),
        [strategy](const std::shared_ptr<AbstractVisualStrategy>& current) {
            return current.get() == strategy.get();
        });
    if (it == m_overlayStrategies.end()) {
        return;
    }

    if (m_renderer) {
        strategy->DetachRenderer(m_renderer);
    }
    m_overlayStrategies.erase(it);
    m_isDirty = true;
}

void AbstractAppService::ClearOverlayStrategies() {
    if (m_renderer) {
        for (auto& s : m_overlayStrategies) {
            s->DetachRenderer(m_renderer);
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
    std::lock_guard<std::mutex> lk(m_activeLoadMutex);
    if (m_activeLoadFuture.valid())
        m_activeLoadFuture.wait();
}

// ─────────────────────────────────────────────────────────────────────
// RenderContext 绑定
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::SetRenderContext(
    vtkSmartPointer<vtkRenderWindow> win,
    vtkSmartPointer<vtkRenderer> ren)
{
    AbstractAppService::SetRenderContext(win, ren);
    ApplyRendererBackground();
    if (!m_stateEventSource) return;

    std::weak_ptr<MedicalVizService> weakSelf =
        std::static_pointer_cast<MedicalVizService>(shared_from_this());

    m_stateEventSource->SetObserver(shared_from_this(),
        [weakSelf](UpdateFlags flags)
        {
            auto self = weakSelf.lock();
            if (!self) return;

            if (HasFlag(flags, UpdateFlags::DataReady) && self->m_sharedState) {
                self->m_pendingLoadEventKind = self->m_sharedState->GetPendingLoadEventKind();
            }

            // 广播回调只做“事件翻译”为主线程可消费的原子标志，
            // 不在这里直接碰 VTK/Strategy，避免后台线程或任意调用线程破坏渲染线程边界。
            self->m_stateSyncStrategy.HandleFlags(flags, *self);
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

void MedicalVizService::StartTask(
    std::packaged_task<void()> task,
    bool keepActiveLoadFuture)
{
    if (keepActiveLoadFuture) {
        std::lock_guard<std::mutex> lk(m_activeLoadMutex);
        m_activeLoadFuture = task.get_future(); // 当前活动加载任务的 future，用于析构时等待后台线程结束
    }
    // 线程统一 detach，说明 Service 只关心生命周期托管和结果回收，不在调用点阻塞等待后台任务。
    std::thread(std::move(task)).detach();
}

void MedicalVizService::LoadFileAsync(
    const std::string& path,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool success)> onComplete)
{
    if (!m_sharedState) {
        m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return;
    }

    if (m_sharedState->GetFileLoadState() == LoadState::Loading
        || m_sharedState->GetReloadLoadState() == LoadState::Loading)
    {
        std::cerr << "[LoadFileAsync] Loading is already in progress.\n";
        m_fileLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return;
    }

    m_fileLoadCallbackState.SetCallback(std::move(onComplete));
    m_sharedState->SetFileLoadStarted();

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::packaged_task<void()> task([dataMgr, sharedState, path, spacing, origin]() mutable
        {
            // 后台任务只负责 I/O 和基础数据快照准备；
            // 一旦成功，就通过 SharedState 发出 DataReady，让主线程后续统一重建渲染管线。
            bool ok = dataMgr->SetDataLoaded(path,spacing,origin);

            if (ok) {
                auto img = dataMgr->GetVtkImage();
                if (img) {
                    const auto range = dataMgr->GetScalarRange();
                    const auto spacing = dataMgr->GetSpacing();
                    sharedState->SetFileDataReady(range[0], range[1], spacing);
                }
                else {
                    std::cerr << "[LoadFileAsync] GetVtkImage() returned null after load.\n";
                    sharedState->SetFileLoadFailed();
                }
            }
            else {
                std::cerr << "[LoadFileAsync] Failed to load: " << path << "\n";
                sharedState->SetFileLoadFailed();
            }
        });

    StartTask(std::move(task), true);
}

bool MedicalVizService::ReloadFromBufferAsync(
    const float* data,
    const std::array<int, 3>& dims,
    const std::array<float, 3>& spacing,
    const std::array<float, 3>& origin,
    std::function<void(bool success)> onComplete)
{
    if (!m_sharedState) {
        m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return false;
    }

    if (m_sharedState->GetFileLoadState() == LoadState::Loading
        || m_sharedState->GetReloadLoadState() == LoadState::Loading)
    {
        std::cerr << "[ReloadFromBufferAsync] Loading is already in progress.\n";
        m_reloadLoadCallbackState.SetCallbackReady(false, std::move(onComplete));
        return false;
    }

    m_reloadLoadCallbackState.SetCallback(std::move(onComplete));
    m_sharedState->SetReloadLoadStarted();

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::packaged_task<void()> task(
        [dataMgr, sharedState, data, dims, spacing, origin]() mutable
        {
            // 在后台线程内只构建待提交镜像；真正的 vtkImage 切换要等主线程在 ProcessPendingUpdates 中消费。
            bool ok = dataMgr->SetFromBuffer(data, dims, spacing, origin);

            if (!ok)
            {
                sharedState->SetReloadLoadFailed();
            }
        });

    StartTask(std::move(task), true);
    return true;
}

void MedicalVizService::SaveTransformedDataAsync(
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
    std::array<double, 16> modelToWorldMatrixSnapshot = sharedState->GetModelMatrix();
    std::weak_ptr<MedicalVizService> weakSelf = shared_from_this();

    // 封装异步任务
    std::packaged_task<void()> task([dataMgr, resolvedPath, modelToWorldMatrixSnapshot, weakSelf]() mutable {
        // 进入后台线程，执行重采样和保存操作
        bool ok = dataMgr->SaveTransformedData(resolvedPath, modelToWorldMatrixSnapshot);

        auto self = weakSelf.lock();
        if (self) {
            self->SetSaveCallbackReady(ok);
        }
    });

    StartTask(std::move(task), false);
}

void MedicalVizService::SaveSliceImagesAsync(
    const std::string& path,
    const double angle,
    std::function<void(bool success)> onComplete)
{
    SetSaveCallback(std::move(onComplete));

    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;
    const std::string resolvedPath = path;

    if (!dataMgr || !sharedState) {
        SetSaveCallbackReady(false);
        return;
    }

    const WindowLevelParams currentWindowLevel = sharedState->GetWindowLevel();
    const std::array<double, 16> modelToWorldMatrixSnapshot = sharedState->GetModelMatrix(); // 当前姿态矩阵，导出切片前需要先应用
    const VizMode currentMode = static_cast<VizMode>(m_pendingVizModeInt.load());
    const SliceExportData exportData = InteractionComputeService::GetSliceExportData(
        modelToWorldMatrixSnapshot,
        currentMode,
        sharedState->GetCursorWorld(),
        angle); // 当前切片导出任务的朝向与姿态矩阵

    std::weak_ptr<MedicalVizService> weakSelf = shared_from_this();
    std::packaged_task<void()> task([dataMgr, resolvedPath, exportData, currentWindowLevel, weakSelf]() mutable {
        bool ok = dataMgr->SaveSliceImages(resolvedPath, exportData.orientation, currentWindowLevel, exportData.matrix);

        auto self = weakSelf.lock();
        if (self) {
            self->SetSaveCallbackReady(ok);
        }
    });

    StartTask(std::move(task), false);
}

void MedicalVizService::CancelFileLoad()
{
    // 当前未实现可中断加载，接口保留仅为兼容 IFileLoadControlService。
}

// ─────────────────────────────────────────────────────────────────────
// AbstractInteractiveService — 交互接口
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ScrollSlice(int delta)
{
    if (!m_sharedState || !m_dataManager || !m_dataManager->GetVtkImage()) return;
    const VizMode mode = static_cast<VizMode>(m_pendingVizModeInt.load());
    const int axis = InteractionComputeService::GetSliceAxis(mode); // 当前切片滚动应推进的模型坐标轴
    if (axis < 0)
		return;

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
    RequestSync();
}

void MedicalVizService::SetCursorWorldPosition(double worldPos[3], int axis)
{
    if (!m_dataManager || !m_dataManager->GetVtkImage() || !m_sharedState) return;
    auto currentPos = m_sharedState->GetCursorWorld();
    m_sharedState->SetCursorRawWorld(worldPos[0], worldPos[1], worldPos[2]);
    m_sharedState->SetCursorAxis(axis);

    // 0 1 2 分别表示 SliceLeft_right SliceFront_back SliceTop_down
    if (axis == -1)
    {
        m_sharedState->SetCursorWorld(worldPos[0], worldPos[1], worldPos[2]);
    }
    else {
        double newPos[3] = { worldPos[0], worldPos[1], worldPos[2] };
        newPos[axis] = currentPos[axis];
        m_sharedState->SetCursorWorld(newPos[0], newPos[1], newPos[2]);
    }
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

void MedicalVizService::SyncModelMatrix(vtkMatrix4x4* modelToWorldMatrix)
{
    if (!modelToWorldMatrix) return;

    std::array<double, 16> matData = { 0 }; // 当前模型矩阵快照，回写 SharedState 使用
    std::memcpy(matData.data(), modelToWorldMatrix->GetData(), 16 * sizeof(double));
    if (m_sharedState) {
        m_sharedState->SetModelMatrix(matData);
    }
}

void MedicalVizService::SetElementVisible(uint32_t flagBit, bool show)
{
    m_sharedState->SetElementVisible(flagBit, show);
}

void MedicalVizService::AdjustWindowLevel(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC)
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
    RequestSync();
}

void MedicalVizService::SetModelTransform(
    double translate[3], double rotate[3], double scale[3])
{
    auto currentModelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，作为 TRS 叠加基准
    currentModelToWorldMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        currentModelToWorldMatrix->DeepCopy(matrixData.data());
    }

    auto nextModelToWorldMatrix = InteractionComputeService::GetModelMatrix(
        currentModelToWorldMatrix, translate, rotate, scale);
    std::array<double, 16> matData = { 0 }; // TRS 叠加后的模型矩阵快照，供状态同步使用
    std::memcpy(matData.data(), nextModelToWorldMatrix->GetData(), 16 * sizeof(double));
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
    auto worldToModelMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // 由 SharedState 现算的逆矩阵，避免维护第二份真相
    worldToModelMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        worldToModelMatrix->DeepCopy(matrixData.data());
        worldToModelMatrix->Invert();
    }

    InteractionComputeService::GetModelPositionFromWorld(worldToModelMatrix, w, m);
}

void MedicalVizService::GetWorldPositionFromModel(const double m[3], double w[3]) const
{
    auto modelToWorldMatrix = vtkSmartPointer<vtkMatrix4x4>::New(); // SharedState 当前模型矩阵快照，保证 world/model 换算只依赖单一真源
    modelToWorldMatrix->Identity();
    if (m_sharedState) {
        const auto matrixData = m_sharedState->GetModelMatrix();
        modelToWorldMatrix->DeepCopy(matrixData.data());
    }

    InteractionComputeService::GetWorldPositionFromModel(modelToWorldMatrix, m, w);
}

// ─────────────────────────────────────────────────────────────────────
// AbstractAppService — 主线程后处理入口
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ProcessPendingUpdates()
{
    // 统一主线程后处理链路：
    // 1. 消费后台提交的新 vtkImageData
    // 2. 执行异步导出/加载完成回调
    // 3. 处理失败与重建请求
    // 4. 最后再做普通增量同步

    // 数据源只有一份；pending image 由 DataManager 自己保证只被消费一次。
    if (m_dataManager && m_dataManager->ConsumePendingImage()) {
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
    if (ConsumeSaveCallback()) {
        ExecutePendingSaveCallback();
    }

    // 延迟缓存清理（Detach 必须在主线程）
    if (m_needsCacheClear.exchange(false))
        ClearStrategyCache();

    bool shouldReturnAfterLoadFailure = false;
    bool loadEventHandled = false;

    // 加载失败处理
    if (m_needsLoadFailed.exchange(false)) {
        const LoadEventKind loadEventKind = m_pendingLoadEventKind;
        HandleLoadFailure();
        if (loadEventKind == LoadEventKind::File) {
            m_fileLoadCallbackState.SetCallbackReady(false);
        }
        else if (loadEventKind == LoadEventKind::Reload) {
            m_reloadLoadCallbackState.SetCallbackReady(false);
        }
        shouldReturnAfterLoadFailure = true;
        loadEventHandled = true;
    }

    // DataReady → 重建管线（优先于增量同步）
    if (!shouldReturnAfterLoadFailure && m_needsDataRefresh.exchange(false)) {
        const LoadEventKind loadEventKind = m_pendingLoadEventKind;
        RebuildPipeline();
        if (loadEventKind == LoadEventKind::File) {
            m_fileLoadCallbackState.SetCallbackReady(true);
        }
        else if (loadEventKind == LoadEventKind::Reload) {
            m_reloadLoadCallbackState.SetCallbackReady(true);
        }
        loadEventHandled = true;
        // return; // 本帧只做重建，同步留到下帧
    }

    if (shouldReturnAfterLoadFailure) {
        if (m_fileLoadCallbackState.GetPendingCallbackConsumed()) {
            m_fileLoadCallbackState.ExecutePendingCallback();
        }
        if (m_reloadLoadCallbackState.GetPendingCallbackConsumed()) {
            m_reloadLoadCallbackState.ExecutePendingCallback();
        }
        if (loadEventHandled) {
            m_pendingLoadEventKind = LoadEventKind::None;
        }
        return;
    }

    // 普通事件增量同步
    SyncStrategyState();

    // 文件加载/重载回调延后到策略同步之后，保证上层 workflow 看到的是主线程收敛后的最终状态。
    if (m_fileLoadCallbackState.GetPendingCallbackConsumed()) {
        m_fileLoadCallbackState.ExecutePendingCallback();
    }
    if (m_reloadLoadCallbackState.GetPendingCallbackConsumed()) {
        m_reloadLoadCallbackState.ExecutePendingCallback();
    }

    if (loadEventHandled) {
        m_pendingLoadEventKind = LoadEventKind::None;
    }
}

// ─────────────────────────────────────────────────────────────────────
// 私有辅助
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::RebuildPipeline()
{
    // 这一步只处理“结构性变化后的管线重建”：选对 Strategy、喂入最新图像、重新挂接渲染器。
    // 具体材质、TF、窗宽窗位等参数同步故意留到后续增量同步阶段再做。
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
    ApplyRendererBackground();
    RequestSync(); // 重建后触发一次全量参数同步
}

void MedicalVizService::SyncStrategyState()
{
    bool expected = true;
    if (!m_needsSync.compare_exchange_strong(expected, false)) return;
    if (!m_currentStrategy) return;

    // 用 exchange(0) 取走当前整包增量标志，相当于把这一帧前累计的状态改动做一次原子快照。
    // 后续新来的事件会写入新的 m_pendingFlags，留到下一帧继续消费，不会和本次同步互相覆盖。
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
        ApplyRendererBackground();
        flags = static_cast<UpdateFlags>(
            static_cast<int>(flags) & ~static_cast<int>(UpdateFlags::Background));
    }
	// 判断取了背景色标志后剩下的位，如果没有了才标脏，否则继续走 Strategy 同步剩余状态
    // 避免每次背景色改动都重置全局脏标志导致不必要的渲染刷新。
    if (flags == UpdateFlags::None) {
        m_isDirty = true;
        return;
    }

    RenderParams params = GetRenderParams(flags);
    m_currentStrategy->SetVisualState(params, flags);

    for (auto& overlay : m_overlayStrategies) {
        overlay->SetVisualState(params, flags);
    }

    m_isDirty = true;
    // m_needsSync = false;
}

void MedicalVizService::HandleLoadFailure()
{
    std::cerr << "[HandleLoadFailure] Load failed; clearing pipeline state.\n";

    // 清理策略缓存（无有效数据，不应保留旧 Strategy）
    ClearStrategyCache();

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

    // RenderParams 是当前这一帧需要下发给 Strategy 的“最小快照”，
    // 只按 flags 拿必要字段，避免每次同步都把全部状态搬运一遍。

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

    if (HasFlag(flags, UpdateFlags::Visibility))
		p.visibilityMask = m_sharedState->GetVisibilityMask();

    return p;
}

std::shared_ptr<AbstractVisualStrategy> MedicalVizService::GetStrategy(VizMode mode)
{
    // Strategy 以 VizMode 为键缓存，说明模式切换是“复用既有渲染对象”而不是“每次全新构建”。
    auto it = m_strategyCache.find(mode);
    if (it != m_strategyCache.end()) return it->second;

    auto s = StrategyFactory::GetStrategy(mode);
    if (s) m_strategyCache[mode] = s;
    return s;
}

void MedicalVizService::ApplyRendererBackground()
{
    if (!m_renderer || !m_sharedState) {
        return;
    }

    const auto bg = m_sharedState->GetBackground();
    m_renderer->SetBackground(bg.r, bg.g, bg.b);
}

void MedicalVizService::MergePendingFlags(UpdateFlags flags)
{
    int old = m_pendingFlags.load();
    // compare_exchange_weak 在竞争下允许伪失败，但循环体很小，适合这里做位图 OR 合并。
    // 这样多个线程/回调同时上报更新时，只会不断把新位并进同一个原子整数，不会丢标志。
    while (!m_pendingFlags.compare_exchange_weak(
        old, old | static_cast<int>(flags))) {
    }
}

void MedicalVizService::RequestDataRefresh()
{
    m_needsDataRefresh = true;
}

void MedicalVizService::RequestLoadFailed()
{
    m_needsLoadFailed = true;
    m_isDirty = true;
}

void MedicalVizService::RequestStrategyCacheClear()
{
    m_needsCacheClear = true;
}

void MedicalVizService::ClearStrategyCache()
{
    // 清缓存时先把当前 Strategy 从 renderer 上摘掉，再清 overlay 和缓存表，
    // 这样可以保证下一次重建拿到的是一套完全干净的渲染节点。
    if (m_currentStrategy && m_renderer) {
        m_currentStrategy->DetachRenderer(m_renderer);
        m_currentStrategy = nullptr;
    }

    ClearOverlayStrategies();
    m_strategyCache.clear();
}

void MedicalVizService::CenterCursor()
{
    if (!m_dataManager || !m_dataManager->GetVtkImage()) return;
    // DataReady 后把联动光标重置到新体数据中心，避免沿用旧数据上的 cursor 位置导致切片落在无效区域。
    double imgCenter[3] = { 0.0 };
    auto img = m_dataManager->GetVtkImage();
    img->GetCenter(imgCenter);

    double imgCenterWorld[3];
    GetWorldPositionFromModel(imgCenter, imgCenterWorld);
    m_sharedState->SetCursorRawWorld(imgCenterWorld[0], imgCenterWorld[1], imgCenterWorld[2]);
    m_sharedState->SetCursorAxis(-1);
    m_sharedState->SetCursorWorld(imgCenterWorld[0], imgCenterWorld[1], imgCenterWorld[2]);
}

void MedicalVizService::RequestSync()
{
    // 这里只声明“下一帧需要把状态推给 Strategy”，不直接同步，保持所有渲染改动都经由 Timer 主循环收口。
    m_needsSync = true;
    m_isDirty = true;
}
