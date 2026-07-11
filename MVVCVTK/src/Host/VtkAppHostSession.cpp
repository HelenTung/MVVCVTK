#include "Host/VtkAppHostSession.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"

#include "AppState.h"
#include "DataManager.h"
#include "Interaction/CropBridge.h"
#include "Services/GapAnalysisService.h"
#include "StdRenderContext.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class VtkAppHostSession::Impl final {
public:
    // 宿主输入配置的快照；BuildSession 之后不再把 main / 上位机的临时变量带入内部链路。
    VtkAppHostSession::Config config;
    // 窗口无关的核心服务集合，生命周期跟随 session。
    HostCoreServices core;
    // 当前 session 的窗口集合，负责 id/role 查询和 endpoint 生成。
    HostRenderViewSet renderViews;
    // host 到 feature 的绑定层；shared_ptr 是为了让 VTK observer 用 weak_ptr 安全回调。
    std::shared_ptr<HostFeatureBindings> featureBindings;
    // host 命令分发器只暴露单一 Dispatch 入口，具体分发由内部标志位判断。
    std::shared_ptr<HostCommandRouter> commandRouter;
    // BuildSession 后暴露给 Qt / 上位机的非拥有窗口句柄缓存，避免外部看到内部 runtime/service。
    std::vector<HostRenderViewEndpoint> renderViewEndpoints;
    // 防止 Start 或命令入口重复执行组装；命令入口可懒 BuildSession，方便上位机先配置后调用。
    bool isInitialized = false;

    explicit Impl(VtkAppHostSession::Config sessionConfig);

    void BuildSession();
    HostCommandRouterRequest BuildRequest(HostCommandKind command) const;
    bool SendCommand(HostCommandRouterRequest request);

    std::string BuildKeyLabel(char key) const;
    std::string BuildControlKeyLabel(char key) const;
    std::string BuildStartupControlsText(const HostHotkeyBindings& hotkeys) const;
    void SendStartupStatus() const;

private:
    static HostCoreServices BuildCore();
};

VtkAppHostSession::Impl::Impl(VtkAppHostSession::Config sessionConfig)
    : config(std::move(sessionConfig))
    , featureBindings(std::make_shared<HostFeatureBindings>())
{
}

HostCoreServices VtkAppHostSession::Impl::BuildCore()
{
    // 返回值拥有本次 session 的窗口无关服务；此阶段不读取 renderViews，
    // 因此所有后续创建的 view 都共享这一组数据、状态和 feature owner。
    // 1. 先创建数据与状态真源，让所有视图和 feature 共享同一份会话事实。
    // 2. 再创建依赖这些真源的交互服务；窗口拓扑仍由 BuildSession 单独组装。
    HostCoreServices nextCore;
    nextCore.sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    nextCore.sharedStateBroadcaster = std::make_shared<SharedStateBroadcaster>();
    nextCore.sharedState = std::make_shared<SharedInteractionState>(nextCore.sharedStateBroadcaster);
    nextCore.gapAnalysis = std::make_shared<GapAnalysisService>();
    nextCore.orthogonalCropBridge = std::make_shared<CropBridge>();
    return nextCore;
}

HostCommandRouterRequest VtkAppHostSession::Impl::BuildRequest(HostCommandKind command) const
{
    HostCommandRouterRequest request;
    request.command = command;
    return request;
}

bool VtkAppHostSession::Impl::SendCommand(HostCommandRouterRequest request)
{
    // request 按值进入并在分发时移动，调用方后续修改原 DTO 不影响已接受命令。
    // 命令入口允许懒初始化；BuildSession 的 isInitialized 闸门保证装配只执行一次。
    BuildSession();
    if (!commandRouter) {
        std::cerr << "[Host] Command dispatch skipped: command router is not ready." << std::endl;
        return false;
    }
    return commandRouter->DispatchCommand(std::move(request)); // 所有 Host 命令的唯一分发路径。
}

std::string VtkAppHostSession::Impl::BuildKeyLabel(char key) const
{
    if (key == 0) {
        return "<unassigned>";
    }
    const char labelKey = (key >= 'a' && key <= 'z')
        ? static_cast<char>(key - 'a' + 'A')
        : key;
    return std::string(1, labelKey);
}

