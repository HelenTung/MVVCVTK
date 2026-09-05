#pragma once

#include "Host/HostFeature.h"
#include "Host/SurfaceDeterminationHostTypes.h"

#include <memory>
#include <string_view>

class SurfaceDeterminationHostFeature final : public HostFeature {
public:
    explicit SurfaceDeterminationHostFeature(
        SurfaceDeterminationConfig config = {});
    ~SurfaceDeterminationHostFeature() noexcept override;

    SurfaceDeterminationHostFeature(
        const SurfaceDeterminationHostFeature&) = delete;
    SurfaceDeterminationHostFeature& operator=(
        const SurfaceDeterminationHostFeature&) = delete;
    SurfaceDeterminationHostFeature(
        SurfaceDeterminationHostFeature&&) = delete;
    SurfaceDeterminationHostFeature& operator=(
        SurfaceDeterminationHostFeature&&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    SurfaceDeterminationAdmission SendRequest(
        SurfaceDeterminationRequest request,
        SurfaceDeterminationCallback onComplete = nullptr);
    SurfaceDeterminationState GetState() const;
    std::shared_ptr<const SurfaceGenerationSnapshot>
        GetSurfaceSnapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
