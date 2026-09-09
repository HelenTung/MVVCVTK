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
namespace Manual {
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
    void QueueObserve();
    struct View { QString id; QVTKOpenGLNativeWidget* widget; vtkSmartPointer<vtkGenericOpenGLRenderWindow> window; };
    TestSession m_runtime;
    TestWorkflow m_workflow;
    TestRecordWriter m_records;
    QtHostPump m_pump;
    std::vector<View> m_views;
    std::vector<ModulePanel*> m_modules;
    QStackedWidget* m_pages = nullptr;
    QStackedWidget* m_browsers = nullptr;
    QComboBox* m_renderMode = nullptr;
    QTabBar* m_featureTabs = nullptr;
    QWidget* m_viewArea = nullptr;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_log = nullptr;
    bool m_observeQueued = false;
    bool m_visibilityQueued = false;
    bool m_closeRetryQueued = false;
    bool m_isReady = false;
    bool m_isClosing = false;
    QString m_failure;
};
}
