// 测试用途：为功能测试受控发布名义参考和二值掩码，保持真实数据图修订链。
#pragma once
#include "Host/HostFeature.h"
#include "Data/ImageReadTypes.h"
#include "BusinessStatistics.h"
#include "../../../../MVVCVTK/features/common/FeatureResultScopes.h"
#include <QJsonObject>
#include <QString>
#include <map>
#include <optional>
class VtkAppHostSession;
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
    QJsonObject GetViewTransforms();
    QJsonObject GetResultEvidence();
    QJsonObject ReadRoi(DataRevisionRef roi, DataRevisionRef source, const QString& maskPath = {});
    QJsonObject ReadImageEvidence(DataRevisionRef source, const QString& maskPath);
    std::vector<std::uint64_t> GetRoiVertices(DataRevisionRef roi, DataRevisionRef source, DataRevisionRef mesh) const;
    QJsonObject ReadTransform();
    static std::size_t GetGraySampleStride(std::size_t total, const std::array<int,3>& dimensions);
    StatisticsSamples ReadGraySamples(DataRevisionRef source, DataRevisionRef roi, std::size_t offset, GrayPointFilter filter = {});
    StatisticsSamples ReadRecordSamples(DataRevisionRef table, const std::string& column, std::size_t offset);
    QJsonObject ReadRecordPage(DataRevisionRef table, std::size_t offset, std::size_t count,
        std::optional<std::array<double,2>> volumeRange = {});
    std::array<double,3> GetRecordLocation(DataRevisionRef table, std::size_t row, DataRevisionRef source);
private:
    std::shared_ptr<TrustedDataPort> m_data;
    FeatureInternal::ResultScopes m_resultScopes;
    std::shared_ptr<FeatureViewDirectory> m_views;
};
}
