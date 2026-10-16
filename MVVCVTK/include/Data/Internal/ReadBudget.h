#pragma once
#include <cstddef>
#include <limits>

// 默认不以物理内存快照预估拒绝整份值复制；非零仅表示调用方的显式限制。
// 实际分配失败由读取入口报告。分块仍限制单次复制大小，并继续处理完整输入。
inline std::size_t GetReadBudget(std::size_t requestedBytes) noexcept
{
    return requestedBytes?requestedBytes:std::numeric_limits<std::size_t>::max();
}
