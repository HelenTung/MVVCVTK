#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

// 操作标识由 SDK 或所属 Feature 声明；提供方只决定触发方式。
enum class InputBindingOverride : std::uint8_t {
    UseDefault,
    Replace,
    Disable
};

enum class InputTriggerKind : std::uint8_t {
    KeyPress,
    PointerPress,
    Drag,
    WheelForward,
    WheelBackward
};

enum class InputMouseButton : std::uint8_t {
    None,
    Primary,
    Secondary
};

enum class InputModifierFlags : std::uint8_t {
    None = 0,
    Shift = 1,
    Ctrl = 2,
    Alt = 4
};

struct InputBinding final {
    InputTriggerKind trigger = InputTriggerKind::Drag;
    InputMouseButton button = InputMouseButton::Primary;
    std::uint8_t requiredModifiers = 0;
    std::uint8_t forbiddenModifiers = 0;
    std::uint32_t key = 0; // KeyPress 使用 SDK 键码；当前支持 ASCII 与 Escape。
};

enum class InputBindingStatus : std::uint8_t {
    Applied,
    Invalid,
    Conflict,
    Unsupported,
    Busy,
    Failed
};

class IInputBindings {
public:
    virtual ~IInputBindings() = default;

    // 仅在 Replace 时读取 out；调用方仅在配置阶段读取并保存值快照。
    virtual InputBindingOverride GetOverride(
        std::string_view bindingKey,
        InputBinding& out) const noexcept = 0;
};

// 普通上位机可直接配置值；模块在应用时负责检查冲突和支持范围。
class ConfigurableInputBindings final : public IInputBindings {
public:
    bool SetOverride(
        std::string bindingKey,
        InputBindingOverride choice,
        InputBinding binding = {})
    {
        if (bindingKey.empty()) return false;
        switch (choice) {
        case InputBindingOverride::UseDefault:
            m_overrides.erase(bindingKey);
            return true;
        case InputBindingOverride::Replace:
        case InputBindingOverride::Disable:
            m_overrides.insert_or_assign(
                std::move(bindingKey), Entry{ choice, binding });
            return true;
        }
        return false;
    }

    InputBindingOverride GetOverride(
        std::string_view bindingKey,
        InputBinding& out) const noexcept override
    {
        const auto found = m_overrides.find(bindingKey);
        if (found == m_overrides.end()) {
            return InputBindingOverride::UseDefault;
        }
        if (found->second.choice == InputBindingOverride::Replace) {
            out = found->second.binding;
        }
        return found->second.choice;
    }

private:
    struct Entry final {
        InputBindingOverride choice;
        InputBinding binding;
    };
    std::map<std::string, Entry, std::less<>> m_overrides;
};

namespace NavigationBindingKeys {
inline constexpr std::string_view SliceForward = "navigation.slice.forward";
inline constexpr std::string_view SliceBackward = "navigation.slice.backward";
inline constexpr std::string_view CrosshairDrag = "navigation.crosshair.drag";
inline constexpr std::string_view WindowLevelDrag = "navigation.windowLevel.drag";
inline constexpr std::string_view ZoomDrag = "navigation.zoom.drag";
inline constexpr std::string_view PlaneDrag = "navigation.plane.drag";
inline constexpr std::string_view ModelPanDrag = "navigation.modelPan.drag";
inline constexpr std::string_view ModelScaleDrag = "navigation.modelScale.drag";
inline constexpr std::string_view ModelSecondaryPanDrag =
    "navigation.modelSecondaryPan.drag";
}
