#include "Host/HostFeatureBindings.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppState.h"
#include "DataManager.h"
#include "Interaction/CropBridge.h"
#include "OrthogonalCropTypes.h"
#include "Services/GapAnalysisService.h"
#include "StdRenderContext.h"

#include <vtkImageData.h>

#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Host 与 feature 的会话级适配器：保存 core owner、解析窗口目标，并把异步/VTK 收尾收敛到 host timer 线程。
class HostFeatureBindings::Impl final {
public:
    ~Impl();

    void AttachFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);
    bool StartCrop(const HostCropTargetRequest& request);
    bool SwitchCropBox(const HostCropTargetRequest& request);
    bool SwitchCropPlane(const HostCropTargetRequest& request);
    bool SwitchCropView(const HostCropPreviewRequest& request);
    bool SendCrop(const HostCropTargetRequest& request, HostCompleteCallback onComplete);
    bool ExitCrop();
    bool StartGap(const HostGapStartRequest& request, HostCompleteCallback onComplete);
    bool SwitchGapLayer();
    bool ExitGap();
    bool GetGapView() const;
    bool GetCropActive() const;
    void ClearCropInput() const;
    bool SendCropInput();
    bool AttachHostTimer(const HostTimerConfig& config);

private:
    struct SubmitContext final {
        std::mutex mutex; // 保护析构清空与 submit handler 取得同一组依赖快照。
        const HostRenderViewSet* renderViews = nullptr; // 非拥有拓扑；Impl 析构时在锁内置空。
        std::shared_ptr<RawVolumeDataManager> dataManager; // 提交裁切 image 的 pending/current owner。
        std::shared_ptr<SharedInteractionState> sharedState; // Reload admission 与终态发布源。
        std::weak_ptr<CropBridge> bridge; // 成功后回灌新输入，不形成 bridge->handler->bridge 环。
        std::size_t generation = 0; // 记录 context 失效代次，供生命周期诊断；当前 handler 以 renderViews 置空判失效。
    };

    void DetachHostTimer();
    void OnHostTimer();
    static bool GetImageReady(vtkImageData* image);
    static bool ResetCropReload(
        const std::shared_ptr<RawVolumeDataManager>& dataManager,
        const std::shared_ptr<SharedInteractionState>& sharedState,
        const ImageSnapshot& oldSnapshot,
        DataVersion promotedVersion,
        const std::vector<std::shared_ptr<VizService>>& updatedServices);

    bool GetGapConfigValid(const HostGapConfig& config) const;
    GapVoidParams BuildVoidParams(const HostGapVoidConfig& config) const;
    GapSurfaceRequest BuildGapSurfaceRequest(const HostGapSurfaceConfig& config) const;
    std::optional<Orientation> GetGapSliceOrient(HostRenderViewRole role) const;
    CropRemovalMode GetCropRemovalMode(HostCropPreviewMode mode) const;

    // shared_ptr 集合按值保存，确保 feature 运行期间数据、状态和算法服务仍然存在。
    HostCoreServices m_core;
    // 非拥有引用：窗口集合由 session 持有，且析构顺序晚于本绑定层。
    const HostRenderViewSet* m_renderViews = nullptr;
    // Timer context 只用于卸载 observer，不应因 feature tick 延长窗口生命周期。
    std::weak_ptr<StdRenderContext> m_timerContext;
    std::shared_ptr<SubmitContext> m_submitContext;
};

HostFeatureBindings::Impl::~Impl()
{
    DetachHostTimer();
    if (m_core.orthogonalCropBridge) {
        m_core.orthogonalCropBridge->ClearBindings();
    }
    if (m_submitContext) {
        std::lock_guard<std::mutex> lock(m_submitContext->mutex);
        m_submitContext->renderViews = nullptr;
        ++m_submitContext->generation;
    }
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ClearView();
    }
}

bool HostFeatureBindings::Impl::GetImageReady(vtkImageData* image)
{
    if (!image || !image->GetScalarPointer()) {
        return false;
    }

    int dims[3] = { 0, 0, 0 };
    image->GetDimensions(dims);
    return dims[0] > 0 && dims[1] > 0 && dims[2] > 0;
}

