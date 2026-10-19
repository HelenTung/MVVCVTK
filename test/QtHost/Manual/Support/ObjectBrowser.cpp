#include "ObjectBrowser.h"
#include "UiText.h"
#include <QTreeWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSet>
namespace Manual {
ObjectBrowser::ObjectBrowser(QWidget* parent) : QWidget(parent)
{
    setObjectName("sharedObjectBrowser");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
    m_search = new QLineEdit(this); m_search->setObjectName("objectSearch");
    m_search->setPlaceholderText("搜索名称或修订"); m_search->setAccessibleName("搜索对象"); m_search->setClearButtonEnabled(true); layout->addWidget(m_search);
    m_tree = new QTreeWidget(this); m_tree->setObjectName("workbenchObjects");
    m_tree->setHeaderLabels({"对象", "状态"}); m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true); m_tree->setIndentation(14); m_tree->setAccessibleName("对象与结果目录");
    m_tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1,QHeaderView::Interactive); m_tree->header()->resizeSection(1,90); layout->addWidget(m_tree,1);
    m_summary = new QLabel("单击只选中；显隐和采用通过独立操作。",this);
    m_summary->setObjectName("selectedObjectSummary"); m_summary->setWordWrap(true); layout->addWidget(m_summary);
    m_summary->setTextFormat(Qt::PlainText); m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_use = new QPushButton("设为工作输入",this); m_use->setObjectName("useSelectedObject"); m_use->setEnabled(false); layout->addWidget(m_use);
    auto* visibility=new QHBoxLayout;m_show=new QPushButton("显示",this);m_hide=new QPushButton("隐藏",this);
    m_show->setObjectName("showSelectedObject");m_hide->setObjectName("hideSelectedObject");visibility->addWidget(m_show);visibility->addWidget(m_hide);layout->addLayout(visibility);m_show->setEnabled(false);m_hide->setEnabled(false);
    for(const auto& entry:{std::pair<QPushButton*,bool>{m_show,true},{m_hide,false}})connect(entry.first,&QPushButton::clicked,this,[this,visible=entry.second] {
        if(!m_tree->currentItem())return;const auto node=m_tree->currentItem()->data(0,Qt::UserRole).toJsonObject();
        SendObjectAction(node["id"].toString(),node["isVolume"].toBool()?"SetVolumeVisible":"Visibility",{{"isVisible",visible}});
    });
    connect(m_search,&QLineEdit::textChanged,this,[this]{FilterObjects();});
    connect(m_tree,&QTreeWidget::currentItemChanged,this,[this]{SetSelection();});
    connect(m_use,&QPushButton::clicked,this,[this] {
        if (!m_tree->currentItem() || !onAction) return;
        const auto node=m_tree->currentItem()->data(0,Qt::UserRole).toJsonObject();
        if(node["isVolume"].toBool()) SendObjectAction(node["id"].toString(),"UseData");
    });
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree,&QTreeWidget::customContextMenuRequested,this,[this](const QPoint& point) {
        auto* item=m_tree->itemAt(point); if(!item) return; m_tree->setCurrentItem(item);
        const auto node=item->data(0,Qt::UserRole).toJsonObject(); QMenu menu(this);
        const auto ref=node["ref"].toString();
        if(!ref.isEmpty())menu.addAction("查看报告",this,[this,id=node["id"].toString()] {
            for(const auto group:m_objects)for(const auto value:group.toObject()["children"].toArray()) {
                const auto current=value.toObject();if(current["id"]==id && onReport) {
                    onReport(current["isVolume"].toBool()?current["ref"].toString():QString(),current["isVolume"].toBool()?QString():current["ref"].toString());return;
                }
            }
        });
        if(node["isVolume"].toBool() && node["actions"].toArray().contains("SetVolumeVisible"))for(const bool visible:{true,false})
            menu.addAction(visible?"显示当前三维体数据":"隐藏当前三维体数据",this,[this,id=node["id"].toString(),visible]{SendObjectAction(id,"SetVolumeVisible",{{"isVisible",visible}});});
        for(const auto value:node["actions"].toArray()) {
            const auto action=value.toString(),module=node["module"].toString();
            if(action=="SetVolumeVisible")continue;
            if(action=="Visibility") {
                for(const bool visible:{true,false})menu.addAction(visible?"显示此对象":"隐藏此对象",this,[this,id=node["id"].toString(),visible] {
                    SendObjectAction(id,"Visibility",{{"isVisible",visible}});
                });
                continue;
            }
            menu.addAction(GetActionText(module,action),this,[this,id=node["id"].toString(),action] {
                SendObjectAction(id,action);
            });
        }
        if(!menu.isEmpty()) menu.exec(m_tree->viewport()->mapToGlobal(point));
    });
}
void ObjectBrowser::SetObjects(const QJsonObject& graph,const QJsonArray& regions,
    const QJsonArray& moduleNodes,const QString& current,const QStringList& roiActions,std::optional<double> currentOpacity)
{
    QJsonArray groups;
    for(const auto* title:{"数据","范围","分析与结果","参考"})
        groups.append(QJsonObject{{"id",QString(title)},{"title",QString(title)},{"children",QJsonArray{}}});
    const auto append=[&](int group,QJsonObject node) {
        auto category=groups[group].toObject(); auto children=category["children"].toArray();
        children.append(node); category["children"]=children; groups[group]=category;
    };
    QSet<QString> represented;
    for(const auto value:regions) {
        const auto roi=value.toObject(); const auto ref=roi["roi"].toString(); represented.insert(ref);
        QJsonArray actions;
        for(const auto* action:{"Edit","Visibility","Copy","SetMetadata","OpenCrop","OpenGap"}) {
            if(!roiActions.contains(action))continue;
            if((QString(action)=="OpenCrop" || QString(action)=="OpenGap") && (roi["isArchived"].toBool() || roi["source"]!=current))continue;
            actions.append(QString(action));
        }
        append(1,{{"id","roi:"+ref},{"title",roi["name"]},{"status",roi["isArchived"].toBool()?"已归档":"正式范围"},
            {"ref",ref},{"module","Roi"},{"source",roi["source"]},{"actions",actions},
            {"patches",QJsonObject{{"Edit",QJsonObject{{"roi",ref}}},{"Visibility",QJsonObject{{"roi",ref}}},{"Copy",QJsonObject{{"roi",ref}}},
                {"SetMetadata",QJsonObject{{"roi",ref},{"name",roi["name"]},{"group",roi["group"]},{"description",roi["description"]},{"isArchived",roi["isArchived"]}}},
                {"OpenCrop",QJsonObject{{"roi",ref}}},{"OpenGap",QJsonObject{{"roi",ref}}}}}});
    }
    for(const auto value:moduleNodes) {
        auto node=value.toObject(); const auto id=node["id"].toString(); const auto module=node["module"].toString();
        const bool reference=id.startsWith("alignment-reference:") || id.startsWith("alignment-recipe:");
        const bool result=id.startsWith("wall-result:") || id.startsWith("gap-result:") || id.startsWith("alignment-result:") || id.startsWith("surface:");
        const bool candidate=id.startsWith("artifact-candidate:") || id.startsWith("edit-preview:") || id.startsWith("surface-preview:");
        if(!reference && !result && !candidate && !id.startsWith("part:")) continue;
        const auto ref=id.section(':',1); represented.insert(ref); node["ref"]=ref;
        node["children"]=QJsonArray{}; append(reference?3:2,node);
    }
    for(const auto value:graph["nodes"].toArray()) {
        const auto data=value.toObject(); const auto ref=data["ref"].toString(),type=data["type"].toString();
        const bool volume=data["isVolume"].toBool();
        if(!volume && (represented.contains(ref) || (type!="org.mvvcvtk.surface-mesh" && !type.contains("label",Qt::CaseInsensitive)))) continue;
        const auto producer=data["producer"].toString();
        QString title=volume?(producer.contains("crop",Qt::CaseInsensitive)?"裁切体":producer.contains("artifact",Qt::CaseInsensitive)?"校正体":"原始体"):
            type=="org.mvvcvtk.surface-mesh"?"正式表面":"标签";
        const QJsonObject target{{"graphRevision",ref}}; QJsonArray actions{"GraphInfo"}; if(volume){actions.prepend("UseData");if(ref==current && currentOpacity)actions.append("SetVolumeVisible");}
        const QString status=ref==current?QString("当前输入")+(currentOpacity?*currentOpacity>0?" · 三维显示":" · 三维隐藏":" · 三维尚未就绪"):QString("已发布");
        append(volume?0:2,{{"id","published:"+ref},{"title",title+" · #"+data["order"].toString()},
            {"status",status},{"isVolume",volume},{"ref",ref},{"module","Data"},
            {"source",data["parents"]},{"actions",actions},{"patches",QJsonObject{{"GraphInfo",target},{"UseData",target},{"SetVolumeVisible",target}}}});
    }
    if(groups==m_objects) return;
    m_objects=groups; const auto selected=m_tree->currentItem()?m_tree->currentItem()->data(0,Qt::UserRole).toJsonObject()["id"].toString():QString();
    const auto scroll=m_tree->verticalScrollBar()->value(); const QSignalBlocker blocker(m_tree);
    // 差量协调保留原有行、选择和折叠；目录刷新不执行任何业务。
    const std::function<void(QTreeWidgetItem*,const QJsonArray&)> reconcile=[&](QTreeWidgetItem* parent,const QJsonArray& nodes) {
        QMap<QString,QTreeWidgetItem*> old;
        for(int i=0;i<parent->childCount();++i) old[parent->child(i)->data(0,Qt::UserRole).toJsonObject()["id"].toString()]=parent->child(i);
        for(int i=0;i<nodes.size();++i) {
            const auto node=nodes[i].toObject(); auto* item=old.take(node["id"].toString()); const bool created=!item;
            if(created){item=new QTreeWidgetItem;parent->insertChild(i,item);item->setExpanded(true);}
            else if(parent->indexOfChild(item)!=i){parent->takeChild(parent->indexOfChild(item));parent->insertChild(i,item);}
            if(item->data(0,Qt::UserRole).toJsonObject()!=node) {
                item->setData(0,Qt::UserRole,node); item->setText(0,node["title"].toString()); item->setText(1,node["status"].toString());
                item->setToolTip(0,node["title"].toString()+"\n修订："+node["ref"].toString());
                item->setToolTip(1,node["status"].toString());
            }
            reconcile(item,node["children"].toArray()); if(node["id"]==selected)m_tree->setCurrentItem(item);
        }
        for(auto* item:old) delete item;
    };
    reconcile(m_tree->invisibleRootItem(),groups);m_tree->verticalScrollBar()->setValue(scroll);FilterObjects();SetSelection();
}
void ObjectBrowser::SetSelection()
{
    const auto node=m_tree->currentItem()?m_tree->currentItem()->data(0,Qt::UserRole).toJsonObject():QJsonObject{};
    m_use->setEnabled(node["isVolume"].toBool());
    const bool visibility=node["actions"].toArray().contains("Visibility") || node["actions"].toArray().contains("SetVolumeVisible");m_show->setEnabled(visibility);m_hide->setEnabled(visibility);
    m_show->setToolTip(node["isVolume"].toBool()?"仅改变当前三维体数据的透明度；切片与工作输入不变。":"显隐不改变正式对象或工作输入。");m_hide->setToolTip(m_show->toolTip());
    m_summary->setText(node["ref"].toString().isEmpty()?"单击只选中；显隐和采用通过独立操作。":node["title"].toString()+"\n"+node["status"].toString());
    m_summary->setToolTip("精确修订："+node["ref"].toString()+"\n来源："+node["source"].toVariant().toString());
}
void ObjectBrowser::SendObjectAction(const QString& id,const QString& action,const QJsonObject& overrides)
{
    // 菜单可能跨越刷新：触发时重查正式身份，不能引用已删除的树项。
    for(const auto group:m_objects)for(const auto value:group.toObject()["children"].toArray()) {
        const auto node=value.toObject();if(node["id"]!=id || !node["actions"].toArray().contains(action))continue;
        auto patch=node["patches"].toObject()[action].toObject();for(auto it=overrides.begin();it!=overrides.end();++it)patch[it.key()]=it.value();
        patch["$objectId"]=id;if(onAction)onAction(action=="SetVolumeVisible"?"View":node["module"].toString(),action,patch);return;
    }
}
void ObjectBrowser::FilterObjects()
{
    const auto text=m_search->text().trimmed();
    for(int i=0;i<m_tree->topLevelItemCount();++i) {
        auto* group=m_tree->topLevelItem(i);bool visible=false;
        for(int j=0;j<group->childCount();++j){auto* item=group->child(j);const bool match=text.isEmpty() || item->text(0).contains(text,Qt::CaseInsensitive) || item->toolTip(0).contains(text,Qt::CaseInsensitive);item->setHidden(!match);visible|=match;}
        group->setHidden(!visible);
    }
}
}
