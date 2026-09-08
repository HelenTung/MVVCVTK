#pragma once
#include "Host/HostFeature.h"

struct RoiEditingConfig final {
    HostViewTarget referenceView;
    HostViewTargets targetViews;
};
enum class RoiEditingAction : std::uint8_t { Begin, SetDraft, Commit, Cancel, SetVisible };
struct RoiEditingRequest final {
    RoiEditingAction action = RoiEditingAction::Begin;
    std::optional<RoiRequest> draft;
    std::optional<std::array<double,16>> boxToSource;
    std::optional<bool> isVisible;
};
struct RoiEditingState final {
    bool isAttached = false;
    bool hasDraft = false;
    bool isVisible = true;
    bool isDragging = false;
    RoiError error = RoiError::None;
    std::optional<RoiDefinition> draft;
    std::optional<DataRevisionRef> committedRoi;
};
// 仓内可选编辑器；请求、VTK 交互和正常 Detach 均在 Host owner thread。
// 草稿不是正式数据，只有 Commit 调用 Host ROI 事务；应用负责组合本 Feature。
class RoiEditingHostFeature final : public HostFeature {
public:
    explicit RoiEditingHostFeature(RoiEditingConfig config);
    ~RoiEditingHostFeature() noexcept override;
    RoiEditingHostFeature(const RoiEditingHostFeature&) = delete;
    RoiEditingHostFeature& operator=(const RoiEditingHostFeature&) = delete;
    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;
    RoiResult SendRequest(const RoiEditingRequest& request);
    RoiEditingState GetState() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