bool HostFeatureBindings::Impl::ResetCropReload(
    const std::shared_ptr<RawVolumeDataManager>& dataManager,
    const std::shared_ptr<SharedInteractionState>& sharedState,
    const ImageSnapshot& oldSnapshot,
    DataVersion promotedVersion,
    const std::vector<std::shared_ptr<VizService>>& updatedServices)
{
    const bool isRestored = dataManager
        && dataManager->SetCurrentData(oldSnapshot, promotedVersion);
    if (!isRestored) {
        std::cerr << "[Host] Orthogonal crop compensation skipped for promoted version "
                  << promotedVersion << ": current version changed."
                  << std::endl;
    }

    bool isPipelineRestored = true;
    // CAS 成功时读取恢复的旧 current；CAS 失败时读取更晚 current，避免已更新 view 停留在失败候选。
    // 失败 view 的 BuildPipeline 会自行恢复其旧 snapshot，因此只重建此前成功的 view。
    for (const auto& service : updatedServices) {
        if (!service || !service->SendReloadUpdate()) {
            isPipelineRestored = false;
        }
    }
    if (!isPipelineRestored) {
        std::cerr << "[Host] Orthogonal crop compensation for promoted version "
                  << promotedVersion << " did not restore every view pipeline."
                  << std::endl;
    }

    if (sharedState) {
        sharedState->SetReloadLoadFailed();
        sharedState->ResetLoad(LoadEventKind::Reload);
    }
    return isRestored && isPipelineRestored;
}

bool HostFeatureBindings::Impl::GetGapConfigValid(const HostGapConfig& config) const
{
    if (!std::isfinite(config.surface.dataRangeRatio)
        || !std::isfinite(config.surface.absoluteIsoValue)
        || !std::isfinite(config.voidDetection.grayMin)
        || !std::isfinite(config.voidDetection.grayMax)
        || !std::isfinite(config.voidDetection.minVolumeMM3)
        || !std::isfinite(config.voidDetection.angleThresholdDeg)) {
        return false;
    }
    if (config.surface.isoMode == HostGapAnalysisIsoMode::DataRangeRatio
        && (config.surface.dataRangeRatio < 0.0 || config.surface.dataRangeRatio > 1.0)) {
        std::cerr << "[Host] Gap Analysis activation skipped: iso data range ratio must be within [0, 1]." << std::endl;
        return false;
    }
    if (config.voidDetection.grayMin > config.voidDetection.grayMax) {
        std::cerr << "[Host] Gap Analysis activation skipped: grayMin must not be greater than grayMax." << std::endl;
        return false;
    }
    if (config.voidDetection.tensorWindowSize <= 0) {
        std::cerr << "[Host] Gap Analysis activation skipped: tensorWindowSize must be positive." << std::endl;
        return false;
    }
    if (config.voidDetection.erosionIterations < 0) {
        std::cerr << "[Host] Gap Analysis activation skipped: erosionIterations must not be negative." << std::endl;
        return false;
    }
    return true;
}

GapVoidParams HostFeatureBindings::Impl::BuildVoidParams(const HostGapVoidConfig& config) const
{
    GapVoidParams params;
    params.grayMin = config.grayMin;
    params.grayMax = config.grayMax;
    params.minVolumeMM3 = config.minVolumeMM3;
    params.angleThresholdDeg = config.angleThresholdDeg;
    params.tensorWindowSize = config.tensorWindowSize;
    params.erosionIterations = config.erosionIterations;
    return params;
}

GapSurfaceRequest HostFeatureBindings::Impl::BuildGapSurfaceRequest(const HostGapSurfaceConfig& config) const
{
    GapSurfaceRequest surfaceRequest;
    surfaceRequest.isoMode = config.isoMode == HostGapAnalysisIsoMode::AbsoluteValue
        ? GapIsoMode::AbsoluteValue
        : GapIsoMode::DataRangeRatio;
    surfaceRequest.dataRangeRatio = config.dataRangeRatio;
    surfaceRequest.absoluteIsoValue = config.absoluteIsoValue;
    return surfaceRequest;
}

