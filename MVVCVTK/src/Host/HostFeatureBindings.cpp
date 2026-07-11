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
#include <optional>
#include <string>
#include <utility>
#include <vector>

class HostFeatureBindings::Impl final {
public:
    ~Impl();

    void AttachFeatures(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);
    bool StartCrop(const HostCropViewRequest& request);
    bool StartGapView(const HostGapViewRequest& request);
    bool SwitchGapView(const HostGapViewRequest& request);
    bool SwitchGapLayer();
    bool ExitGapView();
    bool GetGapView() const;
    bool SwitchCropBox(const HostCropViewRequest& request);
    bool SwitchCropPlane(const HostCropViewRequest& request);
    bool SwitchCropView(
        const HostCropViewRequest& request,
        HostCropPreviewMode previewMode);
    bool SendCrop(const HostCropViewRequest& request);
    bool ExitCrop();
    bool ExitFeature();
    bool GetCropActive() const;
    void ClearCropInput() const;
    std::function<bool()> BuildCropInput() const;
    void AttachHostTimer(const HostTimerEventPumpConfig& eventPumpConfig);
    void DetachHostTimer();

private:
    void OnHostTimer();
    bool SetCropInput();
    static bool GetImageReady(vtkImageData* image);

    bool GetGapConfigValid(const HostGapConfig& config) const;
    VoidDetectionParams BuildVoidParams(const HostGapVoidConfig& config) const;
    GapAnalysisSurfaceRequest BuildGapSurfaceRequest(const HostGapSurface& config) const;
    std::optional<Orientation> GetGapSliceOrient(HostRenderViewRole role) const;
    CropRemovalMode GetCropRemovalMode(HostCropPreviewMode mode) const;

    // shared_ptr 集合按值保存，确保 feature 运行期间数据、状态和算法服务仍然存在。
    HostCoreServices m_core;
    // 非拥有引用：窗口集合由 session 持有，且析构顺序晚于本绑定层。
    const HostRenderViewSet* m_renderViews = nullptr;
    // Timer context 只用于卸载 observer，不应因 feature tick 延长窗口生命周期。
    std::weak_ptr<StdRenderContext> m_timerContext;
};

