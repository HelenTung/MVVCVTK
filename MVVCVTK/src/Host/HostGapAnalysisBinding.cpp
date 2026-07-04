#include "Host/HostGapAnalysisBinding.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppState.h"
#include "DataManager.h"
#include "Host/HostRenderViewSet.h"
#include "Render/Strategies/GapAlgorithmOverlayStrategies.h"
#include "Services/GapAnalysisService.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindowInteractor.h>

#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
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
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D;
}

static double ResolveGapIsoValue(
    const HostGapAnalysisSurfaceConfig& surface,
    const std::array<double, 2>& dataRange)
{
    if (surface.isoMode == HostGapAnalysisIsoMode::AbsoluteValue) {
        return surface.absoluteIsoValue;
    }

    // DataRangeRatio 模式的数学含义：
    // iso = min + (max - min) * ratio。ratio 由上位机配方给出，host 只负责把数据范围代入。
    return dataRange[0] + (dataRange[1] - dataRange[0]) * surface.dataRangeRatio;
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

class GapAnalysisOverlayController final {
public:
    // RenderServiceBinding 是孔隙 overlay 的投递目标快照：
    // role 决定采用 3D mesh 还是 2D slice 策略，service 是实际接收 overlay 的窗口服务。
    struct RenderServiceBinding {
        HostRenderViewRole role = HostRenderViewRole::Auxiliary;
        std::shared_ptr<MedicalVizService> service;
    };

    bool GetAnalysisDisplayActive() const
    {
        return m_isAnalysisDisplayActive;
    }

    bool GetAnalysisRunRequested() const
    {
        return m_hasPendingAnalysisRunRequest;
    }

    std::optional<HostGapAnalysisAlgorithmConfig> GetAnalysisAlgorithmConfig() const
    {
        return m_algorithmConfig;
    }

    void ClearAnalysisRunRequest()
    {
        m_hasPendingAnalysisRunRequest = false;
    }

    bool ActivateAnalysisDisplay(const HostGapAnalysisActivationRequest& request)
    {
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

        // 进入显示模式和选择窗口目标是同一个宿主命令边界；初始化阶段不预绑窗口，避免默认进入孔隙分析链路。
        // 这里先隐藏旧 overlay，是因为同一模式可以被上位机重新指定目标窗口，旧窗口残留会让“当前命令作用域”变得含糊。
        m_isAnalysisDisplayActive = true;
        m_shouldShowOverlay = true;
        m_hasPendingAnalysisRunRequest = true;
        m_algorithmConfig = request.algorithm;
        m_voidMesh = nullptr;
        m_labelImage = nullptr;
        HideOverlays();
        return true;
    }

    void SetRenderViews(std::vector<RenderServiceBinding> renderViews)
    {
        // render view 拓扑可能由 Qt host 重新注入，替换目标前先移除旧 overlay，避免残留策略挂在旧窗口上。
        HideOverlays();
        m_renderViews = std::move(renderViews);
    }

    bool ApplyAnalysisResult(
        vtkSmartPointer<vtkPolyData> voidMesh,
        vtkSmartPointer<vtkImageData> labelImage)
    {
        if (!m_isAnalysisDisplayActive) {
            return false;
        }

        m_voidMesh = std::move(voidMesh);
        m_labelImage = std::move(labelImage);
        HideOverlays();
        if (!m_shouldShowOverlay) {
            std::cout << "[Host] Gap Analysis completed, but overlays are hidden. Use the host overlay toggle command to show them." << std::endl;
            return false;
        }

        return ShowStoredAnalysisResult();
    }

    bool ToggleAnalysisOverlayVisibility()
    {
        if (!m_isAnalysisDisplayActive) {
            std::cerr << "[Host] Gap Analysis overlay toggle ignored: display mode is not active." << std::endl;
            return false;
        }

        m_shouldShowOverlay = !m_shouldShowOverlay;
        // 宿主再次触发显隐命令只切换 overlay，不退出 display mode；这样用户可以临时看原图，再恢复同一次分析结果。
        if (!m_shouldShowOverlay) {
            const bool removedAnyOverlay = HideOverlays();
            std::cout << "[Host] Gap Analysis overlays hidden. Use the host overlay toggle command to show them again." << std::endl;
            return removedAnyOverlay;
        }

        if (!HasStoredAnalysisResult()) {
            std::cout << "[Host] Gap Analysis overlays enabled. They will appear after analysis completes." << std::endl;
            return true;
        }

        return ShowStoredAnalysisResult();
    }

    bool ExitAnalysisDisplay()
    {
        // 宿主退出命令的语义是彻底退出孔隙显示模式，所以 pending 请求、缓存结果和 overlay 策略都一起清掉。
        const bool wasAnalysisDisplayActive = m_isAnalysisDisplayActive;
        const bool hadStoredAnalysisResult = HasStoredAnalysisResult();
        m_isAnalysisDisplayActive = false;
        m_shouldShowOverlay = false;
        m_hasPendingAnalysisRunRequest = false;
        m_algorithmConfig.reset();
        m_voidMesh = nullptr;
        m_labelImage = nullptr;
        const bool removedAnyOverlay = HideOverlays();
        if (wasAnalysisDisplayActive || removedAnyOverlay || hadStoredAnalysisResult) {
            std::cout << "[Host] Gap Analysis display mode exited. Void overlays are hidden." << std::endl;
        }
        return wasAnalysisDisplayActive || removedAnyOverlay || hadStoredAnalysisResult;
    }

private:
    // OverlayBinding 保存“哪个 service 加了哪个 strategy”，这是 HideOverlays 能精确移除的最小单位。
    // 不反向扫描 service，是为了避免 host 层依赖渲染服务内部 overlay 容器结构。
    struct OverlayBinding {
        std::shared_ptr<MedicalVizService> service;
        std::shared_ptr<AbstractVisualStrategy> overlayStrategy;
    };

    bool HasStoredAnalysisResult() const
    {
        return m_voidMesh != nullptr || m_labelImage != nullptr;
    }

    bool ShowStoredAnalysisResult()
    {
        HideOverlays();

        // overlay 分发只看 role，不看窗口序号；这样 Qt host 可以增减窗口而不改孔隙分析链路。
        // A. 3D role 接收 void mesh，用于整体孔隙空间观察。
        // B. slice role 接收 label image，并按 role 映射到对应切片方向。
        const vtkIdType meshPoints = m_voidMesh ? m_voidMesh->GetNumberOfPoints() : 0;
        const vtkIdType meshCells = m_voidMesh ? m_voidMesh->GetNumberOfCells() : 0;
        int labelDims[3] = { 0, 0, 0 };
        if (m_labelImage) {
            m_labelImage->GetDimensions(labelDims);
        }

        const bool hasMeshOverlayInput = m_voidMesh && meshPoints > 0 && meshCells > 0;
        const bool hasSliceOverlayInput = m_labelImage && labelDims[0] > 0 && labelDims[1] > 0 && labelDims[2] > 0;
        bool addedMeshOverlay = false;
        bool addedSliceOverlay = false;

        for (const auto& view : m_renderViews) {
            if (hasMeshOverlayInput && ShouldReceiveGapMeshOverlay(view.role)) {
                AddMeshOverlay(view.service, m_voidMesh);
                addedMeshOverlay = true;
            }

            const auto orientation = GetGapSliceOverlayOrientation(view.role);
            if (hasSliceOverlayInput && orientation) {
                AddSliceOverlay(view.service, m_labelImage, *orientation);
                addedSliceOverlay = true;
            }
        }

        if (!addedMeshOverlay) {
            std::cerr << "[Host] Gap Analysis produced no 3D void mesh overlay target." << std::endl;
        }
        if (!addedSliceOverlay) {
            std::cerr << "[Host] Gap Analysis produced no 2D label overlay target." << std::endl;
        }

        if (!m_overlayBindings.empty()) {
            std::cout << "[Host] Gap Analysis overlays shown: mesh points = "
                << meshPoints << ", mesh cells = " << meshCells
                << ", label dims = " << labelDims[0] << "x" << labelDims[1] << "x" << labelDims[2]
                << std::endl;
        }
        return !m_overlayBindings.empty();
    }

    bool HideOverlays()
    {
        bool removedAnyOverlay = false;
        for (const auto& binding : m_overlayBindings) {
            if (!binding.service || !binding.overlayStrategy) {
                continue;
            }
            binding.service->RemoveOverlayStrategy(binding.overlayStrategy);
            binding.service->MarkDirty();
            removedAnyOverlay = true;
        }
        m_overlayBindings.clear();
        return removedAnyOverlay;
    }

    void AddMeshOverlay(
        const std::shared_ptr<MedicalVizService>& service,
        vtkSmartPointer<vtkPolyData> voidMesh)
    {
        if (!service || !voidMesh) {
            return;
        }

        auto overlay = std::make_shared<GapMeshOverlayStrategy>();
        overlay->SetInputData(voidMesh);
        service->AddOverlayStrategy(overlay);
        m_overlayBindings.push_back({ service, overlay });
    }

    void AddSliceOverlay(
        const std::shared_ptr<MedicalVizService>& service,
        vtkSmartPointer<vtkImageData> labelImage,
        Orientation orientation)
    {
        if (!service || !labelImage) {
            return;
        }

        auto overlay = std::make_shared<GapSliceOverlayStrategy>(orientation);
        overlay->SetInputData(labelImage);
        service->AddOverlayStrategy(overlay);
        m_overlayBindings.push_back({ service, overlay });
    }

    // 隐藏与退出故意分开：宿主显隐命令只移除当前 overlay 句柄并保留算法结果；
    // 宿主退出命令才清掉缓存并关闭显示模式，避免“临时隐藏”被误解释为放弃分析。
    // m_overlayBindings 是当前显示出来的 overlay 句柄集合，HideOverlays 会清空它。
    std::vector<OverlayBinding> m_overlayBindings;
    // m_voidMesh 是最近一次孔隙分析生成的 3D mesh 缓存；隐藏 overlay 时保留，退出 display mode 时清除。
    vtkSmartPointer<vtkPolyData> m_voidMesh;
    // m_labelImage 是最近一次孔隙分析生成的 slice label 缓存，供切片 overlay 恢复显示。
    vtkSmartPointer<vtkImageData> m_labelImage;
    // m_renderViews 是本次显示命令解析出的目标窗口快照；重新激活时会替换并先清理旧 overlay。
    std::vector<RenderServiceBinding> m_renderViews;
    // m_algorithmConfig 是本次显式激活命令携带的算法参数；Timer 只消费它，不再保存经验默认值。
    std::optional<HostGapAnalysisAlgorithmConfig> m_algorithmConfig;
    // m_isAnalysisDisplayActive 表示“孔隙显示模式”是否存在；只有 active 时 Timer 才能启动/提交算法。
    bool m_isAnalysisDisplayActive = false;
    // m_shouldShowOverlay 表示 active 模式下当前是否可见；false 仍保留结果以支持再次显示。
    bool m_shouldShowOverlay = false;
    // m_hasPendingAnalysisRunRequest 是 host 命令交给 Timer 的一次性启动信号，避免激活时直接依赖数据加载线程。
    bool m_hasPendingAnalysisRunRequest = false;
};

// Timer observer 是 VTK 事件循环到孔隙算法结果的提交桥。
// 它不决定进入模式、显示目标或算法参数；这些都由显式 host 请求写入 GapAnalysisOverlayController。
class GapAnalysisOverlayCommitObserver final : public vtkCommand {
public:
    static GapAnalysisOverlayCommitObserver* New() { return new GapAnalysisOverlayCommitObserver; }

    // 以下依赖由 HostGapAnalysisBinding 注入，observer 本身不创建服务，避免事件循环对象拥有业务生命周期。
    std::shared_ptr<GapAnalysisService> gapAnalysis;
    // 只用于把本轮算法 iso 写回共享显示状态；不表达 timer 承载窗口就是主 3D 窗口。
    std::shared_ptr<MedicalVizService> visualConfigService;
    std::shared_ptr<GapAnalysisOverlayController> gapAnalysisOverlayController;
    std::shared_ptr<AbstractDataManager> dataMgr;
    std::shared_ptr<SharedInteractionState> sharedState;

    void Execute(vtkObject* caller, unsigned long eventId, void* callData) override {
        (void)caller;
        (void)callData;
        if (eventId != vtkCommand::TimerEvent || !gapAnalysis || !gapAnalysisOverlayController) {
            return;
        }

        if (!gapAnalysisOverlayController->GetAnalysisDisplayActive()) {
            return;
        }

        if (gapAnalysisOverlayController->GetAnalysisRunRequested()) {
            // Timer 只响应宿主层留下的显式请求；这样分析启动条件和显示模式边界都集中在 host/session。
            if (gapAnalysis->GetAnalysisState() != GapAnalysisState::Running && TryStartAnalysis()) {
                gapAnalysisOverlayController->ClearAnalysisRunRequest();
                m_completionHandled = false;
                m_failureLogged = false;
            }
            return;
        }

        if (m_completionHandled) {
            return;
        }

        const GapAnalysisState state = gapAnalysis->GetAnalysisState();
        if (state == GapAnalysisState::Idle || state == GapAnalysisState::Running) {
            return;
        }

        if (gapAnalysis->ConsumePendingCompletionCallback()) {
            gapAnalysis->ExecutePendingCompletionCallback();
        }

        if (state == GapAnalysisState::Failed) {
            if (!m_failureLogged) {
                std::cerr << "[Host] Gap Analysis failed; overlay will not be attached." << std::endl;
                m_failureLogged = true;
            }
            return;
        }

        CommitOverlays();
        m_completionHandled = true;
    }

private:
    // 每次分析完成只提交一次 overlay；重新激活或重新开始分析时重置。
    bool m_completionHandled = false;
    // 失败日志只输出一次，避免 TimerEvent 持续刷屏掩盖真实错误。
    bool m_failureLogged = false;

    bool TryStartAnalysis()
    {
        if (!sharedState || sharedState->GetFileLoadState() != LoadState::Succeeded) {
            // 孔隙分析消费的是已经进入共享 DataManager 的体数据；等待 DataReady 可避免算法直接依赖文件加载线程。
            return false;
        }

        if (!dataMgr || !gapAnalysis->SetInputImage(dataMgr->GetVtkImage())) {
            return false;
        }

        const auto algorithmConfig = gapAnalysisOverlayController->GetAnalysisAlgorithmConfig();
        if (!algorithmConfig) {
            std::cerr << "[Host] Gap Analysis start skipped: algorithm parameters are missing." << std::endl;
            return false;
        }

        const auto range = sharedState->GetDataRange();
        const double isoValue = ResolveGapIsoValue(algorithmConfig->surface, range);
        if (visualConfigService) {
            visualConfigService->SetIsoThreshold(isoValue);
        }

        SurfaceParams surfaceParams;
        surfaceParams.isoValue = static_cast<float>(isoValue);
        gapAnalysis->SetSurfaceParams(surfaceParams);
        gapAnalysis->SetVoidParams(BuildVoidDetectionParams(algorithmConfig->voidDetection));

        gapAnalysis->RunAsync();
        return true;
    }

    void CommitOverlays()
    {
        if (!gapAnalysisOverlayController) {
            std::cout << "[Host] Gap Analysis completed, but overlay display controller is missing." << std::endl;
            return;
        }

        if (!gapAnalysisOverlayController->GetAnalysisDisplayActive()) {
            std::cout << "[Host] Gap Analysis completed, but display mode has exited." << std::endl;
            return;
        }

        auto voidMesh = gapAnalysis->BuildVoidMesh();
        auto labelImage = gapAnalysis->BuildLabelImage();

        // 算法服务只提供稳定结果；是否显示、显示到哪些窗口、如何隐藏都由显示控制器决定。
        // 这样临时隐藏和彻底退出不会相互污染。
        gapAnalysisOverlayController->ApplyAnalysisResult(voidMesh, labelImage);
    }
};

static std::vector<GapAnalysisOverlayController::RenderServiceBinding> BuildGapAnalysisRenderBindings(
    const std::vector<const HostRenderViewRuntime*>& views)
{
    // 只把 overlay 需要的 role + service 投给控制器，避免控制器看到 id、context、interactor 等 host 拓扑细节。
    std::vector<GapAnalysisOverlayController::RenderServiceBinding> bindings;
    bindings.reserve(views.size());
    for (const auto* view : views) {
        if (view && view->service) {
            bindings.push_back({ view->config.role, view->service });
        }
    }
    return bindings;
}
}

