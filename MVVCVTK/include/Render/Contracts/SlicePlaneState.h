#pragma once
#include "App/ViewTypes.h"
#include <array>

// vtkImageResliceMapper 的 SlicePlane 位于世界空间，image direction 不改变该法线。
// 同一 cursor 快照构造主图与标签平面；输入几何由 mapper 的 world-to-data 消费。
struct SlicePlaneState final {
    std::array<double, 3> worldOrigin{};
    std::array<double, 3> worldNormal{};
    static SlicePlaneState Build(Orientation orientation,
        const std::array<double, 3>& cursor, double offset = 0)
    {
        SlicePlaneState state;
        const int axis = orientation == Orientation::Top_down ? 2
            : orientation == Orientation::Front_back ? 1 : 0;
        state.worldOrigin = cursor;
        state.worldNormal[axis] = 1;
        state.worldOrigin[axis] += offset;
        return state;
    }
};
