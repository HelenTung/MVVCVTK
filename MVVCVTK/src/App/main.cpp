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
static HostHotkeyBindings BuildStandaloneHotkeyBindings()
{
    // 这些按键只模拟当前 standalone 调试入口的输入协议。
    // 为什么集中在 main：后续真实上位机接入时可以替换为 UI action / IPC 命令，而 feature 不需要重编译。
    HostHotkeyBindings hotkeys;
    hotkeys.modelTransformToggleKey = 'm';
    hotkeys.saveTransformedDataKey = 's';
    hotkeys.saveSliceImagesKey = 't';
    hotkeys.cropToggleKey = 'o';
    hotkeys.planarCropToggleKey = 'p';
    hotkeys.gapOverlayToggleKey = 'j';
    hotkeys.keepInsidePreviewKey = '1';
    hotkeys.removeInsidePreviewKey = '2';
    hotkeys.submitKey = '3';
    hotkeys.exitKeySym = "Escape";
    return hotkeys;
}
}

int main()
{
    vtkSMPTools::Initialize();

    VtkAppHostSession::Config config;
    const std::vector<HostRenderViewRole> standaloneViewRoles = {
        HostRenderViewRole::Primary3D,
        HostRenderViewRole::Composite3D,
        HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice,
        HostRenderViewRole::LeftRightSlice
    };

    // main 现在扮演“临时上位机适配层”：显式声明调试键位、监听范围和 feature 作用目标。
    // 这样 session/feature 不需要默认假设输入协议，也不会在创建时自动进入某个分析链路。
    config.hotkeys = BuildStandaloneHotkeyBindings();

    // 这里临时模拟上位机下发“加载体数据”命令，所以文件路径和 RAW 物理元数据都在 main 中显式展开。
    // 后续真实上位机接入时，只替换这三个输入值的来源，不把样本事实下沉到 HostSessionTypes 或 feature。
    const std::string simulatedVolumeFilePath = "F:\\data\\1000x1000x1000.raw";
    const std::array<float, 3> simulatedVolumeSpacing = { 0.02125f, 0.02125f, 0.02125f };
    const std::array<float, 3> simulatedVolumeOriginValues = { 0.0f, 0.0f, 0.0f };
    if (!simulatedVolumeFilePath.empty()) {
        config.initialVolume.enableInitialLoad = true;
        config.initialVolume.filePath = simulatedVolumeFilePath;
        config.initialVolume.geometry.emplace(simulatedVolumeSpacing, simulatedVolumeOriginValues);
    }

    config.renderContextInput.enableStandaloneHotkeys = true;
    config.renderContextInput.targetViewRoles = standaloneViewRoles;

    // 热键监听范围和 feature 作用范围分开配置：
    // A. targetViewRoles 决定哪些 interactor 收 standalone 键。
    // B. orthogonalCropRequest / gapAnalysisRequest 决定命令真正作用到哪些窗口。
    config.commandInput.enableStandaloneHotkeys = true;
    config.commandInput.targetViewRoles = standaloneViewRoles;
    config.commandInput.orthogonalCropRequest.useReferenceRole = true;
    config.commandInput.orthogonalCropRequest.referenceRole = HostRenderViewRole::Primary3D;
    config.commandInput.orthogonalCropRequest.previewViewRoles = standaloneViewRoles;
    config.commandInput.gapAnalysisRequest.targetViewRoles = standaloneViewRoles;

    VtkAppHostSession session(std::move(config));
    session.Start();

    return 0;
}
