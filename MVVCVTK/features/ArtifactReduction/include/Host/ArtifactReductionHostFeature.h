#pragma once

#include "Host/ArtifactReductionHostTypes.h"
#include "Host/HostFeature.h"

#include <memory>
#include <string_view>
#include <vector>

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
    // 仓内候选的有界只读对照样本（源/候选存储灰度）；最多4096对，不发布、不改变输入。
    // 只接受当前 Ready requestId；步长由调用方明确给出，无效掩码/非有限值不进入样本。
    std::optional<std::vector<std::array<double,2>>> GetCandidateSamples(
        std::uint64_t requestId,std::size_t firstVoxel,std::size_t stride,std::size_t maxCount) const;

private:
    ArtifactAdmission StartCandidate(ArtifactRequest request);
    ArtifactError StopCandidate();
    ArtifactError ClearCandidate();
    ArtifactCommitResult SetCandidate(std::uint64_t requestId);
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
