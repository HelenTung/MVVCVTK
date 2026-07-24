#include "HostHotkeyRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
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
    auto bindings = std::make_shared<HostFeatureBindings>();
    auto commandRouter =
        std::make_shared<HostCommandRouter>(core, views, bindings);
    HostHotkeyConfig config;
    config.isContextInputEnabled = true;
    config.contextInputViews.viewIds = { "primary" };
    config.isCommandInputEnabled = true;
    config.commandInputViews.viewIds = { "primary" };
    config.modelSwitchKey = 'm';
    config.saveTransformedDataKey = 'v';
    config.saveSliceImagesKey = 's';
    config.gapSwitchKey = 'g';
    config.exitKeySym = "Escape";
    HostHotkeyTemplates templates;
    templates.gapStart.targetViews.viewIds = { "primary" };
    templates.volumeExportRequest.outputPath = "volume.raw";
    templates.sliceExportRequest.outputDir = "slices";
    templates.sliceExportRequest.sourceView.viewId = "slice";

    {
        HostHotkeyRouter hotkeys(views, bindings, commandRouter);
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

        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'g'));
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'g'));
        SetExpect(
            bindings->GetGapStartCount() == 1 && bindings->GetGapView(),
            "gap hotkey 应发送一次 typed start 请求。",
            failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'g'));
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'g'));
        SetExpect(
            bindings->GetGapLayerCount() == 1,
            "活动 gap 的相同 hotkey 应切换 overlay。",
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
        const auto featureResult =
            context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'x'));
        SetExpect(
            featureResult.isHandled && !featureResult.isPropagationStopped
                && featureInputCount == 1,
            "feature input 应按目标 view 收到通用输入事件。",
            failureCount);
        SetExpect(
            hotkeys.GetInputPort().DetachInput("feature.input"),
            "feature input 应支持按 id 对称卸载。",
            failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'x'));
        SetExpect(
            featureInputCount == 1,
            "卸载后 feature input 不应继续收到事件。",
            failureCount);

        InteractionEvent escape;
        escape.eventKind = InteractionEventKind::KeyPress;
        escape.keySym = "Escape";
        context->OnInput(escape);
        SetExpect(
            !bindings->GetGapView() && bindings->GetGapExitCount() == 1,
            "Escape 应优先退出活动 Gap。",
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
