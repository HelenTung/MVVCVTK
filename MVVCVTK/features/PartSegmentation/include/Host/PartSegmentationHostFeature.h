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