std::optional<Orientation> HostFeatureBindings::Impl::GetGapSliceOrient(HostRenderViewRole role) const
{
    switch (role) {
    case HostRenderViewRole::TopDownSlice:
        return Orientation::Top_down;
    case HostRenderViewRole::FrontBackSlice:
        return Orientation::Front_back;
    case HostRenderViewRole::LeftRightSlice:
        return Orientation::Left_right;
    default:
        return std::nullopt;
    }
}

CropRemovalMode HostFeatureBindings::Impl::GetCropRemovalMode(HostCropPreviewMode mode) const
{
    switch (mode) {
    case HostCropPreviewMode::KeepInside:
        return CropRemovalMode::KeepInside;
    case HostCropPreviewMode::RemoveInside:
        return CropRemovalMode::RemoveInside;
    default:
        return CropRemovalMode::KeepInside;
    }
}

void HostFeatureBindings::Impl::AttachFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    // core 提供会话级共享 owner，renderViews 提供非拥有窗口拓扑；绑定层把两者降级为 feature 能力。
    // 保存新 owner 后先退出其 Gap 显示，避免复用同一 service 时把上轮 overlay 状态带入新窗口拓扑。
    m_core = core;
    m_renderViews = &renderViews;
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ClearView();
    }
    if (m_core.orthogonalCropBridge) {
        m_submitContext = std::make_shared<SubmitContext>();
        m_submitContext->renderViews = &renderViews;
        m_submitContext->dataManager = m_core.sharedDataMgr;
        m_submitContext->sharedState = m_core.sharedState;
        m_submitContext->bridge = m_core.orthogonalCropBridge;
        const std::weak_ptr<SubmitContext> weakContext = m_submitContext;
        m_core.orthogonalCropBridge->SetSubmitReloadHandler(
            [weakContext](
                vtkSmartPointer<vtkImageData> image,
                std::function<void(bool isSuccess)> onComplete) {
                const auto context = weakContext.lock();
                if (!context || !image) return false;
                // 1. 在 context 锁内复制会话依赖，后续数据提交、管线更新和回调均在锁外执行。
                const HostRenderViewSet* renderViews = nullptr;
                std::shared_ptr<RawVolumeDataManager> dataManager;
                std::shared_ptr<SharedInteractionState> sharedState;
                std::weak_ptr<CropBridge> bridge;
                {
                    std::lock_guard<std::mutex> lock(context->mutex);
                    renderViews = context->renderViews;
                    dataManager = context->dataManager;
                    sharedState = context->sharedState;
                    bridge = context->bridge;
                }
                if (!renderViews || !dataManager || !sharedState) return false;

                // 2. 领取全局 Reload admission；失败表示另一个 File/Reload 事务仍在途。
                if (!sharedState->StartLoad(LoadEventKind::Reload)) {
                    std::cerr << "[Host] Orthogonal crop submit failed: reload is already in progress." << std::endl;
                    return false;
                }
                const auto oldSnapshot = dataManager->GetImageSnapshot();
                bool hasPending = false;
                // 3. 新 image 先进入 pending，再由本 host 线程原子提交为 current；任一步失败发布 ReloadFailed。
                if (!dataManager->SetImageSnapshot(std::move(image))
                    || !dataManager->SetCurrentFromPending(hasPending)
                    || !hasPending) {
                    dataManager->ClearPending();
                    sharedState->SetReloadLoadFailed();
                    sharedState->ResetLoad(LoadEventKind::Reload);
                    return false;
                }

                const auto imageState = dataManager->GetImageSnapshot();
                // 4. 每个 view 用同一 current snapshot 重建管线，全部成功后才发布共享 DataReady 终态。
                bool isPipelineReady = imageState != nullptr;
                std::vector<std::shared_ptr<VizService>> updatedServices;
                for (const auto& view : renderViews->GetViews()) {
                    if (!view.service || !view.service->SendReloadUpdate()) {
                        isPipelineReady = false;
                        break;
                    }
                    updatedServices.push_back(view.service);
                }
                if (!imageState || !isPipelineReady) {
                    ResetCropReload(
                        dataManager,
                        sharedState,
                        oldSnapshot,
                        imageState ? imageState->version : dataManager->GetDataVersion(),
                        updatedServices);
                    return false;
                }
                if (!sharedState->SetReloadDataReady(
                    imageState->scalarRange[0], imageState->scalarRange[1], imageState->spacing)) {
                    ResetCropReload(
                        dataManager,
                        sharedState,
                        oldSnapshot,
                        imageState->version,
                        updatedServices);
                    return false;
                }
                if (const auto lockedBridge = bridge.lock()) {
                    // 5. bridge 只在全局提交成功后接收新输入；最后释放 admission 并通知业务回调。
                    if (GetImageReady(imageState->image)) {
                        lockedBridge->SetInputSnapshot(imageState->image);
                    }
                    else {
                        lockedBridge->SetInputSnapshot(nullptr);
                    }
                }
                sharedState->ResetLoad(LoadEventKind::Reload);
                if (onComplete) {
                    onComplete(true);
                }
                return true;
            });
    }
}

