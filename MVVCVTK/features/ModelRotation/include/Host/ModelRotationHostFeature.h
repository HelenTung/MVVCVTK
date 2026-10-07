#pragma once
#include "Host/HostFeature.h"
#include "Host/ModelRotationHostTypes.h"
#include "Interaction/InputBindings.h"
#include <memory>

namespace ModelRotationBindingKeys {
inline constexpr std::string_view EnabledDrag = "modelRotation.enabled.drag";
inline constexpr std::string_view SliceShortcutDrag =
    "modelRotation.sliceShortcut.drag";
inline constexpr std::string_view CancelKey = "modelRotation.cancel.key";
}

class ModelRotationHostFeature final : public HostFeature,
    public std::enable_shared_from_this<ModelRotationHostFeature> {
public:
    explicit ModelRotationHostFeature(ModelRotationConfig config = {});
    ~ModelRotationHostFeature() noexcept override;
    ModelRotationHostFeature(const ModelRotationHostFeature&) = delete;
    ModelRotationHostFeature& operator=(const ModelRotationHostFeature&) = delete;
    ModelRotationHostFeature(ModelRotationHostFeature&&) = delete;
    ModelRotationHostFeature& operator=(ModelRotationHostFeature&&) = delete;
    std::string_view GetFeatureId() const noexcept override;
    bool AttachHost(const HostFeatureContext& context) override;
    InputBindingStatus SetInputBindings(const IInputBindings& bindings);
    bool DetachHost() override;
    bool OnHostTick() override;
    // true 表示已接纳；GetState().status 区分等待帧提交与已完成。
    bool SendRequest(const ModelRotationRequest& request);
    ModelRotationState GetState() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
