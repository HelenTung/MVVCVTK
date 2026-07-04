#include "Host/HostRenderViewSet.h"

#include "AppInterfaces.h"
#include "AppService.h"
#include "AppStateEvents.h"
#include "DataManager.h"
#include "StdRenderContext.h"

#include <vtkRenderWindowInteractor.h>

#include <algorithm>
#include <iostream>
#include <utility>

namespace {
// VTK 同时提供 KeyCode 和 KeySym；host 统一采样后再匹配配置键位，
// 是为了让字母键大小写和 Escape 这类控制键走同一条输入边界。
struct HostKeyInput {
    char keyCode = 0;
    std::string keySym;
};

static HostKeyInput BuildHostKeyInput(vtkRenderWindowInteractor* interactor)
{
    HostKeyInput input;
    if (!interactor) {
        return input;
    }

    input.keyCode = interactor->GetKeyCode();
    input.keySym = interactor->GetKeySym() ? interactor->GetKeySym() : "";
    return input;
}

static char ToUpperAscii(char value)
{
    return (value >= 'a' && value <= 'z')
        ? static_cast<char>(value - 'a' + 'A')
        : value;
}

static bool MatchesCharacterKey(const HostKeyInput& input, char key)
{
    if (key == 0) {
        // 0 表示 host 没有分配这个调试键；空键位不参与匹配，避免默认配置意外响应窗口事件。
        return false;
    }

    const char upperKey = ToUpperAscii(key);
    const std::string keySymbol(1, key);
    const std::string upperKeySymbol(1, upperKey);
    return input.keyCode == key
        || input.keyCode == upperKey
        || input.keySym == keySymbol
        || input.keySym == upperKeySymbol;
}

static bool MatchesKeySymbol(const HostKeyInput& input, const std::string& keySym)
{
    return !keySym.empty() && input.keySym == keySym;
}

static std::pair<
    std::shared_ptr<MedicalVizService>,
    std::shared_ptr<StdRenderContext>>
    BuildRenderViewRuntimePair(
        const WindowConfig& cfg,
        vtkSmartPointer<vtkRenderWindow> renderWindow,
        std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> sharedState,
        std::shared_ptr<IStateEventSource> stateEventSource)
{
    // 1. service 绑定共享数据和状态，负责业务渲染能力。
    // 2. context 绑定 VTK window/interactor，负责单窗口渲染生命周期。
    // 3. 二者在这里组装，是因为 HostRenderViewSet 知道窗口拓扑，但不把 topology 写进 StdRenderContext。
    auto service = std::make_shared<MedicalVizService>(
        std::move(dataMgr),
        std::move(sharedState),
        std::move(stateEventSource));
    auto context = std::make_shared<StdRenderContext>();

    if (renderWindow) {
        // 外部窗口必须先注入，再绑定 service；否则 service 会先拿到默认 window，后续 overlay 目标会短暂错位。
        context->SetRenderWindow(std::move(renderWindow));
    }
    context->SetServiceBound(service);
    service->SetVisualConfig(cfg.preInitCfg);
    context->SetWindowTitle(cfg.title);
    context->SetWindowSize(cfg.width, cfg.height);
    context->SetWindowPosition(cfg.posX, cfg.posY);
    context->ApplyCameraStyle(cfg.preInitCfg.vizMode);
    if (cfg.showAxes) {
        context->SetOrientationAxesVisible(true);
    }

    if (cfg.preInitCfg.hasBgColor) {
        service->SetBackground(cfg.preInitCfg.bgColor);
    }

    return { service, context };
}

static std::vector<TFNode> BuildVolumeTransferFunction()
{
    // 这组 transfer function 是 standalone 五视图调试布局的默认视觉参数，不属于体数据或孔隙算法事实。
    return {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };
}

static std::string BuildFallbackRenderViewId(size_t index)
{
    return "view-" + std::to_string(index + 1);
}

static HostRenderViewConfig BuildRenderViewConfig(
    std::string id,
    HostRenderViewRole role,
    WindowConfig window,
    bool startStandaloneEventLoop = false)
{
    HostRenderViewConfig view;
    view.id = std::move(id);
    view.role = role;
    view.window = std::move(window);
    view.startStandaloneEventLoop = startStandaloneEventLoop;
    return view;
}

static bool ContainsRole(
    const std::vector<HostRenderViewRole>& roles,
    HostRenderViewRole role)
{
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

static bool ContainsId(
    const std::vector<std::string>& ids,
    const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

static bool HandleHostRenderContextHotkey(
    const HostHotkeyBindings& hotkeys,
    const std::shared_ptr<MedicalVizService>& service,
    vtkRenderWindowInteractor* interactor,
    StdRenderContext& context)
{
    const HostKeyInput keyInput = BuildHostKeyInput(interactor);
    if (MatchesCharacterKey(keyInput, hotkeys.modelTransformToggleKey)) {
        // 工具模式切换属于宿主输入映射：RenderContext 提供能力，具体键位不写进 core。
        context.SetToolMode(context.GetToolMode() == ToolMode::ModelTransform
            ? ToolMode::Navigation
            : ToolMode::ModelTransform);
        return true;
    }

    if (MatchesCharacterKey(keyInput, hotkeys.saveTransformedDataKey)) {
        if (service) {
            service->SaveTransformedDataAsync({});
        }
        return true;
    }

    if (MatchesCharacterKey(keyInput, hotkeys.saveSliceImagesKey)) {
        if (service) {
            service->SaveSliceImagesAsync({}, context.GetAngle());
        }
        return true;
    }

    if (MatchesKeySymbol(keyInput, hotkeys.exitKeySym)) {
        if (context.GetToolMode() == ToolMode::ModelTransform) {
            context.SetToolMode(ToolMode::Navigation);
            return true;
        }
        return false;
    }

    return false;
}

static void AttachHostRenderContextHotkey(
    const std::shared_ptr<StdRenderContext>& context,
    const std::shared_ptr<MedicalVizService>& service,
    const HostHotkeyBindings& hotkeys)
{
    if (!context) {
        return;
    }

    context->SetHostKeyEventHandler(
        [service, hotkeys](vtkRenderWindowInteractor* interactor, StdRenderContext& renderContext) {
            return HandleHostRenderContextHotkey(hotkeys, service, interactor, renderContext);
        });
}
}

void HostRenderViewSet::Build(
    const HostCoreServices& core,
    const std::vector<HostRenderViewConfig>& configs)
{
    // Build 是窗口拓扑进入 session 的唯一入口；调用方传多少 configs，就创建/接管多少窗口。
    // 这里不补默认五窗口，默认值由 VtkAppHostSession::Impl 处理，避免集合层同时承担配置决策。
    m_views.clear();
    m_views.reserve(configs.size());
    for (size_t index = 0; index < configs.size(); ++index) {
        HostRenderViewConfig config = configs[index];
        if (config.id.empty()) {
            // endpoint 需要稳定 id 供 Qt / 上位机回填 widget 映射；缺省 id 只用于本地调试兜底。
            config.id = BuildFallbackRenderViewId(index);
        }

        auto pair = BuildRenderViewRuntimePair(
            config.window,
            config.renderWindow,
            core.sharedDataMgr,
            core.sharedState,
            core.sharedStateBroadcaster);

        HostRenderViewRuntime view;
        view.config = std::move(config);
        view.service = std::move(pair.first);
        view.context = std::move(pair.second);
        m_views.push_back(std::move(view));
    }
}

const std::vector<HostRenderViewRuntime>& HostRenderViewSet::GetViews() const
{
    return m_views;
}

const HostRenderViewRuntime* HostRenderViewSet::GetViewById(const std::string& id) const
{
    for (const auto& view : m_views) {
        if (view.config.id == id) {
            return &view;
        }
    }
    return nullptr;
}

const HostRenderViewRuntime* HostRenderViewSet::GetFirstViewByRole(HostRenderViewRole role) const
{
    for (const auto& view : m_views) {
        if (view.config.role == role) {
            return &view;
        }
    }
    return nullptr;
}

const HostRenderViewRuntime* HostRenderViewSet::GetPrimaryView() const
{
    // 先选明确 Primary3D，再退到任意 3D 视图；非标准窗口拓扑也要给裁切和初始加载一个可解释的参考视图。
    if (const auto* view = GetFirstViewByRole(HostRenderViewRole::Primary3D)) {
        return view;
    }

    for (const auto& view : m_views) {
        if (GetRoleIs3DView(view.config.role)) {
            return &view;
        }
    }

    return m_views.empty() ? nullptr : &m_views.front();
}

const HostRenderViewRuntime* HostRenderViewSet::GetStandaloneStartView() const
{
    // 独立 VTK host 只能由一个 interactor 进入阻塞主循环；Qt host 不调用 Start，因此不会被这里约束。
    for (const auto& view : m_views) {
        if (view.config.startStandaloneEventLoop) {
            return &view;
        }
    }
    return m_views.empty() ? nullptr : &m_views.front();
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::GetViewsByIdsAndRoles(
    const std::vector<std::string>& ids,
    const std::vector<HostRenderViewRole>& roles) const
{
    std::vector<const HostRenderViewRuntime*> selectedViews;
    selectedViews.reserve(m_views.size());

    // 空 ids/roles 表示宿主没有声明作用域，返回空而不是全选；这样 feature 激活不会因为漏配目标而接管所有窗口。
    for (const auto& view : m_views) {
        const bool idSelected = !ids.empty() && ContainsId(ids, view.config.id);
        const bool roleSelected = !roles.empty() && ContainsRole(roles, view.config.role);
        if (idSelected || roleSelected) {
            selectedViews.push_back(&view);
        }
    }

    return selectedViews;
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::GetConfiguredCropPreviewViews() const
{
    // includeInCropPreview 是 host 对“配置默认 preview 集合”的声明；只有请求明确允许 fallback 时才会走到这里。
    std::vector<const HostRenderViewRuntime*> selectedViews;
    selectedViews.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (view.config.includeInCropPreview) {
            selectedViews.push_back(&view);
        }
    }
    return selectedViews;
}

std::vector<const HostRenderViewRuntime*> HostRenderViewSet::GetDefaultGapOverlayViews() const
{
    // 默认孔隙 overlay 只按 role 判断可显示能力，不按窗口序号或历史五窗口布局判断。
    std::vector<const HostRenderViewRuntime*> selectedViews;
    selectedViews.reserve(m_views.size());
    for (const auto& view : m_views) {
        if (GetRoleIsGapOverlayRole(view.config.role)) {
            selectedViews.push_back(&view);
        }
    }
    return selectedViews;
}

std::vector<std::shared_ptr<AbstractInteractiveService>> HostRenderViewSet::BuildInteractiveServices(
    const std::vector<const HostRenderViewRuntime*>& views) const
{
    // 裁切 bridge 只需要刷新/overlay 这种交互服务接口；这里降到抽象类型，防止 feature 依赖 HostRenderViewRuntime。
    std::vector<std::shared_ptr<AbstractInteractiveService>> services;
    services.reserve(views.size());
    for (const auto* view : views) {
        if (view && view->service) {
            services.push_back(view->service);
        }
    }
    return services;
}

void HostRenderViewSet::ConfigureInitialVisibility() const
{
    // 初始可见性是视图角色策略，不是窗口编号策略；新增窗口只要声明 role 即可进入正确默认状态。
    for (const auto& view : m_views) {
        if (!view.service) {
            continue;
        }

        if (GetRoleIs3DView(view.config.role)) {
            view.service->SetElementVisible(VisFlags::Planes3D, false);
            view.service->SetElementVisible(VisFlags::Ruler, false);
        }

        if (GetRoleIsSliceView(view.config.role)) {
            view.service->SetElementVisible(VisFlags::Crosshair, true);
        }
    }
}

void HostRenderViewSet::AttachRenderContextHotkeys(
    const HostRenderContextInputConfig& inputConfig,
    const HostHotkeyBindings& hotkeys) const
{
    if (!inputConfig.enableStandaloneHotkeys) {
        return;
    }

    const auto targetViews = GetViewsByIdsAndRoles(
        inputConfig.targetViewIds,
        inputConfig.targetViewRoles);
    if (targetViews.empty()) {
        std::cerr << "[Host] Standalone render context hotkeys skipped: no target render view was requested." << std::endl;
        return;
    }

    // RenderContext 只暴露按键转交点；当前独立 VTK host 在这里安装自己的非 feature 功能键映射。
    for (const auto* view : targetViews) {
        if (view) {
            AttachHostRenderContextHotkey(view->context, view->service, hotkeys);
        }
    }
}

void HostRenderViewSet::RenderAll() const
{
    // 初始化末尾先渲染一次，让外部 host 获取 endpoint 后看到的是已创建 renderer/interactor 的窗口。
    for (const auto& view : m_views) {
        if (view.context) {
            view.context->Render();
        }
    }
}

void HostRenderViewSet::InitializeAllInteractors() const
{
    // VTK interactor 需要在 endpoint 暴露前完成初始化；Qt host 注入的 window 也沿用同一顺序。
    for (const auto& view : m_views) {
        if (view.context) {
            view.context->InitializeInteractor();
        }
    }
}

std::vector<HostRenderViewEndpoint> HostRenderViewSet::BuildEndpoints() const
{
    std::vector<HostRenderViewEndpoint> endpoints;
    endpoints.reserve(m_views.size());
    for (const auto& view : m_views) {
        // endpoint 是非拥有观察句柄，给外部 host 做 widget 映射；service 仍留在 session 内部维持边界。
        HostRenderViewEndpoint endpoint;
        endpoint.id = view.config.id;
        endpoint.role = view.config.role;
        endpoint.renderer = view.context ? view.context->GetRenderer() : nullptr;
        endpoint.renderWindow = view.context ? view.context->GetRenderWindow() : nullptr;
        endpoint.interactor = view.context ? view.context->GetInteractor() : nullptr;
        endpoints.push_back(endpoint);
    }
    return endpoints;
}

std::vector<HostRenderViewConfig> HostRenderViewSet::BuildDefaultConfigs()
{
    std::vector<HostRenderViewConfig> views;
    const auto volTF = BuildVolumeTransferFunction();

    // 默认五视图保留原独立 VTK 调试入口的观察布局；真正的窗口数量仍由 Config::renderViews 决定。
    WindowConfig compositeVolume;
    compositeVolume.title = "Window E: Composite Volume";
    compositeVolume.width = 600; compositeVolume.height = 600;
    compositeVolume.posX = 660; compositeVolume.posY = 50;
    compositeVolume.preInitCfg.vizMode = VizMode::CompositeVolume;
    compositeVolume.preInitCfg.tfNodes = volTF;
    compositeVolume.preInitCfg.hasTF = true;
    compositeVolume.preInitCfg.bgColor = { 0.08, 0.08, 0.12 };
    compositeVolume.preInitCfg.hasBgColor = true;

    WindowConfig topDownSlice;
    topDownSlice.title = "Window B: Top_down Slice";
    topDownSlice.width = 400; topDownSlice.height = 400;
    topDownSlice.posX = 50;  topDownSlice.posY = 660;
    topDownSlice.preInitCfg.vizMode = VizMode::SliceTop_down;
    topDownSlice.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    topDownSlice.preInitCfg.hasBgColor = true;
    topDownSlice.preInitCfg.windowLevel = { 400.0, 40.0 };
    topDownSlice.preInitCfg.hasWindowLevel = true;

    WindowConfig frontBackSlice;
    frontBackSlice.title = "Window C: Front_back Slice";
    frontBackSlice.width = 400; frontBackSlice.height = 400;
    frontBackSlice.posX = 460; frontBackSlice.posY = 660;
    frontBackSlice.preInitCfg.vizMode = VizMode::SliceFront_back;
    frontBackSlice.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    frontBackSlice.preInitCfg.hasBgColor = true;
    frontBackSlice.preInitCfg.windowLevel = { 400.0, 40.0 };
    frontBackSlice.preInitCfg.hasWindowLevel = true;

    WindowConfig leftRightSlice;
    leftRightSlice.title = "Window D: Left_right Slice";
    leftRightSlice.width = 400; leftRightSlice.height = 400;
    leftRightSlice.posX = 870; leftRightSlice.posY = 660;
    leftRightSlice.preInitCfg.vizMode = VizMode::SliceLeft_right;
    leftRightSlice.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    leftRightSlice.preInitCfg.hasBgColor = true;
    leftRightSlice.preInitCfg.windowLevel = { 400.0, 40.0 };
    leftRightSlice.preInitCfg.hasWindowLevel = true;

    WindowConfig primary3D;
    primary3D.title = "Window A: Composite IsoSurface";
    primary3D.width = 600; primary3D.height = 600;
    primary3D.posX = 50;  primary3D.posY = 50;
    primary3D.showAxes = true;
    primary3D.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    primary3D.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    primary3D.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    primary3D.preInitCfg.hasBgColor = true;

    views.push_back(BuildRenderViewConfig("primary-3d", HostRenderViewRole::Primary3D, std::move(primary3D)));
    views.push_back(BuildRenderViewConfig("composite-volume", HostRenderViewRole::Composite3D, std::move(compositeVolume)));
    views.push_back(BuildRenderViewConfig("slice-top-down", HostRenderViewRole::TopDownSlice, std::move(topDownSlice), true));
    views.push_back(BuildRenderViewConfig("slice-front-back", HostRenderViewRole::FrontBackSlice, std::move(frontBackSlice)));
    views.push_back(BuildRenderViewConfig("slice-left-right", HostRenderViewRole::LeftRightSlice, std::move(leftRightSlice)));
    return views;
}

bool HostRenderViewSet::GetRoleIs3DView(HostRenderViewRole role)
{
    return role == HostRenderViewRole::Primary3D
        || role == HostRenderViewRole::Composite3D;
}

bool HostRenderViewSet::GetRoleIsSliceView(HostRenderViewRole role)
{
    return role == HostRenderViewRole::TopDownSlice
        || role == HostRenderViewRole::FrontBackSlice
        || role == HostRenderViewRole::LeftRightSlice;
}

bool HostRenderViewSet::GetRoleIsGapOverlayRole(HostRenderViewRole role)
{
    // 当前 overlay 分为 3D mesh 和 2D label 两类；其他辅助窗口默认不接收，除非后续扩展新 role 语义。
    return GetRoleIs3DView(role) || GetRoleIsSliceView(role);
}
