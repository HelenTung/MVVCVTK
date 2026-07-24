#include "HostHotkeyRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <iostream>
#include <memory>
#include <string>

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
    HostCoreServices core;
    HostRenderViewSet views;
    auto context = std::make_shared<StdRenderContext>();
    auto sliceContext = std::make_shared<StdRenderContext>();
    views.CreateView("primary", HostRenderViewRole::Primary3D, context);
    views.CreateView("slice", HostRenderViewRole::TopDownSlice, sliceContext);
    const auto* primaryView = views.GetPrimaryView();
    auto service = primaryView ? primaryView->service : nullptr;
    const auto* sliceView = views.GetViewBySelector(
        { "slice", false, HostRenderViewRole::TopDownSlice });
    auto sliceService = sliceView ? sliceView->service : nullptr;
    auto commandRouter =
        std::make_shared<HostCommandRouter>(core, views);
    HostHotkeyConfig config;
    config.isContextInputEnabled = true;
    config.contextInputViews.viewIds = { "primary" };
    config.isCommandInputEnabled = true;
    config.commandInputViews.viewIds = { "primary" };
    config.modelSwitchKey = 'm';
    config.saveTransformedDataKey = 'v';
    config.saveSliceImagesKey = 's';
    config.exitKeySym = "Escape";
    HostHotkeyTemplates templates;
    templates.volumeExportRequest.outputPath = "volume.raw";
    templates.sliceExportRequest.outputDir = "slices";
    templates.sliceExportRequest.sourceView.viewId = "slice";

    {
        HostHotkeyRouter hotkeys(views, commandRouter);
        SetExpect(
            hotkeys.AttachHotkeys(config, templates),
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

        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'v'));
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'v'));
        SetExpect(
            service && service->GetExportCount() == 1,
            "volume export hotkey 应精确投递一次。",
            failureCount);

        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 's'));
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 's'));
        SetExpect(
            sliceService && sliceService->GetSliceCount() == 1,
            "slice export hotkey 应精确投递一次。",
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
                invalidConfig, templates),
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
            hotkeys.GetInputPort().AttachInput(
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
            hotkeys.GetInputPort().AttachInput(std::move(featureInput)),
            "feature input 应通过通用 HostInputPort 注册。",
            failureCount);

        HostInputBinding duplicateInput;
        duplicateInput.featureId = "feature.input";
        duplicateInput.targetViews.viewIds = { "primary" };
        duplicateInput.onInput =
            [](const InteractionEvent&) { return InteractionResult{}; };
        SetExpect(
            !hotkeys.GetInputPort().AttachInput(std::move(duplicateInput)),
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
            hotkeys.GetInputPort().AttachInput(std::move(stopInput)),
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
            hotkeys.GetInputPort().AttachInput(std::move(sliceInput)),
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
            hotkeys.GetInputPort().DetachInput("feature.input"),
            "feature input 应支持按 id 对称卸载。",
            failureCount);
        SetExpect(
            hotkeys.GetInputPort().DetachInput("feature.stop")
                && hotkeys.GetInputPort().DetachInput("feature.slice")
                && hotkeys.GetInputPort().DetachInput("feature.throw"),
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
        SetExpect(
            hotkeys.AttachHotkeys(config, templates),
            "析构清理测试前应重新绑定 hotkey。",
            failureCount);
    }

    const int modelCount = context->GetToolModeSetCount();
    const auto detached =
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
    SetExpect(
        !detached.isHandled && !detached.isPropagationStopped
            && context->GetToolModeSetCount() == modelCount,
        "HostHotkeyRouter 析构后必须清除 context callback。",
        failureCount);
}

} // namespace

int HostHotkeySuite::GetFailCount() const
{
    int failureCount = 0;
    StartHotkeyCases(failureCount);
    return failureCount;
}
