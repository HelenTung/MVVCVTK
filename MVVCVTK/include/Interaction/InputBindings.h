#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    WheelBackward,
    Modifiers
};

enum class InputMouseButton : std::uint8_t {
    None,
    Primary,
    Secondary,
    Middle
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
    std::uint32_t key = 0; // KeyPress 使用 SDK 键码；字符键填写 Unicode/ASCII 值。
    bool allowsRepeat = false;
};

// 功能键由 SDK 定义；适配层负责转换，字符键使用 Unicode/ASCII 值。
enum class InputKey : std::uint32_t {
    Escape = 27, Enter = 13, Tab = 9, Backspace = 8,
    Delete = 0x110000, Left, Right, Up, Down, Home, End, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};
enum class InputArea : std::uint8_t { View, Text, Modal, Application };
enum class InteractionViewClass : std::uint8_t { Any, Slice, ThreeD };
enum class BindingOrigin : std::uint8_t { Default, Session, ViewClass, View };
struct BindingScope final {
    std::string viewId;
    InteractionViewClass viewClass = InteractionViewClass::Any;
    InputArea area = InputArea::View;
    static BindingScope ViewArea() { return {}; }
    static BindingScope View(std::string id) { BindingScope s; s.viewId=std::move(id); return s; }
    static BindingScope Slices() { BindingScope s; s.viewClass=InteractionViewClass::Slice; return s; }
};
struct BindingChange final {
    std::string bindingKey;
    InputBindingOverride choice = InputBindingOverride::UseDefault;
    InputBinding binding;
    BindingScope scope;
};
struct BindingPatch final {
    std::vector<BindingChange> changes;
    void Replace(std::string key, InputBinding value, BindingScope scope = {}) {
        changes.push_back({std::move(key),InputBindingOverride::Replace,value,std::move(scope)});
    }
    void Disable(std::string key, BindingScope scope = {}) {
        changes.push_back({std::move(key),InputBindingOverride::Disable,{},std::move(scope)});
    }
    void UseDefault(std::string key, BindingScope scope = {}) {
        changes.push_back({std::move(key),InputBindingOverride::UseDefault,{},std::move(scope)});
    }
};

enum class InputBindingStatus : std::uint8_t {
    Applied,
    Invalid,
    Conflict,
    Unsupported,
    Busy,
    Failed
};

struct BindingDiagnostic final {
    std::string bindingKey;
    std::string conflictingKey;
    std::string viewId;
    InputBindingStatus status = InputBindingStatus::Invalid;
};
struct BindingUpdateResult final {
    InputBindingStatus status = InputBindingStatus::Failed;
    std::vector<BindingDiagnostic> diagnostics;
};
struct EffectiveBinding final {
    std::string bindingKey;
    std::string actionId;
    std::string ownerId;
    InputBinding binding;
    BindingOrigin origin = BindingOrigin::Default;
    bool isEnabled = false;
    bool isAvailable = false;
    bool hasConflict = false;
    std::string viewId;
    InputBindingStatus status=InputBindingStatus::Applied;
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
class ConfigurableInputBindings : public IInputBindings {
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
inline constexpr std::string_view ZoomWheelForward = "navigation.zoom.wheel.forward";
inline constexpr std::string_view ZoomWheelBackward = "navigation.zoom.wheel.backward";
inline constexpr std::string_view PanDrag = "navigation.pan.drag";
inline constexpr std::string_view CameraRotateDrag = "navigation.camera.rotate.drag";
inline constexpr std::string_view CameraPanDrag = "navigation.camera.pan.drag";
inline constexpr std::string_view CameraZoomDrag = "navigation.camera.zoom.drag";
inline constexpr std::string_view CameraZoomForward = "navigation.camera.zoom.forward";
inline constexpr std::string_view CameraZoomBackward = "navigation.camera.zoom.backward";
inline constexpr std::string_view CameraPanShift = "navigation.camera.pan.shift";
inline constexpr std::string_view CameraRollCtrl = "navigation.camera.roll.ctrl";
inline constexpr std::string_view CameraZoomShiftCtrl = "navigation.camera.zoom.shiftCtrl";
inline constexpr std::string_view Precision = "navigation.precision.modifiers";
inline constexpr std::string_view Accelerated = "navigation.accelerated.modifiers";
inline constexpr std::string_view PlaneDrag = "navigation.plane.drag";
inline constexpr std::string_view ModelPanDrag = "navigation.modelPan.drag";
inline constexpr std::string_view ModelScaleDrag = "navigation.modelScale.drag";
inline constexpr std::string_view ModelSecondaryPanDrag =
    "navigation.modelSecondaryPan.drag";
}
