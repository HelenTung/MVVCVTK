// 测试用途：协调功能测试中的计算占用、输入选择和交互结束，避免互斥操作并发。
#pragma once
#include "Host/VtkAppHostSession.h"
#include "Support/TestResources.h"
#include "Support/BusinessStatistics.h"
#include <functional>
#include <QString>
#include <QJsonObject>
#include <vector>
#include <map>
#include "Support/ReportDocument.h"
#include <QSet>
#if defined(MANUAL_WALL)
#include "Host/WallThicknessHostTypes.h"
#endif

namespace Manual {
enum class TestPolicy { Read, View, Stop, Change, Compute, Input, Interaction };
struct MeasurementSurface final {
    DataRevisionRef source;
    DataRevisionRef mesh;
    std::optional<double> threshold;
};
#if defined(MANUAL_WALL)
struct ThicknessInputSelection final {
    std::optional<DataRevisionRef> source, labels, mesh;
    std::vector<std::uint64_t> materialLabels;
    ThicknessUnit unit = ThicknessUnit::Millimeter;
};
#endif
class TestWorkflow final {
public:
    TestResources resources;
    QJsonObject partEditContext;
    // 仅识别用户的展示命令；不把 Host 内部展示提交误认成用户改动。
    std::uint64_t primaryPresentationCommand = 0;
    bool StartOperation(std::uint64_t id, TestPolicy policy, bool exitTools, const QString& module = {});
    void SetComplete(std::uint64_t id);
    void Stop() { m_isClosing = true; }
    bool GetIsClosing() const { return m_isClosing; }
    std::uint64_t GetBusyOperation() const { return m_busyId; }
    void AttachExit(QString module, std::function<bool()> exit) { m_exits.emplace_back(std::move(module), std::move(exit)); }
    std::optional<MeasurementSurface> GetMeasurementSurface() const
    { return getMeasurementSurface ? getMeasurementSurface() : std::nullopt; }
    QJsonObject GetPublishedGraph(VtkAppHostSession& session);
#if defined(MANUAL_WALL)
    ThicknessStartRequest BuildThicknessRequest(VtkAppHostSession& session,
        const ThicknessInputSelection& selection, ThicknessParams params) const;
#endif
    std::function<void(const QString&, const QString&, const QJsonObject&)> onCopyParameters;
    std::function<void(const QString&, const QString&, const QJsonObject&)> onNavigate;
    // 仅记录编辑工具的返回位置；区域仍由 Host 目录拥有。
    QJsonObject roiConsumer;
    std::function<void(const QString&)> onShowStatistics;
    std::map<QString,std::function<QVector<ReportDocument>(const QJsonObject&,const QSet<QString>&)>> readReports;
    std::function<void(const QString&,const QString&)> onShowReports;
    std::function<void(const QString&,const QString&)> onShowBusinessReport;
    std::function<bool(const QString&,const QString&)> getReportAvailable;
    std::function<void()> ensureCropViews;
    std::function<bool(const QString&, const QString&)> getActionAvailable;
    std::function<bool(const std::string&)> getRenderPending;
    std::function<bool(const std::string&)> getViewRenderPending;
    std::function<QJsonObject()> getPublishedGraph;
    std::function<bool(DataRevisionRef)> getResultRetained;
    std::function<DataCommitResult(DataRevisionRef,bool)> setResultRetained;
    std::function<DataCommitResult(DataRevisionRef)> unloadResult;
    std::function<std::optional<ImageDescriptor>(DataRevisionRef)> getImageInput;
    std::function<QJsonObject()> getViewTransforms;
    std::function<DataRevisionRef()> getPartLabels;
    std::function<StatisticsSamples(DataRevisionRef,DataRevisionRef,std::size_t,GrayPointFilter)> readGraySamples;
    std::function<StatisticsSamples(DataRevisionRef,const std::string&,std::size_t)> readRecordSamples;
    std::function<QJsonObject(DataRevisionRef,std::size_t,std::size_t,std::optional<std::array<double,2>>)> readRecordPage;
    std::function<std::array<double,3>(DataRevisionRef,std::size_t,DataRevisionRef)> getRecordLocation;
    std::function<std::array<double,3>(DataRevisionRef,const std::array<double,3>&)> getModelWorldPosition;
    std::function<std::optional<MeasurementSurface>()> getMeasurementSurface;
    std::function<std::vector<std::uint64_t>(DataRevisionRef,DataRevisionRef,DataRevisionRef)> getRoiVertices;
private:
    bool m_isClosing = false;
    std::uint64_t m_busyId = 0;
    std::vector<std::pair<QString, std::function<bool()>>> m_exits;
    std::map<QString, std::uint64_t> m_catalogOrder;
};
}