bool HostFeatureBindings::Impl::StartCrop(
    const HostCropTargetRequest& request)
{
    // 裁切至少需要一个 reference view，因为 widget interactor、renderer 和输入模型坐标都来自这一路。
    // preview view 可以为空；那只意味着不显示预览，不影响 reference 链路边界。
    if (request.referenceView.viewId.empty() && !request.referenceView.isViewRoleUsed) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view was not specified." << std::endl;
        return false;
    }

    // 这一步是 host 窗口语义到裁切 bridge 的唯一转换点：
    // 1. reference view 提供 renderer/interactor，并决定 widget 所在坐标语境。
    // 2. preview views 只提供 InteractiveService，用于显示预览和刷新 dirty 状态。
    // 3. 空 preview 不失败，空 reference 必须失败。
    if (!m_renderViews || !m_core.orthogonalCropBridge) {
        std::cerr << "[Host] Orthogonal crop activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }

    const auto* referenceView = m_renderViews->GetViewBySelector(request.referenceView);

    if (!referenceView || !referenceView->service || !referenceView->context) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view is missing." << std::endl;
        return false;
    }

    std::vector<const HostRenderViewRuntime*> previewViews =
        m_renderViews->GetViewsByTargets(request.previewViews);
    if (previewViews.empty() && request.isPreviewViewsUsed) {
        // 裁切预览目标也必须由请求允许后才退到配置默认值；空请求不全选，避免误把新窗口纳入 preview。
        previewViews = m_renderViews->GetCropPreviewViews();
    }

    const auto imageState = m_core.sharedDataMgr
        ? m_core.sharedDataMgr->GetImageSnapshot() : ImageSnapshot{};
    if (!imageState || !GetImageReady(imageState->image)) return false;

    CropViewRequest viewRequest;
    viewRequest.inputImage = imageState->image;
    viewRequest.dataSource = OrthogonalCropDataSource::ImageData;
    viewRequest.interactor = referenceView->context->GetInteractor();
    viewRequest.renderer = referenceView->context->GetRenderer();
    viewRequest.referenceService = referenceView->service;
    viewRequest.previewServices = m_renderViews->BuildServices(previewViews);
    return m_core.orthogonalCropBridge->StartView(std::move(viewRequest));
}

