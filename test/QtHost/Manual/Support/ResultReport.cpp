// 测试用途：报告序列化和表格显示只消费运行记录，不调用业务或渲染核心。
#include "ResultReport.h"
#include "UiText.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QShowEvent>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <algorithm>
namespace Manual {
namespace {
QString CsvCell(QString value)
{
    value.replace('"', "\"\"");
    return '"' + value + '"';
}
bool IsMatrix(const QString& key)
{
    return key.contains("matrix", Qt::CaseInsensitive) || QStringList{"directionLPS", "directionRAS", "boxToSource", "localToSource", "sourceToTarget", "modelToWorld"}.contains(key);
}
}
QVector<ReportRow> GetReportRows(const QJsonObject& record)
{
    QVector<ReportRow> rows;
    const std::function<void(const QString&, const QString&, const QJsonValue&, const QString&)> append =
        [&](const QString& section, const QString& path, const QJsonValue& value, const QString& key) {
            if (value.isObject()) {
                const auto object = value.toObject();
                if (object.isEmpty()) rows.append({section,path,"object","{}"});
                for (auto it = object.begin(); it != object.end(); ++it)
                    append(section,path.isEmpty() ? it.key() : path + '.' + it.key(),it.value(),it.key());
            } else if (value.isArray()) {
                const auto array = value.toArray();
                if (array.isEmpty()) rows.append({section,path,"array","[]"});
                const int side = IsMatrix(key) && (array.size() == 9 || array.size() == 16) ? (array.size() == 9 ? 3 : 4) : 0;
                for (int index = 0; index < array.size(); ++index) {
                    const auto suffix = side ? QString("[%1,%2]").arg(index / side + 1).arg(index % side + 1) : QString("[%1]").arg(index);
                    append(section,path + suffix,array[index],key);
                }
            } else {
                QString type, text;
                if (value.isNull() || value.isUndefined()) { type = "null"; text = "未提供"; }
                else if (value.isBool()) { type = "boolean"; text = value.toBool() ? "true" : "false"; }
                else if (value.isDouble()) { type = "number"; text = QString::number(value.toDouble(),'g',17); }
                else { type = "string"; text = value.toString(); }
                rows.append({section,path,type,text});
            }
        };
    for (const auto* key : {"operationId","module","action","status","startedUtc","elapsedMs","completeCount"})
        if (record.contains(key)) append("执行信息", key, record[key], key);
    append("参数", QString(), record["parameters"], QString());
    append("结果", QString(), record["result"], QString());
    append("输入", "source", record["source"], "source");
    if (record.contains("current")) append("输入", "current", record["current"], "current");
    return rows;
}
QString GetReportCsv(const QJsonObject& record)
{
    QString text = "分组,字段,类型,值\r\n";
    for (const auto& row : GetReportRows(record))
        text += CsvCell(row.section) + ',' + CsvCell(row.field) + ',' + CsvCell(row.type) + ',' + CsvCell(row.value) + "\r\n";
    return text;
}
void ExportReportCsv(const QString& path, const QJsonObject& record)
{
    const auto bytes = QByteArray("\xef\xbb\xbf") + GetReportCsv(record).toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("CSV 报告保存失败");
}
ResultReport::ResultReport(QWidget* parent) : QWidget(parent)
{
    setObjectName("resultReport");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(4,4,4,4);
    auto* toolbar = new QHBoxLayout;
    m_operations = new QComboBox(this); m_operations->setObjectName("reportOperation"); toolbar->addWidget(m_operations,1);
    m_copy = new QPushButton("复制 CSV",this); m_copy->setObjectName("copyReportCsv"); toolbar->addWidget(m_copy);
    m_export = new QPushButton("导出 CSV",this); m_export->setObjectName("exportReportCsv"); toolbar->addWidget(m_export);
    layout->addLayout(toolbar);
    m_summary = new QLabel("执行操作后可查看参数、结果和输入；报告不触发业务计算。",this); layout->addWidget(m_summary);
    m_groups = new QTabWidget(this); m_groups->setObjectName("reportGroups"); layout->addWidget(m_groups,1);
    for (const auto* group : {"执行信息","参数","结果","输入"}) {
        auto* table = new QTableWidget(0,3,m_groups); table->setObjectName("reportTable");
        table->setHorizontalHeaderLabels({"字段","类型","值"}); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true); table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);
        m_tables[group] = table; m_groups->addTab(table,group);
    }
    m_groups->setCurrentIndex(2); m_copy->setEnabled(false); m_export->setEnabled(false);
    connect(m_operations,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{Refresh();});
    connect(m_copy,&QPushButton::clicked,this,[this]{QApplication::clipboard()->setText(GetReportCsv(GetSelectedRecord()));});
    connect(m_export,&QPushButton::clicked,this,[this]{
        const auto path = QFileDialog::getSaveFileName(this,"导出结果报告",QString(),"CSV (*.csv)");
        if (path.isEmpty()) return;
        try { ExportReportCsv(path,GetSelectedRecord()); }
        catch (const std::exception& error) { QMessageBox::warning(this,"导出失败",QString::fromUtf8(error.what())); }
    });
}
void ResultReport::SetRecord(const QJsonObject& record)
{
    if (!record["isTerminal"].toBool()) return;
    const auto id = record["operationId"].toString();
    m_records[id] = record;
    int index = m_operations->findData(id);
    const auto title = QString("#%1 · %2 · %3 · %4").arg(id,GetModuleText(record["module"].toString()),GetActionText(record["module"].toString(),record["action"].toString()),record["status"].toString());
    if (index < 0) { index = m_operations->count(); m_operations->addItem(title,id); }
    else m_operations->setItemText(index,title);
    m_operations->setCurrentIndex(index); m_copy->setEnabled(true); m_export->setEnabled(true);
    if (isVisible()) Refresh();
}
QJsonObject ResultReport::GetSelectedRecord() const { return m_records.value(m_operations->currentData().toString()); }
void ResultReport::showEvent(QShowEvent* event) { QWidget::showEvent(event); Refresh(); }
void ResultReport::Refresh()
{
    if (!isVisible()) return;
    const auto rows = GetReportRows(GetSelectedRecord());
    QMap<QString,QVector<ReportRow>> grouped;
    for (const auto& row : rows) grouped[row.section].append(row);
    constexpr int displayLimit = 1000;
    bool truncated = false;
    for (auto it = m_tables.begin(); it != m_tables.end(); ++it) {
        const auto values = grouped.value(it.key()); const auto count = std::min(values.size(),displayLimit);
        truncated = truncated || values.size() > displayLimit;
        auto* table = it.value(); table->setRowCount(count);
        for (int index = 0; index < count; ++index) {
            table->setItem(index,0,new QTableWidgetItem(values[index].field));
            table->setItem(index,1,new QTableWidgetItem(values[index].type));
            table->setItem(index,2,new QTableWidgetItem(values[index].value));
        }
    }
    m_summary->setText(truncated ? "每个分组显示前 1000 行；CSV 导出包含完整报告。" : "只读运行结果；矩阵按行列索引展开，CSV 保留完整参数和结果。");
}
}
