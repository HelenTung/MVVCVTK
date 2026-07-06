#include "Host/HostFeatureBindings.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppState.h"
#include "DataManager.h"
#include "Interaction/OrthogonalCropInteractionBridgeService.h"
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

namespace {
static CropRemovalMode GetCropRemovalMode(HostCropPreviewMode previewMode)
{
    switch (previewMode) {
    case HostCropPreviewMode::KeepInside:
        return CropRemovalMode::KeepInside;
    case HostCropPreviewMode::RemoveInside:
        return CropRemovalMode::RemoveInside;
    default:
        return CropRemovalMode::KeepInside;
    }
}

static std::optional<Orientation> GetGapSliceOverlayOrientation(HostRenderViewRole role)
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

static bool ShouldReceiveGapMeshOverlay(HostRenderViewRole role)
{
    return HostRenderViewSet::GetRoleIs3DView(role);
}

static VoidDetectionParams BuildVoidDetectionParams(
    const HostGapAnalysisVoidDetectionConfig& config)
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

static GapAnalysisSurfaceRequest BuildGapAnalysisSurfaceRequest(
    const HostGapAnalysisSurfaceConfig& config)
{
    GapAnalysisSurfaceRequest request;
    request.isoMode = config.isoMode == HostGapAnalysisIsoMode::AbsoluteValue
        ? GapAnalysisIsoValueMode::AbsoluteValue
        : GapAnalysisIsoValueMode::DataRangeRatio;
    request.dataRangeRatio = config.dataRangeRatio;
    request.absoluteIsoValue = config.absoluteIsoValue;
    return request;
}

static bool ValidateGapAnalysisAlgorithmConfig(
    const HostGapAnalysisAlgorithmConfig& config)
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

} // namespace

HostFeatureBindings::HostFeatureBindings() = default;

HostFeatureBindings::~HostFeatureBindings()
{
    DetachHostTimerEventPump();
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ExitDisplay();
    }
}

void HostFeatureBindings::RegisterFeatures(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_core = core;
    m_renderViews = &renderViews;
    if (m_core.gapAnalysis) {
        m_core.gapAnalysis->ExitDisplay();
    }
    if (m_core.orthogonalCropBridge) {
        // submit 后需要刷新所有现有视图的共享数据，但不应该把“哪些窗口参与 preview”的语义写进 reload。
        // 因此这里保存弱引用集合，只做数据更新通知；窗口消失时 weak_ptr 自然失效。
        std::vector<std::weak_ptr<MedicalVizService>> renderViewServices;
        renderViewServices.reserve(renderViews.GetViews().size());
        for (const auto& view : renderViews.GetViews()) {
            renderViewServices.push_back(view.service);
        }

        // DataManager 与 submit reload 是裁切 feature 的稳定能力边界；具体窗口目标等激活请求到来后再绑定。
        // submit 回调只提交新图像和刷新共享状态，不持有窗口角色，避免裁切结果反向耦合到某一组视图。
        m_core.orthogonalCropBridge->SetDataManager(m_core.sharedDataMgr);
        m_core.orthogonalCropBridge->SetSubmitReloadHandler(
            [
                sharedDataMgr = m_core.sharedDataMgr,
                sharedState = m_core.sharedState,
                renderViewServices = std::move(renderViewServices)
            ](
                vtkSmartPointer<vtkImageData> image,
                std::function<void(bool success)> onComplete) {
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

                sharedState->SetReloadLoadStarted();
                if (!sharedDataMgr->TakeImageSnapshot(std::move(image))
                    || !sharedDataMgr->ConsumePendingImage()) {
                    sharedState->SetReloadLoadFailed();
                    return false;
                }

                const auto range = sharedDataMgr->GetScalarRange();
                const auto spacing = sharedDataMgr->GetSpacing();
                sharedState->SetReloadDataReady(range[0], range[1], spacing);
                for (const auto& service : renderViewServices) {
                    if (const auto lockedService = service.lock()) {
                        lockedService->ProcessPendingUpdates();
                    }
                }

                if (onComplete) {
                    onComplete(true);
                }
                return true;
            });
    }
}

bool HostFeatureBindings::ActivateOrthogonalCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    // 裁切至少需要一个 reference view，因为 widget interactor、renderer 和输入模型坐标都来自这一路。
    // preview view 可以为空；那只意味着不显示预览，不影响 reference 链路边界。
    if (request.referenceViewId.empty() && !request.useReferenceRole) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view was not specified." << std::endl;
        return false;
    }

    return ConfigureOrthogonalCrop(request);
}