std::string VtkAppHostSession::Impl::BuildControlKeyLabel(char key) const
{
    if (key == 0) {
        return "<unassigned>";
    }
    return "Ctrl+" + BuildKeyLabel(key);
}

std::string VtkAppHostSession::Impl::BuildStartupControlsText(const HostHotkeyBindings& hotkeys) const
{
    // 控制台文本从 Config 派生，不写死具体键位；这样 main 改模拟输入时提示不会和实际 observer 脱节。
    return "Controls: "
        + BuildKeyLabel(hotkeys.modelSwitchKey) + " = switch model transform | "
        + BuildKeyLabel(hotkeys.saveTransformedDataKey) + " = save transformed data | "
        + BuildKeyLabel(hotkeys.saveSliceImagesKey) + " = save slice images | "
        + BuildKeyLabel(hotkeys.cropSwitchKey) + " = switch orthogonal crop box | "
        + BuildKeyLabel(hotkeys.planarSwitchKey) + " = switch planar crop | "
        + BuildKeyLabel(hotkeys.gapSwitchKey) + " = enter/switch gap overlays | "
        + (hotkeys.exitKeySym.empty() ? "<unassigned>" : hotkeys.exitKeySym) + " = exit active crop/gap/model-transform mode | "
        + BuildKeyLabel(hotkeys.keepInsidePreviewKey) + " = keep inside/normal-side preview | "
        + BuildKeyLabel(hotkeys.removeInsidePreviewKey) + " = remove inside/normal-side preview | "
        + BuildControlKeyLabel(hotkeys.submitKey) + " = apply submit";
}

void VtkAppHostSession::Impl::SendStartupStatus() const
{
    const bool hasInitialVolumeLoad =
        this->config.initialVolume.isInitialLoadEnabled
        && !this->config.initialVolume.filePath.empty()
        && this->config.initialVolume.geometry.has_value();

    std::cout << (hasInitialVolumeLoad
        ? "Application started. Loading data in background...\n"
        : "Application started. Waiting for host data command...\n");
    if (config.renderContextInput.isHotkeyEnabled
        || config.commandInput.isHotkeyEnabled) {
        // 控制台提示只描述当前独立 VTK host 的真实输入映射；Qt / 上位机关闭热键时不输出固定键位假象。
        std::cout << BuildStartupControlsText(config.hotkeys) << '\n';
    }
}

void VtkAppHostSession::Impl::BuildSession()
{
    if (isInitialized) {
        return;
    }

    // core 不知道窗口数量，先建立所有 view 将共享的数据、状态和算法生命周期。
    core = BuildCore();
    // view set 再按 host 配置创建/接管窗口，建立 service/context owner。
    renderViews.Build(core, config.renderViews);
    // feature 只注入窗口查询与 reload/tick 能力，不因装配而默认激活 Crop/Gap。
    featureBindings->AttachFeatures(core, renderViews);
    // router 最后创建，保证任何命令都能看到完整 core、views 和 feature。
    commandRouter = std::make_shared<HostCommandRouter>(
        core,
        renderViews,
        featureBindings);

    // 设置初始视图状态并发起可选加载后再初始化 interactor，避免首帧读取半初始化 service。
    renderViews.SetInitialVisibility();
    if (config.initialVolume.isInitialLoadEnabled) {
        auto initialLoadRequest = BuildRequest(HostCommandKind::Load);
        initialLoadRequest.initialVolume = config.initialVolume;
        commandRouter->DispatchCommand(std::move(initialLoadRequest));
    }

    renderViews.SendRenderAll();
    renderViews.SetInteractorsReady();
    // endpoint 必须在 interactor 初始化后生成，Qt 接 QVTKOpenGLNativeWidget 时才能拿到完整 renderWindow/interactor。
    renderViewEndpoints = renderViews.BuildEndpoints();

    // observer 最后安装；isInitialized 只在 hotkey/timer 都绑定完成后置位。
    auto attachHotkeysRequest = BuildRequest(HostCommandKind::Hotkeys);
    attachHotkeysRequest.renderContextInput = config.renderContextInput;
    attachHotkeysRequest.dataExportConfig = config.dataExport;
    attachHotkeysRequest.commandInput = config.commandInput;
    attachHotkeysRequest.hotkeys = config.hotkeys;
    commandRouter->DispatchCommand(std::move(attachHotkeysRequest));
    featureBindings->AttachHostTimer(config.timerEventPump);
    SendStartupStatus();

    isInitialized = true;
}

