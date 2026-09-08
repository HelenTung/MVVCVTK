#pragma once

#include "Data/DataGraphTypes.h"
#include <optional>
#include <string_view>

// nullopt 表示数据已退役；无 scope 的普通数据返回有效但不持有 lease 的值。
inline std::optional<std::shared_ptr<const DataResourceLease>> StartDataResourceUse(
    const DataSnapshot& data, const std::string_view owner,
    const DataResourceKind kind = DataResourceKind::Reader)
{
    if (!data) return std::nullopt;
    if (!GetDataEntityIdValid(data->lifetimeScope)) return std::shared_ptr<const DataResourceLease>{};
    const auto access = data->lifetime.lock();
    auto lease = access ? access->StartResourceUse(data->self, std::string(owner), kind) : nullptr;
    if (!lease) return std::nullopt;
    return lease;
}
