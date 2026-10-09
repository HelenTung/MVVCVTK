// 测试用途：只读展示手动操作的参数与结果，并在宿主层导出 CSV。
#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QMap>
#include <QVector>
class QComboBox;
class QTabWidget;
class QTableWidget;
class QPushButton;
class QLabel;
namespace Manual {
struct ReportRow final { QString section, field, type, value; };
QVector<ReportRow> GetReportRows(const QJsonObject& record);
QString GetReportCsv(const QJsonObject& record);
void ExportReportCsv(const QString& path, const QJsonObject& record);
class ResultReport final : public QWidget {
public:
    explicit ResultReport(QWidget* parent = nullptr);
    void SetRecord(const QJsonObject& record);
    QJsonObject GetSelectedRecord() const;
protected:
    void showEvent(QShowEvent* event) override;
private:
    void Refresh();
    QMap<QString, QJsonObject> m_records;
    QComboBox* m_operations = nullptr;
    QTabWidget* m_groups = nullptr;
    QMap<QString, QTableWidget*> m_tables;
    QPushButton* m_export = nullptr;
    QPushButton* m_copy = nullptr;
    QLabel* m_summary = nullptr;
};
}
