#pragma once

#include "Model/PartCatalog.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

struct PartRenderState final {
    std::array<double, 4> color{};
    bool isSelected = false;
};

inline bool operator==(
    const PartRenderState& left,
    const PartRenderState& right)
{
    return left.color == right.color
        && left.isSelected == right.isSelected;
}

struct PartRenderStateTable final {
    std::vector<PartRenderState> statesByLabel;
};

inline bool operator==(
    const PartRenderStateTable& left,
    const PartRenderStateTable& right)
{
    return left.statesByLabel == right.statesByLabel;
}

class PartOverlayControl {
public:
    virtual ~PartOverlayControl() noexcept = default;
    virtual bool SetPartStates(
        const PartRenderStateTable& states) noexcept = 0;
};

std::optional<PartRenderStateTable> BuildPartRenderStateTable(
    const PartCatalog& catalog);
bool SetPartStates(
    const std::vector<std::shared_ptr<PartOverlayControl>>& controls,
    const PartRenderStateTable& next,
    const PartRenderStateTable& previous) noexcept;
