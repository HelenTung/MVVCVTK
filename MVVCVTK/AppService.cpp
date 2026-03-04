// =====================================================================
// AppService.cpp  v2
// =====================================================================
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
// 构造
// ─────────────────────────────────────────────────────────────────────
MedicalVizService::MedicalVizService(
    std::shared_ptr<AbstractDataManager>    dataMgr,
    std::shared_ptr<SharedInteractionState> state)
    : m_sharedState(state)
    , m_transformService(std::make_unique<VolumeTransformService>(state))
{
    m_dataManager = std::move(dataMgr);
}

// ─────────────────────────────────────────────────────────────────────
// Initialize（由 BindService 触发，shared_from_this() 已安全）
//
// Observer 回调中不再直接调用 ClearStrategyCache()（有 VTK Detach）
//         改为只设 m_needsCacheClear=true，主线程 ProcessPendingUpdates 执行
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

            if (HasFlag(flags, UpdateFlags::DataReady)) {
                // 后台线程：只设标记，禁止任何 VTK 操作
                self->RequestClearStrategyCache(); // 只设 m_needsCacheClear
                self->ResetCursorToCenter();
                self->m_pendingFlags.fetch_or(static_cast<int>(UpdateFlags::All));
                self->m_needsDataRefresh = true;
                // 不调 MarkNeedsSync()，DataReady 走独立路径
                return;
            }

            // 普通事件：原子位或 + 标记需要同步
            int old = self->m_pendingFlags.load();
            while (!self->m_pendingFlags.compare_exchange_weak(
                old, old | static_cast<int>(flags)));
            self->MarkNeedsSync();
        }
    );
}

// ─────────────────────────────────────────────────────────────────────
// IPreInitService — 逐项设置（向后兼容）
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

// ─────────────────────────────────────────────────────────────────────
// IPreInitService::PreInit_CommitConfig（批量提交）
//
// SharedState 只加一次锁，只广播一次，减少多次逐项设置的开销
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::PreInit_CommitConfig(const PreInitConfig& cfg)
{
    // VizMode 只需记录意图，无需写 SharedState
    m_pendingVizModeInt.store(static_cast<int>(cfg.vizMode));
    // 其余参数批量写入 SharedState（内部一次锁 + 一次广播）
    m_sharedState->CommitPreInitConfig(cfg);
}

// ─────────────────────────────────────────────────────────────────────
// IDataLoaderService::LoadFileAsync
//
// 【线程安全】
//   - 加载在 detach 线程执行，dataMgr / sharedState 值捕获保生命周期
//   - onComplete 在后台线程，只允许操作 SharedState
//   - 通知通过 NotifyDataReady 广播，服务通过 Observer 回调接收
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::LoadFileAsync(
    const std::string& path,
    std::function<void(bool success)> onComplete)
{
    auto dataMgr = m_dataManager;
    auto sharedState = m_sharedState;

    std::thread([dataMgr, sharedState, path, onComplete]() {
        bool ok = dataMgr->LoadData(path);

        if (ok) {
            auto img = dataMgr->GetVtkImage();
            if (img) {
                double range[2];
                img->GetScalarRange(range);
                sharedState->NotifyDataReady(range[0], range[1]);
            }
        }
        else {
            std::cerr << "[LoadFileAsync] Failed: " << path << "\n";
        }

        if (onComplete) onComplete(ok);
        }).detach();
}

// ─────────────────────────────────────────────────────────────────────
// ProcessPendingUpdates（主线程 Timer 驱动）
//
// 路由优先级：
//   1. m_needsCacheClear  → ExecuteClearStrategyCache（VTK Detach 在主线程）
//   2. m_needsDataRefresh → PostData_RebuildPipeline
//   3. m_needsSync        → PostData_SyncStateToStrategy
// ─────────────────────────────────────────────────────────────────────
void MedicalVizService::ProcessPendingUpdates()
{
    // 执行延迟缓存清理（修复：Detach 必须在主线程）
    if (m_needsCacheClear.exchange(false))
        ExecuteClearStrategyCache();

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
    if (!strategy) return;

    auto img = m_dataManager->GetVtkImage();
    if (img) {
        int dims[3] = { 0, 0, 0 };
        img->GetDimensions(dims);
        if (dims[0] > 0 && dims[1] > 0 && dims[2] > 0)
            strategy->SetInputData(img);
    }

    SwitchStrategy(strategy);
    MarkNeedsSync(); // 重建后触发一次全量参数同步
}

// ─────────────────────────────────────────────────────────────────────
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

    RenderParams params = BuildRenderParams(flags);
    m_currentStrategy->UpdateVisuals(params, flags);

    m_isDirty = true;
    m_needsSync = false;
}

// ─────────────────────────────────────────────────────────────────────
// BuildRenderParams（私有，只读 SharedState）
// ─────────────────────────────────────────────────────────────────────
RenderParams MedicalVizService::BuildRenderParams(UpdateFlags flags) const
{
    RenderParams p;

    auto pos = m_sharedState->GetCursorPosition();
    p.cursor = { pos[0], pos[1], pos[2] };
    auto range = m_sharedState->GetDataRange();
    p.scalarRange[0] = range[0];
    p.scalarRange[1] = range[1];
    p.material = m_sharedState->GetMaterial();

    if (HasFlag(flags, UpdateFlags::TF))
        m_sharedState->GetTFNodes(p.tfNodes);

    if (HasFlag(flags, UpdateFlags::IsoValue))
        p.isoValue = m_sharedState->GetIsoValue();

    if (HasFlag(flags, UpdateFlags::Transform))
        p.modelMatrix = m_sharedState->GetModelMatrix();

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

// 后台线程调用：只设标记，不碰 VTK
void MedicalVizService::RequestClearStrategyCache()
{
    m_needsCacheClear = true;
}

// 主线程调用：真正执行 Detach + clear
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