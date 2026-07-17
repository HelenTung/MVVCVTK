#include "HostHotkeyRouterTests.h"

#include "Host/HostCommandRouter.h"
#include "Host/HostCoreServices.h"
#include "Host/HostFeatureBindings.h"
#include "Host/HostHotkeyRouter.h"
#include "Host/HostRenderViewSet.h"
#include "StdRenderContext.h"

#include <vtkCommand.h>

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

InteractionEvent BuildKey(unsigned long eventId, char key, bool isCtrlDown = false)
{
    InteractionEvent event;
    event.vtkEventId = eventId;
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
    HostHotkeyRouter hotkeys(views, bindings, commandRouter);

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

    SetExpect(hotkeys.AttachHotkeys(config, templates),
        "合法 hotkey 配置应完成绑定。", failureCount);
    auto result = context->OnInput(BuildKey(vtkCommand::KeyPressEvent, 'm'));
    SetExpect(result.isHandled && result.hasVtkAbort
            && context->GetToolMode() == ToolMode::ModelTransform,
        "model hotkey 应翻译为 tool switch。", failureCount);
    context->OnInput(BuildKey(vtkCommand::KeyReleaseEvent, 'm'));

    context->OnInput(BuildKey(vtkCommand::KeyPressEvent, 'c'));
    SetExpect(bindings->GetCropBoxCount() == 1 && bindings->GetCropActive(),
        "crop hotkey 应构造 typed Crop Box 请求。", failureCount);
    context->OnInput(BuildKey(vtkCommand::KeyReleaseEvent, 'c'));

    context->OnInput(BuildKey(vtkCommand::KeyPressEvent, 's'));
    SetExpect(bindings->GetCropSendCount() == 0,
        "Submit 未按 Ctrl 时只消费按键，不应提交。", failureCount);
    context->OnInput(BuildKey(vtkCommand::KeyReleaseEvent, 's'));
    context->OnInput(BuildKey(vtkCommand::KeyPressEvent, 's', true));
    SetExpect(bindings->GetCropSendCount() == 1,
        "Ctrl+Submit 应精确提交一次。", failureCount);
    context->OnInput(BuildKey(vtkCommand::KeyReleaseEvent, 's', true));

    InteractionEvent escape;
    escape.vtkEventId = vtkCommand::KeyPressEvent;
    escape.keySym = "Escape";
    context->OnInput(escape);
    SetExpect(!bindings->GetCropActive() && bindings->GetCropExitCount() == 1,
        "Escape 应优先退出活动 Crop。", failureCount);

    SetExpect(hotkeys.ClearHotkeys(), "ClearHotkeys 应成功。", failureCount);
    SetExpect(!context->OnInput(BuildKey(vtkCommand::KeyPressEvent, 'm')).isHandled,
        "清理后输入 handler 不应继续消费事件。", failureCount);
}

} // namespace

int HostHotkeySuite::GetFailCount() const
{
    int failureCount = 0;
    StartHotkeyCases(failureCount);
    return failureCount;
}
