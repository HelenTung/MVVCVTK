// 测试用途：用场景节点选择对象，通过业务按钮直接分派请求并编辑紧凑参数。
#include "ModulePanel.h"
#include "Support/UiText.h"
#include "Support/ActionPolicy.h"
#include "Support/SceneNodes.h"
#include "Support/CatalogNodes.h"
#include "Support/SceneGraph.h"
#include "Support/PathInput.h"
#include "Support/ParameterEditor.h"
#include "Support/BusinessStep.h"
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
#include <QTabWidget>
#include <QTabBar>
#include <QStackedWidget>
#include <algorithm>
namespace Manual {
ModulePanel::ModulePanel(TestContext context, QString name, QWidget* parent)
    : QWidget(parent), m_context(context), m_name(std::move(name))
{
    setAcceptDrops(true);
    m_statistics=new BusinessStatistics(this);m_statistics->hide();
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(10,10,10,10); layout->setSpacing(8);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(GetDisplayName(), this); title->setObjectName("moduleTitle"); title->setWordWrap(true); heading->addWidget(title,1);
    m_help = new QToolButton(this); m_help->setText("模块说明"); m_help->setCheckable(true); m_help->setObjectName("showOperationHelp"); heading->addWidget(m_help); layout->addLayout(heading);
    m_noticeLabel = new QLabel(this); m_noticeLabel->setObjectName("moduleNotice"); m_noticeLabel->setWordWrap(true); m_noticeLabel->hide(); layout->addWidget(m_noticeLabel);
    connect(m_help,&QToolButton::toggled,this,[this](bool visible){m_noticeLabel->setVisible((visible || m_entries.empty()) && !m_notice.isEmpty());});
    m_businessStage = new QLabel(this); m_businessStage->setObjectName("businessStage"); m_businessStage->setWordWrap(true); layout->addWidget(m_businessStage);
    m_businessSource = new QLabel(this); m_businessSource->setObjectName("businessInput"); m_businessSource->setWordWrap(true); layout->addWidget(m_businessSource);
    m_businessDisplay = new QLabel(this);m_businessDisplay->setObjectName("businessDisplay");m_businessDisplay->setWordWrap(true);m_businessDisplay->hide();layout->addWidget(m_businessDisplay);
    m_businessNext = new QLabel(this); m_businessNext->setObjectName("businessNext"); m_businessNext->setWordWrap(true); layout->addWidget(m_businessNext);
    m_primary = new QPushButton(this); m_primary->setObjectName("businessPrimary"); m_primary->setProperty("primaryCommand",true); m_primary->setMinimumHeight(42); layout->addWidget(m_primary);
    m_executionMessage = new QLabel(this); m_executionMessage->setObjectName("businessRecovery"); m_executionMessage->setWordWrap(true); m_executionMessage->hide(); layout->addWidget(m_executionMessage);
    m_nextActions = new QWidget(this); m_nextLayout = new QGridLayout(m_nextActions); m_nextLayout->setContentsMargins(0,0,0,0); m_nextLayout->setSpacing(4); layout->addWidget(m_nextActions);
    connect(m_primary,&QPushButton::clicked,this,[this] {
        try {
        Observe();const auto action=m_primary->property("businessAction").toString();
        if (!action.isEmpty() && m_primary->isEnabled()) SendBusinessAction(action,m_primary->property("usesOutput").toBool());
        } catch(const std::exception& e){SetState({{"inputError",QString::fromUtf8(e.what())}});}
    });
    // 计算/停止是命令按钮，标签栏只切换参数分组，不执行命令。
    m_quickActions = new QWidget(this); m_quickActions->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_quickLayout = new QGridLayout(m_quickActions); m_quickLayout->setContentsMargins(0,0,0,0); m_quickLayout->setSpacing(6);
    auto* navigation = new QHBoxLayout;
    m_parameterTabs = new QTabBar(this); m_parameterTabs->setObjectName("parameterTabs");
    navigation->addWidget(new QLabel("常用操作", this),1);
    m_parameterTabs->setExpanding(false); m_parameterTabs->setUsesScrollButtons(true); m_parameterTabs->setElideMode(Qt::ElideNone); m_parameterTabs->setDrawBase(false);
    m_moreOperations = new QToolButton(this); m_moreOperations->setObjectName("moreOperations"); m_moreOperations->setText("全部操作");
    m_moreOperations->setPopupMode(QToolButton::InstantPopup); m_operationMenu = new QMenu(m_moreOperations); m_moreOperations->setMenu(m_operationMenu);
    navigation->addWidget(m_moreOperations); layout->addLayout(navigation); layout->addWidget(m_parameterTabs);
    connect(m_parameterTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0) SelectAction(m_parameterTabs->tabData(index).toString());
    });
    m_operationTitle = new QLabel(this); m_operationTitle->setObjectName("operationTitle"); m_operationTitle->setWordWrap(true);
    m_operationDescription = new QLabel(this); m_operationDescription->setObjectName("operationDescription"); m_operationDescription->setWordWrap(true);
    m_browser = new QTabWidget(this); m_browser->setObjectName("objectBrowser");
    auto* catalogPage = new QWidget(m_browser); auto* catalogLayout = new QVBoxLayout(catalogPage);
    catalogLayout->setContentsMargins(4, 6, 4, 4);
    m_scene = new QTreeWidget(m_browser); m_scene->setObjectName("sceneNodes");
    m_scene->setColumnCount(2); m_scene->setHeaderLabels({"场景对象", "显示状态"});
    m_scene->setUniformRowHeights(true); m_scene->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_scene->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_browser->addTab(m_scene, "对象"); m_browser->addTab(catalogPage, "历史与目录");
    m_sources = new QTreeWidget(m_browser); m_sources->setObjectName("businessSources"); m_sources->setHeaderLabels({"实际来源关系", "精确修订"});
    m_sources->header()->setSectionResizeMode(0,QHeaderView::Stretch); m_browser->addTab(m_sources,"来源");
    connect(m_browser,&QTabWidget::currentChanged,this,[this](int index) {
        if(index==2) RefreshSources(m_context.workflow.getPublishedGraph ? m_context.workflow.getPublishedGraph() : QJsonObject(),GetDescriptor(GetSession()->GetImageDescriptor()));
    });
    if (m_name == "Part") {
        m_nodeSearch = new QLineEdit(this); m_nodeSearch->setObjectName("nodeSearch");
        m_nodeSearch->setPlaceholderText("搜索零件名称或标签"); m_nodeSearch->setClearButtonEnabled(true);
        catalogLayout->addWidget(m_nodeSearch);
        auto* tools = new QHBoxLayout;
        for (const auto& entry : {std::pair<QString,QString>{"高亮零件", "part-highlights"}, {"编辑对象", "part-edit-targets"}}) {
            auto* button = new QPushButton(entry.first, this); button->setObjectName("focus_" + entry.second);
            tools->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, entry] {
                if (!SelectNodeGroup(entry.second) && onMessage) onMessage("当前没有可定位的" + entry.first + "。");
            });
        }
        catalogLayout->addLayout(tools);
        connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { FilterNodes(); });
    }
    m_nodes = new SceneGraphTree(this); m_nodes->setObjectName("resultCatalog");
    m_nodes->setAlternatingRowColors(true);
    m_nodes->setMinimumHeight(100); m_nodes->setRootIsDecorated(true); m_nodes->setUniformRowHeights(true);
    if (m_name == "Part") m_nodes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_nodes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_nodes->header()->setStretchLastSection(false);
    m_nodes->header()->setSectionResizeMode(1, QHeaderView::Interactive); m_nodes->header()->resizeSection(1, 76);
    catalogLayout->addWidget(m_nodes);
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
            entry->setEnabled(m_buttons.at(action)->isEnabled());
            entry->setToolTip(m_buttons.at(action)->toolTip());
        }
        menu.addSeparator(); menu.addAction("复制节点信息", this, [copyText] { QApplication::clipboard()->setText(copyText); });
        menu.exec(m_nodes->viewport()->mapToGlobal(point));
    });
    m_parameterScroll = new QScrollArea(this); m_parameterScroll->setObjectName("operationScroll");
    m_parameterScroll->setWidgetResizable(true); m_parameterScroll->setFrameShape(QFrame::NoFrame);
    m_parameterScroll->setMinimumHeight(96);
    auto* operations = new QWidget(m_parameterScroll); m_actionLayout = new QVBoxLayout(operations);
    m_actionLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    m_actionLayout->setContentsMargins(0,0,8,0); m_actionLayout->setSpacing(12); m_actionLayout->setAlignment(Qt::AlignTop);
    m_actionLayout->addWidget(m_operationTitle);m_actionLayout->addWidget(m_operationDescription);
    m_parameterScroll->setWidget(operations); layout->addWidget(m_parameterScroll,1);
    m_commandArea = new QStackedWidget(this); m_commandArea->setObjectName("operationCommands");
    m_commandArea->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum); m_actionLayout->addWidget(m_commandArea);
    layout->addWidget(m_quickActions);
    connect(m_scene, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        const auto id = item->data(0, Qt::UserRole).toJsonObject()["id"];
        for (QTreeWidgetItemIterator it(m_nodes); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toJsonObject()["id"] != id) continue;
            m_nodes->clearSelection(); m_nodes->setCurrentItem(*it); (*it)->setSelected(true); SetNode(*it); return;
        }
    });
}
QWidget* ModulePanel::GetBrowser() const { return m_browser; }
ModulePanel::~ModulePanel(){m_statistics->Cancel();delete m_statistics;}

