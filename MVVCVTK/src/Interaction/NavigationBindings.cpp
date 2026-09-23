#include "Interaction/NavigationBindings.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace {
using Action = NavigationAction;
constexpr std::size_t count = static_cast<std::size_t>(Action::Count);
constexpr std::uint8_t shift = static_cast<std::uint8_t>(InputModifierFlags::Shift);
constexpr std::uint8_t ctrl = static_cast<std::uint8_t>(InputModifierFlags::Ctrl);
constexpr std::uint8_t alt = static_cast<std::uint8_t>(InputModifierFlags::Alt);
constexpr std::uint8_t allModifiers = shift | ctrl | alt;

constexpr std::array<std::string_view, count> keys{
    NavigationBindingKeys::SliceForward,
    NavigationBindingKeys::SliceBackward,
    NavigationBindingKeys::CrosshairDrag,
    NavigationBindingKeys::WindowLevelDrag,
    NavigationBindingKeys::ZoomDrag,
    NavigationBindingKeys::PlaneDrag,
    NavigationBindingKeys::ModelPanDrag,
    NavigationBindingKeys::ModelScaleDrag,
    NavigationBindingKeys::ModelSecondaryPanDrag
};

bool GetValid(
    const NavigationAction action,
    const InputBinding& binding) noexcept
{
    if ((binding.requiredModifiers | binding.forbiddenModifiers)
            & ~allModifiers
        || (binding.requiredModifiers & binding.forbiddenModifiers)
        || binding.key != 0) {
        return false;
    }
    const bool isWheel = action == Action::SliceForward
        || action == Action::SliceBackward;
    if (isWheel) {
        return binding.button == InputMouseButton::None
            && (binding.trigger == InputTriggerKind::WheelForward
                || binding.trigger == InputTriggerKind::WheelBackward);
    }
    return binding.trigger == InputTriggerKind::Drag
        && (binding.button == InputMouseButton::Primary
            || binding.button == InputMouseButton::Secondary);
}

bool GetOverlap(
    const NavigationBinding& first,
    const NavigationBinding& second) noexcept
{
    if (!first.isEnabled || !second.isEnabled) return false;
    const auto& a = first.input;
    const auto& b = second.input;
    return a.trigger == b.trigger && a.button == b.button
        && !(a.requiredModifiers & b.forbiddenModifiers)
        && !(b.requiredModifiers & a.forbiddenModifiers);
}

bool GetGroupConflict(
    const std::array<NavigationBinding, count>& rules,
    const std::size_t first,
    const std::size_t end) noexcept
{
    for (std::size_t i = first; i < end; ++i) {
        for (std::size_t j = i + 1; j < end; ++j) {
            if (GetOverlap(rules[i], rules[j])) return true;
        }
    }
    return false;
}

std::uint8_t GetModifiers(const InteractionEvent& event) noexcept
{
    return (event.isShiftDown ? shift : 0)
        | (event.isCtrlDown ? ctrl : 0)
        | (event.isAltDown ? alt : 0);
}
} // namespace

NavigationBindings::NavigationBindings() noexcept
{
    m_rules[0].input = { InputTriggerKind::WheelForward,
        InputMouseButton::None, 0, 0, 0 };
    m_rules[1].input = { InputTriggerKind::WheelBackward,
        InputMouseButton::None, 0, 0, 0 };
    m_rules[2].input = { InputTriggerKind::Drag,
        InputMouseButton::Primary, shift, ctrl, 0 };
    m_rules[3].input = { InputTriggerKind::Drag,
        InputMouseButton::Primary, 0,
        static_cast<std::uint8_t>(shift | ctrl), 0 };
    m_rules[4].input = { InputTriggerKind::Drag,
        InputMouseButton::Secondary, 0, 0, 0 };
    m_rules[5].input = { InputTriggerKind::Drag,
        InputMouseButton::Primary, 0, 0, 0 };
    m_rules[6].input = { InputTriggerKind::Drag,
        InputMouseButton::Primary, shift, ctrl, 0 };
    m_rules[7].input = { InputTriggerKind::Drag,
        InputMouseButton::Primary,
        static_cast<std::uint8_t>(shift | ctrl), 0, 0 };
    m_rules[8].input = { InputTriggerKind::Drag,
        InputMouseButton::Secondary, 0, 0, 0 };
}

InputBindingStatus NavigationBindings::Build(
    const IInputBindings& source,
    NavigationBindings& out) noexcept
{
    NavigationBindings candidate;
    for (std::size_t i = 0; i < count; ++i) {
        InputBinding replacement;
        const auto choice = source.GetOverride(keys[i], replacement);
        switch (choice) {
        case InputBindingOverride::UseDefault:
            break;
        case InputBindingOverride::Replace:
            if (!GetValid(static_cast<Action>(i), replacement))
                return InputBindingStatus::Unsupported;
            candidate.m_rules[i].input = replacement;
            break;
        case InputBindingOverride::Disable:
            candidate.m_rules[i].isEnabled = false;
            break;
        default:
            return InputBindingStatus::Invalid;
        }
    }
    if (GetGroupConflict(candidate.m_rules, 0, 2)
        || GetGroupConflict(candidate.m_rules, 2, 5)
        || GetGroupConflict(candidate.m_rules, 6, 9)) {
        return InputBindingStatus::Conflict;
    }

    // Slice 视图的 Ctrl+普通左键留给 ModelRotation Feature。
    for (std::size_t i = 2; i < 5; ++i) {
        const auto& rule = candidate.m_rules[i];
        if (rule.isEnabled
            && rule.input.button == InputMouseButton::Primary
            && !(rule.input.forbiddenModifiers & ctrl)
            && !(rule.input.requiredModifiers & (shift | alt))) {
            return InputBindingStatus::Conflict;
        }
    }
    // ModelTransform 中的普通左键留给已装配的旋转 Feature。
    for (std::size_t i = 6; i < 9; ++i) {
        const auto& rule = candidate.m_rules[i];
        if (rule.isEnabled
            && rule.input.button == InputMouseButton::Primary
            && rule.input.requiredModifiers == 0) {
            return InputBindingStatus::Conflict;
        }
    }
    out = candidate;
    return InputBindingStatus::Applied;
}

bool NavigationBindings::GetMatched(
    const NavigationAction action,
    const InteractionEvent& event) const noexcept
{
    const auto index = static_cast<std::size_t>(action);
    if (index >= count || !m_rules[index].isEnabled) return false;
    const auto& input = m_rules[index].input;
    switch (input.trigger) {
    case InputTriggerKind::Drag:
        if ((input.button == InputMouseButton::Primary
                && event.eventKind != InteractionEventKind::PrimaryPress)
            || (input.button == InputMouseButton::Secondary
                && event.eventKind != InteractionEventKind::SecondaryPress)) {
            return false;
        }
        break;
    case InputTriggerKind::WheelForward:
        if (event.eventKind != InteractionEventKind::WheelForward)
            return false;
        break;
    case InputTriggerKind::WheelBackward:
        if (event.eventKind != InteractionEventKind::WheelBackward)
            return false;
        break;
    default:
        return false;
    }
    const auto modifiers = GetModifiers(event);
    return (modifiers & input.requiredModifiers) == input.requiredModifiers
        && !(modifiers & input.forbiddenModifiers);
}

bool NavigationBindings::GetIsEnabled(const NavigationAction action) const noexcept
{
    const auto index = static_cast<std::size_t>(action);
    return index < count && m_rules[index].isEnabled;
}
