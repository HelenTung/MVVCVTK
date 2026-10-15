#pragma once
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <memory>
class QIODevice;
namespace Manual {
// 仅保存表格；不拥有 Session、体数据或原生结果快照。
class ReportTable final {
public:
    ReportTable();
    ~ReportTable();
    void Append(const QStringList& row);
    QStringList Read(int row) const;
    int GetCount() const;
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
    QStringList relatedResults;
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
ReportDocument ReadReportCsv(QIODevice& input, const QString& title);
void WriteReportCsv(QIODevice& output, const ReportDocument& document, const QVector<int>* rows = nullptr);
void ExportReport(const QString& path, const ReportDocument& document, const QVector<int>* rows = nullptr);
QString GetReportCell(const QStringList& row, int column);
bool GetReportNumber(const QString& text, double& value);
}
