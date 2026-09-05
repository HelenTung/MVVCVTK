#pragma once

#include "Host/Types/HostSessionTypes.h"

#include <memory>

class HostCommandRouter;
class HostInputRegistry;

// standalone host 的按键适配层：把主体热键转换为具体 HostRequest，
// 并直接下沉到主体请求路由。Context 安装与 Feature binding
// 均归 HostInputRegistry，本类只是 HostExtension callback adapter。
class HostHotkeyRouter final {
public:
    HostHotkeyRouter(
        HostInputRegistry& inputRegistry,
        std::weak_ptr<HostCommandRouter> commandRouter);
    ~HostHotkeyRouter();

    bool AttachHotkeys(const HostHotkeyConfig& config);
    bool ClearHotkeys();

private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
};