VtkAppHostSession::VtkAppHostSession(Config config)
    : m_impl(std::make_unique<VtkAppHostSession::Impl>(std::move(config)))
{
}

VtkAppHostSession::~VtkAppHostSession() = default;

VtkAppHostSession::VtkAppHostSession(VtkAppHostSession&&) noexcept = default;

VtkAppHostSession& VtkAppHostSession::operator=(VtkAppHostSession&&) noexcept = default;

void VtkAppHostSession::BuildSession()
{
    m_impl->BuildSession();
}

void VtkAppHostSession::Start()
{
    BuildSession();
    if (const auto* startView = m_impl->renderViews.GetStandaloneStartView()) {
        if (startView->context) {
            // 独立 VTK host 需要一个 interactor 承载主循环；Qt host 可只调用 BuildSession 后接管 endpoints。
            startView->context->Start();
        }
    }
}

bool VtkAppHostSession::LoadVolume(
    const InitialVolumeLoadConfig& request,
    std::function<void(bool isSuccess)> onComplete)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::Load);
    routerRequest.initialVolume = request;
    routerRequest.loadComplete = std::move(onComplete);
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::StartCrop(
    const HostCropViewRequest& request)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::CropStart);
    routerRequest.cropViewRequest = request;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::SwitchCropBox(
    const HostCropViewRequest& request)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::CropBox);
    routerRequest.cropViewRequest = request;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::SwitchCropPlane(
    const HostCropViewRequest& request)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::CropPlane);
    routerRequest.cropViewRequest = request;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::SwitchCropView(
    const HostCropViewRequest& request,
    HostCropPreviewMode previewMode)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::CropPreview);
    routerRequest.cropViewRequest = request;
    routerRequest.cropPreviewMode = previewMode;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::SendCrop(
    const HostCropViewRequest& request)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::CropApply);
    routerRequest.cropViewRequest = request;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ExitCrop()
{
    return m_impl->SendCommand(m_impl->BuildRequest(HostCommandKind::CropExit));
}

bool VtkAppHostSession::StartGapView(
    const HostGapViewRequest& request)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::GapStart);
    routerRequest.gapViewRequest = request;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::SwitchGapLayer()
{
    return m_impl->SendCommand(m_impl->BuildRequest(HostCommandKind::GapOverlay));
}

bool VtkAppHostSession::ExitGapView()
{
    return m_impl->SendCommand(m_impl->BuildRequest(HostCommandKind::GapExit));
}

bool VtkAppHostSession::SetViewConfig(const HostViewConfig& config)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::ViewConfig);
    routerRequest.viewConfig = config;
    return m_impl->SendCommand(std::move(routerRequest));
}

bool VtkAppHostSession::ExportData(
    const HostDataExportConfig& dataExportConfig,
    std::function<void(bool isSuccess)> onComplete)
{
    auto routerRequest = m_impl->BuildRequest(HostCommandKind::Export);
    routerRequest.dataExportConfig = dataExportConfig;
    routerRequest.dataExportComplete = std::move(onComplete);
    return m_impl->SendCommand(std::move(routerRequest));
}

const std::vector<HostRenderViewEndpoint>& VtkAppHostSession::GetRenderViewEndpoints()
{
    BuildSession();
    return m_impl->renderViewEndpoints;
}

const HostRenderViewEndpoint* VtkAppHostSession::GetRenderViewEndpoint(
    const std::string& id)
{
    BuildSession();
    for (const auto& endpoint : m_impl->renderViewEndpoints) {
        if (endpoint.id == id) {
            return &endpoint;
        }
    }
    return nullptr;
}

const HostRenderViewEndpoint* VtkAppHostSession::GetPrimaryEndpoint()
{
    BuildSession();
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
