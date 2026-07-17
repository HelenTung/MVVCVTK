#pragma once

#include "Host/Types/HostAdapterTypes.h"

#include <memory>

class HostCommandRouter;
class HostFeatureBindings;
class HostRenderViewSet;

// standalone host 的按键适配层：把 VTK KeyPress/KeyRelease/Char 事件转换为 HostCommand。
// 本类不执行业务；renderViews 为会话期非拥有拓扑，feature/router 使用 weak_ptr，
// 已安装 context 也只保存 weak_ptr，析构或 ClearHotkeys 时对称卸载输入 handler。
class HostHotkeyRouter final {
public:
    HostHotkeyRouter(
        const HostRenderViewSet& renderViews,
        std::weak_ptr<HostFeatureBindings> featureBindings,
        std::weak_ptr<HostCommandRouter> commandRouter);
    ~HostHotkeyRouter();

    bool AttachHotkeys(
        const HostHotkeyConfig& config,
        HostHotkeyTemplates templates);
    bool ClearHotkeys();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
