#define _CRT_SECURE_NO_WARNINGS

#include <vtkAutoInit.h>
#include <vtkSMPTools.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

#include "Host/VtkAppHostSession.h"

namespace {

class AppLaunchConfig final {
public:
static std::vector<TFNode> BuildVolumeTF()
{
    // 这组 transfer function 是 standalone 五视图调试布局的视觉输入；真实上位机应按自己的显示配方下发 WindowConfig。
    return {
        { 0.00, 0.0, 0.0, 0.0, 0.0 },
        { 0.50, 0.0, 0.0, 0.5, 0.0 },
        { 0.85, 0.8, 0.0, 0.5, 0.0 },
        { 1.00, 1.0, 0.0, 0.5, 0.0 },
    };
}

static HostRenderViewConfig BuildViewConfig(
    std::string id,
    HostRenderViewRole role,
    WindowConfig window,
    bool isEventLoopEnabled = false)
{
    HostRenderViewConfig view;
    view.id = std::move(id);
    view.role = role;
    view.window = std::move(window);
    view.isEventLoopEnabled = isEventLoopEnabled;
    return view;
}

static std::vector<HostRenderViewConfig> BuildViewConfigs()
{
    std::vector<HostRenderViewConfig> views;
    const auto volTF = BuildVolumeTF();

    // 这五个窗口只是 standalone 调试拓扑；放在 main 是为了让 session/HostRenderViewSet 不再携带固定窗口数假设。
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
    primary3D.isAxesVisible = true;
    primary3D.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    primary3D.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
    primary3D.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    primary3D.preInitCfg.hasBgColor = true;

    views.push_back(BuildViewConfig("primary-3d", HostRenderViewRole::Primary3D, std::move(primary3D)));
    views.push_back(BuildViewConfig("composite-volume", HostRenderViewRole::Composite3D, std::move(compositeVolume)));
    views.push_back(BuildViewConfig("slice-top-down", HostRenderViewRole::TopDownSlice, std::move(topDownSlice), true));
    views.push_back(BuildViewConfig("slice-front-back", HostRenderViewRole::FrontBackSlice, std::move(frontBackSlice)));
    views.push_back(BuildViewConfig("slice-left-right", HostRenderViewRole::LeftRightSlice, std::move(leftRightSlice)));
    return views;
}

static HostHotkeyBindings BuildHotkeys()
{
    // 这些按键只模拟当前 standalone 调试入口的输入协议。
    // 为什么集中在 main：后续真实上位机接入时可以替换为 UI action / IPC 命令，而 feature 不需要重编译。
    HostHotkeyBindings hotkeys;
    hotkeys.modelSwitchKey = 'm';
    hotkeys.saveTransformedDataKey = 's';
    hotkeys.saveSliceImagesKey = 't';
    hotkeys.cropSwitchKey = 'o';
    hotkeys.planarSwitchKey = 'p';
    hotkeys.gapSwitchKey = 'j';
    hotkeys.keepInsidePreviewKey = '1';
    hotkeys.removeInsidePreviewKey = '2';
    hotkeys.submitKey = '3';
    hotkeys.exitKeySym = "Escape";
    return hotkeys;
}

static HostGapConfig BuildGapConfig()
{
    // 这些值只是 standalone 调试样本的孔隙分析配方。
    // 为什么放在 main：真实上位机应按材料、批次或用户参数下发同一结构，Host/timer 只负责执行显式命令。
    HostGapConfig config;
    config.surface.isoMode = HostGapAnalysisIsoMode::DataRangeRatio;
    config.surface.dataRangeRatio = 0.55;
    config.voidDetection.grayMin = -0.2262536138296127f;
    config.voidDetection.grayMax = 0.15f;
    config.voidDetection.minVolumeMM3 = 0.0001f;
    config.voidDetection.angleThresholdDeg = 30.0f;
    config.voidDetection.tensorWindowSize = 1;
    config.voidDetection.erosionIterations = 2;
    return config;
}
};

}

