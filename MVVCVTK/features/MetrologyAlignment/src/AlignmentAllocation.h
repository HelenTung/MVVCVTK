#pragma once
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

namespace AlignmentMemory {
// 计量容器的申请字节计账，包含扩容时同时存活的新旧缓冲；仅保护计数溢出。
struct AllocationAccounting final {
    std::size_t limit = 0, used = 0;
    void Acquire(std::size_t bytes) {
        if (bytes > limit - used) throw std::length_error("Alignment work allocation size accounting overflows.");
        used += bytes;
    }
    void Release(std::size_t bytes) noexcept { used -= bytes; }
};
template<class T> struct AccountingAllocator final {
    using value_type = T;
    AllocationAccounting *budget = nullptr;
    explicit AccountingAllocator(AllocationAccounting *value) noexcept : budget(value) {}
    template<class U> AccountingAllocator(const AccountingAllocator<U> &other) noexcept : budget(other.budget) {}
    T *allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::length_error("Alignment allocation size overflows.");
        const auto bytes = count * sizeof(T);
        budget->Acquire(bytes);
        try { return std::allocator<T>{}.allocate(count); }
        catch (...) { budget->Release(bytes); throw; }
    }
    void deallocate(T *pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count); budget->Release(count * sizeof(T));
    }
    template<class U> bool operator==(const AccountingAllocator<U> &other) const noexcept { return budget == other.budget; }
    template<class U> bool operator!=(const AccountingAllocator<U> &other) const noexcept { return !(*this == other); }
};
} // namespace AlignmentMemory
