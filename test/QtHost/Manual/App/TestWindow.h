// 测试用途：组合场景、一个三维与三个切片视窗和功能参数，管理测试会话生命周期。
#pragma once
#include "TestSession.h"
#include "TestWorkflow.h"
#include "QtHostPump.h"
#include "Support/TestRecordWriter.h"
#include "Modules/ModulePanel.h"
#include <QMainWindow>
#include <QLabel>
#include <QStackedWidget>
#include <QTabBar>
#include <vtkGenericOpenGLRenderWindow.h>
class QVTKOpenGLNativeWidget;
class QGridLayout;
namespace Manual {
class ResultReport;
class ObjectBrowser;
class TestWindow final : public QMainWindow {
public:
    explicit TestWindow(std::uint64_t budgetMiB = 0);
    ~TestWindow() override;
    void BuildSession();
    ModulePanel* GetModule(const QString& name) const;
    TestRecordWriter& GetRecords() { return m_records; }
    TestWorkflow& GetWorkflow() { return m_workflow; }
    std::shared_ptr<VtkAppHostSession> GetSession() const { return m_runtime.GetSession(); }
    bool GetIsReady() const { return m_isReady; }
    QString GetFailure() const { return m_failure; }
    void SetViewsVisible(bool visible);
    void SetInputFile(const QString& path);
    bool StopSession();
    std::uint64_t GetUpdateCount() const { return m_pump.GetUpdateCount(); }
    std::uint64_t GetRenderCount() const { return m_pump.GetRenderCount(); }
    QJsonObject GetDiagnostics() const { return m_pump.GetDiagnostics(); }
protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    void AppendLog(const QString& message);
    void ShowMessage(const QString& message);
    void UpdateMeasurements();
    void QueueObserve();
    void SetWorkspace(int index);
    void SyncNavigation();
    void SetViewLayout(const QString& mode);
    void UpdateStopCommand();
    void UpdateInputIdentity();
    void RefreshObjects();
    struct View { QString id; QVTKOpenGLNativeWidget* widget; vtkSmartPointer<vtkGenericOpenGLRenderWindow> window; QWidget* frame; };
    TestSession m_runtime;
    TestWorkflow m_workflow;
    TestRecordWriter m_records;
    QtHostPump m_pump;
    std::vector<View> m_views;
    std::vector<ModulePanel*> m_modules;
    QStackedWidget* m_pages = nullptr;
    QStackedWidget* m_browsers = nullptr;
    QStackedWidget* m_statisticsPages = nullptr;
    ObjectBrowser* m_objects = nullptr;
    QTabWidget* m_resultTabs = nullptr;
    QToolButton* m_showStatistics = nullptr;
    QComboBox* m_renderMode = nullptr;
    QPushButton* m_openVolume = nullptr;
    QPushButton* m_stop = nullptr;
    QTabBar* m_workspaceTabs = nullptr;
    QTabBar* m_featureTabs = nullptr;
    QStringList m_workspaceSelections{"Data", "Artifact", "Alignment"};
    QGridLayout* m_viewGrid = nullptr;
    QWidget* m_viewArea = nullptr;
    QLabel* m_inputIdentity = nullptr;
    QLabel* m_inputGeometry = nullptr;
    QLabel* m_messageData = nullptr;
    QPushButton* m_sourceButton = nullptr;
    QPushButton* m_originalInput = nullptr;
    QString m_originalRevision;
    QString m_inputSummary;
    QJsonObject m_objectGraph;
    QJsonArray m_objectRegions;
    std::vector<QJsonArray> m_objectModules;
    QString m_objectInput;
    std::optional<double> m_objectOpacity;
    QLabel* m_measurements = nullptr;
    QLabel* m_latestMessage = nullptr;
    QPlainTextEdit* m_log = nullptr;
    ResultReport* m_report = nullptr;
    bool m_observeQueued = false;
    bool m_visibilityQueued = false;
    bool m_closeRetryQueued = false;
    bool m_isReady = false;
    bool m_isClosing = false;
    QMap<QString,bool> m_statisticsPreferences;
    QString m_failure;
};
}
