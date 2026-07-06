#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"

#include "StdRenderContext.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
static char ToUpperAscii(char value)
{
    return (value >= 'a' && value <= 'z')
        ? static_cast<char>(value - 'a' + 'A')
        : value;
}

static std::string BuildKeyLabel(char key)
{
    if (key == 0) {
        return "<unassigned>";
    }
    return std::string(1, ToUpperAscii(key));
}

static std::string BuildControlKeyLabel(char key)
{
    if (key == 0) {
        return "<unassigned>";
    }
    return "Ctrl+" + BuildKeyLabel(key);
}

static std::string BuildStartupControlsText(const HostHotkeyBindings& hotkeys)
{
    // 控制台文本从 Config 派生，不写死具体键位；这样 main 改模拟输入时提示不会和实际 observer 脱节。
    return "Controls: "
        + BuildKeyLabel(hotkeys.modelTransformToggleKey) + " = toggle model transform | "
        + BuildKeyLabel(hotkeys.saveTransformedDataKey) + " = save transformed data | "
        + BuildKeyLabel(hotkeys.saveSliceImagesKey) + " = save slice images | "
        + BuildKeyLabel(hotkeys.cropToggleKey) + " = toggle orthogonal crop box | "
        + BuildKeyLabel(hotkeys.planarCropToggleKey) + " = toggle planar crop | "
        + BuildKeyLabel(hotkeys.gapOverlayToggleKey) + " = enter/toggle gap overlays | "
        + (hotkeys.exitKeySym.empty() ? "<unassigned>" : hotkeys.exitKeySym) + " = exit active crop/gap/model-transform mode | "
        + BuildKeyLabel(hotkeys.keepInsidePreviewKey) + " = keep inside/normal-side preview | "
        + BuildKeyLabel(hotkeys.removeInsidePreviewKey) + " = remove inside/normal-side preview | "
        + BuildControlKeyLabel(hotkeys.submitKey) + " = apply submit";
}

static void PrintStartupStatus(
    const VtkAppHostSession::Config& config)
{
    const bool willLoadInitialVolume =
        config.initialVolume.enableInitialLoad
        && !config.initialVolume.filePath.empty()
        && config.initialVolume.geometry.has_value();

    std::cout << (willLoadInitialVolume
        ? "Application started. Loading data in background...\n"
        : "Application started. Waiting for host data command...\n");
    if (config.renderContextInput.enableStandaloneHotkeys
        || config.commandInput.enableStandaloneHotkeys) {
        // 控制台提示只描述当前独立 VTK host 的真实输入映射；Qt / 上位机关闭热键时不输出固定键位假象。
        std::cout << BuildStartupControlsText(config.hotkeys) << '\n';
    }
}

} // namespace

struct VtkAppHostSession::Impl {
    // 宿主输入配置的快照；Initialize 之后不再把 main / 上位机的临时变量带入内部链路。
    Config config;
    // 窗口无关的核心服务集合，生命周期跟随 session。
    HostCoreServices core;
    // 当前 session 的窗口集合，负责 id/role 查询和 endpoint 生成。
    HostRenderViewSet renderViews;
    // host 到 feature 的绑定层；shared_ptr 是为了让 VTK observer 用 weak_ptr 安全回调。
    std::shared_ptr<HostFeatureBindings> featureBindings;
    // host 命令分发器只暴露单一 Dispatch 入口，具体分发由内部标志位判断。
    std::shared_ptr<HostCommandRouter> commandRouter;
    // Initialize 后暴露给 Qt / 上位机的非拥有窗口句柄缓存，避免外部看到内部 runtime/service。
    std::vector<HostRenderViewEndpoint> renderViewEndpoints;
    // 防止 Start 或命令入口重复执行组装；命令入口可懒 Initialize，方便上位机先配置后调用。
    bool isInitialized = false;

    explicit Impl(Config sessionConfig)
        : config(std::move(sessionConfig))
        , featureBindings(std::make_shared<HostFeatureBindings>())
    {
    }
};

