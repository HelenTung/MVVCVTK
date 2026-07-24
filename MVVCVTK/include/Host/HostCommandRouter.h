#pragma once

#include "Host/Types/HostCommandTypes.h"

#include <memory>

class HostRenderViewSet;
struct HostCoreServices;

// HostCommandRouter 是 typed host 命令分发器，不是 feature，也不是业务 service。
class HostCommandRouter final {
public:
    HostCommandRouter(
        const HostCoreServices& core,
        const HostRenderViewSet& renderViews);
    ~HostCommandRouter();

    bool DispatchCommand(HostCommand command) const;

private:
    // router 独占业务命令分发实现。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
