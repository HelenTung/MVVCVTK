#pragma once

#include "Host/Types/HostInputTypes.h"

#include <memory>

class HostCommandRouter;
class HostInputPort;
class IHostViewDirectory;

// standalone host 的按键适配层：把主体热键转换为具体 HostRequest，
// 并直接下沉到主体请求路由。
// 本类不执行业务；输入只来自窄 route，router 使用 weak_ptr，
// 已安装 context 也只保存 weak_ptr，析构或 ClearHotkeys 时对称卸载输入 handler。
class HostHotkeyRouter final {
public:
    HostHotkeyRouter(
        std::weak_ptr<IHostViewDirectory> directory,
        std::weak_ptr<HostCommandRouter> commandRouter);
    ~HostHotkeyRouter();

    bool AttachHotkeys(const HostHotkeyConfig& config);
    bool ClearHotkeys();
    HostInputPort& GetInputPort();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
