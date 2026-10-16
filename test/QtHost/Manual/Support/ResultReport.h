// 测试用途：只读展示手动操作的参数与结果，并在宿主层导出 CSV。
#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <QSet>
#include <cstdint>
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
    void AddReport(ReportDocument document, bool select = true);
    bool SelectReport(const QString& id);
    void ShowReports(const QString& source = {}, const QString& result = {}, const QString& module = {});
    void ImportReport(const QString& path);
    void ExportReportFile(const QString& path, bool filtered = false);
    void QueueReport(const QJsonObject& record);
    bool GetReportAvailable(const QString& module, const QString& result) const;
    bool GetGenerationPending() const;
    void RemoveReport(QString id);
    void SetReportSource(const QString& id, const QString& source);
    // 普通取消立即返回；最终关闭先等读者退出，再释放会话结果与临时文件。
    void CancelTasks(bool waitForExit = false);
    const ReportDocument& GetSelectedReport() const { return m_selected; }
    QSet<QString> GetReportIds() const;
    QVector<ReportDocument> GetReports(const QString& module, const QSet<QString>& results) const;
    ReportRows GetFilteredRows() const;
    ReportRowId GetFilteredCount() const;
    std::function<ReportRowId(ReportRowId)> CreateFilteredReader() const;
    std::function<void(const ReportDocument&,ReportRowId)> onLocate;
    std::function<QVector<ReportDocument>(const QJsonObject&,const QSet<QString>&)> onReadReports;
    std::function<ReportPreparation(const QJsonObject&,const QSet<QString>&)> onPrepareReports;
    std::function<QMap<QString,QString>()> getSources;
protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
private:
    friend class ReportTaskTestAccess;
    std::function<void()> m_beforeWorkerStart;
    void CancelRows();
    void Refresh();
    void SelectCurrent();
    void RefreshRows();
    void ContinueRows();
    ReportRowId GetRowCount() const;
    void RefreshPage();
    void SetReadFailure(const QString& error);
    void CompareSelected();
    void ClearSelection();
    void ContinueGeneration();
    void StartTask(std::function<ReportDocument(const ReportCheckpoint&)> work, QVector<ReportDocument> inputs = {});
    void StartReportsTask(ReportPreparation work,QVector<ReportDocument> inputs,const QString& operation = {});
    void RefreshFailures();
    void ContinueTask();
    QMap<QString, QJsonObject> m_records;
    QMap<QString, ReportDocument> m_documents;
    QMap<QString, QJsonObject> m_pendingReports;
    QMap<QString, QString> m_failedReports;
    QSet<QString> m_completedOperations, m_removedReports;
    std::uint64_t m_viewRevision = 0;
    std::uint64_t m_buildingViewRevision = 0;
    int m_buildingIndex = 0;
    ReportRowId m_buildingPreviewRow = 0;
    qulonglong m_generationProgress = 0;
    QString m_buildingOperation;
    QVector<ReportDocument> m_buildingReports;
    QTimer *m_buildTimer = nullptr, *m_taskTimer = nullptr;
    class Task;
    std::unique_ptr<Task> m_task;
    std::uint64_t m_taskRequest=0,m_rowsRequest=0;
    bool m_acceptingRequests=true;
    QLabel* m_taskStatus = nullptr;
    QPushButton* m_cancelTask = nullptr;
    QComboBox* m_failedReport = nullptr;
    QPushButton* m_retryReport = nullptr;
    ReportDocument m_selected;
    QString m_baseline, m_sourceFilter, m_resultFilter, m_moduleFilter;
    QTreeWidget* m_reports = nullptr;
    QTableView* m_table = nullptr;
    QLineEdit *m_search = nullptr, *m_minimum = nullptr, *m_maximum = nullptr;
    QComboBox* m_filterColumn = nullptr;
    QCheckBox* m_follow = nullptr;
    QPlainTextEdit* m_conditions = nullptr;
    QLabel *m_range = nullptr, *m_baselineLabel = nullptr;
    QPushButton *m_first = nullptr, *m_previous = nullptr, *m_next = nullptr, *m_last = nullptr;
    QPushButton* m_locate = nullptr;
    ReportRows m_rows;
    class RowScan;
    std::unique_ptr<RowScan> m_rowScan,m_pendingRowScan;
    QTimer* m_scanTimer = nullptr;
    QPushButton *m_cancelScan = nullptr, *m_exportFiltered = nullptr;
    bool m_allRows = false;
    bool m_rowsReady = false;
    bool m_pageReady = false;
    ReportRowId m_page = 0;
    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    QPushButton* m_export = nullptr;
    QPushButton* m_copy = nullptr;
    QLabel* m_summary = nullptr;
};
}
