#include "HostHotkeyRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostFeature.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostInputRegistry.h"
#include "Host/HostRoutes.h"
#include "Host/Types/HostRequestTypes.h"
#include "ViewContext.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

void SetExpect(bool isExpected, const char* message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

InteractionEvent BuildKey(
    InteractionEventKind eventKind,
    char key,
    bool isCtrlDown = false)
{
    InteractionEvent event;
    event.eventKind = eventKind;
    event.keyCode = key;
    event.keySym = std::string(1, key);
    event.isCtrlDown = isCtrlDown;
    return event;
}

void StartHotkeyCases(int& failureCount)
{
    auto views = std::make_shared<HostRouteStub>();
    auto context = std::make_shared<ViewContextStub>();
    auto sliceContext = std::make_shared<ViewContextStub>();
    views->CreateView("primary", HostRenderViewRole::Primary3D, context);
    views->CreateView("slice", HostRenderViewRole::FrontBackSlice, sliceContext);
    auto service = views->GetState("primary");
    auto sliceService = views->GetState("slice");
    auto commandRouter =
        std::make_shared<HostCommandRouter>(views->GetViewDirectory());
    HostInputRegistry inputRegistry(views->GetViewDirectory());
    HostViewTargets allViews;
    allViews.viewIds = { "primary", "slice" };
    SetExpect(
        inputRegistry.Start(allViews),
        "Host input registry should install one stable callback per view.",
        failureCount);
    HostHotkeyConfig config;
    config.isContextInputEnabled = true;
    config.contextInputViews.viewIds = { "primary" };
    config.isCommandInputEnabled = true;
    config.commandInputViews.viewIds = {
        "primary", "slice" };
    config.modelSwitchKey = 'm';
    config.dataExportKey = 'v';
    config.sliceExportKey = 's';
    config.exitKeySym = "Escape";
    config.sliceExportDir = "configured-slices";

    {
        HostHotkeyRouter hotkeys(
            inputRegistry, commandRouter);
        SetExpect(
            hotkeys.AttachHotkeys(config),
            "合法 hotkey 配置应完成绑定。",
            failureCount);

        auto result =
            context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
        SetExpect(
            result.isHandled && result.isPropagationStopped
                && context->GetToolMode() == ToolMode::ModelTransform,
            "model hotkey 应翻译为 tool switch。",
            failureCount);

        const int modelCount = context->GetToolModeSetCount();
        result =
            context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
        SetExpect(
            result.isHandled && result.isPropagationStopped
                && context->GetToolModeSetCount() == modelCount,
            "重复 KeyPress 应被消费但不重复发送命令。",
            failureCount);
        result =
            context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'm'));
        SetExpect(
            result.isHandled && result.isPropagationStopped
                && context->GetToolModeSetCount() == modelCount,
            "KeyRelease 应只重置按下态。",
            failureCount);

        result = context->OnInput(
            BuildKey(
                InteractionEventKind::KeyPress, 'v'));
        context->OnInput(
            BuildKey(
                InteractionEventKind::KeyRelease, 'v'));
        SetExpect(
            result.isHandled
                && result.isPropagationStopped
                && service
                && service->GetExportCount() == 1
                && service->GetExportDir() == "."
                && service->GetExportExtension() == ".raw",
            "数据热键应直接下沉到统一命令路由。",
            failureCount);

        HostDataExportRequest explicitData;
        explicitData.outputPath = ".";
        explicitData.sourceView = {
            "", true, HostRenderViewRole::Primary3D };
        SetExpect(
            commandRouter->Dispatch(std::move(explicitData))
                && service
                && service->GetExportCount() == 2
                && service->GetExportDir() == "."
                && service->GetExportExtension() == ".raw",
            "数据热键与显式数据动作必须进入同一命令链。",
            failureCount);

        result = sliceContext->OnInput(
            BuildKey(
                InteractionEventKind::KeyPress, 's'));
        sliceContext->OnInput(
            BuildKey(
                InteractionEventKind::KeyRelease, 's'));
        SetExpect(
            result.isHandled
                && result.isPropagationStopped
                && sliceService
                && sliceService->GetSliceCount() == 1
                && sliceService->GetSlicePath()
                    == "configured-slices",
            "切片热键应保留缺省目录，并由触发窗口补齐来源。",
            failureCount);

        HostSliceExportRequest explicitSlices;
        explicitSlices.outputDir = ".";
        explicitSlices.sourceView = {
            "", true, HostRenderViewRole::FrontBackSlice };
        SetExpect(
            commandRouter->Dispatch(std::move(explicitSlices))
                && sliceService
                && sliceService->GetSliceCount() == 2
                && sliceService->GetSlicePath() == ".",
            "切片热键与显式切片动作必须进入同一命令链。",
            failureCount);

        auto invalidConfig = config;
        invalidConfig.contextInputViews.viewIds = {
            "missing" };
        invalidConfig.commandInputViews.viewIds = {
            "missing" };
        const int rollbackModelCount =
            context->GetToolModeSetCount();
        SetExpect(
            !hotkeys.AttachHotkeys(
                invalidConfig),
            "无法解析目标的 hotkey 重配必须被拒绝。",
            failureCount);
        context->OnInput(
            BuildKey(
                InteractionEventKind::KeyPress, 'm'));
        context->OnInput(
            BuildKey(
                InteractionEventKind::KeyRelease, 'm'));
        SetExpect(
            context->GetToolModeSetCount()
                == rollbackModelCount + 1,
            "hotkey 重配失败后应恢复旧 handler。",
            failureCount);

        HostInputBinding throwingInput;
        throwingInput.featureId = "feature.throw";
        throwingInput.targetViews.viewIds = { "primary" };
        throwingInput.onInput =
            [](const InteractionEvent&)
                -> InteractionResult {
                throw 1;
            };
        SetExpect(
            inputRegistry.GetFeaturePort().AttachInput(
                std::move(throwingInput)),
            "Feature 输入异常隔离测试应完成绑定。",
            failureCount);

        int featureInputCount = 0;
        HostInputBinding featureInput;
        featureInput.featureId = "feature.input";
        featureInput.targetViews.viewIds = { "primary" };
        featureInput.onInput =
            [&featureInputCount](const InteractionEvent&) {
                ++featureInputCount;
                return InteractionResult{ true, false };
            };
        SetExpect(
            inputRegistry.GetFeaturePort().AttachInput(std::move(featureInput)),
            "feature input 应通过通用 HostInputPort 注册。",
            failureCount);

        HostInputBinding duplicateInput;
        duplicateInput.featureId = "feature.input";
        duplicateInput.targetViews.viewIds = { "primary" };
        duplicateInput.onInput =
            [](const InteractionEvent&) { return InteractionResult{}; };
        SetExpect(
            !inputRegistry.GetFeaturePort().AttachInput(std::move(duplicateInput)),
            "重复 feature id 的 input binding 必须被拒绝。",
            failureCount);

        int stopInputCount = 0;
        HostInputBinding stopInput;
        stopInput.featureId = "feature.stop";
        stopInput.targetViews.viewIds = { "primary" };
        stopInput.onInput =
            [&stopInputCount](const InteractionEvent& event) {
                ++stopInputCount;
                return InteractionResult{
                    false,
                    event.eventKind == InteractionEventKind::KeyPress };
            };
        SetExpect(
            inputRegistry.GetFeaturePort().AttachInput(std::move(stopInput)),
            "多个 feature input 应能绑定到同一目标视图。",
            failureCount);

        int sliceInputCount = 0;
        HostInputBinding sliceInput;
        sliceInput.featureId = "feature.slice";
        sliceInput.targetViews.viewIds = { "slice" };
        sliceInput.onInput =
            [&sliceInputCount](const InteractionEvent&) {
                ++sliceInputCount;
                return InteractionResult{ true, false };
            };
        SetExpect(
            inputRegistry.GetFeaturePort().AttachInput(std::move(sliceInput)),
            "feature input 应支持独立目标视图。",
            failureCount);

        const auto featureResult =
            context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'x'));
        SetExpect(
            featureResult.isHandled && featureResult.isPropagationStopped
                && featureInputCount == 1 && stopInputCount == 1
                && sliceInputCount == 0,
            "feature input 应按注册顺序聚合并在 propagation stop 后停止。",
            failureCount);

        const int priorityModelCount = context->GetToolModeSetCount();
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
        SetExpect(
            context->GetToolModeSetCount() == priorityModelCount,
            "Feature propagation stop 应先于主体 hotkey 生效。",
            failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'm'));

        sliceContext->OnInput(
            BuildKey(InteractionEventKind::KeyPress, 'x'));
        SetExpect(
            sliceInputCount == 1 && featureInputCount == 3
                && stopInputCount == 3,
            "目标视图匹配应隔离不同 Feature binding。",
            failureCount);

        SetExpect(
            inputRegistry.GetFeaturePort().DetachInput("feature.input"),
            "feature input 应支持按 id 对称卸载。",
            failureCount);
        SetExpect(
            inputRegistry.GetFeaturePort().DetachInput("feature.stop")
                && inputRegistry.GetFeaturePort().DetachInput("feature.slice")
                && inputRegistry.GetFeaturePort().DetachInput("feature.throw"),
            "多个 feature input 应能对称卸载。",
            failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'x'));
        SetExpect(
            featureInputCount == 3 && stopInputCount == 3,
            "卸载后 feature input 不应继续收到事件。",
            failureCount);

        InteractionEvent escape;
        escape.eventKind = InteractionEventKind::KeyPress;
        escape.keySym = "Escape";
        const auto escapeResult = context->OnInput(escape);
        SetExpect(
            escapeResult.isHandled && escapeResult.isPropagationStopped
                && context->GetToolMode() == ToolMode::Navigation,
            "主体 Escape 应回到 Navigation。",
            failureCount);

        SetExpect(
            hotkeys.ClearHotkeys(),
            "ClearHotkeys 应成功。",
            failureCount);
        SetExpect(
            !context
                 ->OnInput(
                     BuildKey(InteractionEventKind::KeyPress, 'm'))
                 .isHandled,
            "清理后输入 handler 不应继续消费事件。",
            failureCount);
        auto disabledConfig = config;
        disabledConfig.isCommandInputEnabled = false;
        SetExpect(
            hotkeys.AttachHotkeys(disabledConfig),
            "关闭 command 热键后仍应保留 context 输入绑定。",
            failureCount);
        const auto disabledExport =
            context->OnInput(
                BuildKey(InteractionEventKind::KeyPress, 'v'));
        SetExpect(
            !disabledExport.isHandled
                && service
                && service->GetExportCount() == 2,
            "未启用 command 热键时不得触发已配置的数据导出动作。",
            failureCount);
        SetExpect(
            hotkeys.AttachHotkeys(config),
            "析构清理测试前应重新绑定 hotkey。",
            failureCount);
    }

    {
        auto specifiedConfig = config;
        specifiedConfig.dataExportPath = "specified-data";
        specifiedConfig.dataExportFormat =
            HostDataExportFormat::Obj;
        specifiedConfig.sliceExportDir =
            "specified-slices";
        specifiedConfig.sliceSourceView = {
            "slice", false, HostRenderViewRole::Auxiliary };
        specifiedConfig.sliceAngleDeg = 12.5;
        HostHotkeyRouter hotkeys(
            inputRegistry, commandRouter);
        SetExpect(
            hotkeys.AttachHotkeys(specifiedConfig),
            "显式导出默认值的 hotkey 配置应完成绑定。",
            failureCount);

        auto result = sliceContext->OnInput(
            BuildKey(InteractionEventKind::KeyPress, 'v'));
        sliceContext->OnInput(
            BuildKey(InteractionEventKind::KeyRelease, 'v'));
        SetExpect(
            result.isHandled
                && result.isPropagationStopped
                && service
                && service->GetExportCount() == 3
                && service->GetExportDir() == "specified-data"
                && service->GetExportExtension() == ".obj",
            "显式格式的数据热键不应依赖触发窗口。",
            failureCount);

        result = context->OnInput(
            BuildKey(InteractionEventKind::KeyPress, 's'));
        context->OnInput(
            BuildKey(InteractionEventKind::KeyRelease, 's'));
        SetExpect(
            result.isHandled
                && result.isPropagationStopped
                && sliceService
                && sliceService->GetSliceCount() == 3
                && sliceService->GetSlicePath() == "specified-slices"
                && sliceService->GetSliceAngleDeg() == 12.5,
            "切片热键必须保留构造时指定的目录、来源视图和角度。",
            failureCount);
    }

    const int modelCount = context->GetToolModeSetCount();
    const auto detached =
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
    SetExpect(
        !detached.isHandled && !detached.isPropagationStopped
            && context->GetToolModeSetCount() == modelCount,
        "HostHotkeyRouter 析构后必须清除 HostExtension binding。",
        failureCount);
    SetExpect(
        inputRegistry.Stop(),
        "Host input registry should clear stable callbacks after adapters stop.",
        failureCount);
}

} // namespace

int HostHotkeySuite::GetFailCount() const
{
    int failureCount = 0;
    StartHotkeyCases(failureCount);
    return failureCount;
}
