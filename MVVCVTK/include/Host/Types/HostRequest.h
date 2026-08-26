#pragma once

#include <functional>
#include <string>

enum class HostErrorCode {
    None,
    SessionNotReady,
    WrongThread,
    RequestRejected,
    OperationFailed
};

struct HostResult final {
    bool isSucceeded = false;
    HostErrorCode errorCode = HostErrorCode::OperationFailed;
    std::string message;
};

using HostCompleteCallback =
    std::function<void(bool isSuccess)>;
using HostResultCallback =
    std::function<void(HostResult result)>;

struct HostRequest {
    virtual ~HostRequest() = default;
};
