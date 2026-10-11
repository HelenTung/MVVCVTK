#pragma once
// CPU 测试参照/CUDA 共用的 outward 区间和 Q1 多项式运算。
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#ifdef __CUDACC__
#define WALL_HD __host__ __device__
#else
#define WALL_HD
#endif
namespace ThicknessMaterialField {
WALL_HD inline double Infinity() {return std::numeric_limits<double>::infinity();}
WALL_HD inline bool IsFinite(double v) {
#ifdef __CUDA_ARCH__
 return isfinite(v);
#else
 return std::isfinite(v);
#endif
}
WALL_HD inline double NextAfter(double x,double y) {
#ifdef __CUDA_ARCH__
 return nextafter(x,y);
#else
 return std::nextafter(x,y);
#endif
}
WALL_HD inline double Floor(double v) {return floor(v);}
WALL_HD inline double Ceil(double v) {return ceil(v);}
WALL_HD inline double Abs(double v) {return fabs(v);}
template<class T> WALL_HD inline T Minimum(T a,T b) {return b<a?b:a;}
template<class T> WALL_HD inline T Maximum(T a,T b) {return a<b?b:a;}
// 每个初等运算向外扩一 ULP；只有符号得到区间证明时才跳过有理数路径。
struct Interval
{
    double low = 0, high = 0;
    WALL_HD Interval() = default;
    WALL_HD Interval(double value) : low(value), high(value)
    {
    }
    WALL_HD Interval(double lo, double hi) : low(lo), high(hi)
    {
    }
};
WALL_HD inline Interval GetInterval(double lo, double hi)
{
    return {NextAfter(lo, -Infinity()),
            NextAfter(hi, Infinity())};
}
WALL_HD inline Interval operator+(Interval a, Interval b)
{
    return GetInterval(a.low + b.low, a.high + b.high);
}
WALL_HD inline Interval operator-(Interval a, Interval b)
{
    return GetInterval(a.low - b.high, a.high - b.low);
}
WALL_HD inline Interval operator-(Interval a)
{
    return {-a.high, -a.low};
}
WALL_HD inline Interval operator*(Interval a, Interval b)
{
    const std::array<double,4> p{a.low*b.low,a.low*b.high,a.high*b.low,a.high*b.high};
    double low=p[0],high=p[0];
    for (unsigned i=0;i<4;++i) {
        if (!IsFinite(p[i])) return {-Infinity(),Infinity()};
        if (p[i]<low) low=p[i]; if (p[i]>high) high=p[i];
    }
    return GetInterval(low,high);
}
WALL_HD inline Interval operator/(Interval a, double positive)
{
    return GetInterval(a.low / positive, a.high / positive);
}
WALL_HD inline Interval &operator+=(Interval &a, Interval b)
{
    a = a + b;
    return a;
}
WALL_HD inline int GetSign(Interval value)
{
    return value.low > 0 ? 1 : value.high < 0 ? -1 : 0;
}
template <class Number>
WALL_HD inline std::array<Number, 4> GetLinePolynomial(const std::array<Number, 8> &nodes,
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
            weight = next;
        }
        for (unsigned k = 0; k < 4; ++k)
            result[k] += nodes[corner] * weight[k];
    }
    return result;
}


}
#undef WALL_HD
