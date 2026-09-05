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

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
