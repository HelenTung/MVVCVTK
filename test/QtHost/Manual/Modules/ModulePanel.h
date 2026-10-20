// 测试用途：构建功能操作表单并分派实际请求，保持中文显示与内部用例标识分离。
#pragma once
#include "App/TestSession.h"
#include "App/TestWorkflow.h"
#include "Support/TestRecordWriter.h"
#include "Support/JsonInput.h"
#include <QComboBox>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QWidget>
#include <QMap>
#include <QSet>
#include <QPointer>
#include <functional>
#include <map>
#include <set>
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QGridLayout;
class QGroupBox;
class QScrollArea;
class QVBoxLayout;
class QSplitter;
class QLineEdit;
class QTabWidget;
class QToolButton;
class QMenu;

namespace Manual {
class ParameterEditor;
class ColorLegendEditor;
struct TestContext { TestSession& runtime; TestWorkflow& workflow; TestRecordWriter& records; };
struct ReadTaskContext final {
    ReportCheckpoint checkpoint;
    // 提交与取消争用同一状态：开始提交后，取消明确不再被接纳。
    std::function<void(const std::function<void()>&)> commit;
};
class ModulePanel : public QWidget {
public:
    using Action = std::function<void(std::uint64_t, const QJsonObject&)>;
    ModulePanel(TestContext context, QString name, QWidget* parent = nullptr);
    ~ModulePanel() override;
    QWidget* GetBrowser() const;
    QTreeWidget* GetCatalogTree() const { return m_nodes; }
    QTreeWidget* GetSceneTree() const { return m_scene; }
    QJsonArray GetObjectNodes() const { return m_sceneNodes; }
    void AttachAction(const QString& name, const QJsonObject& defaults, Action action,
        TestPolicy policy = TestPolicy::Change, bool exitTools = false);
    using Completion = std::function<void(const QString&, const QJsonObject&)>;
    std::uint64_t SendAction(const QString& name, const QJsonObject& params, Completion onComplete = {});
    void SetComplete(std::uint64_t id, const QString& status, QJsonObject result = {});
    void SetAdmission(std::uint64_t id, bool accepted, QJsonObject raw = {});
    // 只读长任务持有固定输入，在后台工作；完成回调和记录仍在界面线程执行。
    void StartReadTask(std::uint64_t id, QString status,
        std::function<QJsonObject(const ReadTaskContext&)> work,
        std::function<void()> onSucceeded = {});
    std::size_t CancelReadTasks();
    void SetState(const QJsonObject& value);
    void SetNotice(const QString& text);
    void SetFeedback(const QString& text, const char* tone = "neutral");
    QString GetFeedback() const;
    QString GetNotice() const { return m_notice; }
    void SendHost(std::uint64_t id, HostRequest&& request);
    void SetInput(std::uint64_t id, const DataRevisionRef& revision);
    void SetParameters(const QString& action, const QJsonObject& values);
    void SetParameterPatch(const QString& action, const QJsonObject& patch);
    QJsonObject GetParameters() const;
    ParameterEditor* GetParameterEditor(const QString& action) const;
    QJsonObject GetObservedState() const { return m_observed; }
    QJsonObject GetActionParameters(const QString& action) const;
    void SendBusinessAction(const QString& action, bool usesOutput = false);
    BusinessStatistics* GetStatisticsWidget() const { return m_statistics; }
    void RefreshStatistics();
    void ShowSources(DataRevisionRef root);
    QString GetName() const { return m_name; }
    QString GetDisplayName() const;
    void ShowColorEditor(const QPoint& position,const QString& viewId={});
    std::function<QJsonObject()> getColorParameters;
    std::function<QString()> getColorDescription;
    std::function<QPointF(const QString&)> getColorPosition;
    std::function<QString()> getColorSurface;
    std::function<void(bool)> setColorVisible;
    std::function<void(const QString&,bool,int)> setViewLegend;
    void SelectAction(const QString& action);
    bool SelectNodeGroup(const QString& id);
    bool SelectPartTarget(const QJsonObject& binding);
    void RefreshWorkflow();
    void Observe(bool refresh = true);
    bool GetHasPending() const { return !m_pending.empty(); }
    bool observeInBackground = false;
    void SetDroppedPath(const QString& path);
    QString GetCurrentAction() const { return m_currentAction; }
    QString GetParameterAction() const { return m_parameterAction; }
    QStringList GetActions() const;
    std::shared_ptr<VtkAppHostSession> GetSession() const { return m_context.runtime.GetSession(); }
    TestContext GetContext() const { return m_context; }
    std::function<void()> onObserve;
    std::function<void(const QString&,const QJsonObject&)> onParametersEdited;
    std::function<bool()> onStatistics;
    std::function<GrayStatisticsScope()> getGrayScope;
    std::function<void()> onStop;
    // 关闭会话时释放分页快照；页面和业务完成回调仍保留到 Host 停止完成。
    std::function<void()> onSessionClosing;
    QString stopDescription;
    std::function<void(const QString&)> onMessage;
    std::function<QString(const QString&, const QJsonObject&)> validateAction;
protected:
    TestContext m_context;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    class ReadTask;
    std::map<std::uint64_t, std::unique_ptr<ReadTask>> m_readTasks;
    struct Entry { QJsonObject defaults; Action action; TestPolicy policy; bool exitTools = false; };
    QString m_name;
    QString m_notice;
    QPointer<ColorLegendEditor> m_colorEditor;
    std::map<QString, Entry> m_entries;
    QString m_currentAction;
    std::set<std::uint64_t> m_pending;
    std::map<std::uint64_t, Completion> m_completions;
    QJsonObject m_observed;
    QTreeWidget* m_nodes = nullptr;
    QTreeWidget* m_scene = nullptr;
    QTreeWidget* m_sources = nullptr;
    QLabel* m_businessStage = nullptr;
    QLabel* m_businessNext = nullptr;
    QLabel* m_businessSource = nullptr;
    QLabel* m_businessDisplay = nullptr;
    QLabel* m_executionMessage = nullptr;
    QPushButton* m_primary = nullptr;
    QPushButton* m_viewReport = nullptr;
    BusinessStatistics* m_statistics = nullptr;
    QWidget* m_nextActions = nullptr;
    QGridLayout* m_nextLayout = nullptr;
    QStringList m_nextActionIds;
    QTabWidget* m_browser = nullptr;
    QComboBox* m_parameterTabs = nullptr;
    QComboBox* m_taskTabs = nullptr;
    QString m_taskGroup;
    QMap<QString,QString> m_taskSelections;
    QString m_parameterAction;
    QLabel* m_noticeLabel = nullptr;
    QLabel* m_operationTitle = nullptr;
    QLabel* m_operationDescription = nullptr;
    QToolButton* m_help = nullptr;
    QToolButton* m_moreOperations = nullptr;
    QMenu* m_operationMenu = nullptr;
    QWidget* m_commandArea = nullptr;
    QVBoxLayout* m_commandLayout = nullptr;
    QWidget* m_currentCommands = nullptr;
    QLabel* m_feedback = nullptr;
    std::map<QString, QWidget*> m_commandRows;
    std::map<QString, QLabel*> m_requirements;
    QLineEdit* m_nodeSearch = nullptr;
    QSet<QString> m_searchExpanded;
    bool m_isSearching = false;
    QVBoxLayout* m_actionLayout = nullptr;
    QGridLayout* m_quickLayout = nullptr;
    QWidget* m_quickActions = nullptr;
    QScrollArea* m_parameterScroll = nullptr;
    std::map<QString, QWidget*> m_cards;
    std::map<QString, ParameterEditor*> m_forms;
    std::map<QString, QPushButton*> m_buttons;
    QStringList m_actionOrder;
    QJsonObject m_node;
    QJsonObject m_lastResult;
    QString m_sourceRoot;
    QJsonArray m_sceneNodes;
    QJsonArray m_displayNodes;
    // 分别保留 Qt 隐式共享值，避免每次把大型目录嵌入一个新 JSON 对象后再比较。
    struct RefreshState {
        QJsonObject state, input, node, result, graph;
        QStringList actions;
        QString action;
        std::uint64_t busy = 0;
        bool closing = false;
    };
    RefreshState m_refreshState;
    bool m_hasRefreshed = false;
    QStringList m_visibleActions;
    bool m_isObserving = false;
    bool m_parameterFocusQueued = false;
    void QueueParameterFocus();
    void SetNode(QTreeWidgetItem* item);
    void SelectSceneItem(QTreeWidgetItem* item);
    void FilterNodes();
    void SendButton(const QString& action, const QJsonObject& context = {});
    QString GetActionGroup(const QString& action) const;
    void RefreshBusinessStep(const QJsonObject& state, const QJsonObject& input);
    void RefreshSources(const QJsonObject& graph, const QJsonObject& input);
};
HostViewTargets GetAllViews();
HostViewTargets GetMainViews();
HostViewTargets GetPartViews();
}
