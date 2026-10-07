#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

// Feature 内部的可选数量限制运算；未设置表示不做策略准入，数值表示显式上限。
// 字节、样本数量及毫秒单位由调用方字段声明，不代表进程资源总量。
class WorkLimit final {
public:
    WorkLimit() = default;
    WorkLimit(std::size_t value) : m_value(value) {}
    WorkLimit(std::optional<std::size_t> value) : m_value(value) {}
    const std::optional<std::size_t>& GetValue() const noexcept { return m_value; }
    std::size_t GetBound(std::size_t required) const noexcept {
        return m_value ? std::min(required, *m_value) : required;
    }
    std::string GetText() const { return m_value ? std::to_string(*m_value) : "unlimited"; }
    WorkLimit operator-(std::size_t used) const noexcept {
        return m_value ? WorkLimit(used >= *m_value ? 0 : *m_value - used) : WorkLimit{};
    }
    WorkLimit operator/(std::size_t bytes) const noexcept {
        return bytes && m_value ? WorkLimit(*m_value / bytes) : (bytes ? WorkLimit{} : WorkLimit(0));
    }
    auto GetDeadline(std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now()) const {
        const auto maximum = std::chrono::steady_clock::time_point::max();
        const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(maximum - start).count();
        if (!m_value || available <= 0 || *m_value >= static_cast<std::size_t>(available)) return maximum;
        return start + std::chrono::milliseconds(*m_value);
    }
    friend bool operator>(std::size_t n, WorkLimit limit) { return limit.m_value && n > *limit.m_value; }
    friend bool operator>=(std::size_t n, WorkLimit limit) { return limit.m_value && n >= *limit.m_value; }
    friend bool operator<(std::size_t n, WorkLimit limit) { return !limit.m_value || n < *limit.m_value; }
    friend bool operator<=(std::size_t n, WorkLimit limit) { return !limit.m_value || n <= *limit.m_value; }
    friend bool operator<(WorkLimit limit, std::size_t n) { return n > limit; }
    friend bool operator<=(WorkLimit limit, std::size_t n) { return n >= limit; }
    friend bool operator>(WorkLimit limit, std::size_t n) { return n < limit; }
    friend bool operator>=(WorkLimit limit, std::size_t n) { return n <= limit; }
    friend bool operator==(WorkLimit limit, std::size_t n) { return limit.m_value && *limit.m_value == n; }
    friend bool operator!=(WorkLimit limit, std::size_t n) { return !(limit == n); }
    friend bool operator==(WorkLimit a, WorkLimit b) { return a.m_value == b.m_value; }
    friend bool operator!=(WorkLimit a, WorkLimit b) { return !(a == b); }
    friend std::ostream& operator<<(std::ostream& s, WorkLimit limit) { return s << limit.GetText(); }
    friend std::istream& operator>>(std::istream& s, WorkLimit& limit) {
        std::string token; if (!(s >> token)) return s;
        if (token == "unlimited") { limit = {}; return s; }
        try { std::size_t end = 0; auto value = std::stoull(token, &end);
            if (end != token.size() || token.front() == '-') s.setstate(std::ios::failbit);
            else limit = WorkLimit(static_cast<std::size_t>(value));
        } catch (...) { s.setstate(std::ios::failbit); }
        return s;
    }
private:
    std::optional<std::size_t> m_value;
};