bool HostFeatureBindings::Impl::StartGap(
    const HostGapStartRequest& request,
    HostCompleteCallback onComplete)
{
    // request 同时携带宿主窗口选择和算法参数：target id/role 只在 Host 层解析，
    // Gap service 接收降级后的 3D/slice OverlayService 列表，不知道窗口编号或布局。
    if (!m_renderViews) {
        std::cerr << "[Host] Gap Analysis display activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }
    if (m_timerContext.expired()) {
        std::cerr << "[Host] Gap Analysis display activation skipped: timer is not attached." << std::endl;
        return false;
    }
    if (request.targetViews.viewIds.empty()
        && request.targetViews.viewRoles.empty()
        && !request.isDefaultOverlayUsed) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no render view target was requested." << std::endl;
        return false;
    }
    if (!GetGapConfigValid(request.algorithm)) {
        return false;
    }

    std::vector<const HostRenderViewRuntime*> targetViews =
        m_renderViews->GetViewsByTargets(request.targetViews);
    if (targetViews.empty() && request.isDefaultOverlayUsed) {
        // 默认 overlay role 只有在宿主明确允许 fallback 时才使用；空请求不能被解释成全窗口。
        targetViews = m_renderViews->GetGapOverlayViews();
    }
    if (targetViews.empty()) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no overlay render view target was found." << std::endl;
        return false;
    }

    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis display activation skipped: feature service is not ready." << std::endl;
        return false;
    }

    std::vector<std::shared_ptr<OverlayService>> meshOverlayTargets;
    std::vector<std::pair<Orientation, std::shared_ptr<OverlayService>>> sliceOverlayTargets;
    meshOverlayTargets.reserve(targetViews.size());
    sliceOverlayTargets.reserve(targetViews.size());
    for (const auto* view : targetViews) {
        if (!view || !view->service) {
            continue;
        }

        if (m_renderViews->GetRoleIs3DView(view->config.role)) {
            meshOverlayTargets.push_back(view->service);
        }

        const auto orientation = GetGapSliceOrient(view->config.role);
        if (orientation) {
            sliceOverlayTargets.push_back({ *orientation, view->service });
        }
    }

    // Host 启动时一次性交付隔离 image；后续 timer 只消费 worker 终态，不查询 feature 私有阶段。
    const auto imageState = m_core.sharedDataMgr
        ? m_core.sharedDataMgr->GetImageSnapshot() : ImageSnapshot{};
    if (!imageState || !GetImageReady(imageState->image)) return false;

    GapViewRequest viewRequest;
    viewRequest.inputImage = imageState->image;
    viewRequest.surface = BuildGapSurfaceRequest(request.algorithm.surface);
    viewRequest.voidParams = BuildVoidParams(request.algorithm.voidDetection);
    viewRequest.meshTargets = std::move(meshOverlayTargets);
    viewRequest.sliceTargets = std::move(sliceOverlayTargets);
    return m_core.gapAnalysis->StartView(
        std::move(viewRequest), std::move(onComplete));
}

bool HostFeatureBindings::Impl::SwitchGapLayer()
{
    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis overlay switch skipped: feature service is not ready." << std::endl;
        return false;
    }
    return m_core.gapAnalysis->SwitchOverlay();
}

bool HostFeatureBindings::Impl::ExitGap()
{
    if (!m_core.gapAnalysis) {
        return false;
    }
    return m_core.gapAnalysis->ExitView();
}

bool HostFeatureBindings::Impl::GetGapView() const
{
    return m_core.gapAnalysis && m_core.gapAnalysis->GetViewOn();
}

bool HostFeatureBindings::Impl::SwitchCropBox(
    const HostCropTargetRequest& request)
{
    // Switch 前先 Start，是为了让上位机切换窗口布局后，裁切 bridge 始终使用最新 reference/preview 目标。
    if (!StartCrop(request)) {
        return false;
    }
    return m_core.orthogonalCropBridge->SwitchCropBox();
}

bool HostFeatureBindings::Impl::SwitchCropPlane(
    const HostCropTargetRequest& request)
{
    if (!StartCrop(request)) {
        return false;
    }
    return m_core.orthogonalCropBridge->SwitchCropPlane();
}

bool HostFeatureBindings::Impl::SwitchCropView(
    const HostCropPreviewRequest& request)
{
    if (!StartCrop(request.target)) {
        return false;
    }

    m_core.orthogonalCropBridge->SwitchPreview(GetCropRemovalMode(request.previewMode));
    return true;
}

bool HostFeatureBindings::Impl::SendCrop(
    const HostCropTargetRequest& request,
    HostCompleteCallback onComplete)
{
    // 每次 submit 前重绑 request 指定的 reference/preview 目标并刷新输入版本。
    // 返回值只表示 reload handler 是否接受本次结果，不保证完成回调或新帧已经发生。
    if (!StartCrop(request)) {
        return false;
    }

    return m_core.orthogonalCropBridge->SendSubmit(std::move(onComplete));
}

bool HostFeatureBindings::Impl::ExitCrop()
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->ExitCrop();
}

bool HostFeatureBindings::Impl::GetCropActive() const
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->GetCropActive();
}

void HostFeatureBindings::Impl::ClearCropInput() const
{
    if (m_core.orthogonalCropBridge) {
        m_core.orthogonalCropBridge->SetInputSnapshot(nullptr);
    }
}

