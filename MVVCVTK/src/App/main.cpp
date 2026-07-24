#define _CRT_SECURE_NO_WARNINGS

#include <vtkAutoInit.h>
#include <vtkSMPTools.h>

#include "Host/VtkAppHostSession.h"
#include "Host/CropHostFeature.h"
#include "Host/GapHostFeature.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
VTK_MODULE_INIT(vtkRenderingFreeType);

namespace {
class AppLaunchConfig final {
public:
    static std::vector<HostTransferNode> BuildVolumeTF()
    {
        return {
            { 0.00, 0.0, 0.0, 0.0, 0.0 },
            { 0.50, 0.0, 0.0, 0.5, 0.0 },
            { 0.85, 0.8, 0.0, 0.5, 0.0 },
            { 1.00, 1.0, 0.0, 0.5, 0.0 }
        };
    }

    static HostRenderViewConfig BuildView(
        std::string id, HostRenderViewRole role, HostWindowConfig window,
        bool isEventLoopEnabled = false)
    {
        HostRenderViewConfig view;
        view.id = std::move(id);
        view.role = role;
        view.window = std::move(window);
        view.isEventLoopEnabled = isEventLoopEnabled;
        return view;
    }

    static std::vector<HostRenderViewConfig> BuildViews()
    {
        const auto transferNodes = BuildVolumeTF();
        HostWindowConfig composite;
        composite.title = "Window E: Composite Volume";
        composite.width = 600; composite.height = 600;
        composite.posX = 660; composite.posY = 50;
        composite.viewInit.viewMode = HostRenderMode::CompositeVolume;
        composite.viewInit.transferNodes = transferNodes;
        composite.viewInit.hasTransferNodes = true;
        composite.viewInit.background = { 0.08, 0.08, 0.12 };
        composite.viewInit.hasBackground = true;

        HostWindowConfig topDown;
        topDown.title = "Window B: Top_down Slice";
        topDown.width = 400; topDown.height = 400;
        topDown.posX = 50; topDown.posY = 660;
        topDown.viewInit.viewMode = HostRenderMode::SliceTopDown;
        topDown.viewInit.background = { 0.0, 0.0, 0.0 };
        topDown.viewInit.hasBackground = true;
        topDown.viewInit.windowLevel = { 400.0, 40.0 };
        topDown.viewInit.hasWindowLevel = true;

        HostWindowConfig frontBack = topDown;
        frontBack.title = "Window C: Front_back Slice";
        frontBack.posX = 460;
        frontBack.viewInit.viewMode = HostRenderMode::SliceFrontBack;

        HostWindowConfig leftRight = topDown;
        leftRight.title = "Window D: Left_right Slice";
        leftRight.posX = 870;
        leftRight.viewInit.viewMode = HostRenderMode::SliceLeftRight;

        HostWindowConfig primary;
        primary.title = "Window A: Composite IsoSurface";
        primary.width = 600; primary.height = 600;
        primary.posX = 50; primary.posY = 50;
        primary.isAxesVisible = true;
        primary.viewInit.viewMode = HostRenderMode::CompositeIsoSurface;
        primary.viewInit.material = { 0.3, 0.6, 0.2, 15.0, 0.4, false };
        primary.viewInit.background = { 0.05, 0.05, 0.05 };
        primary.viewInit.hasBackground = true;

        std::vector<HostRenderViewConfig> views;
        views.push_back(BuildView("primary-3d", HostRenderViewRole::Primary3D, std::move(primary)));
        views.push_back(BuildView("composite-volume", HostRenderViewRole::Composite3D, std::move(composite)));
        views.push_back(BuildView("slice-top-down", HostRenderViewRole::TopDownSlice, std::move(topDown), true));
        views.push_back(BuildView("slice-front-back", HostRenderViewRole::FrontBackSlice, std::move(frontBack)));
        views.push_back(BuildView("slice-left-right", HostRenderViewRole::LeftRightSlice, std::move(leftRight)));
        return views;
    }

    static HostHotkeyConfig GetHotkeys(
        const HostViewTargets& targets)
    {
        HostHotkeyConfig config;
        config.isContextInputEnabled = true;
        config.contextInputViews = targets;
        config.isCommandInputEnabled = true;
        config.commandInputViews = targets;
        config.modelSwitchKey = 'm';
        config.saveTransformedDataKey = 's';
        config.saveSliceImagesKey = 't';
        config.exitKeySym = "Escape";
        return config;
    }

