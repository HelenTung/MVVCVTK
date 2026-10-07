#pragma once

#include "Interaction/InputBindings.h"
#include "Interaction/InteractionTypes.h"

#include <array>
#include <cstddef>

enum class NavigationAction : std::size_t {
    SliceForward,
    SliceBackward,
    CrosshairDrag,
    WindowLevelDrag,
    ZoomDrag,
    PlaneDrag,
    ModelPanDrag,
    ModelScaleDrag,
    ModelSecondaryPanDrag,
    Count
};

struct NavigationBinding final {
    InputBinding input;
    bool isEnabled = true;
};

// 配置阶段生成值快照；运行期匹配不访问外部提供方。
class NavigationBindings final {
public:
    NavigationBindings() noexcept;

    static InputBindingStatus Build(
        const IInputBindings& source,
        NavigationBindings& out) noexcept;

    bool GetMatched(
        NavigationAction action,
        const InteractionEvent& event) const noexcept;

    bool GetIsEnabled(NavigationAction action) const noexcept;

private:
    std::array<NavigationBinding,
        static_cast<std::size_t>(NavigationAction::Count)> m_rules;
};
