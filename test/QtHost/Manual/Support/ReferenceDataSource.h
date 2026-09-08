// 测试用途：为功能测试受控发布名义参考和二值掩码，保持真实数据图修订链。
#pragma once
#include "Host/HostFeature.h"
#include <QJsonObject>
#include <QString>
#include <map>
namespace Manual {
struct ReferenceInput {
    DataRevisionRef source;
    DataRevisionRef mesh;
    DataRevisionRef nominal;
    DataRevisionRef scope;
    QString coordinateFrame;
    QJsonObject document;
};
class ReferenceDataSource final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "manual.reference-input"; }
    bool AttachHost(const HostFeatureContext& context) override;
    bool DetachHost() override;
    bool OnHostTick() override { return true; }
    ReferenceInput LoadReference(const QString& path, DataRevisionRef source, DataRevisionRef mesh);
    DataRevisionRef CreateMask(DataRevisionRef source, const QJsonObject& params);
    QJsonObject GetPublishedGraph();
private:
    std::shared_ptr<TrustedDataPort> m_data;
    std::optional<DataCommitId> m_sceneCommit;
    QJsonObject m_sceneGraph;
    std::map<QString, std::uint64_t> m_sceneOrder;
};
}