struct HostGapAnalysisBinding::Impl {
    // core 是 session 级共享服务集合；binding 不创建数据或算法服务，只保存 shared_ptr 副本。
    HostCoreServices core;
    // 非拥有窗口集合指针；VtkAppHostSession 保证 render view set 生命周期覆盖 binding。
    const HostRenderViewSet* renderViews = nullptr;
    // 单一显示状态机，连接 public 命令和 Timer observer。
    std::shared_ptr<GapAnalysisOverlayController> overlayController = std::make_shared<GapAnalysisOverlayController>();
};

HostGapAnalysisBinding::HostGapAnalysisBinding()
    : m_impl(std::make_unique<Impl>())
{
}

HostGapAnalysisBinding::~HostGapAnalysisBinding() = default;

HostGapAnalysisBinding::HostGapAnalysisBinding(HostGapAnalysisBinding&&) noexcept = default;

HostGapAnalysisBinding& HostGapAnalysisBinding::operator=(HostGapAnalysisBinding&&) noexcept = default;

void HostGapAnalysisBinding::Register(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews)
{
    m_impl->core = core;
    m_impl->renderViews = &renderViews;
    m_impl->overlayController = std::make_shared<GapAnalysisOverlayController>();
}

bool HostGapAnalysisBinding::ActivateDisplay(
    const HostGapAnalysisActivationRequest& request)
{
    // 激活孔隙显示必须先解析目标窗口；如果这里允许空目标，Timer 完成后就只能猜测要全局显示还是丢弃结果。
    if (!m_impl->renderViews || !m_impl->overlayController) {
        std::cerr << "[Host] Gap Analysis display activation skipped: host gap binding is not ready." << std::endl;
        return false;
    }
    if (!request.algorithm) {
        std::cerr << "[Host] Gap Analysis display activation skipped: algorithm parameters were not specified." << std::endl;
        return false;
    }
    if (!ValidateGapAnalysisAlgorithmConfig(*request.algorithm)) {
        return false;
    }

    HostGapAnalysisActivationRequest resolvedRequest = request;
    std::vector<const HostRenderViewRuntime*> targetViews =
        m_impl->renderViews->GetViewsByIdsAndRoles(resolvedRequest.targetViewIds, resolvedRequest.targetViewRoles);
    if (targetViews.empty() && resolvedRequest.useDefaultOverlayRoles) {
        // 默认 overlay 角色必须由请求显式打开；空 ids/roles 本身不等于“所有窗口”，避免漏配目标时全局接管。
        targetViews = m_impl->renderViews->GetDefaultGapOverlayViews();
    }
    if (targetViews.empty()) {
        std::cerr << "[Host] Gap Analysis display activation skipped: no overlay render view target was found." << std::endl;
        return false;
    }

    // 每次激活都重新绑定目标窗口，使上位机可以在运行期切换 overlay 目标而不重建 session。
    m_impl->overlayController->SetRenderViews(BuildGapAnalysisRenderBindings(targetViews));
    const bool activated = m_impl->overlayController->ActivateAnalysisDisplay(resolvedRequest);
    if (activated) {
        std::cout << "[Host] Gap Analysis display mode requested. Analysis will start after volume data is ready." << std::endl;
    }
    return activated;
}

