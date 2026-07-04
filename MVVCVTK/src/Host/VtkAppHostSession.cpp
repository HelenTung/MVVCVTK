#include "Host/VtkAppHostSession.h"

#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostRenderViewSet.h"

#include "AppService.h"
#include "AppState.h"
#include "StdRenderContext.h"
#include "VolumeAnalysisService.h"

#include <functional>
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

static void StartInitialVolumeLoad(
    const HostCoreServices& core,
    const HostRenderViewSet& renderViews,
    std::function<bool()> refreshOrthogonalCropInput,
    const InitialVolumeLoadConfig& initialVolume,
    bool showStandaloneHotkeyHelp,
    const HostHotkeyBindings& hotkeys)
{
    // 初始加载是 host 命令，不是 session 默认动作：
    // 1. 没有显式 enable 时直接等待上位机后续命令。
    // 2. enable 后必须同时具备路径和 RAW 几何元数据。
    // 3. 加载完成只刷新共享状态和裁切输入，不自动进入孔隙显示模式。
    if (!initialVolume.enableInitialLoad) {
        // 数据来源必须由宿主显式决定；Qt / 上位机初始化 session 时不应隐式读取独立调试 RAW 文件。
        return;
    }
    if (initialVolume.filePath.empty()) {
        // enableInitialLoad 表达“宿主要执行初始加载”，filePath 表达“加载哪个外部数据源”；两者分开校验，避免空路径落到 DataManager。
        std::cerr << "[Host] Initial volume load skipped: file path was not specified." << std::endl;
        return;
    }
    if (!initialVolume.geometry) {
        // RAW 路径只能定位字节来源，不能说明物理坐标；缺少 geometry 时拒绝加载，避免下游裁切和孔隙分析使用伪 spacing/origin。
        std::cerr << "[Host] Initial volume load skipped: volume geometry was not specified." << std::endl;
        return;
    }

    const auto* primaryView = renderViews.GetPrimaryView();
    if (!primaryView || !primaryView->service) {
        std::cerr << "[Host] Initial volume load skipped: primary render view is missing." << std::endl;
        return;
    }

    // 数据只加载一次到共享 DataManager；其他视图通过共享状态和 Timer 同步，不各自触发文件 I/O。
    primaryView->service->LoadFileAsync(
        initialVolume.filePath,
        initialVolume.geometry->spacing,
        initialVolume.geometry->origin,
        [
            sharedState = core.sharedState,
            sharedDataMgr = core.sharedDataMgr,
            primaryService = primaryView->service,
            imageAnalysis = core.imageAnalysis,
            refreshOrthogonalCropInput = std::move(refreshOrthogonalCropInput),
            initialVolume,
            showStandaloneHotkeyHelp,
            hotkeys
        ](bool success)
        {
            if (!success) {
                std::cerr << "[Host] Volume data load failed.\n";
                return;
            }

            auto range = sharedState->GetDataRange();
            if (initialVolume.initialIsoDataRangeRatio) {
                const double ratio = *initialVolume.initialIsoDataRangeRatio;
                if (ratio < 0.0 || ratio > 1.0) {
                    std::cerr << "[Host] Initial iso threshold skipped: data range ratio must be within [0, 1]." << std::endl;
                }
                else {
                    // 初始 iso 只按宿主显式配方设置显示状态；session 不保存样本数据的经验阈值。
                    const double isoVal = range[0] + (range[1] - range[0]) * ratio;
                    primaryService->SetIsoThreshold(isoVal);
                }
            }

            auto histogramTable = imageAnalysis->GetHistogramData(initialVolume.histogramBinCount);
            if (histogramTable && histogramTable->GetNumberOfRows() > 0) {
                if (!initialVolume.histogramFilePath.empty()) {
                    // 直方图图片是调试导出产物，不是加载链路的必需步骤；路径不明确时不触发 VTK writer。
                    imageAnalysis->SaveHistogramImage(initialVolume.histogramFilePath, initialVolume.histogramBinCount);
                }
            }
            else {
                std::cerr << "[Host] Histogram generation failed.\n";
            }

            if (showStandaloneHotkeyHelp && refreshOrthogonalCropInput && refreshOrthogonalCropInput()) {
                std::cout << "[Host] Crop tools armed. " << BuildStartupControlsText(hotkeys) << std::endl;
            }

            // GapAnalysis 不随加载自动进入；宿主收到对应命令后，再由 Timer 消费显式分析请求。
        }
    );
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
    // Initialize 后暴露给 Qt / 上位机的非拥有窗口句柄缓存，避免外部看到内部 runtime/service。
    std::vector<HostRenderViewEndpoint> renderViewEndpoints;
    // 防止 Start 或命令入口重复执行组装；命令入口可懒 Initialize，方便上位机先配置后调用。
    bool isInitialized = false;

    explicit Impl(Config sessionConfig)
        : config(std::move(sessionConfig))
        , featureBindings(std::make_shared<HostFeatureBindings>())
    {
        if (config.renderViews.empty()) {
            // 空配置保持历史调试入口可直接运行；传入 renderViews 时则完全尊重外部宿主拓扑。
            config.renderViews = HostRenderViewSet::BuildDefaultConfigs();
        }
    }
};

