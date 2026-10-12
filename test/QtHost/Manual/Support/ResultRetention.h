// 测试宿主通过现有可信数据口提供结果保留/卸载，不扩展 SDK 契约。
#pragma once
#include "Host/HostFeature.h"
#include <mutex>
#include <vector>
namespace Manual {
class ResultRetention final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "manual.result-retention"; }
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override;
    bool GetRetained(DataRevisionRef ref) const;
    DataCommitResult SetRetained(DataRevisionRef ref, bool retained);
    DataCommitResult UnloadResult(DataRevisionRef ref);
private:
    std::shared_ptr<TrustedDataPort> m_data;
    DataObserverId m_observer = 0;
    struct ReleaseQueue {
        std::mutex mutex;
        std::vector<DataEntityId> scopes;
    };
    std::shared_ptr<ReleaseQueue> m_releasing;
};
}