bool HostGapAnalysisBinding::ToggleOverlayVisibility()
{
    return m_impl->overlayController
        && m_impl->overlayController->ToggleAnalysisOverlayVisibility();
}

bool HostGapAnalysisBinding::ExitDisplay()
{
    return m_impl->overlayController
        && m_impl->overlayController->ExitAnalysisDisplay();
}

bool HostGapAnalysisBinding::GetDisplayActive() const
{
    return m_impl->overlayController
        && m_impl->overlayController->GetAnalysisDisplayActive();
}

void HostGapAnalysisBinding::AttachTimer(
    const HostGapAnalysisEventPumpConfig& eventPumpConfig)
{
    if (!eventPumpConfig.enableTimer) {
        return;
    }
    if (!m_impl->renderViews) {
        return;
    }

    const HostRenderViewRuntime* timerView = nullptr;
    if (!eventPumpConfig.timerViewId.empty()) {
        timerView = m_impl->renderViews->GetViewById(eventPumpConfig.timerViewId);
    }
    else if (eventPumpConfig.useTimerViewRole) {
        timerView = m_impl->renderViews->GetFirstViewByRole(eventPumpConfig.timerViewRole);
    }

    if (!timerView || !timerView->context || !timerView->context->GetInteractor()) {
        std::cerr << "[Host] Gap Analysis timer skipped: explicit timer render view is missing." << std::endl;
        return;
    }

    // Timer 挂在 host 明确选择的事件循环承载视图上；它只消费显式请求，不代表默认进入孔隙分析模式。
    // Qt host 后续可以把事件泵窗口配置为自己的主 QVTK 视图，而不用改 feature 插件。
    auto observer = vtkSmartPointer<GapAnalysisOverlayCommitObserver>::New();
    observer->gapAnalysis = m_impl->core.gapAnalysis;
    observer->visualConfigService = timerView->service;
    observer->gapAnalysisOverlayController = m_impl->overlayController;
    observer->dataMgr = m_impl->core.sharedDataMgr;
    observer->sharedState = m_impl->core.sharedState;
    timerView->context->GetInteractor()->AddObserver(vtkCommand::TimerEvent, observer, 0.2f);
}