VtkAppHostSession::VtkAppHostSession()
    : VtkAppHostSession(Config{})
{
}

VtkAppHostSession::VtkAppHostSession(Config config)
    : m_impl(std::make_unique<Impl>(std::move(config)))
{
}

VtkAppHostSession::~VtkAppHostSession() = default;

VtkAppHostSession::VtkAppHostSession(VtkAppHostSession&&) noexcept = default;

VtkAppHostSession& VtkAppHostSession::operator=(VtkAppHostSession&&) noexcept = default;

std::vector<HostRenderViewConfig> VtkAppHostSession::BuildDefaultRenderViewConfigs()
{
    return HostRenderViewSet::BuildDefaultConfigs();
}

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

    m_impl->renderViews.AttachRenderContextHotkeys(
        m_impl->config.renderContextInput,
        m_impl->config.hotkeys);
    m_impl->renderViews.ConfigureInitialVisibility();
    StartInitialVolumeLoad(
        m_impl->core,
        m_impl->renderViews,
        m_impl->featureBindings->BuildOrthogonalCropInputRefreshHandler(),
        m_impl->config.initialVolume,
        m_impl->config.commandInput.enableStandaloneHotkeys,
        m_impl->config.hotkeys);

    m_impl->renderViews.RenderAll();
    m_impl->renderViews.InitializeAllInteractors();
    // endpoint 必须在 interactor 初始化后生成，Qt 接 QVTKOpenGLNativeWidget 时才能拿到完整 renderWindow/interactor。
    m_impl->renderViewEndpoints = m_impl->renderViews.BuildEndpoints();

    m_impl->featureBindings->AttachStandaloneHotkeys(
        m_impl->config.commandInput,
        m_impl->config.hotkeys);
    m_impl->featureBindings->AttachGapAnalysisTimer(m_impl->config.gapAnalysisEventPump);
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

bool VtkAppHostSession::ActivateOrthogonalCrop(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    return m_impl->featureBindings->ActivateOrthogonalCrop(request);
}

bool VtkAppHostSession::ToggleOrthogonalCropBox(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    return m_impl->featureBindings->ToggleInteractiveCrop(request);
}

bool VtkAppHostSession::ToggleOrthogonalCropPlane(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    return m_impl->featureBindings->ToggleInteractivePlanarCrop(request);
}

bool VtkAppHostSession::ToggleOrthogonalCropPreview(
    const HostOrthogonalCropActivationRequest& request,
    HostCropPreviewMode previewMode)
{
    Initialize();
    return m_impl->featureBindings->ToggleCropPreview(
        request,
        previewMode);
}

bool VtkAppHostSession::ApplyOrthogonalCropSubmit(
    const HostOrthogonalCropActivationRequest& request)
{
    Initialize();
    return m_impl->featureBindings->ApplyCropSubmit(request);
}

bool VtkAppHostSession::ExitOrthogonalCrop()
{
    Initialize();
    return m_impl->featureBindings->ExitInteractiveCrop();
}

bool VtkAppHostSession::ActivateGapAnalysisDisplay(
    const HostGapAnalysisActivationRequest& request)
{
    Initialize();
    return m_impl->featureBindings->ActivateGapAnalysisDisplay(request);
}

bool VtkAppHostSession::ToggleGapAnalysisOverlayVisibility()
{
    Initialize();
    return m_impl->featureBindings->ToggleGapAnalysisOverlayVisibility();
}

bool VtkAppHostSession::ExitGapAnalysisDisplay()
{
    Initialize();
    return m_impl->featureBindings->ExitGapAnalysisDisplay();
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
