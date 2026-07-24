#pragma once

#include "Host/Types/HostRequestTypes.h"

#include <functional>
#include <variant>

using HostCompleteCallback = std::function<void(bool isSuccess)>;

struct HostDataCommand {
    HostDataRequest request;
    HostCompleteCallback onComplete;
};

struct HostViewCommand { HostViewRequest request; };
struct HostToolCommand { HostToolRequest request; };

using HostCommand = std::variant<std::monostate, HostDataCommand,
    HostViewCommand, HostToolCommand>;
