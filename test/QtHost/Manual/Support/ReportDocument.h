#pragma once
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <memory>
#include <functional>
#include <cstddef>
#include <vector>
class QIODevice;
class QThread;
namespace Manual {
using ReportRowId = qint64;
using ReportRows = std::vector<ReportRowId>;
using ReportCheckpoint = std::function<void()>;
// 完成后只保存表格；构建候选可暂持只读原生数据，物化或取消后释放。
class ReportTable final {
public:
    ReportTable();
    ReportTable(quint64 count,std::function<QStringList(ReportRowId)> readRow);
    ~ReportTable();
    void Append(const QStringList& row);
    QStringList Read(ReportRowId row) const;
    ReportRowId GetCount() const;
    void SetRows(std::size_t count, std::function<QStringList(ReportRowId)> read);
    bool ContinueBuild();
    bool GetIsComplete() const;
    void PrepareRead();
    void SetOwnerThread(QThread* owner);
    // 独立文件游标；调用方持有 table，且仅在物化完成后创建读取器。
    std::function<QStringList(ReportRowId)> CreateReader() const;
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
    // 仅在该正式结果内稳定的对象/点编号；与跨结果稳定的外部业务编号分开。
    QString identityResult;
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
    // 所属线程每次只准备一个工作单元；完成后替换为实际分组，取消直接释放候选。
    std::function<bool(QVector<ReportDocument>&)> prepareReports;
};
ReportDocument ReadReportCsv(QIODevice& input, const QString& title, const ReportCheckpoint& checkpoint = {});
void WriteReportCsv(QIODevice& output, const ReportDocument& document, const ReportRows* rows = nullptr, const ReportCheckpoint& checkpoint = {});
void ExportReport(const QString& path, const ReportDocument& document, const ReportRows* rows = nullptr, const ReportCheckpoint& checkpoint = {});
QString GetReportCell(const QStringList& row, int column);
bool GetReportNumber(const QString& text, double& value);
QJsonValue GetReportCountValue(quint64 count);
quint64 GetReportCount(const QJsonValue& value);
}
