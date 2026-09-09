#pragma once

#include "Host/HostFeature.h"
#include "Host/WallThicknessHostTypes.h"

// 仓内可选 Feature；不安装到 SDK。生命周期/请求/查询均属于 Session owner thread。
// 拒绝请求不回调；Accepted 恰好一次 owner 回调。短命令可同步完成。
// 回调可查询/再次请求/取消；Host frame 内拓扑变更仍按 Host 原契约拒绝。
class WallThicknessHostFeature final : public HostFeature,
                                       public std::enable_shared_from_this<WallThicknessHostFeature>
{
  public:
    explicit WallThicknessHostFeature(ThicknessConfig config = {});
    ~WallThicknessHostFeature() noexcept override;
    WallThicknessHostFeature(const WallThicknessHostFeature &) = delete;
    WallThicknessHostFeature &operator=(const WallThicknessHostFeature &) = delete;

    std::string_view GetFeatureId() const noexcept override;
    FeatureDataContract GetDataContract() const override;
    std::vector<FeatureOperationState> GetOperationStates() const override;
    bool AttachHost(const HostFeatureContext &context) override;
    bool DetachHost() override;
    bool OnHostTick() override;
    ThicknessAdmission SendRequest(ThicknessRequest request, ThicknessCallback onComplete = {});
    ThicknessState GetState() const;
    std::optional<ThicknessSnapshot> GetResult(const DataRevisionRef &result) const;
    std::optional<ThicknessArchive> GetArchive(const DataRevisionRef &result) const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
