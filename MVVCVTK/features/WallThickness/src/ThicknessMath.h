#pragma once
#include "Host/WallThicknessHostTypes.h"
#include <algorithm>
#include <cmath>

namespace ThicknessMath
{
inline ThicknessPoint Add(const ThicknessPoint &a, const ThicknessPoint &b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline ThicknessPoint Sub(const ThicknessPoint &a, const ThicknessPoint &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline ThicknessPoint Scale(const ThicknessPoint &a, double b)
{
    return {a[0] * b, a[1] * b, a[2] * b};
}
inline double Dot(const ThicknessPoint &a, const ThicknessPoint &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline ThicknessPoint Cross(const ThicknessPoint &a, const ThicknessPoint &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double Length(const ThicknessPoint &a)
{
    return std::hypot(a[0], a[1], a[2]);
}
inline ThicknessPoint Unit(const ThicknessPoint &a)
{
    const double length = Length(a);
    return length > 0 && std::isfinite(length) ? Scale(a, 1.0 / length) : ThicknessPoint{};
}
inline bool Finite(const ThicknessPoint &a)
{
    return std::all_of(a.begin(), a.end(), [](double v) { return std::isfinite(v); });
}
} // namespace ThicknessMath
