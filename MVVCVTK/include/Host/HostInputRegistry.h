#pragma once

#include "Host/HostFeature.h"
#include "Host/Types/HostRequestTypes.h"

#include <functional>
#include <memory>

class IHostViewDirectory;

// Host 私有输入注册中心；管理 binding 和 stable Context callback，
// active pointer capture 的唯一 owner 仍是每个 Context 内的 InteractionRouter。
class HostInputRegistry final
{
public:
    using HostCallback = std::function<InteractionResult(
        const InteractionEvent&,
        const std::string& viewId,
        HostRenderViewRole role)>;

    explicit HostInputRegistry(
        std::weak_ptr<IHostViewDirectory> directory);
    ~HostInputRegistry();

    bool Start(HostViewTargets allViews);
    HostInputPort& GetFeaturePort();
    bool SetHostInput(HostViewTargets targets, HostCallback onInput);
    bool ClearHostInput();
    HostInputResult SendInput(const HostInputEvent& event);
    bool Stop();

private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
};
