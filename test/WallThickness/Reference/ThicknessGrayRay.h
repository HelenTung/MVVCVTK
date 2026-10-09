#pragma once

#include "ThicknessMaterialField.h"

namespace ThicknessGrayRay {
struct Sample final {
    double value = 0;
    ThicknessMaterialField::Point gradient{};
};
enum class Status { Found, MissingSupport, NoBoundary, Ambiguous };
struct Result final {
    Status status = Status::Ambiguous;
    double entry = 0, exit = 0, observedUntil = 0;
    bool hasEntry = false;
};
bool GetSample(const ThicknessMaterialField::Field& field,
               const ThicknessMaterialField::Point& point, Sample& result);
// direction 为每单位物理长度的索引位移。只返回相邻的入口/首次出口，不跨过孔隙。
Result Trace(const ThicknessMaterialField::Field& field,
             const ThicknessMaterialField::Point& point,
             const ThicknessMaterialField::Point& direction,
             double maxDistance, double entryAllowance, double tolerance,
             bool allowClippedEntrySupport);
}
