#pragma once

#include "Host/Types/HostRequestTypes.h"

#include <functional>
#include <memory>
#include <optional>

class IHostViewDirectory;

// HostCommandRouter 是主体请求分发器，不是 feature，也不是业务 service。
class HostCommandRouter final {
public:
    using DisplayCheck = std::function<std::optional<bool>()>;
    using DisplayCallback = std::function<bool(DisplayCheck, HostCompleteCallback)>;
    explicit HostCommandRouter(
        std::weak_ptr<IHostViewDirectory> directory);
    ~HostCommandRouter();

    bool Dispatch(
        HostRequest&& request,
        HostCompleteCallback onComplete = nullptr,
        DisplayCallback onDisplay = nullptr) const;

private:
    // router 独占主体请求分发实现。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
