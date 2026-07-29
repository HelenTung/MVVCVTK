#pragma once

#include <functional>

using HostCompleteCallback =
    std::function<void(bool isSuccess)>;

struct HostRequest {
    virtual ~HostRequest() = default;
};
