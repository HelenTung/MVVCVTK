// 测试用途：只读展示手动操作的参数与结果，并在宿主层导出 CSV。
#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <QSet>
#include <functional>
#include <memory>
#include "ReportDocument.h"
class QComboBox;
class QTabWidget;
class QTableWidget;
class QPushButton;
class QLabel;
class QTreeWidget;
class QTableView;
class QLineEdit;
class QCheckBox;
class QPlainTextEdit;
class QTimer;
namespace Manual {
struct ReportRow final { QString section, field, type, value; };
QVector<ReportRow> GetReportRows(const QJsonObject& record);
QString GetReportCsv(const QJsonObject& record);
void ExportReportCsv(const QString& path, const QJsonObject& record);
class ResultReport final : public QWidget {
public:
    explicit ResultReport(QWidget* parent = nullptr);
    ~ResultReport() override;
    void SetRecord(const QJsonObject& record);
    QJsonObject GetSelectedRecord() const;
    void AddReport(ReportDocument document);
    bool SelectReport(const QString& id);
    void ShowReports(const QString& source = {}, const QString& result = {}, const QString& module = {});
    void ImportReport(const QString& path);
    const ReportDocument& GetSelectedReport() const { return m_selected; }
    QSet<QString> GetReportIds() const;
    QVector<ReportDocument> GetReports(const QString& module, const QSet<QString>& results) const;
    QVector<int> GetFilteredRows() const;
    std::function<void(const ReportDocument&,int)> onLocate;
protected:
    void showEvent(QShowEvent* event) override;
private:
    void Refresh();
    void SelectCurrent();
    void RefreshRows();
    void ContinueRows();
    int GetRowCount() const;
    void RefreshPage();
    void CompareSelected();
    QMap<QString, QJsonObject> m_records;
    QMap<QString, ReportDocument> m_documents;
    ReportDocument m_selected;
    QString m_baseline, m_sourceFilter, m_resultFilter, m_moduleFilter;
    QTreeWidget* m_reports = nullptr;
    QTableView* m_table = nullptr;
    QLineEdit *m_search = nullptr, *m_minimum = nullptr, *m_maximum = nullptr;
    QComboBox* m_filterColumn = nullptr;
    QCheckBox* m_follow = nullptr;
    QPlainTextEdit* m_conditions = nullptr;
    QLabel *m_range = nullptr, *m_baselineLabel = nullptr;
    QPushButton *m_previous = nullptr, *m_next = nullptr;
    QPushButton* m_locate = nullptr;
    QVector<int> m_rows;
    class RowScan;
    std::unique_ptr<RowScan> m_rowScan;
    QTimer* m_scanTimer = nullptr;
    QPushButton *m_cancelScan = nullptr, *m_exportFiltered = nullptr;
    bool m_allRows = false;
    bool m_rowsReady = false;
    int m_page = 0, m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    QPushButton* m_export = nullptr;
    QPushButton* m_copy = nullptr;
    QLabel* m_summary = nullptr;
};
}
