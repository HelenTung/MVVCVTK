#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class vtkObject;
class vtkRenderer;
class vtkShaderProperty;

// 渲染输入身份与版本必须成对比较；identity 只作非拥有身份标记，
// version 由宿主数据真源提供，二者任一变化都代表旧 effect 不可重放。
struct RenderInputStamp final {
    const void* identity = nullptr;
    std::uint64_t version = 0;
};

inline bool operator==(
    const RenderInputStamp& left,
    const RenderInputStamp& right) noexcept
{
    return left.identity == right.identity && left.version == right.version;
}

inline bool operator!=(
    const RenderInputStamp& left,
    const RenderInputStamp& right) noexcept
{
    return !(left == right);
}

enum class RenderTargetKind {
    Unknown,
    Volume,
    PolyData,
    Slice
};

enum class RenderBindingUse {
    Current,
    Candidate
};

enum class RenderEffectStatus {
    Idle,
    Staged,
    Ready,
    Failed,
    Committed
};

enum class RenderEffectFailure {
    None,
    Unsupported,
    InputMismatch,
    ContextLost,
    CompileFailed,
    TextureFailed
};

struct RenderEffectState final {
    RenderEffectStatus status = RenderEffectStatus::Idle;
    RenderEffectFailure failureReason = RenderEffectFailure::None;
    std::uint64_t stagedRevision = 0;
    std::uint64_t activeRevision = 0;
    std::string message;
};

struct RenderEffectTarget final {
    RenderTargetKind targetKind = RenderTargetKind::Unknown;
    vtkObject* mapper = nullptr;
    vtkShaderProperty* shaderProperty = nullptr;
    RenderInputStamp inputStamp;
    std::array<double, 16> localToInput = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
};

// 一个 binding 只服务一个 mapper/context。context 或 window 替换时，
// Strategy 必须先停止并销毁旧 binding，再用新 target 构造全新 binding。
class RenderEffectBinding {
public:
    virtual ~RenderEffectBinding() = default;

    virtual RenderInputStamp GetInputStamp() const = 0;
    virtual RenderBindingUse GetBindingUse() const = 0;
    virtual RenderEffectState GetEffectState() const = 0;
    virtual bool SetBindingUse(RenderBindingUse bindingUse) = 0;
    virtual bool SetEffectCommit(std::uint64_t revision) = 0;
    virtual bool ClearEffectStage(std::uint64_t revision) = 0;
    virtual bool ResetEffect() = 0;
    virtual bool SetLocalToInput(
        const std::array<double, 16>& localToInput) = 0;
    virtual bool SetRenderInput(RenderInputStamp inputStamp) = 0;
    virtual bool OnRenderStart(vtkRenderer* renderer) = 0;
    virtual bool OnRenderStop() = 0;
};

// Feature 级 effect root 只负责按目标创建 binding；具体业务参数和事务
// 留在 Feature 自己的派生类型中，主体 Render 不认识任何可选 Feature。
class RenderEffect {
public:
    virtual ~RenderEffect() = default;

    virtual std::shared_ptr<RenderEffectBinding> BuildEffectBinding(
        const RenderEffectTarget& target,
        RenderBindingUse bindingUse) = 0;
};