void ModulePanel::SelectSceneItem(QTreeWidgetItem* item)
{
    if (!item || m_context.workflow.GetIsClosing()) return;
    m_browser->setCurrentIndex(1);
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
    if (m_name != "Part") return false;
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
        auto* card = new QGroupBox(GetParameterSectionText(m_name, name), m_parameterScroll); card->setObjectName("card_" + name); card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* layout = new QVBoxLayout(card); layout->setSpacing(10); layout->setContentsMargins(12,18,12,12);
        form->onEdited = [this, name] {
            if(name=="GraphInfo")m_sourceRoot.clear();
            m_parameterAction=name;
            if (m_currentAction != name) { m_currentAction = name; if (onMessage) onMessage(GetDisplayName() + " · " + GetActionDescription(m_name, name)); }
            Observe();
        };
        layout->addWidget(form);
        auto* commands = new QWidget(m_commandArea); commands->setObjectName("commands_" + name);
        auto* commandLayout = new QVBoxLayout(commands); commandLayout->setContentsMargins(0,8,0,0); commandLayout->setSpacing(6);
        auto* requirement = new QLabel(commands); requirement->setObjectName("commandRequirement"); requirement->setWordWrap(true);
        requirement->setMinimumWidth(0); requirement->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        commandLayout->addWidget(requirement); m_requirements[name] = requirement;
        auto* footer = new QHBoxLayout; footer->setContentsMargins(0,0,0,0); commandLayout->addLayout(footer);
        auto* files = new QToolButton(commands); files->setText("参数文件"); files->setPopupMode(QToolButton::InstantPopup);
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
        button->setProperty("primaryCommand",false);
        footer->addWidget(files); footer->addStretch(); footer->addWidget(button);
        m_commandArea->addWidget(commands); m_commandRows[name] = commands;
        m_actionLayout->insertWidget(m_actionLayout->count()-1,card); m_cards[name] = card;
        const QSignalBlocker blocker(m_parameterTabs);
        const auto index = m_parameterTabs->addTab(GetParameterSectionText(m_name, name)); m_parameterTabs->setTabData(index, name);
        if (m_parameterAction.isEmpty()) m_parameterAction = name;
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
        const auto complete = [this](auto id, const DataCommitResult& result) {
            QJsonArray blockers;
            for (const auto& blocker : result.blockers) blockers.append(QString::fromStdString(blocker.owner));
            SetComplete(id,result.status==DataCommitStatus::Succeeded?"Succeeded":"Failed",
                {{"message",QString::fromStdString(result.message)},{"blockers",blockers}});
        };
        for (const auto& action : {QString("KeepResult"),QString("ReleaseResult")})
            AttachAction(action,{{"graphRevision",""}},[this,data,complete,action](auto id,const auto& params) {
                const auto ref=GetRef(data(params)["ref"]);
                complete(id,m_context.workflow.setResultRetained(ref,action=="KeepResult"));
            },TestPolicy::Change);
        AttachAction("UnloadResult",{{"graphRevision",""}},[this,data,complete](auto id,const auto& params) {
            const auto node=data(params); const auto ref=GetRef(node["ref"]);
            if (node["producer"]=="org.mvvcvtk.host") {
                HostDataUnloadRequest request; request.dataRevision=ref; SendHost(id,std::move(request));
            } else complete(id,m_context.workflow.unloadResult(ref));
        },TestPolicy::Input,true);
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
    if (m_name == "Part") {
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
    m_sourceRoot.clear();
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
    ParameterChoices sources, labels, meshes;
    for (const auto value : graph["nodes"].toArray()) {
        const auto node = value.toObject(); const auto ref = node["ref"].toString();
        const auto label = QString("%1 · #%2 · %3").arg(node["producer"].toString("数据"), node["order"].toString(), ref.right(10));
        if (node["isVolume"].toBool()) sources.append({ref,label});
        if (node["type"].toString().contains("label",Qt::CaseInsensitive)) labels.append({ref,label});
        if (node["type"] == "org.mvvcvtk.surface-mesh") meshes.append({ref,label});
    }
    for (const auto& form : m_forms) {
        if (auto* field = form.second->GetField("source")) field->SetReferenceChoices(sources);
        if (auto* field = form.second->GetField("labels")) field->SetReferenceChoices(labels);
        if (auto* field = form.second->GetField("mesh")) field->SetReferenceChoices(meshes);
    }
    const auto busy = m_context.workflow.GetBusyOperation(); const bool closing = m_context.workflow.GetIsClosing();
    RefreshBusinessStep(state,input);
    if (m_browser->currentIndex()==2) RefreshSources(graph,input);
    if (m_hasRefreshed && m_refreshState.state == state && m_refreshState.input == input
        && m_refreshState.node == m_node && m_refreshState.result == m_lastResult && m_refreshState.graph == graph && m_refreshState.action == m_currentAction
        && m_refreshState.busy == busy && m_refreshState.closing == closing
        && actions.size() == m_refreshState.actions.size() && std::equal(actions.cbegin(), actions.cend(), m_refreshState.actions.cbegin())) return;
    const auto displayNodes = GetSceneNodes(m_name, state, input, actions);
    if (displayNodes != m_displayNodes) {
        m_displayNodes = displayNodes;
        const auto selected = m_scene->currentItem() ? m_scene->currentItem()->data(0, Qt::UserRole).toJsonObject()["id"].toString() : "root";
        const auto scroll = m_scene->verticalScrollBar()->value();
        const QSignalBlocker blocker(m_scene);
        // 场景对象按稳定身份更新，选择/展开与目录完全独立。
        const std::function<void(QTreeWidgetItem*, const QJsonArray&)> update = [&](QTreeWidgetItem* parent, const QJsonArray& values) {
            QMap<QString, QTreeWidgetItem*> old;
            for (int i = 0; i < parent->childCount(); ++i) old[parent->child(i)->data(0, Qt::UserRole).toJsonObject()["id"].toString()] = parent->child(i);
            for (int i = 0; i < values.size(); ++i) {
                const auto node = values[i].toObject(); auto* item = old.take(node["id"].toString());
                const bool created = !item;
                if (!item) { item = new QTreeWidgetItem; parent->insertChild(i, item); }
                else if (parent->indexOfChild(item) != i) { parent->takeChild(parent->indexOfChild(item)); parent->insertChild(i, item); }
                item->setText(0, node["title"].toString()); item->setText(1, node["status"].toString()); item->setData(0, Qt::UserRole, node);
                update(item, node["children"].toArray());
                if (created) item->setExpanded(true);
                if (node["id"] == selected) m_scene->setCurrentItem(item);
            }
            for (auto* item : old) delete item;
        };
        update(m_scene->invisibleRootItem(), displayNodes); m_scene->verticalScrollBar()->setValue(scroll);
    }
    const auto nodes = GetCatalogNodes(m_name, state, input, actions, m_lastResult, graph);
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
        if (m_name == "Part") {
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
        {"Artifact", {"Ring", "Diffusion", "Combined"}}, {"Surface", {"GlobalAutomatic"}},
        {"Wall", {"Start", "Cancel", "Result", "Clear"}}, {"Alignment", {"ImportReference"}}, {"Rotation", {"Rotate", "SetEnabled"}}};
    allowed.append(globalActions.value(m_name));
    for (const auto& entry : m_entries) if (entry.second.policy == TestPolicy::Stop) allowed.append(entry.first);
    static const QHash<QString, QStringList> primaryPages{
        {"Data",{"Load","Select","ExportData"}}, {"View",{"Set","Visibility","Cursor"}},
        {"Part",{"Start","SetState","Paint","Split"}}, {"Surface",{"GlobalAutomatic","Visibility","SamplePoints"}},
        {"Wall",{"Start","SetDisplay","SetEvaluation","Result"}}, {"Artifact",{"Ring","Diffusion","Combined"}},
        {"Alignment",{"ImportReference","SaveRecipe","Start","Result"}}, {"Crop",{"Start","Mode","BuildResult","AppendExact"}},
        {"Roi",{"Begin","SetDraft"}}, {"Rotation",{"Rotate","SetEnabled"}}, {"Gap",{"Start","SetDisplay"}}};
    QStringList pages;
    for (const auto& name : primaryPages.value(m_name)) if (m_commandRows.count(name)) pages.append(name);
    if (m_commandRows.count(m_parameterAction) && !pages.contains(m_parameterAction)) pages.append(m_parameterAction);
    if (pages.isEmpty()) for (const auto& name : m_actionOrder) if (m_commandRows.count(name)) { pages.append(name); break; }
    {
        const QSignalBlocker blocker(m_parameterTabs);
        QStringList previous;
        for (int i = 0; i < m_parameterTabs->count(); ++i) previous.append(m_parameterTabs->tabData(i).toString());
        if (previous.size() != pages.size() || !std::equal(previous.cbegin(), previous.cend(), pages.cbegin())) {
            while (m_parameterTabs->count()) m_parameterTabs->removeTab(0);
            for (const auto& name : pages) {
                auto title = GetParameterSectionText(m_name,name); if (title.endsWith("参数")) title.chop(2);
                const auto index = m_parameterTabs->addTab(title); m_parameterTabs->setTabData(index,name);
                m_parameterTabs->setTabToolTip(index,GetActionText(m_name,name));
            }
        }
        int active = -1;
        for (int i = 0; i < m_parameterTabs->count(); ++i) {
            const auto name = m_parameterTabs->tabData(i).toString();
            m_parameterTabs->setTabEnabled(i, !closing);
            if (name == m_parameterAction) active = i;
        }
        m_parameterTabs->setCurrentIndex(active); m_parameterTabs->setVisible(!pages.isEmpty());
    }
    static const QHash<QString, QStringList> quickCommands{
        {"Data", {"Descriptor"}}, {"View", {"State"}}, {"Roi", {"Commit", "Cancel"}},
        {"Crop", {"FinishEditing", "Exit"}}, {"Part", {"Commit", "Discard", "Stop", "EditSelected"}},
        {"Surface", {"OpenAlignment", "Stop"}}, {"Artifact", {"Commit", "Discard", "Cancel"}},
        {"Alignment", {"Result", "Cancel"}}, {"Wall", {"Result", "Cancel"}},
        {"Rotation", {"Cancel"}}, {"Gap", {"Overlay", "Exit"}}};
    QStringList visible;
    for (const auto& name : quickCommands.value(m_name))
        if (m_entries.count(name) && m_cards.at(name) == m_buttons.at(name)) visible.append(name);
    if (visible.size() != m_visibleActions.size() || !std::equal(visible.cbegin(), visible.cend(), m_visibleActions.cbegin())) {
        const TestTiming layoutTiming(m_context.records, "Layout.Rebuild." + m_name);
        while (auto* item = m_quickLayout->takeAt(0)) delete item;
        for (int i = 0; i < visible.size(); ++i) m_quickLayout->addWidget(m_buttons.at(visible[i]), i/2, i%2);
        m_visibleActions = visible;
    }
    const TestTiming actionTiming(m_context.records, "Actions." + m_name);
    for (const auto& name : m_actionOrder) {
        auto* button = m_buttons.at(name); const auto& entry = m_entries.at(name);
        const bool shown = m_cards.at(name) == button ? visible.contains(name) : name == m_parameterAction;
        m_cards.at(name)->setVisible(shown);
        auto reason = GetActionRequirement(m_name, name, state, descriptor.has_value());
        if (m_context.workflow.GetBusyOperation() && entry.policy != TestPolicy::Read && entry.policy != TestPolicy::View && entry.policy != TestPolicy::Stop)
            reason = "当前任务未完成，请等待或取消。";
        if (!allowed.contains(name) && reason.isEmpty()) reason = "请在左侧选择适用对象，或先完成所需步骤。";
        if (closing) reason = "测试会话正在关闭。";
        if (m_requirements.count(name)) {
            m_requirements.at(name)->setText(reason); m_requirements.at(name)->setVisible(!reason.isEmpty());
        }
        // 按业务能力禁用命令并说明原因；参数页仍可浏览，后台完成后自动恢复。
        button->setEnabled(!m_context.workflow.GetIsClosing() && reason.isEmpty());
        button->setToolTip(GetActionDescription(m_name, name) + (reason.isEmpty() ? "" : "\n" + reason));
        button->setProperty("selectedOperation", name == m_currentAction);
        if (m_name == "Crop" && (name == "KeepInside" || name == "RemoveInside" || name == "PositionOnly")) {
            button->setCheckable(true);
            button->setChecked(state["isActive"].toBool() && state["editMode"].toInt() == (name == "KeepInside" ? 1 : name == "RemoveInside" ? 2 : 0));
        }
    }
    if (m_commandRows.count(m_parameterAction)) {
        m_commandArea->setCurrentWidget(m_commandRows.at(m_parameterAction)); m_commandArea->show();
    } else m_commandArea->hide();
    m_quickActions->setVisible(!visible.isEmpty());
    m_moreOperations->setVisible(!m_entries.empty());
    m_noticeLabel->setVisible((m_help->isChecked() || m_entries.empty()) && !m_notice.isEmpty());
    m_parameterScroll->setVisible(!m_commandRows.empty());
    m_operationTitle->setText(GetActionText(m_name, m_parameterAction));
    m_operationDescription->setText(GetActionDescription(m_name, m_parameterAction));
    m_operationTitle->setVisible(!m_parameterAction.isEmpty()); m_operationDescription->setVisible(!m_parameterAction.isEmpty());
    // QMenu::clear 不负责销毁子菜单；刷新前显式回收原分类容器。
    const auto oldMenus = m_operationMenu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    m_operationMenu->clear(); qDeleteAll(oldMenus);
    QMap<QString, QMenu*> menus;
    for (const auto& name : m_actionOrder) {
        const auto group = GetActionGroup(name);
        if (!menus.contains(group)) menus[group] = m_operationMenu->addMenu(group);
        const bool parameter = m_commandRows.count(name) != 0;
        auto* entry = menus[group]->addAction(GetActionText(m_name,name),this,[this,name,parameter]{
            QMetaObject::invokeMethod(this,[this,name,parameter]{
                if (m_context.workflow.GetIsClosing()) return;
                if (parameter) SelectAction(name); else m_buttons.at(name)->click();
            },Qt::QueuedConnection);
        });
        entry->setObjectName((parameter ? "navigate_" : "command_") + name);
        entry->setCheckable(parameter); entry->setChecked(parameter && name == m_parameterAction);
        entry->setEnabled(parameter ? !closing : m_buttons.at(name)->isEnabled()); entry->setToolTip(m_buttons.at(name)->toolTip());
    }
    m_refreshState = {state, input, m_node, m_lastResult, graph, actions, m_currentAction, busy, closing};
    m_hasRefreshed = true;
}
QString ModulePanel::GetActionGroup(const QString& action) const
{
    if (m_name == "Alignment") {
        if (action.startsWith("Export") && action.endsWith("Template")) return "参考模板";
        if (QStringList{"ImportReference", "SaveRecipe", "Activate", "Deactivate"}.contains(action)) return "参考准备";
        if (QStringList{"Start", "ApplyModelPose", "Cancel"}.contains(action)) return "对齐与应用";
        if (QStringList{"InspectSection", "InspectGeometry", "InspectPair", "InspectionResult"}.contains(action)) return "尺寸检测";
        if (QStringList{"ImportNominalMesh", "EvaluateSurface", "DeviationResult", "SetDeviationDisplay"}.contains(action)) return "表面偏差";
        if (QStringList{"Visibility", "Result", "SetSceneDisplay"}.contains(action)) return "显示与结果";
        if (QStringList{"EvaluateVoxelVolume", "VoxelVolumeResult"}.contains(action)) return "体素统计";
        if (QStringList{"ExportArchive", "ExportReport", "Restore"}.contains(action)) return "报告与归档";
    }
    if (m_name == "Crop") {
        if (QStringList{"Start", "FinishEditing", "BuildResult", "SelectOutput", "Exit", "ResetPreview"}.contains(action)) return "预览与发布";
        if (QStringList{"Mode", "AppendExact", "Box", "Plane", "Sphere", "Cylinder", "KeepInside", "RemoveInside", "PositionOnly", "SetPolyData", "ClearPolyData"}.contains(action)) return "裁剪工具";
        if (action == "SaveRoi") return "数据与导出";
        if (action != "GraphInfo" && action != "UseData" && action != "KeepResult" && action != "ReleaseResult" && action != "UnloadResult") return "文档与历史";
    }
    if (QStringList{"GraphInfo", "UseData", "KeepResult", "ReleaseResult", "UnloadResult", "Select", "SelectOutput", "Descriptor", "ResultEvidence", "LabelDescriptors", "ReadTransform", "ReadRoi"}.contains(action) || action.startsWith("Export")) return "数据与导出";
    if (QStringList{"Commit", "Discard", "Cancel", "Stop", "Exit"}.contains(action)) return "确认与退出";
    if (QStringList{"SetDisplay", "Visibility", "State", "Overlay", "Result", "SetActive", "CopyIsoToDisplay", "CopyIsoToGap", "CopyIsoToPart", "OpenAlignment"}.contains(action)) return "显示与结果";
    if (QStringList{"Paint", "Erase", "Fill", "Island", "Grow", "Undo", "Redo", "Split", "Merge", "EditSelected", "Catalog"}.contains(action)) return "对象与编辑";
    return m_commandRows.count(action) ? "参数操作" : "其他命令";
}
void ModulePanel::Observe(bool refresh)
{
    if (!onObserve || m_context.workflow.GetIsClosing() || m_isObserving) return;
    m_isObserving = true;
    try { onObserve(); } catch (...) { m_isObserving = false; throw; }
    m_isObserving = false;
    if (refresh) RefreshWorkflow();
    if (refresh) RefreshStatistics();
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
        if (form->GetHasInputs()) m_parameterAction = action;
        form->SetValue(parameters);
        Observe(false);
        // 观察先同步外部模式/目标上下文，再保留调用者本次明确提供的草稿。
        if (!values.isEmpty()) form->SetPatch(values);
    }
    else if (!values.isEmpty()) throw std::invalid_argument("此操作不需要参数");
    Observe(false); RefreshWorkflow();
    QueueParameterFocus();
}
void ModulePanel::SetParameterPatch(const QString& action, const QJsonObject& patch)
{
    if (!m_entries.count(action)) throw std::invalid_argument("参数目标不存在");
    if(action=="GraphInfo" && patch.contains("graphRevision"))m_sourceRoot.clear();
    m_currentAction = action;
    if (auto* form = GetParameterEditor(action)) {
        if (form->GetHasInputs()) m_parameterAction = action;
        auto values = patch; QJsonObject target;
        for (const auto* key : {"viewId", "viewScope", "target"}) if (values.contains(key)) { target[key] = values.take(key); }
        if (!target.isEmpty()) form->SetPatch(target);
        // 即使没有显式 target，外部输入/工具栏也可能已改变观察上下文。
        Observe(false);
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
            if (!isVisible() || m_context.workflow.GetIsClosing() || !m_cards.count(m_parameterAction) || !m_cards.at(m_parameterAction)->isVisible()) return;
            m_parameterScroll->verticalScrollBar()->setValue(m_cards.at(m_parameterAction)->mapTo(m_parameterScroll->widget(), QPoint()).y());
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
std::uint64_t ModulePanel::SendAction(const QString& name, const QJsonObject& params, Completion onComplete)
{
    Observe();
    const auto session = GetSession();
    const auto id = m_context.records.StartRecord(m_name, name, params,
        GetDescriptor(session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{}));
    if (onComplete) m_completions.emplace(id,std::move(onComplete));
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
    if(QStringList{"Failed","Rejected","InvalidInput","SourceChanged"}.contains(status)) {
        m_executionMessage->setText("上次操作 · "+result["message"].toString("操作未完成，请检查输入与前置条件。")); m_executionMessage->show();
    } else if(status!="Observed" && status!="ParametersCopied") { m_executionMessage->clear(); m_executionMessage->hide(); }
    SetState(m_context.records.GetRecord(id));
    const auto found=m_completions.find(id);
    if(found!=m_completions.end()) {
        auto complete=std::move(found->second); m_completions.erase(found);
        QMetaObject::invokeMethod(this,[complete=std::move(complete),status,result]{complete(status,result);},Qt::QueuedConnection);
    }
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
void ModulePanel::SetNotice(const QString& text)
{
    m_notice = text; m_noticeLabel->setText(text); m_help->setToolTip(text);
    m_noticeLabel->setVisible((m_help->isChecked() || m_entries.empty()) && !text.isEmpty());
    if (m_entries.empty()) {
        m_parameterScroll->hide(); m_parameterTabs->hide(); m_commandArea->hide();
        m_quickActions->hide(); m_moreOperations->hide(); m_operationTitle->hide(); m_operationDescription->hide();
    }
}
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
    const auto admission = session->SendRequest(std::move(request), [owner, id](HostResult result) {
        if (owner) owner->SetComplete(id, result.isSucceeded ? "Succeeded" : "Failed",
            {{"errorCode", static_cast<int>(result.errorCode)}, {"message", QString::fromStdString(result.message)}});
    });
    SetAdmission(id, admission.status == HostAdmissionStatus::Accepted,
        {{"errorCode", static_cast<int>(admission.errorCode)}, {"message", QString::fromStdString(admission.message)}});
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
    return {{"primary-3d", "slice-top-down", "slice-front-back", "slice-left-right"}, {}};
}
HostViewTargets GetMainViews() { return {{"primary-3d"}, {}}; }
HostViewTargets GetPartViews() { return GetAllViews(); }
}
