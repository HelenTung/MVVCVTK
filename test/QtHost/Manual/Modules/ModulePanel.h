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

namespace Manual {
class ParameterEditor;
struct TestContext { TestSession& runtime; TestWorkflow& workflow; TestRecordWriter& records; };
class ModulePanel : public QWidget {
public:
    using Action = std::function<void(std::uint64_t, const QJsonObject&)>;
    ModulePanel(TestContext context, QString name, QWidget* parent = nullptr);
    void AttachAction(const QString& name, const QJsonObject& defaults, Action action,
        TestPolicy policy = TestPolicy::Change, bool exitTools = false);
    std::uint64_t SendAction(const QString& name, const QJsonObject& params);
    void SetComplete(std::uint64_t id, const QString& status, QJsonObject result = {});
    void SetAdmission(std::uint64_t id, bool accepted, QJsonObject raw = {});
    void SetState(const QJsonObject& value);
    void SetNotice(const QString& text);
    QString GetNotice() const { return m_notice; }
    void SendHost(std::uint64_t id, HostRequest&& request);
    void SetInput(std::uint64_t id, const DataRevisionRef& revision);
    void SetParameters(const QString& action, const QJsonObject& values);
    void SetParameterPatch(const QString& action, const QJsonObject& patch);
    QJsonObject GetParameters() const;
    ParameterEditor* GetParameterEditor(const QString& action) const;
    QJsonObject GetObservedState() const { return m_observed; }
    QString GetName() const { return m_name; }
    QString GetDisplayName() const;
    void SelectAction(const QString& action);
    bool SelectNodeGroup(const QString& id);
    bool SelectPartTarget(const QJsonObject& binding);
    void RefreshWorkflow();
    void Observe(bool refresh = true);
    bool GetHasPending() const { return !m_pending.empty(); }
    bool observeInBackground = false;
    void SetDroppedPath(const QString& path);
    QString GetCurrentAction() const { return m_currentAction; }
    QStringList GetActions() const;
    std::shared_ptr<VtkAppHostSession> GetSession() const { return m_context.runtime.GetSession(); }
    TestContext GetContext() const { return m_context; }
    std::function<void()> onObserve;
    std::function<void()> onStop;
    std::function<void(const QString&)> onMessage;
    std::function<QString(const QString&, const QJsonObject&)> validateAction;
protected:
    TestContext m_context;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    struct Entry { QJsonObject defaults; Action action; TestPolicy policy; bool exitTools = false; };
    QString m_name;
    QString m_notice;
    std::map<QString, Entry> m_entries;
    QString m_currentAction;
    std::set<std::uint64_t> m_pending;
    QJsonObject m_observed;
    QTreeWidget* m_nodes = nullptr;
    QLineEdit* m_nodeSearch = nullptr;
    QSet<QString> m_searchExpanded;
    bool m_isSearching = false;
    QVBoxLayout* m_actionLayout = nullptr;
    QGridLayout* m_quickLayout = nullptr;
    QWidget* m_quickActions = nullptr;
    QSplitter* m_parameterSplitter = nullptr;
    QScrollArea* m_parameterScroll = nullptr;
    std::map<QString, QWidget*> m_cards;
    std::map<QString, ParameterEditor*> m_forms;
    std::map<QString, QPushButton*> m_buttons;
    QStringList m_actionOrder;
    QJsonObject m_node;
    QJsonObject m_lastResult;
    QJsonArray m_sceneNodes;
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
};
HostViewTargets GetAllViews();
HostViewTargets GetMainViews();
HostViewTargets GetPartViews();
}
