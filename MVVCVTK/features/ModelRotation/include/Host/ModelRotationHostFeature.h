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
    bool DetachHost() override;
    bool OnHostTick() override;
    // 拒绝不回调；Accepted 恰好一次 owner 回调。旋转/撤销成功在帧提交后兑现，失败或短命令可同步完成。
    // 回调可查询、再次请求或取消；Detach/Stop 仍遵循 Session 的拓扑与停止契约。
    ModelRotationAdmission SendRequest(const ModelRotationRequest& request,
        ModelRotationCallback onComplete = {});
    ModelRotationState GetState() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
