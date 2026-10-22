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
#include <QFile>
#include <QElapsedTimer>
#include <vtkGenericOpenGLRenderWindow.h>
class QVTKOpenGLNativeWidget;
class QAction;
class QDockWidget;
class QToolBar;
class QTabBar;
namespace Manual {
class ResultReport;
class ObjectBrowser;
class InputIdentityLabel;
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
    QString GetLogPath() const { return m_log.fileName(); }
    QString GetFeedback() const;
    void SetViewsVisible(bool visible);
    void SetInputFile(const QString& path);
    bool StopSession();
    void SaveLayout();
    void RestoreLayout();
    void ResetLayout();
    QString GetLayoutPath() const;
    std::uint64_t GetUpdateCount() const { return m_pump.GetUpdateCount(); }
    std::uint64_t GetRenderCount() const { return m_pump.GetRenderCount(); }
    QJsonObject GetDiagnostics() const;
    void SetSynchronousSpan(const QString& phase,qint64 durationNs);
protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    void AppendLog(const QString& message, const QString& module = "工作台");
    void ShowMessage(const QString& message, const char* tone = "neutral");
    void SetColumnVisible(int column, bool visible);
    void ShowHistogram();
    QDockWidget* CreateDock(const QString& name, const QString& title, QWidget* content);
    void UpdateMeasurements();
    void QueueObserve();
    void SetWorkspace(int index);
    void SyncNavigation();
    void SetViewLayout(const QString& mode);
    void UpdateStopCommand();
    void UpdateInputIdentity();
    void RefreshObjects();
    void StartResponseProbe();
    void SampleResponse();
    struct View { QString id; QVTKOpenGLNativeWidget* widget; vtkSmartPointer<vtkGenericOpenGLRenderWindow> window; QWidget* frame; QDockWidget* dock; };
    TestSession m_runtime;
    TestWorkflow m_workflow;
    TestRecordWriter m_records;
    QtHostPump m_pump;
    std::vector<View> m_views;
    std::vector<ModulePanel*> m_modules;
    QString m_colorModule;QJsonObject m_colorParameters;
    QStackedWidget* m_pages = nullptr;
    QStackedWidget* m_browsers = nullptr;
    QStackedWidget* m_statisticsPages = nullptr;
    QStackedWidget* m_grayPages = nullptr;
    ObjectBrowser* m_objects = nullptr;
    QTabWidget* m_resultTabs = nullptr;
    QToolButton* m_showStatistics = nullptr;
    QComboBox* m_renderMode = nullptr;
    QPushButton* m_openVolume = nullptr;
    QPushButton* m_stop = nullptr;
    QComboBox* m_workspaceTabs = nullptr;
    QComboBox* m_featureTabs = nullptr;
    QDockWidget* m_objectDock = nullptr;
    QDockWidget* m_parameterDock = nullptr;
    QDockWidget* m_grayDock = nullptr;
    QDockWidget* m_resultDock = nullptr;
    QDockWidget* m_reportDock = nullptr;
    QToolBar* m_contextTools = nullptr;
    QAction* m_contextToolsAction = nullptr;
    QTabBar* m_steps = nullptr;
    QAction* m_histogramAction = nullptr;
    QMenu* m_windowMenu = nullptr;
    QTimer m_layoutTimer;
    QTimer m_responseTimer;
    QElapsedTimer m_responseClock;
    qint64 m_lastResponseMs=0,m_maxResponseMs=0,m_longestSpanNs=0;
    qint64 m_readyAtMs=-1;
    QString m_responseStep=QStringLiteral("初始化"),m_longestPhase;
    QString m_responseOperation="0";
    std::vector<qint64> m_responseDelays;
    std::vector<qint64> m_businessResponseDelays;
    QJsonObject m_startupTimings;
    std::uint64_t m_over100Ms=0,m_over1000Ms=0,m_droppedSamples=0,m_droppedStalls=0;
    QJsonArray m_responseStalls,m_synchronousSpans;
    QToolButton* m_objectToggle = nullptr;
    QToolButton* m_parameterToggle = nullptr;
    bool m_restoringLayout = true;
    bool m_layoutReady = false;
    bool m_firstShow = false;
    bool m_hadSavedLayout = false;
    bool m_reportOpened = false;
    bool m_viewsVisible = true;
    QString m_viewLayoutMode = "FourViews";
    QMap<QString,bool> m_grayPreferences;
    QString m_lastModule,m_savedModule;
    bool m_syncGray = false;
    QStringList m_workspaceSelections{"Data", "Surface", "Roi", "Alignment"};
    InputIdentityLabel* m_inputIdentity = nullptr;
    QLabel* m_inputGeometry = nullptr;
    QAction* m_sourceAction = nullptr;
    QAction* m_originalInputAction = nullptr;
    QString m_originalRevision;
    QString m_inputSummary;
    QJsonObject m_objectGraph;
    QJsonArray m_objectRegions;
    std::vector<QJsonArray> m_objectModules;
    QString m_objectInput;
    std::optional<double> m_objectOpacity;
    QLabel* m_measurements = nullptr;
    QLabel* m_operationState = nullptr;
    QFile m_log;
    QString m_logFailure;
    QString m_feedbackText;
    QString m_lastOutcome;
    QLabel* m_startupFeedback = nullptr;
    bool m_settingsFailureReported = false;
    ResultReport* m_report = nullptr;
    bool m_observeQueued = false;
    bool m_visibilityQueued = false;
    bool m_closeRetryQueued = false;
    bool m_isReady = false;
    int m_buildPhase=0;
    bool m_isClosing = false;
    QString m_failure;
};
}