HostFeatureBindings::Impl::~Impl()
{
    DetachHostTimer();
    if (m_core.orthogonalCropBridge) {
        m_core.orthogonalCropBridge->SetSubmitReloadHandler(nullptr);
    }
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ExitView();
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

bool HostFeatureBindings::Impl::GetGapConfigValid(const HostGapConfig& config) const
{
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

VoidDetectionParams HostFeatureBindings::Impl::BuildVoidParams(const HostGapVoidConfig& config) const
{
    VoidDetectionParams params;
    params.grayMin = config.grayMin;
    params.grayMax = config.grayMax;
    params.minVolumeMM3 = config.minVolumeMM3;
    params.angleThresholdDeg = config.angleThresholdDeg;
    params.tensorWindowSize = config.tensorWindowSize;
    params.erosionIterations = config.erosionIterations;
    return params;
}

GapAnalysisSurfaceRequest HostFeatureBindings::Impl::BuildGapSurfaceRequest(const HostGapSurface& config) const
{
    GapAnalysisSurfaceRequest surfaceRequest;
    surfaceRequest.isoMode = config.isoMode == HostGapAnalysisIsoMode::AbsoluteValue
        ? GapAnalysisIsoValueMode::AbsoluteValue
        : GapAnalysisIsoValueMode::DataRangeRatio;
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
        m_core.gapAnalysis->ExitView();
    }
    if (m_core.orthogonalCropBridge) {
        // submit 后需要刷新所有现有视图的共享数据，但不应该把“哪些窗口参与 preview”的语义写进 reload。
        // 因此这里保存弱引用集合，只做数据更新通知；窗口消失时 weak_ptr 自然失效。
        std::vector<std::weak_ptr<VizService>> renderViewServices;
        renderViewServices.reserve(renderViews.GetViews().size());
        for (const auto& view : renderViews.GetViews()) {
            renderViewServices.push_back(view.service);
        }

        // submit 回调只提交新图像和刷新共享状态，不持有窗口角色。
        // bridge 只接收 host 注入的 image/version，不反向读取 DataManager。
        // reload 能力在这里注入，CropBridge 因而不需要反向依赖 DataManager 或窗口集合。
        m_core.orthogonalCropBridge->SetSubmitReloadHandler(
            [
                bridge = std::weak_ptr<CropBridge>(m_core.orthogonalCropBridge),
                sharedDataMgr = m_core.sharedDataMgr,
                sharedState = m_core.sharedState,
                renderViewServices = std::move(renderViewServices)
            ](
                vtkSmartPointer<vtkImageData> image,
                std::function<void(bool isSuccess)> onComplete) {
                // image 是本次 submit 的结果快照；绑定层按 pending commit 流程回写共享数据。
                if (!sharedDataMgr || !sharedState || !image) {
                    return false;
                }

                if (sharedState->GetFileLoadState() == LoadState::Loading
                    || sharedState->GetReloadLoadState() == LoadState::Loading)
                {
                    // reload 必须串行，否则裁切 submit 产生的新 image 和后台加载 image 可能抢同一个 shared DataManager。
                    std::cerr << "[Host] Orthogonal crop submit failed: reload is already in progress." << std::endl;
                    return false;
                }

                // 先标记 reload，再在同一调用链发布/接管 pending image；失败状态不能发布 DataReady。
                sharedState->SetReloadLoadStarted();
                if (!sharedDataMgr->SetImageSnapshot(std::move(image))
                    || !sharedDataMgr->SetCurrentFromPending()) {
                    sharedState->SetReloadLoadFailed();
                    return false;
                }

                // current image 成为真源后再更新状态和 bridge 输入，并同步调用各 view 消费刷新；
                // 此 handler 不切换线程，调用方必须处于允许修改 VTK pipeline 的线程。
                const auto range = sharedDataMgr->GetScalarRange();
                const auto spacing = sharedDataMgr->GetSpacing();
                sharedState->SetReloadDataReady(range[0], range[1], spacing);
                if (const auto lockedBridge = bridge.lock()) {
                    const auto imageState = sharedDataMgr->GetImageState();
                    if (GetImageReady(imageState.image)) {
                        lockedBridge->SetInputImage(imageState.image, imageState.version);
                    }
                    else {
                        lockedBridge->ClearInputImage();
                    }
                }
                for (const auto& service : renderViewServices) {
                    if (const auto lockedService = service.lock()) {
                        lockedService->SendUpdates();
                    }
                }

                if (onComplete) {
                    onComplete(true); // 分发到 CropBridge::OnSubmitReload，恢复 widget/overlay/camera 状态。
                }
                return true; // reload handler 已接纳结果；最终成功仍通过 onComplete 分发。
            });
    }
}

bool HostFeatureBindings::Impl::StartCrop(
    const HostCropViewRequest& request)
{
    // 裁切至少需要一个 reference view，因为 widget interactor、renderer 和输入模型坐标都来自这一路。
    // preview view 可以为空；那只意味着不显示预览，不影响 reference 链路边界。
    if (request.referenceViewId.empty() && !request.isReferenceRoleUsed) {
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

    const auto* referenceView = m_renderViews->GetViewBySelector(
        request.referenceViewId,
        request.isReferenceRoleUsed,
        request.referenceRole);

    if (!referenceView || !referenceView->service || !referenceView->context) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view is missing." << std::endl;
        return false;
    }

    std::vector<const HostRenderViewRuntime*> previewViews =
        m_renderViews->GetViewsByIdsAndRoles(request.previewViewIds, request.previewViewRoles);
    if (previewViews.empty() && request.isPreviewViewsUsed) {
        // 裁切预览目标也必须由请求允许后才退到配置默认值；空请求不全选，避免误把新窗口纳入 preview。
        previewViews = m_renderViews->GetCropPreviewViews();
    }

    auto bridge = m_core.orthogonalCropBridge;
    // 裁切 reference view 按请求中的 id/role 选择，而不是按第几个窗口选择；后续 Qt 多/少窗口布局仍可复用同一 bridge。
    bridge->SetReferenceRenderService(referenceView->service);
    bridge->SetReferenceRenderer(referenceView->context->GetRenderer());
    bridge->SetPrimaryInteractor(referenceView->context->GetInteractor());
    bridge->SetPreviewRenderServices(m_renderViews->BuildServices(previewViews));

    if (!SetCropInput()) {
        return false;
    }

    return true;
}

bool HostFeatureBindings::Impl::StartGapView(
    const HostGapViewRequest& request)
{
    // request 同时携带宿主窗口选择和算法参数：target id/role 只在 Host 层解析，
    // Gap service 接收降级后的 3D/slice OverlayService 列表，不知道窗口编号或布局。
    if (!m_renderViews) {
        std::cerr << "[Host] Gap Analysis display activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }
    if (request.targetViewIds.empty()
        && request.targetViewRoles.empty()
        && !request.isDefaultOverlayUsed) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no render view target was requested." << std::endl;
        return false;
    }
    if (!request.algorithm) {
        std::cerr << "[Host] Gap Analysis display activation skipped: algorithm parameters were not specified." << std::endl;
        return false;
    }
    if (!GetGapConfigValid(*request.algorithm)) {
        return false;
    }

    std::vector<const HostRenderViewRuntime*> targetViews =
        m_renderViews->GetViewsByIdsAndRoles(request.targetViewIds, request.targetViewRoles);
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

    // Host 只分发降级后的 overlay 目标和算法值对象；worker/display 状态由 Gap service 持有。
    return m_core.gapAnalysis->StartView(
        BuildGapSurfaceRequest(request.algorithm->surface),
        BuildVoidParams(request.algorithm->voidDetection),
        meshOverlayTargets,
        sliceOverlayTargets,
        [sharedState = m_core.sharedState](double isoValue) {
            if (sharedState) {
                sharedState->SetIsoValue(isoValue);
            }
        });
}

bool HostFeatureBindings::Impl::SwitchGapView(
    const HostGapViewRequest& request)
{
    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis display switch skipped: feature service is not ready." << std::endl;
        return false;
    }

    return m_core.gapAnalysis->GetViewOn()
        ? m_core.gapAnalysis->SwitchOverlay()
        : StartGapView(request);
}