bool HostFeatureBindings::ActivateGapAnalysisDisplay(
    const HostGapAnalysisActivationRequest& request)
{
    if (!m_renderViews) {
        std::cerr << "[Host] Gap Analysis display activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }
    if (request.targetViewIds.empty()
        && request.targetViewRoles.empty()
        && !request.useDefaultOverlayRoles) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no render view target was requested." << std::endl;
        return false;
    }
    if (!request.algorithm) {
        std::cerr << "[Host] Gap Analysis display activation skipped: algorithm parameters were not specified." << std::endl;
        return false;
    }
    if (!ValidateGapAnalysisAlgorithmConfig(*request.algorithm)) {
        return false;
    }

    std::vector<const HostRenderViewRuntime*> targetViews =
        m_renderViews->GetViewsByIdsAndRoles(request.targetViewIds, request.targetViewRoles);
    if (targetViews.empty() && request.useDefaultOverlayRoles) {
        // 默认 overlay role 只有在宿主明确允许 fallback 时才使用；空请求不能被解释成全窗口。
        targetViews = m_renderViews->GetDefaultGapOverlayViews();
    }
    if (targetViews.empty()) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no overlay render view target was found." << std::endl;
        return false;
    }

    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis display activation skipped: feature service is not ready." << std::endl;
        return false;
    }

    std::vector<std::shared_ptr<AbstractAppService>> meshOverlayTargets;
    std::vector<std::pair<Orientation, std::shared_ptr<AbstractAppService>>> sliceOverlayTargets;
    meshOverlayTargets.reserve(targetViews.size());
    sliceOverlayTargets.reserve(targetViews.size());
    for (const auto* view : targetViews) {
        if (!view || !view->service) {
            continue;
        }

        if (ShouldReceiveGapMeshOverlay(view->config.role)) {
            meshOverlayTargets.push_back(view->service);
        }

        const auto orientation = GetGapSliceOverlayOrientation(view->config.role);
        if (orientation) {
            sliceOverlayTargets.push_back({ *orientation, view->service });
        }
    }

    return m_core.gapAnalysis->ActivateDisplay(
        BuildGapAnalysisSurfaceRequest(request.algorithm->surface),
        BuildVoidDetectionParams(request.algorithm->voidDetection),
        meshOverlayTargets,
        sliceOverlayTargets,
        [sharedState = m_core.sharedState](double isoValue) {
            if (sharedState) {
                sharedState->SetIsoValue(isoValue);
            }
        });
}

bool HostFeatureBindings::ToggleGapAnalysisDisplay(
    const HostGapAnalysisActivationRequest& request)
{
    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis display toggle skipped: feature service is not ready." << std::endl;
        return false;
    }

    return m_core.gapAnalysis->GetDisplayActive()
        ? m_core.gapAnalysis->ToggleOverlayVisibility()
        : ActivateGapAnalysisDisplay(request);
}

bool HostFeatureBindings::ToggleGapAnalysisOverlayVisibility()
{
    if (!m_core.gapAnalysis) {
        std::cerr << "[Host] Gap Analysis overlay toggle skipped: feature service is not ready." << std::endl;
        return false;
    }
    return m_core.gapAnalysis->ToggleOverlayVisibility();
}

bool HostFeatureBindings::ExitGapAnalysisDisplay()
{
    if (!m_core.gapAnalysis) {
        return false;
    }
    return m_core.gapAnalysis->ExitDisplay();
}

bool HostFeatureBindings::GetGapAnalysisDisplayActive() const
{
    return m_core.gapAnalysis && m_core.gapAnalysis->GetDisplayActive();
}

bool HostFeatureBindings::ToggleInteractiveCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    // Toggle 前先 Activate，是为了让上位机切换窗口布局后，裁切 bridge 始终使用最新 reference/preview 目标。
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }
    return m_core.orthogonalCropBridge->ToggleInteractiveCrop();
}

bool HostFeatureBindings::ToggleInteractivePlanarCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }
    return m_core.orthogonalCropBridge->ToggleInteractivePlanarCrop();
}

bool HostFeatureBindings::ToggleCropPreview(
    const HostOrthogonalCropActivationRequest& request,
    HostCropPreviewMode previewMode)
{
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }

    m_core.orthogonalCropBridge->TogglePreview(GetCropRemovalMode(previewMode));
    return true;
}

bool HostFeatureBindings::ApplyCropSubmit(
    const HostOrthogonalCropActivationRequest& request)
{
    if (!ActivateOrthogonalCrop(request) || !m_core.orthogonalCropBridge) {
        return false;
    }

    m_core.orthogonalCropBridge->ApplySubmit();
    return true;
}

bool HostFeatureBindings::ExitInteractiveCrop()
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->ExitInteractiveCrop();
}

bool HostFeatureBindings::ExitActiveFeatureMode()
{
    if (ExitInteractiveCrop()) {
        return true;
    }
    return ExitGapAnalysisDisplay();
}

bool HostFeatureBindings::GetInteractiveCropActive() const
{
    return m_core.orthogonalCropBridge
        && m_core.orthogonalCropBridge->GetInteractiveCropActive();
}