    static CropHostConfig BuildCrop(
        const HostViewTargets& targets)
    {
        CropHostConfig config;
        config.defaultTarget.referenceView = {
            "", true, HostRenderViewRole::Primary3D };
        config.defaultTarget.targetViews = targets;
        config.defaultTarget.isTargetViewsUsed = true;
        config.defaultTarget.isStatusVisible = true;
        config.inputViews = targets;
        config.keys.box.keyCode = 'o';
        config.keys.plane.keyCode = 'p';
        config.keys.noMode.keyCode = '0';
        config.keys.keepMode.keyCode = '1';
        config.keys.removeMode.keyCode = '2';
        config.keys.exportResult.keyCode = '3';
        config.keys.exportResult.isCtrlDown = true;
        config.keys.restoreOriginal.keyCode = '6';
        config.keys.previous.keyCode = '4';
        config.keys.next.keyCode = '5';
        config.keys.exit.keySym = "Escape";
        for (std::size_t index = 0;
            index < config.keys.nodes.size(); ++index) {
            config.keys.nodes[index].keyCode =
                static_cast<char>('0' + index);
            config.keys.nodes[index].isAltDown = true;
        }
        return config;
    }

    static GapHostConfig GetGapConfig(
        const HostViewTargets& targets)
    {
        GapHostConfig config;
        config.defaultStart.targetViews = targets;
        config.defaultStart.surface.isoMode =
            GapIsoMode::DataRangeRatio;
        config.defaultStart.surface.dataRangeRatio = 0.55;
        config.defaultStart.voidParams.grayMin =
            -0.2262536138296127f;
        config.defaultStart.voidParams.grayMax = 0.15f;
        config.defaultStart.voidParams.minVolumeMM3 =
            0.0001;
        config.defaultStart.voidParams.angleThresholdDeg =
            30.0f;
        config.defaultStart.voidParams.tensorWindowSize = 1;
        config.defaultStart.voidParams.erosionIterations = 2;
        config.inputViews = targets;
        config.keys.switchOverlay.keyCode = 'j';
        config.keys.exit.keySym = "Escape";
        return config;
    }
};
}

int main()
{
    vtkSMPTools::Initialize();

    const HostViewTargets allViews{ {}, {
        HostRenderViewRole::Primary3D,
        HostRenderViewRole::Composite3D,
        HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,
        HostRenderViewRole::LeftRightSlice } };

    HostSessionConfig sessionConfig;
    sessionConfig.renderViews = AppLaunchConfig::BuildViews();
    VtkAppHostSession session(std::move(sessionConfig));
    if (!session.BuildSession()) return 1;
    auto cropFeature = std::make_shared<CropHostFeature>(
        AppLaunchConfig::BuildCrop(allViews));
    if (!session.AttachFeature(cropFeature)) return 2;
    auto gapFeature = std::make_shared<GapHostFeature>(
        AppLaunchConfig::GetGapConfig(allViews));
    if (!session.AttachFeature(gapFeature)) {
        (void)session.DetachFeature(*cropFeature);
        return 3;
    }

    const auto detachFeatures = [&]() {
        const bool isGapDetached =
            session.DetachFeature(*gapFeature);
        const bool isCropDetached =
            session.DetachFeature(*cropFeature);
        return isGapDetached && isCropDetached;
    };

    HostTimerConfig timer;
    timer.isTimerEnabled = true;
    timer.targetView = { "", true, HostRenderViewRole::TopDownSlice };
    if (!session.AttachTimer(timer)) {
        (void)detachFeatures();
        return 4;
    }

    HostHotkeyTemplates templates;
    templates.volumeExportRequest.outputPath =
        "F:\\data\\1000x1000x1000_transformed.raw";
    templates.sliceExportRequest.outputDir =
        "F:\\data\\1000x1000x1000_slice_exports";
    if (!session.AttachHotkeys(
            AppLaunchConfig::GetHotkeys(allViews),
            templates)) {
        (void)session.AttachTimer({});
        (void)detachFeatures();
        return 5;
    }

    HostLoadRequest load;
    load.filePath = "F:\\data\\1000x1000x1000.raw";
    load.geometry.dimensions = { 1000, 1000, 1000 };
    load.geometry.spacing = { 0.02125f, 0.02125f, 0.02125f };
    load.geometry.origin = { 0.0f, 0.0f, 0.0f };
    HostDataRequest dataRequest;
    dataRequest.action = HostDataAction::LoadFile;
    dataRequest.payload = std::move(load);
    if (!session.SendData(std::move(dataRequest))) {
        (void)session.AttachHotkeys({}, {});
        (void)session.AttachTimer({});
        (void)detachFeatures();
        return 6;
    }

    const bool isStarted = session.Start();
    const bool isHotkeyStopped =
        session.AttachHotkeys({}, {});
    const bool isTimerStopped =
        session.AttachTimer({});
    const bool isDetached = detachFeatures();
    gapFeature.reset();
    cropFeature.reset();
    return isStarted
            && isHotkeyStopped
            && isTimerStopped
            && isDetached
        ? 0 : 7;
}
