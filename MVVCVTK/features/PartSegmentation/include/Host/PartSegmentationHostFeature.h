#pragma once

#include "Host/HostFeature.h"
#include "Host/PartSegmentationHostTypes.h"

#include <memory>
#include <string_view>

class PartSegmentationHostFeature final
    : public HostFeature
    , public std::enable_shared_from_this<PartSegmentationHostFeature> {
public:
    explicit PartSegmentationHostFeature(
        PartSegmentationConfig config = {});
    ~PartSegmentationHostFeature() noexcept override;

    PartSegmentationHostFeature(
        const PartSegmentationHostFeature&) = delete;
    PartSegmentationHostFeature& operator=(
        const PartSegmentationHostFeature&) = delete;
    PartSegmentationHostFeature(
        PartSegmentationHostFeature&&) = delete;
    PartSegmentationHostFeature& operator=(
        PartSegmentationHostFeature&&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    PartSegmentationAdmission SendRequest(
        PartSegmentationRequest request,
        PartSegmentationCallback onComplete = nullptr);
    PartSegmentationState GetState() const;
    // owner thread：Accepted 后由 OnHostTick/owner dispatcher 恰好完成一次。
    // 计算只产生 PreviewReady；确认才发布正式标签和目录。拒绝不保留 callback。
    PartSegmentationAdmission SendEditRequest(
        PartEditRequest request,
        PartSegmentationCallback onComplete = nullptr);
    std::shared_ptr<const PartEditPreview> GetEditPreview() const;
    // 确认结果可能在本调用返回前回调；回调前已完成状态转换，可再次请求/Detach。
    // 失败消费当前 previewId，但不改变正式数据或 Undo/Redo 游标。
    PartSegmentationAdmission SetEditCommit(
        std::uint64_t previewId,
        PartSegmentationCallback onComplete = nullptr);
    PartMutationResult ClearEditPreview(std::uint64_t previewId);
    std::shared_ptr<const PartSetSnapshot> GetPartSetSnapshot() const;
    PartMutationResult SetPartState(
        const PartBindingRef& part,
        const PartStatePatch& patch,
        std::uint64_t expectedCatalogRevision);
    // 按快照目录顺序选择上一部件；首项回绕到末项，无选中项时选择末项。
    // expectedCatalogRevision 必须与当前目录一致，返回状态沿用 SetPartState。
    PartMutationResult SetPreviousPart(std::uint64_t expectedCatalogRevision);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
