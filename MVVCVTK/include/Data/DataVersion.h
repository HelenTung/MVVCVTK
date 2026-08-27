#pragma once

#include <cstdint>

// 数据版本标识同一批已提交体数据；保留语义别名，避免跨层契约退化为裸整数。
using DataVersion = std::uint64_t;