VtkAppHostSession::VtkAppHostSession(Config config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

VtkAppHostSession::~VtkAppHostSession() = default;

VtkAppHostSession::VtkAppHostSession(VtkAppHostSession&&) noexcept = default;

VtkAppHostSession& VtkAppHostSession::operator=(VtkAppHostSession&&) noexcept = default;

void VtkAppHostSession::Initialize()
{
    if (m_impl->isInitialized) {
        return;
    }

    // 1. core services 不知道窗口数量，只建立数据、状态和算法服务生命周期。
    // 2. render view set 负责按 host 配置创建/接管窗口，并对外提供 id/role/endpoint。
    // 3. feature bindings 只注册能力；裁切或孔隙显示必须等宿主命令携带目标窗口后才激活。
    m_impl->core = BuildHostCoreServices();
    m_impl->renderViews.Build(m_impl->core, m_impl->config.renderViews);
    m_impl->featureBindings->RegisterFeatures(m_impl->core, m_impl->renderViews);
    m_impl->commandRouter = std::make_shared<HostCommandRouter>(
        m_impl->core,
        m_impl->renderViews,
        m_impl->featureBindings);

    m_impl->renderViews.ConfigureInitialVisibility();
    if (m_impl->config.initialVolume.enableInitialLoad) {
        HostCommandRouterRequest initialLoadRequest;
        initialLoadRequest.command = HostCommandKind::Load;
        initialLoadRequest.initialVolume = m_impl->config.initialVolume;
        m_impl->commandRouter->DispatchHostCommand(std::move(initialLoadRequest));
    }

    m_impl->renderViews.RenderAll();
    m_impl->renderViews.InitializeAllInteractors();
    // endpoint 必须在 interactor 初始化后生成，Qt 接 QVTKOpenGLNativeWidget 时才能拿到完整 renderWindow/interactor。
    m_impl->renderViewEndpoints = m_impl->renderViews.BuildEndpoints();

    HostCommandRouterRequest attachHotkeysRequest;
    attachHotkeysRequest.command = HostCommandKind::Hotkeys;
    attachHotkeysRequest.renderContextInput = m_impl->config.renderContextInput;
    attachHotkeysRequest.dataExportConfig = m_impl->config.dataExport;
    attachHotkeysRequest.commandInput = m_impl->config.commandInput;
    attachHotkeysRequest.hotkeys = m_impl->config.hotkeys;
    m_impl->commandRouter->DispatchHostCommand(std::move(attachHotkeysRequest));
    m_impl->featureBindings->AttachHostTimerEventPump(m_impl->config.timerEventPump);
    PrintStartupStatus(m_impl->config);

    m_impl->isInitialized = true;
}

void VtkAppHostSession::Start()
{
    Initialize();
    if (const auto* startView = m_impl->renderViews.GetStandaloneStartView()) {
        if (startView->context) {
            // 独立 VTK host 需要一个 interactor 承载主循环；Qt host 可只调用 Initialize 后接管 endpoints。
            startView->context->Start();
        }
    }
}

bool VtkAppHostSession::RequestVolumeLoad(
    const InitialVolumeLoadConfig& request,
    std::function<void(bool success)> onComplete)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::Load;
    routerRequest.initialVolume = request;
    routerRequest.loadComplete = std::move(onComplete);
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ActivateOrthogonalCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::CropStart;
    routerRequest.orthogonalCropRequest = request;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ToggleOrthogonalCropBox(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::CropBox;
    routerRequest.orthogonalCropRequest = request;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ToggleOrthogonalCropPlane(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::CropPlane;
    routerRequest.orthogonalCropRequest = request;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ToggleOrthogonalCropPreview(
    const HostOrthogonalCropActivationRequest& request,
    HostCropPreviewMode previewMode)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::CropPreview;
    routerRequest.orthogonalCropRequest = request;
    routerRequest.cropPreviewMode = previewMode;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ApplyOrthogonalCropSubmit(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::CropApply;
    routerRequest.orthogonalCropRequest = request;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ExitOrthogonalCrop()
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::CropExit;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ActivateGapAnalysisDisplay(
    const HostGapAnalysisActivationRequest& request)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::GapStart;
    routerRequest.gapAnalysisRequest = request;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ToggleGapAnalysisOverlayVisibility()
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::GapOverlay;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ExitGapAnalysisDisplay()
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::GapExit;
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

bool VtkAppHostSession::RequestDataExport(
    const HostDataExportConfig& dataExportConfig,
    std::function<void(bool success)> onComplete)
{
    Initialize();
    HostCommandRouterRequest routerRequest;
    routerRequest.command = HostCommandKind::Export;
    routerRequest.dataExportConfig = dataExportConfig;
    routerRequest.dataExportComplete = std::move(onComplete);
    return m_impl->commandRouter->DispatchHostCommand(std::move(routerRequest));
}

const std::vector<HostRenderViewEndpoint>& VtkAppHostSession::GetRenderViewEndpoints()
{
    Initialize();
    return m_impl->renderViewEndpoints;
}

const HostRenderViewEndpoint* VtkAppHostSession::GetRenderViewEndpoint(
    const std::string& id)
{
    Initialize();
    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.id == id) {
            return &endpoint;
        }
    }
    return nullptr;
}

const HostRenderViewEndpoint* VtkAppHostSession::GetPrimaryRenderViewEndpoint()
{
    Initialize();
    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.role == HostRenderViewRole::Primary3D) {
            return &endpoint;
        }
    }

    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.role == HostRenderViewRole::Composite3D) {
            return &endpoint;
        }
    }

    return m_impl->renderViewEndpoints.empty() ? nullptr : &m_impl->renderViewEndpoints.front();
}
