#pragma once
#include "ThicknessProfile.h"
#include "ThicknessPositiveReference.h"
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
#include <optional>
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
inline Interval GetInterval(const Q &value)
{
    const double v = GetDoubleValue(value);
    return GetInterval(v, v);
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
    struct UniformRegion
    {
        // 包含整块所有单元角点的节点范围；零符号表示无证书。
        std::array<int, 6> extent{};
        int sign = 0;
    };
    std::array<int, 6> extent{};
    double threshold = 0;
    std::function<bool(const Index &, double &)> node;
    std::function<void()> check = [] {};
    std::function<UniformRegion(const Index &)> uniformRegion;

    // 相同连续材料证书的区间表示。只有整段严格正且旧预算必然可完成时接受；
    // 面交点次序或符号无法证明时，交回下面的精确参数/根隔离，不猜测、不改容差。
    bool GetPositivePath(const Point& from,const Point& delta,double length,double epsilon,Result& result) const
    {
        std::size_t pieces=0;
        if (!GetPositiveCertificate(*this,from,delta,length,epsilon,pieces)) return false;
        result.status=Status::Valid;result.trim={0,0};result.cells=pieces;
        result.reason="single continuous material interval";return true;
    }

    UniformRegion GetUniformRegion(const Index &cell) const
    {
        if (!uniformRegion) return {};
        const auto region = uniformRegion(cell);
        if (region.sign != 1 && region.sign != -1 && region.sign != 0) return {};
        for (unsigned a = 0; a < 3; ++a)
            if (region.extent[2*a] >= region.extent[2*a+1]
                || region.extent[2*a] < extent[2*a] || region.extent[2*a+1] > extent[2*a+1]
                || cell[a] < region.extent[2*a] || cell[a] >= region.extent[2*a+1]) return {};
        return region;
    }

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
        ThicknessProfile::Increment(ThicknessProfile::PathCalls);
        ThicknessProfile::Scope profile(ThicknessProfile::PathTime);
        Result result;
        try
        {
            if (!std::isfinite(length) || !std::isfinite(epsilon) || !std::isfinite(maximumTrim) ||
                !std::isfinite(tolerance) || epsilon <= 0 || length <= 4 * epsilon || maximumTrim < 0 ||
                tolerance <= 0)
                throw Unsupported("invalid path input");
            if (GetPositivePath(from,delta,length,epsilon,result)) return result;
            QPoint start{}, difference{}, end{};
            const Q L = GetExactValue(length), E = GetExactValue(epsilon), begin = E / L, finish = 1 - begin;
            std::vector<Q> cuts{begin, finish};
            std::vector<int> uniformSigns;
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
                if (!uniformRegion && difference[a] != 0)
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
            if (uniformRegion)
            {
                // 以精确参数和块面交点切分；不借用射线的 double 步进结果作支撑证明。
                cuts.clear(); cuts.push_back(begin);
                Q current = begin;
                while (current < finish)
                {
                    check();
                    Index cell{};
                    for (unsigned a = 0; a < 3; ++a)
                    {
                        const Q coordinate = start[a] + current * difference[a];
                        cell[a] = GetFloor(coordinate);
                        if (difference[a] < 0 && coordinate == Q(cell[a])) --cell[a];
                        cell[a] = std::clamp<std::int64_t>(cell[a], extent[2*a], extent[2*a+1]-1);
                    }
                    const auto region = GetUniformRegion(cell);
                    const bool hasRegion = region.extent[0] < region.extent[1];
                    Q next = finish;
                    for (unsigned a = 0; a < 3; ++a)
                    {
                        if (difference[a] == 0) continue;
                        const auto face = hasRegion ? region.extent[2*a+(difference[a]>0?1:0)]
                            : cell[a] + (difference[a]>0?1:0);
                        const Q crossing = (Q(face)-start[a])/difference[a];
                        if (crossing <= current) throw Unsupported("nonforward support step");
                        next = std::min(next,crossing);
                    }
                    if (next <= current) throw Unsupported("nonforward support step");
                    if (hasRegion && !region.sign)
                    {
                        // 混合块沿用整数单元面的原切分，不逐单元重复构造精确 DDA。
                        std::vector<Q> mixedCuts{current,next};
                        for (unsigned a = 0; a < 3; ++a)
                        {
                            if (difference[a] == 0) continue;
                            const Q p = start[a]+current*difference[a], q = start[a]+next*difference[a];
                            const auto lo = GetFloor(std::min(p,q))+1, hi = GetFloor(std::max(p,q));
                            if (hi-lo > 10000) throw Unsupported("cell budget");
                            for (auto face = lo; face <= hi; ++face)
                            {
                                const Q crossing = (Q(face)-start[a])/difference[a];
                                if (crossing>current && crossing<next) mixedCuts.push_back(crossing);
                            }
                        }
                        std::sort(mixedCuts.begin(),mixedCuts.end());
                        mixedCuts.erase(std::unique(mixedCuts.begin(),mixedCuts.end()),mixedCuts.end());
                        if (mixedCuts.size()-1 > 10000-cuts.size()) throw Unsupported("cell budget");
                        for (std::size_t k=1;k<mixedCuts.size();++k)
                        {
                            cuts.push_back(mixedCuts[k]);uniformSigns.push_back(0);
                        }
                    }
                    else
                    {
                        if (cuts.size() >= 10000) throw Unsupported("cell budget");
                        cuts.push_back(next);uniformSigns.push_back(region.sign);
                    }
                    current = next;
                }
            }
            else
            {
                std::sort(cuts.begin(), cuts.end());
                cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            }
            if (cuts.size() > 10000)
                throw Unsupported("cell budget");
            Detail detail;
            std::optional<Q> rootTolerance;
            for (std::size_t k = 1; k < cuts.size(); ++k)
            {
                ThicknessProfile::Increment(ThicknessProfile::PathCells);
                check();
                const Q a = cuts[k - 1], b = cuts[k], mid = (a + b) / 2;
                if (!uniformSigns.empty() && uniformSigns[k-1])
                {
                    ThicknessProfile::Increment(ThicknessProfile::PathBlocks);
                    detail.spans.push_back({a,b,uniformSigns[k-1]});
                    ++result.cells;
                    continue;
                }
                Index cell{};
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    cell[axis] = std::min<std::int64_t>(GetFloor(start[axis] + mid * difference[axis]),
                                                        extent[2 * axis + 1] - 1);
                }
                std::array<double, 8> rawNodes{};
                // 单元八角严格同号时，三线性凸组合的整段符号已确定。
                // 保持精确 cuts/材料区间合同，避免为纯材料单元构造大整数。
                int uniformSign = 0;
                bool uniform = true;
                for (unsigned corner = 0; corner < 8; ++corner) {
                    auto index = cell;
                    for (unsigned axis = 0; axis < 3; ++axis) index[axis] += (corner >> axis) & 1U;
                    double value = 0;
                    if (!node(index, value) || !std::isfinite(value)) {
                        result.status=Status::Missing;result.reason="missing node";return result;
                    }
                    rawNodes[corner]=value;
                    const int sign = value > threshold ? 1 : value < threshold ? -1 : 0;
                    if (!sign || (uniformSign && sign != uniformSign)) uniform = false;
                    uniformSign = sign;
                }
                if (uniform && uniformSign) {
                    detail.spans.push_back({a,b,uniformSign});
                    ++result.cells;
                    continue;
                }
                QPoint local{}, step{};
                for (unsigned axis=0;axis<3;++axis) {
                    local[axis]=start[axis]+a*difference[axis]-cell[axis];
                    step[axis]=(b-a)*difference[axis];
                }
                ThicknessProfile::Increment(ThicknessProfile::PathMixedCells);
                std::array<Interval, 8> boundedNodes{};
                std::array<Interval, 3> boundedLocal{}, boundedStep{};
                for (unsigned c = 0; c < 8; ++c)
                    // 原始 binary64 经 Q 往返仍是同一个数；保留相同的一 ULP 包围，省去大整数转换。
                    boundedNodes[c] = GetInterval(rawNodes[c],rawNodes[c]);
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
                if (!rootTolerance) rootTolerance=GetExactValue(tolerance)/L;
                if (!GetCertifiedRootIntervals(bounds, a, b, *rootTolerance, detail, check))
                {
                    ThicknessProfile::Increment(ThicknessProfile::ExactFallbacks);
                    detail.spans.resize(spanCount);
                    detail.events.resize(eventCount);
                    detail.visited = visited;
                    std::array<Q,8> nodes;
                    for (unsigned c=0;c<8;++c) nodes[c]=GetExactValue(rawNodes[c]);
                    const auto p = GetLinePolynomial(nodes, local, step, GetExactValue(threshold));
                    const Polynomial bernstein{p[0], p[0] + p[1] / 3, p[0] + 2 * p[1] / 3 + p[2] / 3,
                                               p[0] + p[1] + p[2] + p[3]};
                    IsolateRoots(bernstein, a, b, *rootTolerance, detail, check);
                }
                else ThicknessProfile::Increment(ThicknessProfile::IntervalCertificates);
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