bool HostFeatureBindings::Impl::SendCropInput()
{
    if (!m_core.orthogonalCropBridge || !m_core.sharedDataMgr) {
        return false;
    }

    const auto imageState = m_core.sharedDataMgr->GetImageSnapshot();
    if (!imageState || !GetImageReady(imageState->image)) {
        m_core.orthogonalCropBridge->SetInputSnapshot(nullptr);
        std::cerr << "[Host] Orthogonal crop init failed: input image missing." << std::endl;
        return false;
    }

    m_core.orthogonalCropBridge->SetInputSnapshot(imageState->image);
    return true;
}

bool HostFeatureBindings::Impl::AttachHostTimer(const HostTimerConfig& config)
{
    // eventPumpConfig 只选择提供主线程 TimerEvent 的 view；它不是 Gap overlay 目标。
    if (!config.isTimerEnabled || !m_renderViews) {
        DetachHostTimer();
        return false;
    }

    const auto* timerView = m_renderViews->GetViewBySelector(config.targetView);

    if (!timerView || !timerView->context) {
        std::cerr << "[Host] Timer event pump skipped: explicit timer render view is missing." << std::endl;
        DetachHostTimer();
        return false;
    }

    // 重配时先卸载旧 handler；m_timerContext 的弱引用不会延长窗口生命周期。
    DetachHostTimer();
    m_timerContext = timerView->context;
    timerView->context->SetTimerHandler([impl = this]() {
        impl->OnHostTimer();
    });
    return true;
}

void HostFeatureBindings::Impl::OnHostTimer()
{
    if (!m_core.gapAnalysis) {
        return;
    }

    // host 只提供主线程 tick 和当前数据快照入口；GapAnalysis 自己判断是否有 pending 显示请求。
    if (!m_core.gapAnalysis->GetDisplayTickNeeded()) {
        return;
    }
    m_core.gapAnalysis->OnDisplayTick(nullptr);
}

void HostFeatureBindings::Impl::DetachHostTimer()
{
    if (const auto context = m_timerContext.lock()) {
        context->ClearTimerHandler();
    }
    m_timerContext.reset();
}

HostFeatureBindings::HostFeatureBindings()
    : m_impl(std::make_unique<HostFeatureBindings::Impl>())
{
}

HostFeatureBindings::~HostFeatureBindings() = default;

void HostFeatureBindings::AttachFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_impl->AttachFeatures(core, renderViews);
}

bool HostFeatureBindings::StartCrop(const HostCropTargetRequest& request)
{
    return m_impl && m_impl->StartCrop(request);
}

bool HostFeatureBindings::SwitchCropBox(const HostCropTargetRequest& request)
{
    return m_impl && m_impl->SwitchCropBox(request);
}

bool HostFeatureBindings::SwitchCropPlane(const HostCropTargetRequest& request)
{
    return m_impl && m_impl->SwitchCropPlane(request);
}

bool HostFeatureBindings::SwitchCropView(const HostCropPreviewRequest& request)
{
    return m_impl && m_impl->SwitchCropView(request);
}

bool HostFeatureBindings::SendCrop(
    const HostCropTargetRequest& request,
    HostCompleteCallback onComplete)
{
    return m_impl && m_impl->SendCrop(request, std::move(onComplete));
}

bool HostFeatureBindings::ExitCrop()
{
    return m_impl && m_impl->ExitCrop();
}

bool HostFeatureBindings::StartGap(
    const HostGapStartRequest& request,
    HostCompleteCallback onComplete)
{
    return m_impl && m_impl->StartGap(request, std::move(onComplete));
}

bool HostFeatureBindings::SwitchGapLayer()
{
    return m_impl && m_impl->SwitchGapLayer();
}

bool HostFeatureBindings::ExitGap()
{
    return m_impl && m_impl->ExitGap();
}

bool HostFeatureBindings::GetGapView() const
{
    return m_impl->GetGapView();
}

bool HostFeatureBindings::GetCropActive() const
{
    return m_impl->GetCropActive();
}

void HostFeatureBindings::ClearCropInput() const
{
    m_impl->ClearCropInput();
}

bool HostFeatureBindings::SendCropInput()
{
    return m_impl->SendCropInput();
}

bool HostFeatureBindings::AttachHostTimer(const HostTimerConfig& config)
{
    return m_impl && m_impl->AttachHostTimer(config);
}
