#include "ModulePanel.h"
#include "Support/BusinessStep.h"
#include "Support/ParameterEditor.h"
#include "Support/UiText.h"
#include "Support/ActionPolicy.h"
#include <QGridLayout>
#include <QPushButton>
#include <QPointer>
#include <QTreeWidget>
#include <QTabWidget>
#include <QSignalBlocker>
#include <algorithm>
namespace Manual {
QJsonObject ModulePanel::GetActionParameters(const QString& action) const
{
    const auto* form=GetParameterEditor(action);
    return form ? form->GetValue().toObject() : QJsonObject{};
}
void ModulePanel::SendBusinessAction(const QString& action, bool usesOutput)
{
    try {
    if (action.startsWith("Open") && !m_entries.count(action)) {
        const auto module=action.mid(4);
        if(m_context.workflow.onNavigate) m_context.workflow.onNavigate(module,
            module=="Data" ? "Load" : module=="Roi" ? "Begin" : module=="Surface" ? "Determine" : "Start",{});
        return;
    }
    if(!m_entries.count(action)) return;
    auto params=GetActionParameters(action);
    if(m_name=="Crop" && action=="BuildResult" && params["inputRoi"].toString().isEmpty())params["nodeId"]=m_observed["appliedHead"];
    m_currentAction=action;
    if(m_forms.count(action) && m_forms.at(action)->GetHasInputs())m_parameterAction=action;
    if(m_name=="Surface" && (action=="Preview" || action=="Determine")) params=GetActionParameters("GlobalAutomatic");
    if(!usesOutput) {
        // 参数切换只打开草稿；无参数命令立即发出。阶段主动作始终使用该动作自己的表单。
        SendAction(action,params);
        if(QStringList{"Result","Defects","InspectionResult","DeviationResult","VoxelVolumeResult"}.contains(action) && m_context.workflow.onShowStatistics)m_context.workflow.onShowStatistics(m_name);
        return;
    }
    const auto input=GetSession()->GetImageDescriptor();
    if(!input) return;
    const auto owner=QPointer<ModulePanel>(this);
    const auto operation=std::make_shared<std::uint64_t>(0);
    *operation=SendAction(action,params,[owner,operation,action,source=input->dataRevision,binding=input->bindingRevision](const QString& status,const QJsonObject& result) {
        if(!owner || owner->m_context.workflow.GetIsClosing() || status!="Published") return;
        const auto target=result["output"].isString() ? result["output"].toString() : result["correctedVolume"].toString();
        if(target.isEmpty()) return;
        const auto current=owner->GetSession()->GetImageDescriptor();
        const auto record=owner->m_context.records.GetRecord(*operation);
        const auto completed=record["current"].toObject();
        const auto completedBinding=completed["bindingRevision"].toString().toULongLong();
        const bool sourceAtCompletion=completed["available"].toBool() && completed["dataRevision"].toString()==GetRefText(source);
        // 裁剪重复发布会在同一事务中恢复源绑定；以该次完成值区分自身恢复和后来选择。
        const bool ownRestore=owner->m_name=="Crop" && action=="BuildResult" && result["source"].toString()==GetRefText(source)
            && completedBinding>binding && completedBinding-binding==1;
        // 发布与采用仍是两个真实事务。迟到发布不得把用户后来选择的输入覆盖回去。
        if(!sourceAtCompletion || (!ownRestore && completedBinding!=binding) || !current
            || current->dataRevision!=source || current->bindingRevision!=completedBinding) {
            owner->m_executionMessage->setText("结果已发布；当前输入已变化，自动采用未执行。可查看结果并显式选择使用。");
            owner->m_executionMessage->show(); owner->Observe(); return;
        }
        owner->SendAction("UseData",{{"graphRevision",target}});
    });
    } catch(const std::exception& error){SetState({{"inputError",QString::fromUtf8(error.what())}});}
}
void ModulePanel::RefreshStatistics()
{
    try {
    if(!m_statistics->isVisible() || m_context.workflow.GetIsClosing())return;
    if(m_name=="Rotation"){m_statistics->SetMessage("模型姿态与变换历史","世界轴、世界中心和模型姿态独立于相机与计量对齐；此业务不使用灰度直方图。");m_statistics->SetTable({"量值","状态"},QJsonArray{QJsonArray{"旋转启用",m_observed["isEnabled"].toBool()?"是":"否"},QJsonArray{"可撤销记录",m_observed["undoCount"]}});return;}
    if(onStatistics && onStatistics())return;
    const auto input=GetSession()->GetImageDescriptor();
    if(!input){m_statistics->SetMessage("灰度分布","尚无当前输入。");return;}
    auto source=input->dataRevision;DataRevisionRef roi;
    if(m_name=="Crop" && m_observed["viewingOutput"].toBool() && !m_observed["output"].toString().isEmpty()) source=GetRef(m_observed["output"]);
    if(m_name=="Artifact" && m_observed["canSelectOutput"].toBool()) source=GetRef(m_observed["output"]);
    if(m_name=="Roi" && m_observed["isRoiCurrent"].toBool())roi=GetRef(m_observed["committedRoi"]);
    auto scope=getGrayScope ? getGrayScope() : GrayStatisticsScope{};
    if(GetDataRevisionRefValid(scope.source)){source=scope.source;roi=scope.roi;}
    const auto read=m_context.workflow.readGraySamples;
    if(!read){m_statistics->SetMessage("灰度分布","当前构建没有统计读取器。");return;}
    const auto owner=QPointer<ModulePanel>(this);
    m_statistics->StartHistogram(GetRefText(source)+":"+GetRefText(roi)+":"+scope.key,scope.title.isEmpty() ? GetDataRevisionRefValid(roi) ? "正式 ROI 的有效灰度分布" : m_name=="Crop" && m_observed["viewingOutput"].toBool() ? "所看裁剪结果的有效灰度分布" : "当前输入有效域的灰度分布" : scope.title,
        "原始灰度（未自动标定为 HU）","抽样频数",[owner,read,source,roi,scope](auto offset){
            if(!owner || owner->m_context.workflow.GetIsClosing())throw std::runtime_error("会话已停止");auto result=read(source,roi,offset,scope.filter);result.description+=" · "+scope.description;return result;
        });
    if(!m_statistics->onInterval) m_statistics->onInterval=[owner](double low,double high) {
        if(!owner)return;
        if(owner->m_name=="Part") owner->SetParameterPatch("Start",{{"threshold",(low+high)*0.5}});
        else if(owner->m_name=="Surface")owner->SetParameterPatch("GlobalAutomatic",{{"initialIsoValue",(low+high)*0.5}});
        else if(owner->m_name=="Gap")owner->SetParameterPatch("Start",{{"isoMode","AbsoluteValue"},{"absoluteIsoValue",(low+high)*0.5}});
        else if(owner->m_name=="View") {
            const auto mode=owner->m_observed["displayModeName"].toString();
            if(mode.contains("IsoSurface"))owner->SetParameterPatch("Set",{{"iso",(low+high)*0.5}});
            else if(mode.startsWith("Slice"))owner->SetParameterPatch("Set",{{"windowLevel",QJsonArray{high-low,(low+high)*0.5}}});
            else {auto transfer=owner->GetActionParameters("Set")["transfer"].toObject();if(transfer.isEmpty())transfer=owner->m_observed["appliedDisplay"].toObject()["transfer"].toObject();transfer["opacityNodes"]=QJsonArray{QJsonArray{low,0.},QJsonArray{high,1.}};owner->SetParameterPatch("Set",{{"transfer",transfer}});}
        }
        if(owner->onMessage)owner->onMessage("已选统计区间，参数为草稿；通过主动作明确提交。");
    };
    if(m_name=="View")m_statistics->SetMarkers(m_observed["displayMarkers"].toArray());
    if(m_name=="Part"){const auto p=GetActionParameters("Start");m_statistics->SetMarkers(QJsonArray{QJsonObject{{"label","草稿材料阈值"},{"value",GetInputNumber(p,"threshold",GetScalarMidpoint(*input))}}});}
    if(m_name=="Gap"){const auto p=GetActionParameters("Start");QJsonArray markers{QJsonObject{{"label","草稿 BG"},{"value",GetInputNumber(p,"backgroundMean",input->scalarRange[0])}},QJsonObject{{"label","草稿 M"},{"value",GetInputNumber(p,"materialMean",input->scalarRange[1])}}};if(p["isoMode"]=="AbsoluteValue")markers.append(QJsonObject{{"label","草稿 ISO"},{"value",GetInputNumber(p,"absoluteIsoValue",GetScalarMidpoint(*input))}});m_statistics->SetMarkers(markers);}
    } catch(const std::exception& error){m_statistics->SetMessage("业务统计不可用",QString::fromUtf8(error.what()));}
}
void ModulePanel::RefreshBusinessStep(const QJsonObject& state,const QJsonObject& input)
{
    auto step=GetBusinessStep(m_name,state,input,m_parameterAction);
    if(!step.action.isEmpty() && !m_entries.count(step.action)) {
        for(const auto& alternative:step.secondary)if(m_entries.count(alternative)){step.action=alternative;step.title=GetActionText(m_name,alternative);step.usesOutput=false;break;}
    }
    if(m_entries.empty()) step={"当前构建未启用此功能","请选择启用对应 Feature 的完整项目配置。","","不可用",{}};
    m_businessStage->setText(step.stage); m_businessNext->setText("下一步 · "+step.next);
    auto source=state["source"].toString();if(source.startsWith("00000000000000000000000000000000:"))source.clear();
    m_businessSource->setText(source.isEmpty() ? "本次业务输入 · 顶部当前输入" : "本次业务来源 · "+source.right(18)+
        (source==input["dataRevision"].toString() ? "（当前输入）" : "（与当前输入不同）"));
    m_businessSource->setToolTip("业务源精确修订："+source);
    QString display;
    if(state["hasResult"].toBool() && state.contains("isDisplayReady"))display=state["isDisplayReady"].toBool() ? "显示 · 已就绪" : "显示 · 尚未就绪；已发布结果与统计保留";
    else if(state["hasResult"].toBool() && state.contains("isOverlayVisible"))display=state["isOverlayVisible"].toBool() ? "显示 · 结果叠加可见" : "显示 · 结果叠加未显示；已发布结果与统计保留";
    m_businessDisplay->setText(display);m_businessDisplay->setVisible(!display.isEmpty());
    auto reason=GetActionRequirement(m_name,step.action,state,input["available"].toBool());
    if(m_context.workflow.GetBusyOperation()) reason="当前任务未完成，请等待或取消。";
    if(m_context.workflow.GetIsClosing()) reason="会话正在关闭。";
    m_primary->setText(step.title);
    m_primary->setProperty("businessAction",step.action);
    m_primary->setProperty("usesOutput",step.usesOutput);
    m_primary->setEnabled(!step.action.isEmpty() && m_entries.count(step.action) && reason.isEmpty());
    m_primary->setToolTip(reason.isEmpty() ? step.next : reason);
    if(!reason.isEmpty() && !step.action.isEmpty()) m_businessNext->setText("下一步 · "+step.next+"\n"+reason);
    QStringList actions;
    for(const auto& action:step.secondary) {
        if(action==step.action) continue;
        if(m_entries.count(action)) actions.append(action);
        else if(action.startsWith("Open") && m_context.workflow.getActionAvailable) {
            const auto module=action.mid(4);
            const auto target=module=="Data" ? "Load" : module=="Roi" ? "Begin" : module=="Surface" ? "Determine" : "Start";
            if(m_context.workflow.getActionAvailable(module,target)) actions.append(action);
        }
    }
    // 保留本业务的有效出口；体积和报告不能因统一截断丢失。
    if(actions.size()!=m_nextActionIds.size() || !std::equal(actions.cbegin(),actions.cend(),m_nextActionIds.cbegin())) {
        while(auto* item=m_nextLayout->takeAt(0)){ delete item->widget(); delete item; }
        for(int index=0;index<actions.size();++index) {
            const auto action=actions[index];
            const auto text=action.startsWith("Open") && !m_entries.count(action) ? "进入"+GetModuleText(action.mid(4)) : GetActionText(m_name,action);
            auto* button=new QPushButton(text,m_nextActions); button->setObjectName("next_"+action);
            connect(button,&QPushButton::clicked,this,[this,action] {
                if(m_forms.count(action) && m_forms.at(action)->GetHasInputs()) SelectAction(action);
                else SendBusinessAction(action);
            });
            m_nextLayout->addWidget(button,index/2,index%2);
        }
        m_nextActionIds=actions;
    }
    for(const auto& action:actions) {
        auto* button=m_nextActions->findChild<QPushButton*>("next_"+action);
        const auto requirement=GetActionRequirement(m_name,action,state,input["available"].toBool());
        const bool parameter=m_forms.count(action) && m_forms.at(action)->GetHasInputs();
        const bool busy=m_context.workflow.GetBusyOperation() && m_entries.count(action)
            && m_entries.at(action).policy!=TestPolicy::Stop && m_entries.at(action).policy!=TestPolicy::Read && m_entries.at(action).policy!=TestPolicy::View;
        button->setEnabled(!m_context.workflow.GetIsClosing() && (parameter || (requirement.isEmpty() && !busy)));
        button->setToolTip(requirement);
    }
    m_nextActions->setVisible(!actions.isEmpty());
}
void ModulePanel::RefreshSources(const QJsonObject& graph,const QJsonObject& input)
{
    const auto selected=m_node["patches"].toObject()["GraphInfo"].toObject()["graphRevision"].toString();
    auto ref=!m_sourceRoot.isEmpty() ? m_sourceRoot : selected.isEmpty() ? input["dataRevision"].toString() : selected;
    const auto key=QString::number(graph["commitId"].toString().toULongLong())+":"+ref;
    if(m_sources->property("sourceKey").toString()==key) return;
    m_sources->setProperty("sourceKey",key); const QSignalBlocker blocker(m_sources); m_sources->clear();
    QMap<QString,QJsonObject> nodes;
    for(const auto value:graph["nodes"].toArray()) nodes[value.toObject()["ref"].toString()]=value.toObject();
    QSet<QString> path,expanded;
    const std::function<void(QTreeWidgetItem*,const QString&)> append=[&](QTreeWidgetItem* parent,const QString& current) {
        if(current.isEmpty() || current.startsWith("00000000000000000000000000000000:")) return;
        const auto node=nodes.value(current);
        if(node.isEmpty())return;
        auto* item=parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_sources);
        item->setText(0,node["operation"].toString("来源修订")); item->setText(1,current);
        item->setToolTip(0,node["producer"].toString()+" · "+node["type"].toString()); item->setToolTip(1,current);
        item->setExpanded(true);
        if(path.contains(current)){item->setText(0,"循环关系，停止展开");return;}
        if(expanded.contains(current)){item->setText(0,item->text(0)+"（共用祖先）");return;}expanded.insert(current);
        path.insert(current);
        for(const auto value:node["parents"].toArray()) append(item,value.toString());
        path.remove(current);
    };
    append(nullptr,ref);
}
void ModulePanel::ShowSources(DataRevisionRef root)
{
    m_sourceRoot=GetDataRevisionRefValid(root)?GetRefText(root):QString();
    m_sources->setProperty("sourceKey",QString());m_browser->setCurrentIndex(2);
    RefreshSources(m_context.workflow.getPublishedGraph?m_context.workflow.getPublishedGraph():QJsonObject(),GetDescriptor(GetSession()->GetImageDescriptor()));
}
}
