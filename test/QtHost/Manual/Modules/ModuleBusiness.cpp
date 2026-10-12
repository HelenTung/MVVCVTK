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
    if(m_name=="Crop" && action=="BuildResult")params["nodeId"]=m_observed["appliedHead"];
    if(m_name=="Rotation" && action=="SetEnabled")params["isEnabled"]=true;
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
    SendAction(action,params,[owner,source=input->dataRevision,binding=input->bindingRevision](const QString& status,const QJsonObject& result) {
        if(!owner || owner->m_context.workflow.GetIsClosing() || status!="Published") return;
        const auto target=result["output"].isString() ? result["output"].toString() : result["correctedVolume"].toString();
        if(target.isEmpty()) return;
        const auto current=owner->GetSession()->GetImageDescriptor();
        // 发布与采用仍是两个真实事务。迟到发布不得把用户后来选择的输入覆盖回去。
        if(!current || current->dataRevision!=source || current->bindingRevision!=binding) {
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
    if(onStatistics && onStatistics())return;
    const auto input=GetSession()->GetImageDescriptor();
    if(!input){m_statistics->SetMessage("灰度分布","尚无当前输入。");return;}
    auto source=input->dataRevision;DataRevisionRef roi;
    if(m_name=="Crop" && !m_observed["output"].toString().isEmpty()) source=GetRef(m_observed["output"]);
    if(m_name=="Artifact" && m_observed["canSelectOutput"].toBool()) source=GetRef(m_observed["output"]);
    if(m_name=="Roi" && m_observed["isRoiCurrent"].toBool())roi=GetRef(m_observed["committedRoi"]);
    auto scope=getGrayScope ? getGrayScope() : GrayStatisticsScope{};
    if(GetDataRevisionRefValid(scope.source)){source=scope.source;roi=scope.roi;}
    const auto read=m_context.workflow.readGraySamples;
    if(!read){m_statistics->SetMessage("灰度分布","当前构建没有统计读取器。");return;}
    const auto owner=QPointer<ModulePanel>(this);
    m_statistics->StartHistogram(GetRefText(source)+":"+GetRefText(roi)+":"+scope.key,scope.title.isEmpty() ? m_name=="Crop" ? "已发布裁剪结果的有效灰度分布" : m_name=="Roi" ? "正式 ROI 的有效灰度分布" : "有效灰度分布" : scope.title,
        "原始灰度（未自动标定为 HU）","抽样频数",[owner,read,source,roi,scope](auto offset){
            if(!owner || owner->m_context.workflow.GetIsClosing())throw std::runtime_error("会话已停止");auto result=read(source,roi,offset,scope.filter);result.description+=" · "+scope.description;return result;
        });
    if(!m_statistics->onInterval) m_statistics->onInterval=[owner](double low,double high) {
        if(!owner)return;
        if(owner->m_name=="Part") owner->SetParameterPatch("Start",{{"threshold",(low+high)*0.5}});
        else if(owner->m_name=="Surface")owner->SetParameterPatch("GlobalAutomatic",{{"initialIsoValue",(low+high)*0.5}});
        else if(owner->m_name=="Gap")owner->SetParameterPatch("Start",{{"isoMode","AbsoluteValue"},{"absoluteIsoValue",(low+high)*0.5}});
        else if(owner->m_name=="View")owner->SetParameterPatch("Set",{{"iso",(low+high)*0.5}});
        if(owner->onMessage)owner->onMessage("已选统计区间，参数为草稿；通过主动作明确提交。");
    };
    } catch(const std::exception& error){m_statistics->SetMessage("业务统计不可用",QString::fromUtf8(error.what()));}
}
void ModulePanel::RefreshBusinessStep(const QJsonObject& state,const QJsonObject& input)
{
    auto step=GetBusinessStep(m_name,state,input,m_parameterAction);
    if(m_entries.empty()) step={"当前构建未启用此功能","请选择启用对应 Feature 的完整项目配置。","","不可用",{}};
    m_businessStage->setText(step.stage); m_businessNext->setText("下一步 · "+step.next);
    const auto source=state["source"].toString();
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
    // 主要出口保持紧凑，其余动作仍在按业务分类的完整菜单中可达。
    if(actions.size()>4)actions=actions.mid(0,4);
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
    auto ref=selected.isEmpty() ? input["dataRevision"].toString() : selected;
    if(selected.isEmpty()) for(const auto* key:{"output","mesh","resultSet","lastResult","source"})
        if(!m_observed[key].toString().isEmpty()){ref=m_observed[key].toString();break;}
    const auto key=QString::number(graph["commitId"].toString().toULongLong())+":"+ref;
    if(m_sources->property("sourceKey").toString()==key) return;
    m_sources->setProperty("sourceKey",key); const QSignalBlocker blocker(m_sources); m_sources->clear();
    QMap<QString,QJsonObject> nodes;
    for(const auto value:graph["nodes"].toArray()) nodes[value.toObject()["ref"].toString()]=value.toObject();
    QSet<QString> path;
    const std::function<void(QTreeWidgetItem*,const QString&)> append=[&](QTreeWidgetItem* parent,const QString& current) {
        if(current.isEmpty()) return;
        const auto node=nodes.value(current);
        auto* item=parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_sources);
        item->setText(0,node["operation"].toString("来源修订")); item->setText(1,current);
        item->setToolTip(0,node["producer"].toString()+" · "+node["type"].toString()); item->setToolTip(1,current);
        item->setExpanded(true);
        if(path.contains(current)){item->setText(0,"循环关系，停止展开");return;}
        path.insert(current);
        for(const auto value:node["parents"].toArray()) append(item,value.toString());
        path.remove(current);
    };
    append(nullptr,ref);
}
}