std::function<bool()> HostFeatureBindings::BuildOrthogonalCropInputRefreshHandler() const
{
    // 返回函数而不是立即刷新，是因为初始加载异步完成后才有 vtkImageData；
    // router 可以把同一刷新点传给加载回调，避免裁切链路自己监听文件 I/O。
    return [
        orthogonalCropBridge = m_core.orthogonalCropBridge,
        sharedDataMgr = m_core.sharedDataMgr
    ]() {
        if (!orthogonalCropBridge || !sharedDataMgr) {
            return false;
        }

        orthogonalCropBridge->SetInputImage(sharedDataMgr->GetVtkImage());
        if (!orthogonalCropBridge->GetInputImage()) {
            std::cerr << "[Host] Orthogonal crop init failed: input image missing." << std::endl;
            return false;
        }
        return true;
    };
}

bool HostFeatureBindings::RefreshOrthogonalCropInputImage()
{
    auto refreshInput = BuildOrthogonalCropInputRefreshHandler();
    if (!refreshInput) {
        return false;
    }
    return refreshInput();
}

void HostFeatureBindings::AttachHostTimerEventPump(
    const HostTimerEventPumpConfig& eventPumpConfig)
{
    if (!eventPumpConfig.enableTimer || !m_renderViews) {
        DetachHostTimerEventPump();
        return;
    }

    const HostRenderViewRuntime* timerView = nullptr;
    if (!eventPumpConfig.timerViewId.empty()) {
        timerView = m_renderViews->GetViewById(eventPumpConfig.timerViewId);
    }
    else if (eventPumpConfig.useTimerViewRole) {
        timerView = m_renderViews->GetFirstViewByRole(eventPumpConfig.timerViewRole);
    }

    if (!timerView || !timerView->context) {
        std::cerr << "[Host] Timer event pump skipped: explicit timer render view is missing." << std::endl;
        DetachHostTimerEventPump();
        return;
    }

    DetachHostTimerEventPump();
    m_timerContext = timerView->context;
    std::weak_ptr<HostFeatureBindings> weakSelf = weak_from_this();
    timerView->context->SetTimerHandler([weakSelf]() {
        if (const auto self = weakSelf.lock()) {
            self->ProcessHostTimerTick();
        }
    });
}

void HostFeatureBindings::ProcessHostTimerTick()
{
    if (!m_core.gapAnalysis || !m_core.sharedState || !m_core.sharedDataMgr) {
        return;
    }

    // host 只提供主线程 tick 和当前数据快照入口；GapAnalysis 自己判断是否有 pending 显示请求。
    if (m_core.sharedState->GetDataTrustedState() != LoadState::Succeeded) {
        return;
    }

    m_core.gapAnalysis->ProcessDisplayTick(m_core.sharedDataMgr->GetVtkImage());
}

void HostFeatureBindings::DetachHostTimerEventPump()
{
    if (const auto context = m_timerContext.lock()) {
        context->ClearTimerHandler();
    }
    m_timerContext.reset();
}

bool HostFeatureBindings::ConfigureOrthogonalCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    // 这一步是 host 窗口语义到裁切 bridge 的唯一转换点：
    // 1. reference view 提供 renderer/interactor，并决定 widget 所在坐标语境。
    // 2. preview views 只提供 AbstractInteractiveService，用于显示预览和刷新 dirty 状态。
    // 3. 空 preview 不失败，空 reference 必须失败。
    if (!m_renderViews || !m_core.orthogonalCropBridge) {
        std::cerr << "[Host] Orthogonal crop activation skipped: host feature bindings are not ready." << std::endl;
        return false;
    }

    const HostRenderViewRuntime* referenceView = nullptr;
    if (!request.referenceViewId.empty()) {
        referenceView = m_renderViews->GetViewById(request.referenceViewId);
    }
    else if (request.useReferenceRole) {
        referenceView = m_renderViews->GetFirstViewByRole(request.referenceRole);
    }

    if (!referenceView || !referenceView->service || !referenceView->context) {
        std::cerr << "[Host] Orthogonal crop activation skipped: reference render view is missing." << std::endl;
        return false;
    }

    std::vector<const HostRenderViewRuntime*> previewViews =
        m_renderViews->GetViewsByIdsAndRoles(request.previewViewIds, request.previewViewRoles);
    if (previewViews.empty() && request.useConfiguredPreviewViews) {
        // 裁切预览目标也必须由请求允许后才退到配置默认值；空请求不全选，避免误把新窗口纳入 preview。
        previewViews = m_renderViews->GetConfiguredCropPreviewViews();
    }

    auto bridge = m_core.orthogonalCropBridge;
    // 裁切 reference view 按请求中的 id/role 选择，而不是按第几个窗口选择；后续 Qt 多/少窗口布局仍可复用同一 bridge。
    bridge->SetReferenceRenderService(referenceView->service);
    bridge->SetReferenceRenderer(referenceView->context->GetRenderer());
    bridge->SetPrimaryInteractor(referenceView->context->GetInteractor());
    bridge->SetPreviewRenderServices(m_renderViews->BuildInteractiveServices(previewViews));

    if (!RefreshOrthogonalCropInputImage()) {
        return false;
    }

    return true;
}
