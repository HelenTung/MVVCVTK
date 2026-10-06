#pragma once
// 原始存储灰度的连续三线性材料场；精确符号运算避免漏过单元内部孔隙。
#include <algorithm>
#include <array>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/rational.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ThicknessMaterialField
{
using Int =
    boost::multiprecision::number<boost::multiprecision::cpp_int_backend<>, boost::multiprecision::et_off>;
using Q = boost::rational<Int>;
using Point = std::array<double, 3>;
using Index = std::array<std::int64_t, 3>;
using QPoint = std::array<Q, 3>;
using Polynomial = std::array<Q, 4>;
struct Unsupported : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
// 显式解码 IEEE 位模式，避免依赖浮点数到有理数的库转换策略。
inline Q GetExactValue(double value)
{
    if (!std::isfinite(value))
        throw Unsupported("nonfinite stored input");
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const auto exponent = int((bits >> 52) & 2047);
    Int mantissa = bits & ((std::uint64_t(1) << 52) - 1);
    if (exponent)
        mantissa += Int(1) << 52;
    const int shift = exponent ? exponent - 1023 - 52 : -1074;
    Q result = mantissa;
    if (shift >= 0)
        result *= Int(1) << shift;
    else
        result /= Int(1) << -shift;
    return bits >> 63 ? -result : result;
}
inline std::int64_t GetFloor(const Q &value)
{
    const auto n = value.numerator(), d = value.denominator();
    Int quotient = n / d;
    if (n < 0 && n % d != 0)
        --quotient;
    return quotient.convert_to<std::int64_t>();
}
inline double GetDoubleValue(const Q &value)
{
    using Wide = boost::multiprecision::cpp_bin_float_quad;
    return (Wide(value.numerator()) / Wide(value.denominator())).convert_to<double>();
}
inline int GetSign(const Q &value)
{
    return value > 0 ? 1 : value < 0 ? -1 : 0;
}
// 每个初等运算向外扩一 ULP；只有符号得到区间证明时才跳过有理数路径。
struct Interval
{
    double low = 0, high = 0;
    Interval() = default;
    Interval(double value) : low(value), high(value)
    {
    }
    Interval(double lo, double hi) : low(lo), high(hi)
    {
    }
};
inline Interval GetInterval(double lo, double hi)
{
    return {std::nextafter(lo, -std::numeric_limits<double>::infinity()),
            std::nextafter(hi, std::numeric_limits<double>::infinity())};
}
inline Interval GetInterval(const Q &value)
{
    const double v = GetDoubleValue(value);
    return GetInterval(v, v);
}
inline Interval operator+(Interval a, Interval b)
{
    return GetInterval(a.low + b.low, a.high + b.high);
}
inline Interval operator-(Interval a, Interval b)
{
    return GetInterval(a.low - b.high, a.high - b.low);
}
inline Interval operator-(Interval a)
{
    return {-a.high, -a.low};
}
inline Interval operator*(Interval a, Interval b)
{
    const std::array<double, 4> p{a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high};
    if (!std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); }))
        return {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    return GetInterval(*std::min_element(p.begin(), p.end()), *std::max_element(p.begin(), p.end()));
}
inline Interval operator/(Interval a, double positive)
{
    return GetInterval(a.low / positive, a.high / positive);
}
inline Interval &operator+=(Interval &a, Interval b)
{
    a = a + b;
    return a;
}
inline int GetSign(Interval value)
{
    return value.low > 0 ? 1 : value.high < 0 ? -1 : 0;
}
template <class Number>
inline std::array<Number, 4> GetLinePolynomial(const std::array<Number, 8> &nodes,
                                               const std::array<Number, 3> &start,
                                               const std::array<Number, 3> &delta, const Number &threshold)
{
    std::array<Number, 4> result{-threshold, Number(0), Number(0), Number(0)};
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        std::array<Number, 4> weight{Number(1), Number(0), Number(0), Number(0)};
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const Number a = corner & (1U << axis) ? start[axis] : Number(1) - start[axis];
            const Number b = corner & (1U << axis) ? delta[axis] : -delta[axis];
            std::array<Number, 4> next{};
            for (unsigned k = 0; k <= axis; ++k)
            {
                next[k] += weight[k] * a;
                next[k + 1] += weight[k] * b;
            }
            weight = std::move(next);
        }
        for (unsigned k = 0; k < 4; ++k)
            result[k] += nodes[corner] * weight[k];
    }
    return result;
}

