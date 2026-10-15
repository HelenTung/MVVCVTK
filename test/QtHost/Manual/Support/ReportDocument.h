#pragma once
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <memory>
#include <functional>
class QIODevice;
namespace Manual {
// 仅持有所需的只读目录或临时表；不拥有 Session、体数据或完整几何。
class ReportTable final {
public:
    ReportTable();
    ReportTable(quint64 count,std::function<QStringList(quint64)> readRow);
    ~ReportTable();
    void Append(const QStringList& row);
    QStringList Read(quint64 row) const;
    quint64 GetCount() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
struct ReportDocument;
// 自然顺序只保存总数；筛选/排序目录使用同一后备存储，不与 Qt 页内 int 行号混用。
class ReportRows final {
public:
    ReportRows()=default;
    static ReportRows Natural(quint64 count);
    quint64 size() const;
    bool isEmpty() const {return size()==0;}
    quint64 operator[](quint64 offset) const;
    QVector<quint64> mid(quint64 offset,int count) const;
    void clear();
    void append(quint64 row);
    void Sort(const ReportDocument& document,int column,bool ascending,bool numeric=true);
private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
    quint64 m_naturalCount=0;
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
void WriteReportCsv(QIODevice& output, const ReportDocument& document, const ReportRows* rows = nullptr);
void ExportReport(const QString& path, const ReportDocument& document, const ReportRows* rows = nullptr);
QString GetReportCell(const QStringList& row, int column);
bool GetReportNumber(const QString& text, double& value);
}
