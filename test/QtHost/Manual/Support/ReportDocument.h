#pragma once
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <memory>
#include <functional>
#include <cstddef>
#include "Data/DataPayloads.h"
class QIODevice;
class QThread;
namespace Manual {
using ReportIndex = qint64;
using ReportRows = MeshReadArray<ReportIndex>;
ReportRows GetReportRowRange(ReportIndex count);
// 仅保存表格；不拥有 Session、体数据或原生结果快照。
class ReportTable final {
public:
    ReportTable();
    ~ReportTable();
    void Append(const QStringList& row);
    QStringList Read(ReportIndex row) const;
    ReportIndex GetCount() const;
    void SetRows(std::size_t count, std::function<QStringList(ReportIndex)> read);
    bool ContinueBuild();
    bool GetIsComplete() const;
    void PrepareRead();
    void SetOwnerThread(QThread* owner);
    // 独立文件游标；调用方持有 table，且仅在物化完成后创建读取器。
    std::function<QStringList(ReportIndex)> CreateReader() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
struct ReportColumn final {
    QString name, quantity, unit;
    bool numeric = false;
};
struct ReportDocument final {
    QString id, title, category, module, source, result, description;
    QString coordinateFrame;
    QStringList relatedResults, relatedSources, relatedModules;
    QVector<ReportColumn> columns;
    std::shared_ptr<ReportTable> table;
    QJsonObject conditions;
    int identityColumn = -1, validityColumn = -1;
    int valueColumn = -1;
    int xColumn = -1, yColumn = -1, zColumn = -1;
    bool identityStable = false;
    bool mappingRequired = false;
    QStringList validValues{"0","Valid"};
};
using ReportCheckpoint = std::function<void()>;
using ReportPreparation = std::function<QVector<ReportDocument>(const ReportCheckpoint&)>;
ReportDocument ReadReportCsv(QIODevice& input, const QString& title, const ReportCheckpoint& checkpoint = {});
void WriteReportCsv(QIODevice& output, const ReportDocument& document, const ReportRows* rows = nullptr, const ReportCheckpoint& checkpoint = {});
void ExportReport(const QString& path, const ReportDocument& document, const ReportRows* rows = nullptr, const ReportCheckpoint& checkpoint = {});
QString GetReportCell(const QStringList& row, int column);
bool GetReportNumber(const QString& text, double& value);
}