struct Span
{
    Q low, high;
    int sign;
};
struct Event
{
    Q low, high;
};
struct Detail
{
    std::vector<Span> spans;
    std::vector<Event> events;
    std::size_t visited = 0;
};
// Bernstein 符号变差为 1 时证明开区间内唯一穿越；切触或未解根按无效处理。
inline void IsolateRoots(const Polynomial &b, const Q &low, const Q &high, const Q &tolerance, Detail &detail,
                         const std::function<void()> &check, unsigned depth = 0)
{
    if ((detail.visited++ & 63U) == 0)
        check();
    if (detail.visited > 4096 || depth > 256)
        throw Unsupported("root isolation budget");
    bool positive = false, negative = false;
    int previous = 0, variations = 0;
    for (const auto &c : b)
    {
        const int s = GetSign(c);
        positive |= s > 0;
        negative |= s < 0;
        if (s && previous && s != previous)
            ++variations;
        if (s)
            previous = s;
    }
    if (!positive && !negative)
        throw Unsupported("zero interval");
    if (b[0] == 0)
        detail.events.push_back({low, low});
    if (b[3] == 0)
        detail.events.push_back({high, high});
    if (!positive || !negative)
    {
        detail.spans.push_back({low, high, positive ? 1 : -1});
        return;
    }
    if (variations == 1 && GetSign(b[0]) * GetSign(b[3]) == -1 && high - low <= tolerance)
    {
        detail.events.push_back({low, high});
        return;
    }
    const Q x = (b[0] + b[1]) / 2, y = (b[1] + b[2]) / 2, z = (b[2] + b[3]) / 2;
    const Q xy = (x + y) / 2, yz = (y + z) / 2, mid = (xy + yz) / 2, t = (low + high) / 2;
    IsolateRoots({b[0], x, xy, mid}, low, t, tolerance, detail, check, depth + 1);
    IsolateRoots({mid, yz, z, b[3]}, t, high, tolerance, detail, check, depth + 1);
}
// 局部二进制参数保持精确的二等分；合并同号区间后才换回全局有理数。
struct IntervalSpan
{
    double low = 0, high = 0;
    int sign = 0;
};
struct IntervalDetail
{
    std::vector<IntervalSpan> spans;
    std::vector<std::array<double, 2>> events;
    std::size_t visited = 0;
};
inline bool IsolateIntervalRoots(const std::array<Interval, 4> &b, double low, double high,
                                 unsigned minimumDepth, IntervalDetail &detail,
                                 const std::function<void()> &check, unsigned depth = 0)
{
    if ((detail.visited++ & 63U) == 0)
        check();
    if (detail.visited > 4096 || depth > 52)
        return false;
    std::array<int, 4> signs{};
    unsigned changes = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        signs[i] = GetSign(b[i]);
        if (!signs[i])
            return false;
        if (i && signs[i] != signs[i - 1])
            ++changes;
    }
    if (!changes)
    {
        if (!detail.spans.empty() && detail.spans.back().high == low && detail.spans.back().sign == signs[0])
            detail.spans.back().high = high;
        else
            detail.spans.push_back({low, high, signs[0]});
        return true;
    }
    if (changes == 1 && signs[0] != signs[3] && depth >= minimumDepth)
    {
        detail.events.push_back({low, high});
        return true;
    }
    const auto x = (b[0] + b[1]) / 2, y = (b[1] + b[2]) / 2, z = (b[2] + b[3]) / 2;
    const auto xy = (x + y) / 2, yz = (y + z) / 2, mid = (xy + yz) / 2;
    const double t = (low + high) * .5;
    if (t <= low || t >= high)
        return false;
    return IsolateIntervalRoots({b[0], x, xy, mid}, low, t, minimumDepth, detail, check, depth + 1) &&
           IsolateIntervalRoots({mid, yz, z, b[3]}, t, high, minimumDepth, detail, check, depth + 1);
}
inline bool GetCertifiedRootIntervals(const std::array<Interval, 4> &b, const Q &low, const Q &high,
                                      const Q &tolerance, Detail &detail, const std::function<void()> &check)
{
    const Q width = high - low;
    const double ratio = GetDoubleValue(width / tolerance);
    if (!std::isfinite(ratio) || ratio <= 0)
        return false;
    unsigned depth = unsigned(std::clamp(std::ceil(std::log2(ratio)), 0.0, 52.0));
    Q resolution = width / Q(Int(1) << depth);
    while (resolution > tolerance)
    {
        if (++depth > 52)
            return false;
        resolution /= 2;
    }
    while (depth && resolution * 2 <= tolerance)
    {
        --depth;
        resolution *= 2;
    }
    IntervalDetail fast;
    fast.visited = detail.visited;
    if (!IsolateIntervalRoots(b, 0, 1, depth, fast, check))
        return false;
    const auto global = [&](double local) -> Q { return low + width * GetExactValue(local); };
    for (const auto &span : fast.spans)
        detail.spans.push_back({global(span.low), global(span.high), span.sign});
    for (const auto &event : fast.events)
        detail.events.push_back({global(event[0]), global(event[1])});
    detail.visited = fast.visited;
    return true;
}
enum class Status
{
    Valid,
    Invalid,
    Missing,
    Unresolved
};
struct Result
{
    Status status = Status::Unresolved;
    std::array<double, 2> trim{};
    double maximumRootWidth = 0;
    std::size_t cells = 0, events = 0;
    const char *reason = "unresolved";
};
struct Field
{
    std::array<int, 6> extent{};
    double threshold = 0;
    std::function<bool(const Index &, double &)> node;
    std::function<void()> check = [] {};

