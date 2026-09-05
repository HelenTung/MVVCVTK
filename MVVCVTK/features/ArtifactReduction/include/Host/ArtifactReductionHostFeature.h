#pragma once

#include "Host/ArtifactReductionHostTypes.h"
#include "Host/HostFeature.h"

#include <memory>
#include <string_view>

// 仓内后处理入口。除析构外均由挂载 owner 线程串行调用；不注册视图或改绑定。
class ArtifactReductionHostFeature final : public HostFeature {
public:
    explicit ArtifactReductionHostFeature(ArtifactConfig config = {});
    ~ArtifactReductionHostFeature() noexcept override;
    ArtifactReductionHostFeature(const ArtifactReductionHostFeature&) = delete;
    ArtifactReductionHostFeature& operator=(const ArtifactReductionHostFeature&) = delete;
    ArtifactReductionHostFeature(ArtifactReductionHostFeature&&) = delete;
    ArtifactReductionHostFeature& operator=(ArtifactReductionHostFeature&&) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;

    // 接纳不回调；owner tick 消费一次结果，GetState 只读，不隐式等待或推进任务。
    ArtifactAdmission SendRequest(ArtifactHostRequest request);
    ArtifactState GetState() const;

private:
    ArtifactAdmission StartCandidate(ArtifactRequest request);
    ArtifactError StopCandidate();
    ArtifactError ClearCandidate();
    ArtifactCommitResult SetCandidate(std::uint64_t requestId);
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
