#pragma once
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

namespace AlignmentMemory {
// 计量容器申请的字节数，包含扩容时同时存活的新旧缓冲；分配器元数据不属于该算法预算。
struct AllocationBudget final {
    std::size_t limit = 0, used = 0;
    void Acquire(std::size_t bytes) {
        if (bytes > limit - used) throw std::length_error("Alignment work allocation exceeds budget.");
        used += bytes;
    }
    void Release(std::size_t bytes) noexcept { used -= bytes; }
};
template<class T> struct BudgetAllocator final {
    using value_type = T;
    AllocationBudget *budget = nullptr;
    explicit BudgetAllocator(AllocationBudget *value) noexcept : budget(value) {}
    template<class U> BudgetAllocator(const BudgetAllocator<U> &other) noexcept : budget(other.budget) {}
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
    template<class U> bool operator==(const BudgetAllocator<U> &other) const noexcept { return budget == other.budget; }
    template<class U> bool operator!=(const BudgetAllocator<U> &other) const noexcept { return !(*this == other); }
};
} // namespace AlignmentMemory