    bool GetCornerValues(const Index &cell, std::array<Q, 8> &values) const
    {
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            Index index{};
            for (unsigned a = 0; a < 3; ++a)
            {
                index[a] = cell[a] + ((corner >> a) & 1U);
                if (index[a] < extent[2 * a] || index[a] > extent[2 * a + 1])
                    return false;
            }
            double value = 0;
            if (!node(index, value) || !std::isfinite(value))
                return false;
            values[corner] = GetExactValue(value);
        }
        return true;
    }
    // -2 缺支持，-1 恰在隐式表面上，0 背景，1 材料。
    int GetMaterialAt(const Point &point) const
    {
        QPoint local{};
        Index cell{};
        for (unsigned a = 0; a < 3; ++a)
        {
            if (!std::isfinite(point[a]) || point[a] < extent[2 * a] || point[a] > extent[2 * a + 1])
                return -2;
            cell[a] = std::min<std::int64_t>(std::int64_t(std::floor(point[a])), extent[2 * a + 1] - 1);
        }
        std::array<Interval, 8> boundedNodes{};
        std::array<Interval, 3> boundedLocal{};
        for (unsigned a = 0; a < 3; ++a)
            boundedLocal[a] = Interval(point[a]) - Interval(double(cell[a]));
        for (unsigned c = 0; c < 8; ++c)
        {
            auto index = cell;
            for (unsigned a = 0; a < 3; ++a)
                index[a] += (c >> a) & 1U;
            double value = 0;
            if (!node(index, value) || !std::isfinite(value))
                return -2;
            boundedNodes[c] = Interval(value);
        }
        Interval bounded(-threshold);
        for (unsigned c = 0; c < 8; ++c)
        {
            Interval weight(1);
            for (unsigned a = 0; a < 3; ++a)
                weight = weight * ((c >> a) & 1U ? boundedLocal[a] : Interval(1) - boundedLocal[a]);
            bounded += boundedNodes[c] * weight;
        }
        const int certified = GetSign(bounded);
        if (certified)
            return certified > 0 ? 1 : 0;
        for (unsigned a = 0; a < 3; ++a)
            local[a] = GetExactValue(point[a]) - cell[a];
        std::array<Q, 8> nodes;
        if (!GetCornerValues(cell, nodes))
            return -2;
        const int sign = GetSign(GetLinePolynomial(nodes, local, {0, 0, 0}, GetExactValue(threshold))[0]);
        return sign > 0 ? 1 : sign < 0 ? 0 : -1;
    }
    Result GetMaterialPath(const Point &from, const Point &delta, double length, double epsilon,
                           double maximumTrim, double tolerance = 1e-9) const
    {
        Result result;
        try
        {
            if (!std::isfinite(length) || !std::isfinite(epsilon) || !std::isfinite(maximumTrim) ||
                !std::isfinite(tolerance) || epsilon <= 0 || length <= 4 * epsilon || maximumTrim < 0 ||
                tolerance <= 0)
                throw Unsupported("invalid path input");
            QPoint start{}, difference{}, end{};
            const Q L = GetExactValue(length), E = GetExactValue(epsilon), begin = E / L, finish = 1 - begin;
            std::vector<Q> cuts{begin, finish};
            for (unsigned a = 0; a < 3; ++a)
            {
                if (extent[2 * a] >= extent[2 * a + 1])
                    throw Unsupported("invalid extent");
                start[a] = GetExactValue(from[a]);
                difference[a] = GetExactValue(delta[a]);
                end[a] = start[a] + difference[a];
                // 包括两端在内的整段都必须有原始灰度支持。
                if (std::min(start[a], end[a]) < extent[2 * a] ||
                    std::max(start[a], end[a]) > extent[2 * a + 1])
                {
                    result.status = Status::Missing;
                    result.reason = "outside full support";
                    return result;
                }
                if (difference[a] != 0)
                {
                    const auto lo = GetFloor(std::min(start[a], end[a])) + 1,
                               hi = GetFloor(std::max(start[a], end[a]));
                    if (hi - lo > 10000)
                        throw Unsupported("cell budget");
                    for (auto face = lo; face <= hi; ++face)
                    {
                        const Q t = (Q(face) - start[a]) / difference[a];
                        if (t > begin && t < finish)
                            cuts.push_back(t);
                    }
                }
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            if (cuts.size() > 10000)
                throw Unsupported("cell budget");
            Detail detail;
            for (std::size_t k = 1; k < cuts.size(); ++k)
            {
                check();
                const Q a = cuts[k - 1], b = cuts[k], mid = (a + b) / 2;
                Index cell{};
                QPoint local{}, step{};
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    cell[axis] = std::min<std::int64_t>(GetFloor(start[axis] + mid * difference[axis]),
                                                        extent[2 * axis + 1] - 1);
                    local[axis] = start[axis] + a * difference[axis] - cell[axis];
                    step[axis] = (b - a) * difference[axis];
                }
                std::array<Q, 8> nodes;
                if (!GetCornerValues(cell, nodes))
                {
                    result.status = Status::Missing;
                    result.reason = "missing node";
                    return result;
                }
                std::array<Interval, 8> boundedNodes{};
                std::array<Interval, 3> boundedLocal{}, boundedStep{};
                for (unsigned c = 0; c < 8; ++c)
                    boundedNodes[c] = GetInterval(nodes[c]);
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    boundedLocal[axis] = GetInterval(local[axis]);
                    boundedStep[axis] = GetInterval(step[axis]);
                }
                const auto fp =
                    GetLinePolynomial(boundedNodes, boundedLocal, boundedStep, Interval(threshold));
                const std::array<Interval, 4> bounds{fp[0], fp[0] + fp[1] / 3,
                                                     fp[0] + Interval(2) * fp[1] / 3 + fp[2] / 3,
                                                     fp[0] + fp[1] + fp[2] + fp[3]};
                const auto spanCount = detail.spans.size(), eventCount = detail.events.size(),
                           visited = detail.visited;
                if (!GetCertifiedRootIntervals(bounds, a, b, GetExactValue(tolerance) / L, detail, check))
                {
                    detail.spans.resize(spanCount);
                    detail.events.resize(eventCount);
                    detail.visited = visited;
                    const auto p = GetLinePolynomial(nodes, local, step, GetExactValue(threshold));
                    const Polynomial bernstein{p[0], p[0] + p[1] / 3, p[0] + 2 * p[1] / 3 + p[2] / 3,
                                               p[0] + p[1] + p[2] + p[3]};
                    IsolateRoots(bernstein, a, b, GetExactValue(tolerance) / L, detail, check);
                }
                ++result.cells;
            }
            auto &events = detail.events;
            std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
                return a.low < b.low || (a.low == b.low && a.high < b.high);
            });
            events.erase(std::unique(events.begin(), events.end(),
                                     [](const Event &a, const Event &b) {
                                         return a.low == b.low && a.high == b.high;
                                     }),
                         events.end());
            result.events = events.size();
            for (const auto &event : events)
            {
                result.maximumRootWidth =
                    std::max(result.maximumRootWidth, GetDoubleValue((event.high - event.low) * L));
                if (event.low == event.high && (event.low == begin || event.low == finish))
                    continue;
                int left = 0, right = 0;
                for (const auto &span : detail.spans)
                {
                    if (span.high <= event.low)
                        left = span.sign;
                    if (!right && span.low >= event.high)
                        right = span.sign;
                }
                if (!left || !right || left == right)
                    throw Unsupported("touch or unbounded root event");
            }
            bool material = false, ended = false;
            Q first = 0, last = 0;
            for (const auto &span : detail.spans)
            {
                if (span.sign > 0)
                {
                    if (ended)
                    {
                        result.status = Status::Invalid;
                        result.reason = "internal background";
                        return result;
                    }
                    if (!material)
                        first = span.low;
                    material = true;
                    last = span.high;
                }
                else if (material)
                    ended = true;
            }
            if (!material)
            {
                result.status = Status::Invalid;
                result.reason = "no material";
                return result;
            }
            const Q front = std::max(Q(0), first * L - E), back = std::max(Q(0), (1 - last) * L - E);
            result.trim = {GetDoubleValue(front), GetDoubleValue(back)};
            if ((last - first) * L <= 2 * E || front > GetExactValue(maximumTrim) + E ||
                back > GetExactValue(maximumTrim) + E)
            {
                result.status = Status::Invalid;
                result.reason = "endpoint support exceeds bound";
                return result;
            }
            result.status = Status::Valid;
            result.reason = "single continuous material interval";
        }
        catch (const Unsupported &e)
        {
            (void)e;
            result.status = Status::Unresolved;
            result.reason = "exact model unresolved or budget";
        }
        return result;
    }
};
} // namespace ThicknessMaterialField