bool HostFeatureBindings::Impl::SwitchGapLayer()
{
    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis overlay switch skipped: feature service is not ready." << std::endl;
        return false;
    }
    return m_core.gapAnalysis->SwitchOverlay();
}

bool HostFeatureBindings::Impl::ExitGapView()
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
    const HostCropViewRequest& request)
{
    // Switch 前先 Start，是为了让上位机切换窗口布局后，裁切 bridge 始终使用最新 reference/preview 目标。
    if (!StartCrop(request)) {
        return false;
    }
    return m_core.orthogonalCropBridge->SwitchCropBox();
}

bool HostFeatureBindings::Impl::SwitchCropPlane(
    const HostCropViewRequest& request)
{
    if (!StartCrop(request)) {
        return false;
    }
    return m_core.orthogonalCropBridge->SwitchCropPlane();
}

bool HostFeatureBindings::Impl::SwitchCropView(
    const HostCropViewRequest& request,
    HostCropPreviewMode previewMode)
{
    if (!StartCrop(request)) {
        return false;
    }

    m_core.orthogonalCropBridge->SwitchPreview(GetCropRemovalMode(previewMode));
    return true;
}

bool HostFeatureBindings::Impl::SendCrop(
    const HostCropViewRequest& request)
{
    // 每次 submit 前重绑 request 指定的 reference/preview 目标并刷新输入版本。
    // 返回值只表示 reload handler 是否接受本次结果，不保证完成回调或新帧已经发生。
    if (!StartCrop(request)) {
        return false;
    }

    return m_core.orthogonalCropBridge->SendSubmit(); // 结果沿 reload handler 分发，拒绝时返回 false。
}

bool HostFeatureBindings::Impl::ExitCrop()
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->ExitCrop();
}

bool HostFeatureBindings::Impl::ExitFeature()
{
    if (ExitCrop()) {
        return true;
    }
    return ExitGapView();
}

bool HostFeatureBindings::Impl::GetCropActive() const
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->GetCropActive();
}

void HostFeatureBindings::Impl::ClearCropInput() const
{
    if (m_core.orthogonalCropBridge) {
        m_core.orthogonalCropBridge->ClearInputImage();
    }
}

std::function<bool()> HostFeatureBindings::Impl::BuildCropInput() const
{
    // 返回函数而不是立即刷新，是因为初始加载异步完成后才有 vtkImageData；
    // router 可以把同一刷新点传给加载回调，避免裁切链路自己监听文件 I/O。
    return [
        bridge = m_core.orthogonalCropBridge,
        sharedDataMgr = m_core.sharedDataMgr
    ]() {
        if (!bridge || !sharedDataMgr) {
            return false;
        }

        const auto imageState = sharedDataMgr->GetImageState();
        if (!GetImageReady(imageState.image)) {
            bridge->ClearInputImage();
            std::cerr << "[Host] Orthogonal crop init failed: input image missing." << std::endl;
            return false;
        }

        bridge->SetInputImage(imageState.image, imageState.version);
        return true;
    };
}

