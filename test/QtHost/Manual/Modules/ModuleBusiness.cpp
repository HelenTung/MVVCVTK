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
#include <QToolButton>
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
        if(m_name=="Rotation") {
            m_statistics->SetMessage("模型姿态与变换历史","世界轴、世界中心和模型姿态独立于相机与计量对齐；此业务不使用灰度直方图。");
            m_statistics->SetTable({"量值","状态"},QJsonArray{QJsonArray{"旋转启用",m_observed["isEnabled"].toBool()?"是":"否"},QJsonArray{"可撤销记录",m_observed["undoCount"]}});
            return;
        }
        if(onStatistics && onStatistics())return;
        m_statistics->SetMessage("分析结果","当前工具尚无可显示结果，请执行主操作或查看场景。");
    } catch(const std::exception& error) {m_statistics->SetMessage("分析结果不可用",QString::fromUtf8(error.what()));}
}
void ModulePanel::RefreshGrayStatistics()
{
    try {
    if(!m_grayStatistics->isVisible() || m_context.workflow.GetIsClosing())return;
    if(onGrayStatistics && onGrayStatistics())return;
    const auto input=GetSession()->GetImageDescriptor();
    if(!input){m_grayStatistics->SetMessage("灰度分布","尚无当前输入。");return;}
    auto source=input->dataRevision;DataRevisionRef roi;
    if(m_name=="Crop" && m_observed["viewingOutput"].toBool() && !m_observed["output"].toString().isEmpty()) source=GetRef(m_observed["output"]);
    if(m_name=="Artifact" && m_observed["canSelectOutput"].toBool()) source=GetRef(m_observed["output"]);
    if(m_name=="Roi" && m_observed["isRoiCurrent"].toBool())roi=GetRef(m_observed["committedRoi"]);
    auto scope=getGrayScope ? getGrayScope() : GrayStatisticsScope{};
    if(GetDataRevisionRefValid(scope.source)){source=scope.source;roi=scope.roi;}
    const auto displayView=m_observed["displayView"].toString(),displayMode=m_observed["displayModeName"].toString(),parameterAction=m_parameterAction;
    const auto displayTarget=m_name=="View"?GetSession()->GetRenderViewState({displayView.toStdString()}):std::optional<HostRenderViewState>{};
    const auto targetStamp=displayTarget?GetRefText(displayTarget->dataRevision)+":"+QString::number(displayTarget->bindingRevision)+":"+QString::number(static_cast<int>(displayTarget->viewMode)):QString();
    const auto interactionContext=QString("input=%1/%2;source=%3;roi=%4;scope=%5;view=%6;mode=%7;action=%8").arg(GetRefText(input->dataRevision),QString::number(input->bindingRevision),GetRefText(source),GetRefText(roi),scope.key,displayView,displayMode,parameterAction)+";target="+targetStamp;
    m_grayStatistics->SetInteractionContext(interactionContext);
    const auto read=m_context.workflow.readGraySamples;
    if(!read){m_grayStatistics->SetMessage("灰度分布","当前构建没有统计读取器。");return;}
    const auto owner=QPointer<ModulePanel>(this);
    m_grayStatistics->StartHistogram(GetRefText(source)+":"+GetRefText(roi)+":"+scope.key,scope.title.isEmpty() ? GetDataRevisionRefValid(roi) ? "正式 ROI 的有效灰度分布" : m_name=="Crop" && m_observed["viewingOutput"].toBool() ? "所看裁剪结果的有效灰度分布" : "当前输入有效域的灰度分布" : scope.title,
        "原始灰度（未自动标定为 HU）","抽样频数",[owner,read,source,roi,scope](auto offset){
            if(!owner || owner->m_context.workflow.GetIsClosing())throw std::runtime_error("会话已停止");auto result=read(source,roi,offset,scope.filter);result.description+=" · "+scope.description;return result;
        });
    if(!m_grayStatistics->onInterval) m_grayStatistics->onInterval=[owner](double low,double high) {
        if(!owner)return;
        if(owner->m_name=="Part") owner->SetParameterPatch("Start",{{"threshold",(low+high)*0.5}});
        else if(owner->m_name=="Surface")owner->SetParameterPatch("GlobalAutomatic",{{"initialIsoValue",(low+high)*0.5}});
        else if(owner->m_name=="Gap")owner->SetParameterPatch("Start",{{"isoMode","AbsoluteValue"},{"absoluteIsoValue",(low+high)*0.5}});
        else if(owner->m_name=="Roi" && QStringList{"Threshold","Grow"}.contains(owner->m_parameterAction))owner->SetParameterPatch(owner->m_parameterAction,{{"grayRange",QJsonArray{low,high}}});
        else if(owner->m_name=="View") {
            const auto intent=owner->m_grayStatistics->GetInteractionMode();
            if(intent=="设置等值阈值")owner->SetParameterPatch("Set",{{"iso",(low+high)*0.5}});
            else if(intent=="设置窗宽窗位")owner->SetParameterPatch("Set",{{"windowLevel",QJsonArray{high-low,(low+high)*0.5}}});
            else if(intent=="添加透明度节点") {
                auto transfer=owner->GetActionParameters("Set")["transfer"].toObject();if(transfer.isEmpty())transfer=owner->m_observed["appliedDisplay"].toObject()["transfer"].toObject();
                auto nodes=transfer["opacityNodes"].toArray();const auto value=(low+high)*0.5;bool inserted=false;
                for(int i=0;i<nodes.size();++i)if(nodes[i].toArray()[0].toDouble()==value){inserted=true;break;}else if(nodes[i].toArray()[0].toDouble()>value){nodes.insert(i,QJsonArray{value,0.5});inserted=true;break;}
                if(!inserted)nodes.append(QJsonArray{value,0.5});transfer["opacityNodes"]=nodes;owner->SetParameterPatch("Set",{{"transfer",transfer}});
            }
        }
        if(owner->onMessage)owner->onMessage("已选统计区间，参数为草稿；通过主动作明确提交。");
    };
    if(m_name=="View") {
        const auto mode=m_observed["displayModeName"].toString();
        const auto modes=mode.contains("IsoSurface")?QStringList{"设置等值阈值"}:mode.startsWith("Slice")?QStringList{"设置窗宽窗位"}:QStringList{"添加透明度节点"};
        m_grayStatistics->SetInteractionModes(modes);if(mode.contains("IsoSurface"))m_grayStatistics->SelectInteractionMode("设置等值阈值");
    }else if(m_name=="Part")m_grayStatistics->SetInteractionModes({"设置分割阈值"});
    else if(m_name=="Gap")m_grayStatistics->SetInteractionModes({"设置检出阈值"});
    else if(m_name=="Surface") {
        const auto* choice=findChild<QComboBox*>("surfaceParameterSource");const bool manual=choice && choice->currentData().toBool();
        m_grayStatistics->SetInteractionModes({"编辑表面初值"});m_grayStatistics->SelectInteractionMode("编辑表面初值");
        const auto params=GetActionParameters("GlobalAutomatic");
        m_grayStatistics->SetMarkers(QJsonArray{QJsonObject{{"label",manual?"手动初值（待预览）":"初值草稿（尚未估计）"},{"key","initialIsoValue"},{"editable",true},{"value",GetInputNumber(params,"initialIsoValue",GetScalarMidpoint(*input))}}});
        const auto sourceAtPresentation=input->dataRevision;const auto bindingAtPresentation=input->bindingRevision;const auto roiAtPresentation=params["analysisRoi"];m_grayStatistics->onMarker=[owner,sourceAtPresentation,bindingAtPresentation,roiAtPresentation](const QString& key,double value){
            try {
            if(key!="initialIsoValue" || !owner || owner->m_context.workflow.GetIsClosing())return;
            const auto current=owner->GetSession()->GetImageDescriptor();if(!current || current->dataRevision!=sourceAtPresentation || current->bindingRevision!=bindingAtPresentation || owner->GetActionParameters("GlobalAutomatic")["analysisRoi"]!=roiAtPresentation)return;
            if(auto* sourceChoice=owner->findChild<QComboBox*>("surfaceParameterSource");sourceChoice && !sourceChoice->currentData().toBool()){const auto index=sourceChoice->findData(true);if(index>=0)sourceChoice->setCurrentIndex(index);}
            if(!owner || owner->m_context.workflow.GetIsClosing())return;
            owner->SetParameterPatch("GlobalAutomatic",{{"initialIsoValue",value}});
            if(owner && owner->onMessage)owner->onMessage("已修改手动初值；BG/M 未改，尚未生成正式表面。");
            }catch(const std::exception& error){if(owner)owner->SetFeedback(QString::fromUtf8(error.what()),"error");}
        };
    }else if(m_name=="Roi" && QStringList{"Threshold","Grow"}.contains(m_parameterAction)) {
        m_grayStatistics->SetInteractionModes({"编辑灰度范围"});
        const auto range=GetActionParameters(m_parameterAction)["grayRange"].toArray();
        m_grayStatistics->SetMarkers(QJsonArray{QJsonObject{{"label","灰度下限"},{"key",m_parameterAction+":lower"},{"editable",true},{"value",range[0]}},QJsonObject{{"label","灰度上限"},{"key",m_parameterAction+":upper"},{"editable",true},{"value",range[1]}}});
        m_grayStatistics->onMarker=[owner](const QString& key,double value){if(!owner || owner->m_context.workflow.GetIsClosing() || key.section(':',0,0)!=owner->m_parameterAction)return;auto range=owner->GetActionParameters(owner->m_parameterAction)["grayRange"].toArray();range[key.section(':',1)=="lower"?0:1]=value;
            if(range[0].toDouble()>=range[1].toDouble()){if(owner->onMessage)owner->onMessage("灰度下限必须小于上限；草稿保持原值。");return;}owner->SetParameterPatch(owner->m_parameterAction,{{"grayRange",range}});};
    }else m_grayStatistics->SetInteractionModes({});
    if(m_name=="View")m_grayStatistics->SetMarkers(m_observed["displayMarkers"].toArray());
    if(m_name=="View") {
        const auto expectedInput=GetRefText(input->dataRevision);const auto expectedBinding=input->bindingRevision;
        const auto expectedView=displayView,expectedMode=displayMode,expectedAction=parameterAction,expectedContext=interactionContext;
        m_grayStatistics->onMarker=[owner,expectedInput,expectedBinding,expectedView,expectedMode,expectedAction,expectedContext,targetStamp](const QString& key,double value){
            try {
            if(key!="iso" || !owner || owner->m_context.workflow.GetIsClosing() || owner->m_grayStatistics->GetInteractionMode()!="设置等值阈值")return;
            const auto current=owner->GetSession()->GetImageDescriptor();
            const auto target=owner->GetSession()->GetRenderViewState({expectedView.toStdString()});
            if(!target || GetRefText(target->dataRevision)+":"+QString::number(target->bindingRevision)+":"+QString::number(static_cast<int>(target->viewMode))!=targetStamp)return;
            const auto draft=owner->GetActionParameters("Set");
            if(!current || GetRefText(current->dataRevision)!=expectedInput || current->bindingRevision!=expectedBinding
                || draft["viewId"].toString()!=expectedView || (!draft["mode"].isNull() && draft["mode"].toString()!=expectedMode)
                || owner->m_observed["displayView"].toString()!=expectedView || owner->m_observed["displayModeName"].toString()!=expectedMode
                || owner->m_parameterAction!=expectedAction || expectedMode.isEmpty())return;
            auto currentSource=current->dataRevision;DataRevisionRef currentRoi;
            if(owner->m_name=="Crop" && owner->m_observed["viewingOutput"].toBool() && !owner->m_observed["output"].toString().isEmpty())currentSource=GetRef(owner->m_observed["output"]);
            if(owner->m_name=="Artifact" && owner->m_observed["canSelectOutput"].toBool())currentSource=GetRef(owner->m_observed["output"]);
            if(owner->m_name=="Roi" && owner->m_observed["isRoiCurrent"].toBool())currentRoi=GetRef(owner->m_observed["committedRoi"]);
            const auto currentScope=owner->getGrayScope?owner->getGrayScope():GrayStatisticsScope{};
            if(GetDataRevisionRefValid(currentScope.source)){currentSource=currentScope.source;currentRoi=currentScope.roi;}
            const auto actual=QString("input=%1/%2;source=%3;roi=%4;scope=%5;view=%6;mode=%7;action=%8").arg(GetRefText(current->dataRevision),QString::number(current->bindingRevision),GetRefText(currentSource),GetRefText(currentRoi),currentScope.key,owner->m_observed["displayView"].toString(),owner->m_observed["displayModeName"].toString(),owner->m_parameterAction)+";target="+targetStamp;
            if(actual!=expectedContext)return;
            owner->SetParameterPatch("Set",{{"iso",value}});
            if(owner && !owner->m_context.workflow.GetIsClosing())owner->SendBusinessAction("Set");
            }catch(const std::exception& error){if(owner)owner->SetFeedback(QString::fromUtf8(error.what()),"error");}
        };
    }
    if(m_name=="Part"){const auto p=GetActionParameters("Start");m_grayStatistics->SetMarkers(QJsonArray{QJsonObject{{"label","草稿材料阈值"},{"key","threshold"},{"editable",true},{"value",GetInputNumber(p,"threshold",GetScalarMidpoint(*input))}}});m_grayStatistics->onMarker=[owner](const QString& key,double value){if(owner && !owner->m_context.workflow.GetIsClosing())owner->SetParameterPatch("Start",{{key,value}});};}
    if(m_name=="Gap"){const auto p=GetActionParameters("Start");QJsonArray markers{QJsonObject{{"label","草稿 BG"},{"value",GetInputNumber(p,"backgroundMean",input->scalarRange[0])}},QJsonObject{{"label","草稿 M"},{"value",GetInputNumber(p,"materialMean",input->scalarRange[1])}}};if(p["isoMode"]=="AbsoluteValue")markers.append(QJsonObject{{"label","草稿 ISO"},{"value",GetInputNumber(p,"absoluteIsoValue",GetScalarMidpoint(*input))}});m_grayStatistics->SetMarkers(markers);}
    } catch(const std::exception& error){m_grayStatistics->SetMessage("业务统计不可用",QString::fromUtf8(error.what()));}
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
    m_businessSource->setText(!source.isEmpty() && source!=input["dataRevision"].toString()
        ? "业务来源 · "+source.right(18)+"（与当前输入不同）"
        : input["available"].toBool() ? "输入 · "+input["datasetId"].toString() : "输入 · 尚未加载数据");
    m_businessSource->setToolTip("业务源精确修订："+source);
    m_businessSource->hide();
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
    m_businessNext->setVisible(m_help->isChecked() || (!reason.isEmpty() && !m_requirements.count(m_parameterAction)));
    findChild<QWidget*>("businessContext")->setVisible(!m_businessNext->isHidden() || !display.isEmpty());
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
            const auto actionTitle=action.startsWith("Open") && !m_entries.count(action) ? "进入"+GetModuleText(action.mid(4)) : GetActionText(m_name,action);
            auto text=actionTitle;
            if(m_name=="Roi") {
                if(action=="SetDraft")text="更新草稿";
                else if(action=="Cancel")text="取消草稿";
                else if(action=="Visibility")text="显示设置";
                else if(action=="SetMetadata")text="区域信息";
            }
            auto* button=new QPushButton(text,m_nextActions); button->setObjectName("next_"+action);
            button->setAccessibleName(text);button->setAccessibleDescription(actionTitle);
            connect(button,&QPushButton::clicked,this,[this,action] {
                if(m_forms.count(action) && m_forms.at(action)->GetHasInputs()) SelectAction(action);
                else SendBusinessAction(action);
            });
            const int columns=m_name=="Roi" ? 3 : 2;
            m_nextLayout->addWidget(button,index/columns,index%columns);
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
        button->setVisible(!action.startsWith("Open") || button->isEnabled());
        button->setToolTip(requirement.isEmpty()?button->accessibleName():requirement);
    }
    m_nextActions->hide();
    auto* guidance=findChild<QWidget*>("nextStepNavigation");
    if(!guidance){guidance=new QWidget(this);guidance->setObjectName("nextStepNavigation");auto* grid=new QGridLayout(guidance);grid->setContentsMargins(0,0,0,0);grid->setSpacing(4);static_cast<QVBoxLayout*>(findChild<QWidget*>("businessContext")->layout())->addWidget(guidance);}
    QStringList links;for(const auto& action:actions)if(action.startsWith("Open"))links.append(action);
    const auto key=links.join('|');
    if(guidance->property("links").toString()!=key){while(auto* item=guidance->layout()->takeAt(0)){delete item->widget();delete item;}int i=0;for(const auto& action:links){auto* button=new QPushButton("进入"+GetModuleText(action.mid(4)),guidance);button->setObjectName("prepare_"+action);connect(button,&QPushButton::clicked,this,[this,action]{SendBusinessAction(action);});static_cast<QGridLayout*>(guidance->layout())->addWidget(button,i/2,i%2);++i;}guidance->setProperty("links",key);}
    guidance->hide();findChild<QWidget*>("businessContext")->setVisible(!m_businessNext->isHidden()||!display.isEmpty());
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
