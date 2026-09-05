#pragma once
#include "Host/Types/HostViewTypes.h"
#include <array>
#include <cstddef>
#include <optional>

struct ModelRotationConfig final {
    HostViewTargets targetViews{ {}, { HostRenderViewRole::Primary3D,
        HostRenderViewRole::Composite3D, HostRenderViewRole::TopDownSlice,
        HostRenderViewRole::FrontBackSlice, HostRenderViewRole::LeftRightSlice } };
};

enum class ModelRotationAction { SetEnabled, Rotate, Cancel, Undo };
struct ModelRotationRequest final {
    ModelRotationAction action = ModelRotationAction::Rotate;
    bool isEnabled = true;
    std::array<double, 3> worldAxis{ 0,0,1 };
    double angleDeg = 0;
    std::optional<std::array<double, 3>> worldCenter;
};

enum class ModelRotationStatus { Detached, Idle, Dragging, Pending, Succeeded, Cancelled, Invalidated };
struct ModelRotationState final {
    ModelRotationStatus status = ModelRotationStatus::Detached;
    bool isEnabled = false;
    std::size_t undoCount = 0;
};
