#pragma once

#include "Host/Types/HostRequestTypes.h"

#include <memory>

class IHostViewDirectory;

// HostCommandRouter 是主体请求分发器，不是 feature，也不是业务 service。
class HostCommandRouter final {
public:
    explicit HostCommandRouter(
        std::weak_ptr<IHostViewDirectory> directory);
    ~HostCommandRouter();

    bool Dispatch(
        HostRequest&& request,
        HostCompleteCallback onComplete = nullptr) const;

private:
    // router 独占主体请求分发实现。
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
