#include "HostHotkeyRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <iostream>
#include <memory>

namespace {

void SetExpect(bool isExpected, const char* message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

InteractionEvent BuildKey(
    InteractionEventKind eventKind, char key, bool isCtrlDown = false)
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
    views.CreateView("primary", HostRenderViewRole::Primary3D, context);
    auto bindings = std::make_shared<HostFeatureBindings>();
    auto commandRouter = std::make_shared<HostCommandRouter>(core, views, bindings);
    HostHotkeyConfig config;
    config.isContextInputEnabled = true;
    config.contextInputViews.viewIds = { "primary" };
    config.isCommandInputEnabled = true;
    config.commandInputViews.viewIds = { "primary" };
    config.modelSwitchKey = 'm';
    config.cropSwitchKey = 'c';
    config.submitKey = 's';
    config.exitKeySym = "Escape";
    HostHotkeyTemplates templates;
    templates.cropTarget.referenceView = {
        "primary", false, HostRenderViewRole::Primary3D };

    {
        HostHotkeyRouter hotkeys(views, bindings, commandRouter);
        SetExpect(hotkeys.AttachHotkeys(config, templates),
            "合法 hotkey 配置应完成绑定。", failureCount);
        auto result = context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
        SetExpect(result.isHandled && result.isPropagationStopped
                && context->GetToolMode() == ToolMode::ModelTransform,
            "model hotkey 应翻译为 tool switch。", failureCount);

        const int modelCount = context->GetToolModeSetCount();
        result = context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
        SetExpect(result.isHandled && result.isPropagationStopped
                && context->GetToolModeSetCount() == modelCount,
            "重复 KeyPress 应被消费但不重复发送命令。", failureCount);
        result = context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'm'));
        SetExpect(result.isHandled && result.isPropagationStopped
                && context->GetToolModeSetCount() == modelCount,
            "KeyRelease 应只重置按下态。", failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'm'));
        SetExpect(context->GetToolModeSetCount() == modelCount + 1,
            "KeyRelease 后再次按下应产生新的命令边沿。", failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'm'));

        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 'c'));
        SetExpect(bindings->GetCropBoxCount() == 1 && bindings->GetCropActive(),
            "crop hotkey 应构造 typed Crop Box 请求。", failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 'c'));

        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 's'));
        SetExpect(bindings->GetCropSendCount() == 0,
            "Submit 未按 Ctrl 时只消费按键，不应提交。", failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 's'));
        context->OnInput(BuildKey(InteractionEventKind::KeyPress, 's', true));
        SetExpect(bindings->GetCropSendCount() == 1,
            "Ctrl+Submit 应精确提交一次。", failureCount);
        context->OnInput(BuildKey(InteractionEventKind::KeyRelease, 's', true));

        InteractionEvent escape;
        escape.eventKind = InteractionEventKind::KeyPress;
        escape.keySym = "Escape";
        context->OnInput(escape);
        SetExpect(!bindings->GetCropActive() && bindings->GetCropExitCount() == 1,
            "Escape 应优先退出活动 Crop。", failureCount);

        SetExpect(hotkeys.ClearHotkeys(), "ClearHotkeys 应成功。", failureCount);
        SetExpect(!context->OnInput(
            BuildKey(InteractionEventKind::KeyPress, 'm')).isHandled,
            "清理后输入 handler 不应继续消费事件。", failureCount);
        SetExpect(hotkeys.AttachHotkeys(config, templates),
            "析构清理测试前应重新绑定 hotkey。", failureCount);
    }

    const int modelCount = context->GetToolModeSetCount();
    const auto detached = context->OnInput(
        BuildKey(InteractionEventKind::KeyPress, 'm'));
    SetExpect(!detached.isHandled && !detached.isPropagationStopped
            && context->GetToolModeSetCount() == modelCount,
        "HostHotkeyRouter 析构后必须清除 context callback。", failureCount);
}

} // namespace

int HostHotkeySuite::GetFailCount() const
{
    int failureCount = 0;
    StartHotkeyCases(failureCount);
    return failureCount;
}