bool HostFeatureBindings::Impl::SetCropInput()
{
    if (!m_core.orthogonalCropBridge || !m_core.sharedDataMgr) {
        return false;
    }

    const auto imageState = m_core.sharedDataMgr->GetImageState();
    if (!GetImageReady(imageState.image)) {
        m_core.orthogonalCropBridge->ClearInputImage();
        std::cerr << "[Host] Orthogonal crop init failed: input image missing." << std::endl;
        return false;
    }

    m_core.orthogonalCropBridge->SetInputImage(imageState.image, imageState.version);
    return true;
}

void HostFeatureBindings::Impl::AttachHostTimer(
    const HostTimerEventPumpConfig& eventPumpConfig)
{
    // eventPumpConfig 只选择提供主线程 TimerEvent 的 view；它不是 Gap overlay 目标。
    if (!eventPumpConfig.isTimerEnabled || !m_renderViews) {
        DetachHostTimer();
        return;
    }

    const auto* timerView = m_renderViews->GetViewBySelector(
        eventPumpConfig.timerViewId,
        eventPumpConfig.isTimerRoleUsed,
        eventPumpConfig.timerViewRole);

    if (!timerView || !timerView->context) {
        std::cerr << "[Host] Timer event pump skipped: explicit timer render view is missing." << std::endl;
        DetachHostTimer();
        return;
    }

    // 重配时先卸载旧 handler；m_timerContext 的弱引用不会延长窗口生命周期。
    DetachHostTimer();
    m_timerContext = timerView->context;
    timerView->context->SetTimerHandler([impl = this]() {
        impl->OnHostTimer();
    });
}

void HostFeatureBindings::Impl::OnHostTimer()
{
    if (!m_core.gapAnalysis || !m_core.sharedState || !m_core.sharedDataMgr) {
        return;
    }

    // host 只提供主线程 tick 和当前数据快照入口；GapAnalysis 自己判断是否有 pending 显示请求。
    if (m_core.sharedState->GetDataTrustedState() != LoadState::Succeeded) {
        return;
    }

    m_core.gapAnalysis->OnDisplayTick(m_core.sharedDataMgr->GetVtkImage());
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

bool HostFeatureBindings::StartCrop(const HostCropViewRequest& request)
{
    return m_impl->StartCrop(request);
}

bool HostFeatureBindings::StartGapView(const HostGapViewRequest& request)
{
    return m_impl->StartGapView(request);
}

bool HostFeatureBindings::SwitchGapView(const HostGapViewRequest& request)
{
    return m_impl->SwitchGapView(request);
}

bool HostFeatureBindings::SwitchGapLayer()
{
    return m_impl->SwitchGapLayer();
}

bool HostFeatureBindings::ExitGapView()
{
    return m_impl->ExitGapView();
}

bool HostFeatureBindings::GetGapView() const
{
    return m_impl->GetGapView();
}

bool HostFeatureBindings::SwitchCropBox(const HostCropViewRequest& request)
{
    return m_impl->SwitchCropBox(request);
}

bool HostFeatureBindings::SwitchCropPlane(const HostCropViewRequest& request)
{
    return m_impl->SwitchCropPlane(request);
}

bool HostFeatureBindings::SwitchCropView(
    const HostCropViewRequest& request,
    HostCropPreviewMode previewMode)
{
    return m_impl->SwitchCropView(request, previewMode);
}

bool HostFeatureBindings::SendCrop(const HostCropViewRequest& request)
{
    return m_impl->SendCrop(request);
}

bool HostFeatureBindings::ExitCrop()
{
    return m_impl->ExitCrop();
}

bool HostFeatureBindings::ExitFeature()
{
    return m_impl->ExitFeature();
}

bool HostFeatureBindings::GetCropActive() const
{
    return m_impl->GetCropActive();
}

void HostFeatureBindings::ClearCropInput() const
{
    m_impl->ClearCropInput();
}

std::function<bool()> HostFeatureBindings::BuildCropInput()
{
    return m_impl->BuildCropInput();
}

void HostFeatureBindings::AttachHostTimer(
    const HostTimerEventPumpConfig& eventPumpConfig)
{
    m_impl->AttachHostTimer(eventPumpConfig);
}
