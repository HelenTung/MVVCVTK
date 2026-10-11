// 测试用途：拥有业务参数生成的临时 ROI，消费者释放后按依赖顺序退休。
#pragma once
#include "Host/HostFeature.h"
#include "Data/DataGraphTypes.h"
#include <QJsonValue>
#include <optional>
#include <vector>
namespace Manual {
class InputRoiSource final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "manual.roi-input"; }
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override { return true; }
    std::optional<DataRevisionRef> Create(DataRevisionRef source, const QJsonValue& extent,
        const QJsonValue& mask, const char* name, bool isPhysicalBounds);
private:
    std::shared_ptr<TrustedDataPort> m_data;
    std::vector<DataLifetimeRetirement> m_scopes;
};
}