int main()
{
    vtkSMPTools::Initialize();

    VtkAppHostSession::Config config;
    // standalone 由 Host 先初始化全部视图；Start 仍会在进入事件循环前再渲染承载主循环的视图。
    config.isInitialRenderEnabled = true;
    const std::vector<HostRenderViewRole> standaloneViewRoles = {
        HostRenderViewRole::Primary3D,
        HostRenderViewRole::Composite3D,
        HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,
        HostRenderViewRole::LeftRightSlice
    };

    // main 现在扮演“临时上位机适配层”：显式声明调试键位、监听范围和 feature 作用目标。
    // 这样 session/feature 不需要默认假设输入协议，也不会在创建时自动进入某个分析链路。
    config.hotkeys = AppLaunchConfig::BuildHotkeys();
    // standalone 五窗口也是上位机模拟输入的一部分；session 不再保存或暗补固定窗口拓扑。
    config.renderViews = AppLaunchConfig::BuildViewConfigs();

    // 这里临时模拟上位机下发“加载体数据”命令，所以文件路径和 RAW 物理元数据都在 main 中显式展开。
    // 后续真实上位机接入时，只替换这些输入值的来源，不把样本事实下沉到 HostSessionTypes 或 feature。
    const std::string volumePath = "F:\\data\\1000x1000x1000.raw";
    const std::string volumeExportPath = "F:\\data\\1000x1000x1000_transformed.raw";
    const std::string sliceExportDir = "F:\\data\\1000x1000x1000_slice_exports";
    const std::array<float, 3> volumeSpacing = { 0.02125f, 0.02125f, 0.02125f };
    const std::array<float, 3> volumeOrigin = { 0.0f, 0.0f, 0.0f };
    if (!volumePath.empty()) {
        config.initialVolume.isInitialLoadEnabled = true;
        config.initialVolume.filePath = volumePath;
        config.initialVolume.geometry.emplace(volumeSpacing, volumeOrigin);
    }

    config.renderContextInput.isHotkeyEnabled = true;
    config.renderContextInput.targetViewRoles = standaloneViewRoles;
    // 体数据导出路径由上位机输入；standalone 只在这里模拟，不让 DataManager 按加载路径猜输出位置。
    config.dataExport.transformedDataOutputPath = volumeExportPath;
    // 切片导出目录也是上位机输入事实；standalone 只在这里模拟，不让 host/session 或 DataManager 隐式拼默认路径。
    config.dataExport.sliceOutputDir = sliceExportDir;

    // 热键监听范围和 feature 作用范围分开配置：
    // A. targetViewRoles 决定哪些 interactor 收 standalone 键。
    // B. cropViewRequest / gapViewRequest 决定命令真正作用到哪些窗口。
    config.commandInput.isHotkeyEnabled = true;
    config.commandInput.targetViewRoles = standaloneViewRoles;
    config.commandInput.cropViewRequest.isReferenceRoleUsed = true;
    config.commandInput.cropViewRequest.referenceRole = HostRenderViewRole::Primary3D;
    config.commandInput.cropViewRequest.previewViewRoles = standaloneViewRoles;
    config.commandInput.gapViewRequest.targetViewRoles = standaloneViewRoles;
    config.commandInput.gapViewRequest.algorithm = AppLaunchConfig::BuildGapConfig();

    // standalone 的 VTK 主循环由 TopDownSlice 窗口承载，所以 host/session 主线程 tick 也显式挂到这个 role。
    // Qt / 上位机接入时应改为自己的主事件泵窗口 id，而不是沿用这里的调试 role。
    config.timerEventPump.isTimerEnabled = true;
    config.timerEventPump.isTimerRoleUsed = true;
    config.timerEventPump.timerViewRole = HostRenderViewRole::TopDownSlice;

    VtkAppHostSession session(std::move(config));
    session.Start();

    return 0;
}
