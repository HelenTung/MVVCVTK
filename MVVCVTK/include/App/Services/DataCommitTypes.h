#pragma once

#include "App/AppTypes.h"

#include <array>

// DataManager 最终发布成功后一次写入共享状态的值快照。
// View 候选阶段只计算本值，不得提前修改 SharedInteractionState。
struct DataReadyState final {
    DataRevisionRef dataRevision;
    DataBindingRevision bindingRevision = 0;
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> cursorWorld = { 0.0, 0.0, 0.0 };
};
