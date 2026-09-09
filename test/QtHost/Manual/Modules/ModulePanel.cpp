// 测试用途：用场景节点选择对象，通过业务按钮直接分派请求并编辑紧凑参数。
#include "ModulePanel.h"
#include "Support/UiText.h"
#include "Support/ActionPolicy.h"
#include "Support/SceneNodes.h"
#include "Support/SceneGraph.h"
#include "Support/PathInput.h"
#include "Support/ParameterEditor.h"
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QSet>
#include <QHeaderView>
#include <QMenu>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QHash>
#include <QStyle>
#include <QApplication>
#include <QClipboard>
#include <QSplitter>
#include <QToolButton>
#include <QScrollBar>
#include <algorithm>
namespace Manual {
ModulePanel::ModulePanel(TestContext context, QString name, QWidget* parent)
    : QWidget(parent), m_context(context), m_name(std::move(name))
{
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12,12,12,12); layout->setSpacing(10);
    if (m_name == "Part" || m_name == "PartEdit") {
        auto* tools = new QHBoxLayout;
        m_nodeSearch = new QLineEdit(this); m_nodeSearch->setObjectName("nodeSearch");
        m_nodeSearch->setPlaceholderText("搜索零件名称或标签"); m_nodeSearch->setClearButtonEnabled(true);
        tools->addWidget(m_nodeSearch, 1);
        for (const auto& entry : {std::pair<QString,QString>{"高亮零件", "part-highlights"}, {"编辑对象", "part-edit-targets"}}) {
            auto* button = new QPushButton(entry.first, this); button->setObjectName("focus_" + entry.second);
            tools->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, entry] {
                if (!SelectNodeGroup(entry.second) && onMessage) onMessage("当前没有可定位的" + entry.first + "。");
            });
        }
        layout->addLayout(tools);
        connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { FilterNodes(); });
    }
    m_parameterSplitter = new QSplitter(Qt::Vertical, this); m_parameterSplitter->setObjectName("parameterSplitter");
    m_parameterSplitter->setChildrenCollapsible(false); m_parameterSplitter->setHandleWidth(8);
    m_parameterSplitter->setStyleSheet("QSplitter::handle:vertical { background: #cedbe8; border-top: 1px solid #b2c3d5; border-bottom: 1px solid #b2c3d5; } QSplitter::handle:vertical:hover { background: #8bb6e2; }");
    layout->addWidget(m_parameterSplitter);
    m_nodes = new SceneGraphTree(this); m_nodes->setObjectName("sceneNodes");
    m_nodes->setAlternatingRowColors(true);
    m_nodes->setMinimumHeight(100); m_nodes->setRootIsDecorated(true); m_nodes->setUniformRowHeights(true);
    if (m_name == "PartEdit") m_nodes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_nodes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_nodes->header()->setStretchLastSection(false);
    m_nodes->header()->setSectionResizeMode(1, QHeaderView::Fixed); m_nodes->header()->resizeSection(1, 170);
    m_parameterSplitter->addWidget(m_nodes);
    connect(m_nodes, &QTreeWidget::itemClicked, this, [this](auto* item, int) { SetNode(item); });
    m_nodes->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_nodes, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = m_nodes->itemAt(point); if (!item) return;
        if (!item->isSelected()) { m_nodes->clearSelection(); item->setSelected(true); }
        const auto copyText = item->text(0) + " ｜ " + item->text(1);
        m_nodes->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate); SetNode(item);
        const auto context = m_node;
        QMenu menu(this); menu.setObjectName("sceneNodeMenu");
        for (const auto v : m_node["actions"].toArray()) {
            const auto action = v.toString(); if (!m_entries.count(action)) continue;
            auto* entry = menu.addAction(GetActionText(m_name, action), this, [this, action, context] { SendButton(action, context); });
            entry->setObjectName("node_" + action);
        }
        menu.addSeparator(); menu.addAction("复制节点信息", this, [copyText] { QApplication::clipboard()->setText(copyText); });
        menu.exec(m_nodes->viewport()->mapToGlobal(point));
    });
    m_parameterScroll = new QScrollArea(m_parameterSplitter); m_parameterScroll->setObjectName("operationScroll");
    m_parameterScroll->setWidgetResizable(true); m_parameterScroll->setFrameShape(QFrame::NoFrame);
    m_parameterScroll->setMinimumHeight(180);
    auto* operations = new QWidget(m_parameterScroll); m_actionLayout = new QVBoxLayout(operations);
    m_actionLayout->setContentsMargins(0,0,8,0); m_actionLayout->setSpacing(12); m_actionLayout->setAlignment(Qt::AlignTop);
    m_quickActions = new QWidget(operations); m_quickLayout = new QGridLayout(m_quickActions);
    m_quickLayout->setContentsMargins(0,0,0,0); m_quickLayout->setSpacing(6); m_actionLayout->addWidget(m_quickActions);
    m_parameterScroll->setWidget(operations); m_parameterSplitter->addWidget(m_parameterScroll);
    m_parameterSplitter->setStretchFactor(0, 1); m_parameterSplitter->setStretchFactor(1, 2);
    m_parameterSplitter->setSizes({230, 500});
    m_parameterSplitter->handle(1)->setToolTip("上下拖动，调整场景节点与操作参数区的高度");
}
void ModulePanel::SelectSceneItem(QTreeWidgetItem* item)
{
    if (!item || m_context.workflow.GetIsClosing()) return;
    if (m_nodeSearch) m_nodeSearch->clear();
    for (auto* parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
    m_nodes->clearSelection(); m_nodes->setCurrentItem(item); item->setSelected(true);
    m_nodes->doItemsLayout();
    m_nodes->scrollToItem(item, QAbstractItemView::PositionAtCenter); SetNode(item);
}
bool ModulePanel::SelectNodeGroup(const QString& id)
{
    if (m_context.workflow.GetIsClosing()) return false;
    for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject()["id"] == id) {
        auto* item = *it;
        if (!item->childCount() && !id.startsWith("edit-preview:")) return false;
        item->setExpanded(true);
        SelectSceneItem(item->childCount() == 1 ? item->child(0) : item);
        return true;
    }
    return false;
}
bool ModulePanel::SelectPartTarget(const QJsonObject& binding)
{
    if (m_name != "Part" && m_name != "PartEdit") return false;
    Observe(); QTreeWidgetItem* target = nullptr;
    for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) {
        const auto node = (*it)->data(0, Qt::UserRole).toJsonObject();
        if (node["binding"] != binding) continue;
        if (!target) target = *it;
        if (node["id"].toString().startsWith("editing-part:")) { target = *it; break; }
    }
    if (!target) return false;
    SelectSceneItem(target); return true;
}
void ModulePanel::FilterNodes()
{
    if (!m_nodeSearch || !m_nodes) return;
    const auto query = m_nodeSearch->text().trimmed(); const bool searching = !query.isEmpty();
    if (searching && !m_isSearching) {
        m_searchExpanded.clear();
        for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) if ((*it)->isExpanded())
            m_searchExpanded.insert((*it)->data(0, Qt::UserRole).toJsonObject()["id"].toString());
    }
    const std::function<bool(QTreeWidgetItem*)> filter = [&](QTreeWidgetItem* item) {
        bool hasChild = false;
        for (int i = 0; i < item->childCount(); ++i) hasChild = filter(item->child(i)) || hasChild;
        const bool visible = !searching || hasChild || item->text(0).contains(query, Qt::CaseInsensitive);
        item->setHidden(!visible);
        if (searching && hasChild) item->setExpanded(true);
        else if (!searching && m_isSearching)
            item->setExpanded(m_searchExpanded.contains(item->data(0, Qt::UserRole).toJsonObject()["id"].toString()));
        return visible;
    };
    for (int i = 0; i < m_nodes->topLevelItemCount(); ++i) filter(m_nodes->topLevelItem(i));
    m_isSearching = searching; m_nodes->viewport()->update();
}
void ModulePanel::AttachAction(const QString& name, const QJsonObject& defaults, Action action, TestPolicy policy, bool exitTools)
{
    m_entries.emplace(name, Entry{defaults, std::move(action), policy, exitTools}); m_actionOrder.append(name);
    auto* button = new QPushButton(GetActionText(m_name, name), this); button->setObjectName("action_" + name);
    button->setMinimumHeight(30); m_buttons[name] = button;
    connect(button, &QPushButton::clicked, this, [this, name] { SendButton(name); });
    ParameterEditor* form = nullptr;
    if (!defaults.isEmpty()) { form = new ParameterEditor(m_name, name, {}, defaults, defaults, this); m_forms[name] = form; }
    if (form && form->GetHasInputs()) {
        auto* card = new QGroupBox(GetActionText(m_name, name), m_parameterScroll); card->setObjectName("card_" + name);
        auto* layout = new QVBoxLayout(card); layout->setSpacing(10); layout->setContentsMargins(12,18,12,12);
        form->onEdited = [this, name] {
            if (m_currentAction != name) { m_currentAction = name; if (onMessage) onMessage(GetDisplayName() + " · " + GetActionDescription(m_name, name)); }
            Observe();
        };
        layout->addWidget(form);
        auto* footer = new QHBoxLayout; auto* files = new QToolButton(card); files->setText("参数文件"); files->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(files); files->setMenu(menu);
        menu->addAction("导入参数", this, [this, name] {
            const auto path = QFileDialog::getOpenFileName(this, "导入参数", {}, "JSON (*.json)"); if (path.isEmpty()) return;
            try { SetParameters(name, LoadJson(path)); } catch (const std::exception& e) { SetState({{"inputError", QString::fromUtf8(e.what())}}); }
        });
        menu->addAction("导出参数", this, [this, name] {
            const auto path = QFileDialog::getSaveFileName(this, "导出参数", {}, "JSON (*.json)"); if (path.isEmpty()) return;
            try { ExportJson(path, m_forms.at(name)->GetValue().toObject()); } catch (const std::exception& e) { SetState({{"inputError", QString::fromUtf8(e.what())}}); }
        });
        menu->addAction("恢复默认", this, [this, name] { SetParameters(name, m_entries.at(name).defaults); });
        footer->addWidget(files); footer->addStretch(); footer->addWidget(button); layout->addLayout(footer);
        m_actionLayout->insertWidget(m_actionLayout->count()-1, card); m_cards[name] = card;
    } else { m_cards[name] = button; if (form) form->hide(); }
    if (m_currentAction.isEmpty()) m_currentAction = name;
    RefreshWorkflow();
    if (m_entries.size() == 1) {
        auto data = [this](const QJsonObject& params) {
            const auto ref = GetText(params, "graphRevision");
            if (m_context.workflow.getPublishedGraph) for (const auto value : m_context.workflow.getPublishedGraph()["nodes"].toArray())
                if (value.toObject()["ref"] == ref) return value.toObject();
            throw std::invalid_argument("所选发布修订已不可用，请重新选择场景节点");
        };
        AttachAction("GraphInfo", {{"graphRevision", ""}}, [this, data](auto id, const auto& params) { SetComplete(id, "Observed", data(params)); }, TestPolicy::Read);
        AttachAction("UseData", {{"graphRevision", ""}}, [this, data](auto id, const auto& params) {
            const auto node = data(params); if (!node["isVolume"].toBool()) throw std::invalid_argument("此节点不是可用作输入的体数据");
            SetInput(id, GetRef(node["ref"]));
        }, TestPolicy::Input, true);
    }
}
void ModulePanel::SelectAction(const QString& action)
{
    if (action.isEmpty()) return;
    try {
        SetParameterPatch(action, m_node["patches"].toObject()[action].toObject());
        if (onMessage) onMessage(GetDisplayName() + " · " + GetActionText(m_name, action) + " ｜ " + GetActionDescription(m_name, action));
    } catch (const std::exception& e) { SetState({{"inputError", QString::fromUtf8(e.what())}}); }
}
void ModulePanel::SendButton(const QString& action, const QJsonObject& context)
{
    try {
        auto node = context.isEmpty() ? m_node : context;
        Observe();
        bool present = node["id"] == "root" || node.isEmpty();
        QJsonArray currentParts;
        for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) {
            const auto current = (*it)->data(0, Qt::UserRole).toJsonObject();
            if (current["id"] == node["id"]) { present = true; node = current; }
            if (current["binding"].isObject()) currentParts.append(current["binding"]);
        }
        if (node["id"] == "part-selection") {
            present = true;
            for (const auto part : node["patches"].toObject()["Merge"].toObject()["parts"].toArray()) present = present && currentParts.contains(part);
        }
        if (!present) {
            const auto id = m_context.records.StartRecord(m_name, action, {}, GetDescriptor(GetSession() ? GetSession()->GetImageDescriptor() : std::optional<ImageDescriptor>{}));
            SetComplete(id, "Rejected", {{"message", "所选场景节点已变化，请重新选择对象"}}); return;
        }
        auto patch = node["patches"].toObject()[action].toObject();
        if (context.isEmpty()) {
            const auto bound = GetBoundParameters(m_name, action);
            for (const auto& key : patch.keys()) if (!bound.contains(key)) patch.remove(key);
        }
        SetParameterPatch(action, patch);
        if (onMessage) onMessage(GetDisplayName() + " · " + GetActionText(m_name, action) + " ｜ " + GetActionDescription(m_name, action));
        SendAction(action, GetParameters());
    } catch (const std::exception& e) { SetState({{"inputError", QString::fromUtf8(e.what())}}); }
}
void ModulePanel::SetNode(QTreeWidgetItem* item)
{
    const auto previousNode = m_node;
    try {
    if (!item) return; auto node = item->data(0, Qt::UserRole).toJsonObject();
    const auto selectedText = item->text(0) + " ｜ " + item->text(1);
    if (m_name == "PartEdit") {
        QJsonArray parts;
        for (auto* selected : m_nodes->selectedItems()) {
            const auto binding = selected->data(0, Qt::UserRole).toJsonObject()["binding"].toObject();
            if (!binding.isEmpty() && !parts.contains(binding)) parts.append(binding);
        }
        if (m_entries.count("Merge") && m_entries.at("Merge").defaults.contains("parts")) {
            auto patches = node["patches"].toObject(); patches["Merge"] = QJsonObject{{"parts", parts}};
            node["patches"] = patches;
        }
        if (parts.size() > 1) node = {{"id", "part-selection"}, {"actions", QJsonArray{"Merge"}}, {"patches", QJsonObject{{"Merge", QJsonObject{{"parts", parts}}}}}};
    }
    const auto patches = node["patches"].toObject();
    // 先检查整组字段，避免后面的无参/未知字段错误留下部分已切换的目标。
    for (auto it = patches.begin(); it != patches.end(); ++it) if (m_entries.count(it.key())) {
        const auto& defaults = m_entries.at(it.key()).defaults;
        for (const auto& key : it.value().toObject().keys()) {
            if (defaults.isEmpty()) throw std::invalid_argument("此操作不需要参数");
            if (!defaults.contains(key)) throw std::invalid_argument(("未知参数：" + key).toStdString());
        }
    }
    m_node = node;
    for (auto it = patches.begin(); it != patches.end(); ++it) if (m_entries.count(it.key())) SetParameterPatch(it.key(), it.value().toObject());
    if (onMessage) onMessage("选中：" + selectedText + (m_node["description"].toString().isEmpty() ? QString() : " ｜ " + m_node["description"].toString()));
    const auto actions = m_node["actions"].toArray();
    if (!actions.isEmpty() && m_entries.count(actions.first().toString())) SelectAction(actions.first().toString());
    RefreshWorkflow();
    } catch (const std::exception& e) {
        // Qt 树事件不能向事件循环抛出参数异常；错误提示不覆盖真实业务状态。
        m_node = previousNode;
        const QSignalBlocker blocker(m_nodes); m_nodes->clearSelection();
        const auto parts = m_node["patches"].toObject()["Merge"].toObject()["parts"].toArray();
        bool restored = false;
        for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) {
            const auto candidate = (*it)->data(0, Qt::UserRole).toJsonObject();
            if (candidate["id"] == m_node["id"] || (m_node["id"] == "part-selection" && parts.contains(candidate["binding"]))) {
                if (!restored) m_nodes->setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
                (*it)->setSelected(true); restored = true;
            }
        }
        SetState({{"inputError", QString::fromUtf8(e.what())}});
    }
}
void ModulePanel::RefreshWorkflow()
{
    if (m_isObserving) return;
    const TestTiming timing(m_context.records, "Refresh." + m_name);
    if (!m_nodes) return;
    const auto session = GetSession(); const auto descriptor = session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{};
    auto state = m_observed;
    if (!m_pending.empty() && !state["isBusy"].toBool()) state["isBusy"] = true;
    const auto input = GetDescriptor(descriptor);
    const auto actions = GetActions();
    const auto graph = m_context.workflow.getPublishedGraph ? m_context.workflow.getPublishedGraph() : QJsonObject();
    const auto busy = m_context.workflow.GetBusyOperation(); const bool closing = m_context.workflow.GetIsClosing();
    if (m_hasRefreshed && m_refreshState.state == state && m_refreshState.input == input
        && m_refreshState.node == m_node && m_refreshState.result == m_lastResult && m_refreshState.graph == graph && m_refreshState.action == m_currentAction
        && m_refreshState.busy == busy && m_refreshState.closing == closing
        && actions.size() == m_refreshState.actions.size() && std::equal(actions.cbegin(), actions.cend(), m_refreshState.actions.cbegin())) return;
    const auto nodes = GetSceneNodes(m_name, state, input, actions, m_lastResult, graph);
    if (nodes != m_sceneNodes) {
        const TestTiming treeTiming(m_context.records, "Tree.Update." + m_name);
        m_sceneNodes = nodes; const QSignalBlocker blocker(m_nodes);
        const auto selected = m_nodes->currentItem() ? m_nodes->currentItem()->data(0, Qt::UserRole).toJsonObject()["id"].toString() : "root";
        QSet<QString> selectedKeys;
        for (auto* item : m_nodes->selectedItems()) selectedKeys.insert(item->data(0, Qt::UserRole).toJsonObject()["id"].toString());
        const auto scroll = m_nodes->verticalScrollBar()->value();
        // 按稳定 ID 原位协调子节点；增加一个高亮投影不重建全部零件目录。
        const std::function<void(QTreeWidgetItem*, const QJsonArray&)> reconcile = [&](QTreeWidgetItem* parent, const QJsonArray& values) {
            QMap<QString,QTreeWidgetItem*> previous;
            for (int i = 0; i < parent->childCount(); ++i) {
                auto* item = parent->child(i); previous[item->data(0, Qt::UserRole).toJsonObject()["id"].toString()] = item;
            }
            for (int i = 0; i < values.size(); ++i) {
                const auto node = values[i].toObject();
                auto* item = previous.take(node["id"].toString()); const bool created = !item;
                if (!item) { item = new QTreeWidgetItem; parent->insertChild(i, item); }
                else if (parent->indexOfChild(item) != i) { parent->takeChild(parent->indexOfChild(item)); parent->insertChild(i, item); }
                if (item->data(0, Qt::UserRole).toJsonObject() != node) {
                    item->setText(0, node["title"].toString()); item->setText(1, node["status"].toString());
                    item->setToolTip(0, node["title"].toString()); item->setToolTip(1, node["status"].toString());
                    item->setData(0, Qt::UserRole, node);
                }
                reconcile(item, node["children"].toArray());
                if (created) item->setExpanded(node["id"] == "root" || (item->childCount() && node["defaultExpanded"].toBool(true)));
            }
            for (auto* item : previous) delete item;
        };
        reconcile(m_nodes->invisibleRootItem(), nodes);
        QTreeWidgetItem* selection = nullptr;
        for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) {
            const auto id = (*it)->data(0, Qt::UserRole).toJsonObject()["id"].toString();
            (*it)->setSelected(selectedKeys.contains(id)); if (id == selected) selection = *it;
        }
        if (!selection) selection = m_nodes->topLevelItem(0);
        if (selection) {
            m_nodes->setCurrentItem(selection, 0, QItemSelectionModel::NoUpdate);
            if (m_nodes->selectedItems().isEmpty()) selection->setSelected(true);
            m_node = selection->data(0, Qt::UserRole).toJsonObject();
        }
        m_nodes->verticalScrollBar()->setValue(scroll);
        static_cast<SceneGraphTree*>(m_nodes)->SetGraph(nodes);
        FilterNodes();
        if (m_name == "PartEdit") {
            QJsonArray parts;
            for (auto* item : m_nodes->selectedItems()) {
                const auto binding = item->data(0, Qt::UserRole).toJsonObject()["binding"].toObject();
                if (!binding.isEmpty() && !parts.contains(binding)) parts.append(binding);
            }
            if (parts.size() > 1) m_node = {{"id", "part-selection"}, {"actions", QJsonArray{"Merge"}}, {"patches", QJsonObject{{"Merge", QJsonObject{{"parts", parts}}}}}};
        }
    }
    QStringList allowed; for (const auto value : m_node["actions"].toArray()) allowed.append(value.toString());
    static const QHash<QString, QStringList> globalActions{{"Data", {"Load"}}, {"View", {"Set", "Reset", "Visibility"}},
        {"Crop", {"Box", "Plane", "KeepInside", "RemoveInside", "PositionOnly"}}, {"Part", {"Start"}}, {"Gap", {"Start"}},
        {"Artifact", {"Ring", "Diffusion", "Combined"}}, {"Surface", {"AutomaticIso50", "GlobalIsoPreview", "LocalAdaptiveIso50", "GradientPeak"}},
        {"Alignment", {"ImportReference"}}, {"Rotation", {"Rotate", "SetEnabled"}}};
    allowed.append(globalActions.value(m_name));
    QStringList visible;
    for (const auto& name : m_actionOrder) if (allowed.contains(name) && m_cards.at(name) == m_buttons.at(name)) visible.append(name);
    if (visible.size() != m_visibleActions.size() || !std::equal(visible.cbegin(), visible.cend(), m_visibleActions.cbegin())) {
        const TestTiming layoutTiming(m_context.records, "Layout.Rebuild." + m_name);
        while (auto* item = m_quickLayout->takeAt(0)) delete item;
        for (int i = 0; i < visible.size(); ++i) m_quickLayout->addWidget(m_buttons.at(visible[i]), i/3, i%3);
        m_visibleActions = visible;
    }
    const TestTiming actionTiming(m_context.records, "Actions." + m_name);
    for (const auto& name : m_actionOrder) {
        auto* button = m_buttons.at(name); const auto& entry = m_entries.at(name);
        const bool shown = allowed.contains(name); m_cards.at(name)->setVisible(shown);
        auto reason = GetActionRequirement(m_name, name, state, descriptor.has_value());
        if (m_context.workflow.GetBusyOperation() && entry.policy != TestPolicy::Read && entry.policy != TestPolicy::View && entry.policy != TestPolicy::Stop)
            reason = "当前任务未完成，请等待或取消。";
        // 前置条件失败仍允许点击并给出明确原因；不再把整个业务页静默锁死。
        button->setEnabled(!m_context.workflow.GetIsClosing());
        button->setToolTip(GetActionDescription(m_name, name) + (reason.isEmpty() ? "" : "\n" + reason));
        button->setProperty("selectedOperation", name == m_currentAction);
        if (m_name == "Crop" && (name == "KeepInside" || name == "RemoveInside" || name == "PositionOnly")) {
            button->setCheckable(true);
            button->setChecked(state["isActive"].toBool() && state["editMode"].toInt() == (name == "KeepInside" ? 1 : name == "RemoveInside" ? 2 : 0));
        }
    }
    m_refreshState = {state, input, m_node, m_lastResult, graph, actions, m_currentAction, busy, closing};
    m_hasRefreshed = true;
}
void ModulePanel::Observe(bool refresh)
{
    if (!onObserve || m_context.workflow.GetIsClosing() || m_isObserving) return;
    m_isObserving = true;
    try { onObserve(); } catch (...) { m_isObserving = false; throw; }
    m_isObserving = false;
    if (refresh) RefreshWorkflow();
}
void ModulePanel::dragEnterEvent(QDragEnterEvent* event)
{
    const auto urls = event->mimeData()->urls(); if (urls.size() == 1 && urls.front().isLocalFile()) event->acceptProposedAction();
}
void ModulePanel::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls(); if (urls.size() != 1 || !urls.front().isLocalFile()) return;
    SetDroppedPath(urls.front().toLocalFile()); event->acceptProposedAction();
}
void ModulePanel::SetDroppedPath(const QString& path)
{
    const QFileInfo file(path); if (!file.exists()) { SetState({{"inputError", "拖入的本地路径不存在"}}); return; }
    try {
        auto* form = GetParameterEditor(m_currentAction);
        for (const auto* key : {"filePath", "plyPath", "archivePath", "outputPath", "outputDir"}) {
            auto* field = form ? form->GetField(key) : nullptr; if (!field) continue;
            auto* input = field->findChild<QLineEdit*>("value"); if (!input) continue;
            if (QString(key) != "outputDir" && !file.isFile()) { SetState({{"inputError", "此参数需要文件，不能填入目录"}}); return; }
            input->setText(QString(key) == "outputDir" && file.isFile() ? file.absolutePath() : file.absoluteFilePath());
            if (onMessage) onMessage("已填入路径：" + input->text()); return;
        }
        QString action = m_name == "Data" ? "Load" : m_name == "Crop" ? "SetPolyData" : m_name == "Alignment" ? "ImportReference" : QString();
        if (!action.isEmpty() && m_entries.count(action)) {
            SetParameterPatch(action, {{m_name == "Crop" ? "plyPath" : "filePath", file.absoluteFilePath()}});
            if (onMessage) onMessage("已填入路径：" + file.absoluteFilePath() + "，请核对参数后点击对应业务按钮。");
        } else if (onMessage) onMessage("请将文件拖入对应的路径输入框。");
    } catch (const std::exception& e) { SetState({{"inputError", QString::fromUtf8(e.what())}}); }
}
ParameterEditor* ModulePanel::GetParameterEditor(const QString& action) const
{
    const auto it = m_forms.find(action); return it == m_forms.end() ? nullptr : it->second;
}
QJsonObject ModulePanel::GetParameters() const
{
    const auto* form = GetParameterEditor(m_currentAction); return form ? form->GetValue().toObject() : QJsonObject{};
}
void ModulePanel::SetParameters(const QString& action, const QJsonObject& values)
{
    if (!m_entries.count(action)) throw std::invalid_argument("不存在该操作");
    auto parameters = m_entries.at(action).defaults;
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!parameters.contains(it.key())) throw std::invalid_argument(("此操作不支持参数：" + GetParameterText(it.key())).toStdString());
        parameters[it.key()] = it.value();
    }
    m_currentAction = action;
    if (auto* form = GetParameterEditor(action)) {
        form->SetValue(parameters);
        Observe(false);
        QJsonObject booleans;
        for (auto it = values.begin(); it != values.end(); ++it) if (it.value().isBool() && form->GetField(it.key()) && form->GetField(it.key())->GetIsStateBound()) booleans[it.key()] = it.value();
        if (!booleans.isEmpty()) form->SetPatch(booleans);
    }
    else if (!values.isEmpty()) throw std::invalid_argument("此操作不需要参数");
    Observe(false); RefreshWorkflow();
    QueueParameterFocus();
}
void ModulePanel::SetParameterPatch(const QString& action, const QJsonObject& patch)
{
    if (!m_entries.count(action)) throw std::invalid_argument("参数目标不存在");
    m_currentAction = action;
    if (auto* form = GetParameterEditor(action)) {
        auto values = patch; QJsonObject target;
        for (const auto* key : {"viewId", "viewScope", "target"}) if (values.contains(key)) { target[key] = values.take(key); }
        if (!target.isEmpty()) { form->SetPatch(target); Observe(false); }
        if (!values.isEmpty()) form->SetPatch(values);
    }
    else if (!patch.isEmpty()) throw std::invalid_argument("此操作不需要参数");
    // 每张卡片保留自己的控件；切换或修改其他字段不读取、重建或丢弃未完成输入。
    Observe(false); RefreshWorkflow();
    QueueParameterFocus();
}
void ModulePanel::QueueParameterFocus()
{
    if (m_parameterFocusQueued) return;
    m_parameterFocusQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        if (!isVisible()) { m_parameterFocusQueued = false; return; }
        // 激活新页面布局后，待其产生的 resize/layout 事件送达再定位；只安排两次事件，不使用刷新 timer。
        m_actionLayout->activate();
        QMetaObject::invokeMethod(this, [this] {
            m_parameterFocusQueued = false;
            if (!isVisible() || m_context.workflow.GetIsClosing() || !m_cards.count(m_currentAction) || !m_cards.at(m_currentAction)->isVisible()) return;
            m_parameterScroll->verticalScrollBar()->setValue(m_cards.at(m_currentAction)->mapTo(m_parameterScroll->widget(), QPoint()).y());
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
std::uint64_t ModulePanel::SendAction(const QString& name, const QJsonObject& params)
{
    Observe();
    const auto session = GetSession();
    const auto id = m_context.records.StartRecord(m_name, name, params,
        GetDescriptor(session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{}));
    if (!session || !m_entries.count(name)) {
        SetComplete(id, "Rejected", {{"message", "会话或操作不可用"}});
        return id;
    }
    const auto& entry = m_entries.at(name);
    auto reason = GetActionRequirement(m_name, name, m_observed, session->GetImageDescriptor().has_value());
    try { if (reason.isEmpty() && validateAction) reason = validateAction(name, params); }
    catch (const std::exception& error) { SetComplete(id, "InvalidInput", {{"message", QString::fromUtf8(error.what())}}); return id; }
    if (!reason.isEmpty()) { SetComplete(id, "Rejected", {{"message", reason}}); return id; }
    if (!m_context.workflow.StartOperation(id, entry.policy, entry.exitTools, m_name)) {
        SetComplete(id, "Rejected", {{"message", "正在关闭、计算占用或交互尚未收口"}});
        return id;
    }
    m_pending.insert(id);
    try { entry.action(id, params); }
    catch (const std::exception& error) { SetComplete(id, "InvalidInput", {{"message", QString::fromUtf8(error.what())}}); }
    RefreshWorkflow();
    return id;
}
void ModulePanel::SetComplete(std::uint64_t id, const QString& status, QJsonObject result)
{
    m_pending.erase(id);
    const auto session = GetSession();
    m_context.records.SetComplete(id, status, result,
        GetDescriptor(session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{}));
    m_context.workflow.SetComplete(id);
    SetState(m_context.records.GetRecord(id));
}
void ModulePanel::SetAdmission(std::uint64_t id, bool accepted, QJsonObject raw)
{
    raw["isAccepted"] = accepted;
    m_context.records.SetAdmission(id, raw);
    if (!accepted && !m_context.records.GetRecord(id)["isTerminal"].toBool()) SetComplete(id, "Rejected", raw);
    RefreshWorkflow();
}
void ModulePanel::SetState(const QJsonObject& value)
{
    if (value.contains("operationId") && value["isTerminal"].toBool()) {
        const auto result = value["result"].toObject();
        if (result.contains("mask") || result.contains("samples") || result.contains("residuals")) m_lastResult = result;
    }
    if (!value.contains("operationId") && !value.contains("inputError")) m_observed = value;
    if (value.contains("inputError") && onMessage)
        onMessage(GetDisplayName() + " ｜ 参数错误：" + value["inputError"].toString());
    if (!value.contains("operationId") && (value["progress"].isDouble() || value["progressPercent"].isDouble())) {
        if (!value["requestId"].isString()) return;
        const double progress = value["progressPercent"].isDouble() ? value["progressPercent"].toDouble() : value["progress"].toDouble() * 100.0;
        if (!std::isfinite(progress) || progress < 0 || progress > 100) return;
        for (const auto id : m_pending) {
            const auto admission = m_context.records.GetRecord(id)["admission"].toObject();
            if (value["requestId"] != admission["requestId"]) continue;
            m_context.records.SetProgress(id, static_cast<int>(progress));
        }
    }
    RefreshWorkflow();
}
QString ModulePanel::GetDisplayName() const { return GetModuleText(m_name); }
void ModulePanel::SetNotice(const QString& text) { m_notice = text; }
QStringList ModulePanel::GetActions() const
{
    QStringList result;
    for (const auto& entry : m_entries) result.append(entry.first);
    return result;
}
void ModulePanel::SendHost(std::uint64_t id, HostRequest&& request)
{
    const auto session = GetSession();
    if (!session) { SetComplete(id, "Rejected", {{"message", "会话已停止"}}); return; }
    const QPointer<ModulePanel> owner(this);
    const bool accepted = session->SendRequestResult(std::move(request), [owner, id](HostResult result) {
        if (owner) owner->SetComplete(id, result.isSucceeded ? "Succeeded" : "Failed",
            {{"errorCode", static_cast<int>(result.errorCode)}, {"message", QString::fromStdString(result.message)}});
    });
    SetAdmission(id, accepted);
}
void ModulePanel::SetInput(std::uint64_t id, const DataRevisionRef& revision)
{
    const auto descriptor = GetSession()->GetImageDescriptor();
    if (!descriptor) throw std::invalid_argument("当前输入不存在");
    HostDataSelectRequest request;
    request.dataRevision = revision;
    request.expectedBindingRevision = descriptor->bindingRevision;
    m_context.records.SetAdmission(id, {{"target", GetRefText(revision)},
        {"expectedBindingRevision", QString::number(request.expectedBindingRevision)}});
    SendHost(id, std::move(request));
}
HostViewTargets GetAllViews()
{
    return {{"primary-3d", "composite-volume", "slice-top-down", "slice-front-back", "slice-left-right"}, {}};
}
HostViewTargets GetMainViews() { return {{"primary-3d"}, {}}; }
HostViewTargets GetPartViews() { return GetAllViews(); }
}
