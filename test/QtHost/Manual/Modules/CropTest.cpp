// 测试用途：验证裁剪控件调整、预览确认、历史导航、发布结果及恢复源数据的业务闭环。
#include "ModuleFactories.h"
#include "Host/CropHostFeature.h"
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <vtkNew.h>
#include <vtkPLYReader.h>
#include <vtkPolyData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
namespace Manual {
namespace {
CropHostTarget GetCropTarget()
{
    CropHostTarget target;
    target.inputBinding = std::string(primaryVolumeBinding);
    target.referenceView.viewId = "primary-3d"; target.targetViews = GetAllViews();
    return target;
}
bool GetFramesReady(ModulePanel* panel)
{
    const auto input = panel->GetSession()->GetImageDescriptor();
    if (!input) return false;
    for (const auto& id : GetAllViews().viewIds) {
        const auto scene = panel->GetSession()->GetSceneViewState({id});
        if (!scene || !scene->isAvailable || !scene->presentation || scene->presentation->isInteracting
            || scene->presentation->dataRevision != input->dataRevision
            || (panel->GetContext().workflow.getRenderPending && panel->GetContext().workflow.getRenderPending(id))) return false;
    }
    return true;
}
QString GetDisplayProblem(ModulePanel* panel)
{
    const auto primary = panel->GetSession()->GetRenderViewState({"primary-3d"});
    if (primary && (primary->viewMode == HostRenderMode::IsoSurface || primary->viewMode == HostRenderMode::CompositeIsoSurface)
        && !(primary->isoThreshold > primary->scalarRange[0] && primary->isoThreshold < primary->scalarRange[1]))
        return QString("主三维显示阈值 %1 未落在当前灰度范围 (%2, %3) 内，无法提交裁剪预览。请在“视图显示”设置有效的显示等值阈值，或切换为体渲染。")
            .arg(primary->isoThreshold).arg(primary->scalarRange[0]).arg(primary->scalarRange[1]);
    return {};
}
void ResetInitialCamera(ModulePanel* panel, bool first)
{
    if (!first) return;
    HostViewResetRequest request; request.targetView.viewId = "primary-3d";
    panel->GetSession()->SendRequestResult(std::move(request), [owner = QPointer<ModulePanel>(panel)](HostResult result) {
        if (owner && !result.isSucceeded && owner->onMessage) owner->onMessage("裁剪工具已启用，但主三维定位失败，请重置视图后拖动。");
    });
}
struct CropFlow {
    DataRevisionRef input;
    bool isConfirmed=false;
    std::uint64_t pending=0;
    CropNodeId historyAfter=0;
    std::optional<CropNodeId> expectedNode;
    CropRemovalMode preferredMode=CropRemovalMode::KeepInside;
};
}
ModulePanel* CreateCropTest(TestContext context,std::shared_ptr<CropHostFeature> feature,QWidget* parent)
{
    auto* panel=new ModulePanel(context,"Crop",parent);
    auto flow=std::make_shared<CropFlow>();
    panel->SetNotice("开始会为当前数据创建裁剪文档。盒、平面、球和圆柱的每次有效拖动生成稳定节点；可选择分支、裁除子树、保存 ROI 或发布结果。返回源数据会释放本次裁剪结果，关闭文档会结束其历史。");
    context.workflow.AttachExit("Crop",[owner=QPointer<ModulePanel>(panel),feature,flow] {
        if(!feature->GetState().isActive)return true;
        if(!owner||flow->pending||!GetFramesReady(owner))return false;
        CropHostRequest request;request.action=CropHostAction::Exit;return feature->SendRequest(request);
    });
    auto* deadline=new QTimer(panel);deadline->setSingleShot(true);
    QObject::connect(deadline,&QTimer::timeout,panel,[panel,feature,flow] {
        const auto id=flow->pending;flow->pending=0;flow->expectedNode.reset();
        QJsonArray views;const auto state=feature->GetState();
        for(const auto& view:state.views) {
            const auto endpoint=panel->GetSession()->GetRenderViewEndpoint(view.viewId);
            const auto window=endpoint&&endpoint->renderer?endpoint->renderer->GetRenderWindow():nullptr;
            views.append(QJsonObject{{"viewId",QString::fromStdString(view.viewId)},{"appliedHead",QString::number(view.appliedHead)},
                {"renderedHead",QString::number(view.renderedHead)},{"pending",view.isRenderPending},{"swapBuffers",window?window->GetSwapBuffers():-1},
                {"effectStatus",int(view.effect.status)},{"fencePending",view.effect.isRenderPending},{"effectMessage",QString::fromStdString(view.effect.message)},{"activeRevision",QString::number(view.effect.activeRevision)},{"renderedRevision",QString::number(view.effect.renderedRevision)}});
        }
        if(id&&!panel->GetContext().workflow.GetIsClosing())panel->SetComplete(id,"Failed",{{"message","历史已接纳，但等待视图完成超时，请检查隐藏视图。"},{"views",views},{"framesReady",GetFramesReady(panel)}});
    });
    const auto startWidget=[panel,feature,flow](std::uint64_t id,CropHostAction shape,CropRemovalMode mode) {
        CropHostRequest request;request.action=shape;request.target=GetCropTarget();
        bool accepted=feature->SendRequest(request);
        if(accepted){request={};request.action=CropHostAction::Mode;request.target=GetCropTarget();request.removalMode=mode;accepted=feature->SendRequest(request);}
        flow->pending=0;flow->preferredMode=mode;flow->isConfirmed=false;
        const QString hint=shape==CropHostAction::Sphere
            ? "球裁剪：拖动中心控制点移动球体，拖动绿色控制点调整半径。"
            : shape==CropHostAction::Cylinder
                ? "圆柱裁剪：拖动中心控制点平移，绿色控制点调整半径，两端控制点调整方向和长度。"
                : shape==CropHostAction::Box
                    ? "盒裁剪：拖动面中心控制点调整边界，按住 Shift 拖动可整体平移。"
                    : "平面裁剪：拖动中心控制点移动平面，拖动法线箭头调整方向。";
        if(accepted)panel->SetNotice(hint+" 保留或移除模式下，松开鼠标会更新各视图预览并生成历史节点；仅定位模式不生成历史。可在结果目录选择历史分支、保存 ROI 或发布结果。");
        panel->SetComplete(id,accepted?"Succeeded":"Rejected",{{"message",hint}});
    };
    panel->AttachAction("Start",{{"shape","Box"},{"removalMode","KeepInside"}},[panel,feature,flow,startWidget](auto id,const auto& p) {
        const auto shape=GetEnum<CropHostAction>(p,"shape",{{"Box",CropHostAction::Box},{"Plane",CropHostAction::Plane},{"Sphere",CropHostAction::Sphere},{"Cylinder",CropHostAction::Cylinder}});
        const auto mode=GetEnum<CropRemovalMode>(p,"removalMode",{{"None",CropRemovalMode::None},{"KeepInside",CropRemovalMode::KeepInside},{"RemoveInside",CropRemovalMode::RemoveInside}});
        const auto input=panel->GetSession()->GetImageDescriptor();
        if(!input){panel->SetComplete(id,"Rejected");return;}
        const auto history=feature->GetHistory();
        if(history.documentId&&history.sourceRevision==input->dataRevision){startWidget(id,shape,mode);return;}
        CropDocumentRequest request;request.action=CropDocumentAction::CreateDocument;
        request.requestId=CropHostFeature::CreateRequestId();request.target=GetCropTarget();request.sourceRevision=input->dataRevision;
        flow->pending=id;
        const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),flow,id,shape,mode,startWidget](CropDocumentOutcome value) {
            if(!owner)return;
            if(value.status==CropEditStatus::Succeeded){ResetInitialCamera(owner,true);startWidget(id,shape,mode);}
            else {flow->pending=0;owner->SetComplete(id,"Failed",{{"failureReason",int(value.failureReason)}});}
        });
        if(!admission)flow->pending=0;
        panel->SetAdmission(id,admission.isAccepted);
    },TestPolicy::Interaction,true);
    for(const auto& action:std::vector<std::pair<QString,CropHostAction>>{{"Box",CropHostAction::Box},{"Plane",CropHostAction::Plane},{"Sphere",CropHostAction::Sphere},{"Cylinder",CropHostAction::Cylinder}})
        panel->AttachAction(action.first,{},[startWidget,flow,action](auto id,const auto&){startWidget(id,action.second,flow->preferredMode);},TestPolicy::Interaction,true);
    panel->AttachAction("Mode",{{"removalMode","KeepInside"}},[panel,feature,flow](auto id,const auto& p){
        CropHostRequest request;request.action=CropHostAction::Mode;request.target=GetCropTarget();
        request.removalMode=GetEnum<CropRemovalMode>(p,"removalMode",{{"None",CropRemovalMode::None},{"KeepInside",CropRemovalMode::KeepInside},{"RemoveInside",CropRemovalMode::RemoveInside}});
        const auto accepted=feature->SendRequest(request);if(accepted){flow->preferredMode=*request.removalMode;flow->isConfirmed=false;}
        panel->SetComplete(id,accepted?"Succeeded":"Rejected");
    },TestPolicy::Interaction,true);
    for(const auto& mode:std::vector<std::pair<QString,CropRemovalMode>>{{"KeepInside",CropRemovalMode::KeepInside},{"RemoveInside",CropRemovalMode::RemoveInside},{"PositionOnly",CropRemovalMode::None}})
        panel->AttachAction(mode.first,{},[panel,feature,flow,mode](auto id,const auto&){
            CropHostRequest request;request.action=CropHostAction::Mode;request.target=GetCropTarget();request.removalMode=mode.second;
            const bool accepted=feature->SendRequest(request);if(accepted){flow->preferredMode=mode.second;flow->isConfirmed=false;}
            panel->SetComplete(id,accepted?"Succeeded":"Rejected");
        },TestPolicy::Interaction,true);
    for(const auto& action:std::vector<std::pair<QString,CropEditKind>>{{"Previous",CropEditKind::Select},{"Next",CropEditKind::Select},{"Node",CropEditKind::Select},{"ResetPreview",CropEditKind::Select},{"PruneSubtree",CropEditKind::Prune},{"PruneDescendants",CropEditKind::Prune},{"PruneOutsidePaths",CropEditKind::Prune}}) {
        QJsonObject defaults;if(action.first=="Node"||action.second==CropEditKind::Prune)defaults={{"nodeId","0"}};
        panel->AttachAction(action.first,defaults,[panel,feature,flow,deadline,action](auto id,const auto& p) {
            const auto history=feature->GetHistory();
            CropEditRequest request;request.kind=action.second;request.documentId=history.documentId;
            request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;
            if(action.second==CropEditKind::Select) {
                if(action.first=="Previous") {
                    const auto page=feature->GetHistory(history.documentId,history.appliedHead-1,1);
                    if(!page.nodes.empty()&&page.nodes.front().nodeId==history.appliedHead)request.nodeId=page.nodes.front().parentNodeId;
                } else if(action.first=="Next") {
                    std::vector<CropNodeId> children;auto page=history;
                    for(;;){for(const auto& node:page.nodes)if(node.parentNodeId==history.appliedHead)children.push_back(node.nodeId);
                        if(children.size()>1||!page.nextPageAfter)break;page=feature->GetHistory(history.documentId,page.nextPageAfter);}
                    if(children.size()==1)request.nodeId=children.front();
                } else request.nodeId=action.first=="ResetPreview"?history.rootNodeId:GetId(p["nodeId"]);
                if(!request.nodeId){panel->SetComplete(id,"Rejected",{{"message","没有唯一的相邻节点，请使用节点 ID 选择分支。"}});return;}
            }
            if(action.second==CropEditKind::Prune) {
                request.prune.scope=action.first=="PruneSubtree"?CropPruneScope::Subtrees:action.first=="PruneDescendants"?CropPruneScope::Descendants:CropPruneScope::OutsidePaths;
                request.prune.nodeIds={GetId(p["nodeId"])};request.prune.fallback=CropPruneFallback::NearestSurvivingAncestor;
                const auto impact=feature->GetPruneImpact(history.documentId,request.prune);
                if(impact.failureReason!=CropFailure::None){panel->SetComplete(id,"Rejected",{{"failureReason",int(impact.failureReason)}});return;}
            }
            flow->pending=id;flow->expectedNode.reset();flow->isConfirmed=false;
            const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),feature,flow,deadline,id](CropEditOutcome value) {
                if(!owner||flow->pending!=id)return;
                if(value.status==CropEditStatus::Succeeded){flow->expectedNode=feature->GetState().history.appliedHead;deadline->start(15000);}
                else{flow->pending=0;owner->SetComplete(id,"Failed",{{"failureReason",int(value.failureReason)}});}
            });
            if(!admission)flow->pending=0;
            panel->SetAdmission(id,admission.isAccepted);
        },TestPolicy::Compute);
    }
    for(const auto& action:std::vector<std::pair<QString,CropDocumentAction>>{{"CreateDocument",CropDocumentAction::CreateDocument},{"ActivateDocument",CropDocumentAction::ActivateDocument},{"CloseDocument",CropDocumentAction::CloseDocument},{"RestoreSource",CropDocumentAction::ReturnToSource}}) {
        QJsonObject defaults;if(action.second==CropDocumentAction::ActivateDocument)defaults={{"documentId","0"}};
        panel->AttachAction(action.first,defaults,[panel,feature,flow,action](auto id,const auto& p){
            const auto history=feature->GetHistory();CropDocumentRequest request;request.action=action.second;request.requestId=CropHostFeature::CreateRequestId();
            if(action.second==CropDocumentAction::CreateDocument){const auto input=panel->GetSession()->GetImageDescriptor();if(!input){panel->SetComplete(id,"Rejected");return;}request.target=GetCropTarget();request.sourceRevision=input->dataRevision;}
            else {request.documentId=action.second==CropDocumentAction::ActivateDocument?GetId(p["documentId"]):history.documentId;request.expectedRevision=feature->GetHistory(request.documentId).stateRevision;}
            if(action.second==CropDocumentAction::ActivateDocument)request.target=GetCropTarget();
            const auto previousPending=flow->pending;flow->pending=id;flow->isConfirmed=false;
            const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),flow,id,action](CropDocumentOutcome value){
                if(!owner)return;if(flow->pending==id){flow->pending=0;flow->expectedNode.reset();}
                if(value.status==CropEditStatus::Succeeded&&action.second==CropDocumentAction::CreateDocument)ResetInitialCamera(owner,true);
                owner->SetComplete(id,value.status==CropEditStatus::Succeeded?"Succeeded":"Failed",{{"documentId",QString::number(value.documentId)},{"failureReason",int(value.failureReason)},{"blockerCount",int(value.blockers.size())}});
            });
            if(!admission)flow->pending=previousPending;panel->SetAdmission(id,admission.isAccepted);
        },action.second==CropDocumentAction::CloseDocument||action.second==CropDocumentAction::ReturnToSource?TestPolicy::Stop:TestPolicy::Compute);
    }
    for(const QString action:{QString("FinishEditing"),QString("Exit")})panel->AttachAction(action,{},[panel,feature,flow,action](auto id,const auto&){
        CropHostRequest request;request.action=CropHostAction::Exit;const bool accepted=feature->SendRequest(request);
        if(accepted)flow->isConfirmed=action=="FinishEditing"&&feature->GetState().history.nodeCount>0;
        panel->SetComplete(id,accepted?(flow->isConfirmed?"PreviewConfirmed":"Exited"):"Rejected");
    },TestPolicy::Change);
    panel->AttachAction("BuildResult",{{"nodeId","0"},{"inputRoi",""}},[panel,feature,flow](auto id,const auto& p){
        const auto history=feature->GetHistory();CropBuildRequest request;
        request.documentId=history.documentId;request.nodeId=GetId(p["nodeId"]);if(!request.nodeId)request.nodeId=history.appliedHead;
        request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;
        if(!GetText(p,"inputRoi").isEmpty())request.inputRoi=GetRef(p["inputRoi"]);
        flow->pending=id;
        const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),flow,id](CropBuildResult result){
            if(!owner)return;if(flow->pending==id)flow->pending=0;
            owner->SetComplete(id,result.isSucceeded?"Published":"Failed",{{"failureReason",int(result.failureReason)},{"message",QString::fromStdString(result.message)},
                {"source",GetRefText(result.sourceRevision)},{"output",GetRefText(result.outputRevision)},{"recipe",GetRefText(result.recipeRevision)},{"commitId",QString::number(result.commitId)}});
        });
        if(!admission)flow->pending=0;panel->SetAdmission(id,admission.isAccepted);
    },TestPolicy::Compute);
    panel->AttachAction("SaveRoi",{{"name","crop-roi"}},[panel,feature](auto id,const auto& p){
        CropHostRequest request;request.action=CropHostAction::SaveRoi;request.target=GetCropTarget();request.roiMetadata=RoiMetadata{GetText(p,"name").toStdString()};
        const auto catalog=panel->GetSession()->GetRoiDescriptors(true);request.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;
        const bool accepted=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),id](CropBuildResult result){if(owner)owner->SetComplete(id,result.isSucceeded?"Succeeded":"Failed",{{"recipe",GetRefText(result.recipeRevision)}});});
        if(!accepted)panel->SetComplete(id,"Rejected");
    },TestPolicy::Compute);
    panel->AttachAction("SelectOutput",{},[panel,feature](auto id,const auto&){panel->SetInput(id,feature->GetState().outputRevision);},TestPolicy::Input,true);
    panel->AttachAction("SetPolyData", {{"plyPath", ""}}, [panel, feature, flow](auto id, const auto& p) {
        vtkNew<vtkPLYReader> reader; const auto path = GetText(p, "plyPath").toUtf8(); reader->SetFileName(path.constData()); reader->Update();
        if (!reader->GetOutput() || !reader->GetOutput()->GetNumberOfPoints()) throw std::invalid_argument("PLY 网格为空");
        CropHostRequest request; request.action = CropHostAction::SetPolyData; request.polyData = reader->GetOutput();
        const auto accepted = feature->SendRequest(std::move(request));
        if (accepted) { flow->isConfirmed=false; }
        panel->SetComplete(id, accepted ? "Succeeded" : "Rejected", {{"message", "网格接口输入已设置；体数据引导流程请重新开始裁剪"}});
    });
    panel->AttachAction("ClearPolyData", {}, [panel, feature, flow](auto id, const auto&) {
        CropHostRequest request; request.action = CropHostAction::ClearPolyData;
        const auto accepted = feature->SendRequest(std::move(request));
        if (accepted) { flow->isConfirmed=false; }
        panel->SetComplete(id, accepted ? "Succeeded" : "Rejected");
    });
    for(const bool next:{false,true})panel->AttachAction(next?"NextHistoryPage":"FirstHistoryPage",{},[panel,feature,flow,next](auto id,const auto&){
        flow->historyAfter=next?feature->GetHistory(0,flow->historyAfter).nextPageAfter:0;
        panel->SetComplete(id,"Observed");
    },TestPolicy::Read);
    panel->validateAction=[panel,flow](const QString& action,const QJsonObject&)->QString{
        if(flow->pending && action!="RestoreSource" && action!="CloseDocument")return "请等待当前裁剪请求完成。";
        if(action=="Start"||action=="Box"||action=="Plane"||action=="Sphere"||action=="Cylinder"||action=="KeepInside"||action=="RemoveInside")return GetDisplayProblem(panel);
        return {};
    };
    panel->observeInBackground=true;
    panel->onObserve=[panel,feature,flow,deadline]{
        const auto input=panel->GetSession()->GetImageDescriptor();const auto current=input?input->dataRevision:DataRevisionRef{};
        if(current!=flow->input){flow->input=current;flow->isConfirmed=false;}
        const auto state=feature->GetState();const auto history=feature->GetHistory(0,flow->historyAfter);const bool framesReady=GetFramesReady(panel);
        if(flow->pending&&flow->expectedNode&&framesReady&&state.history.appliedHead==*flow->expectedNode&&state.history.renderedHead==*flow->expectedNode){
            const auto id=flow->pending;flow->pending=0;flow->expectedNode.reset();deadline->stop();
            panel->SetComplete(id,"Succeeded",{{"nodeId",QString::number(state.history.appliedHead)},{"message","历史与已呈现画面均已更新。"}});
        }
        QJsonObject disabled;
        const bool busy=flow->pending||state.isPublishing||state.history.pendingRequestCount||state.history.isDragging;
        if(busy||!framesReady)for(const auto* action:{"Start","Previous","Next","Node","ResetPreview","FinishEditing","PruneSubtree","PruneDescendants","PruneOutsidePaths","BuildResult","SaveRoi"})disabled[action]="请等待当前操作与视图完成。";
        if(!history.documentId)for(const auto* action:{"Box","Plane","Sphere","Cylinder","Previous","Next","Node","BuildResult","SaveRoi","CloseDocument","RestoreSource"})disabled[action]="请先创建裁剪文档。";
        if(!GetDataRevisionRefValid(state.outputRevision))disabled["SelectOutput"]="当前文档没有已发布结果。";
        QJsonArray nodes;for(const auto& node:history.nodes)nodes.append(QJsonObject{{"nodeId",QString::number(node.nodeId)},{"parentNodeId",QString::number(node.parentNodeId)}});
        QJsonArray documents;for(const auto document:feature->GetDocuments())documents.append(QString::number(document));
        panel->SetState({{"nextPageAfter",QString::number(history.nextPageAfter)},{"documents",documents},{"documentId",QString::number(history.documentId)},{"nodes",nodes},
            {"appliedHead",QString::number(state.history.appliedHead)},{"renderedHead",QString::number(state.history.renderedHead)},
            {"isActive",state.isActive},{"isBusy",busy},{"nodeCount",QString::number(state.history.nodeCount)},{"operationCount",QString::number(state.history.operationCount)},
            {"framesReady",framesReady},{"editMode",int(state.history.editMode)},{"isConfirmed",flow->isConfirmed},{"source",GetRefText(state.sourceRevision)},{"output",GetRefText(state.outputRevision)},
            {"documentStatus",int(state.documentStatus)},{"blockerCount",int(state.blockers.size())},{"disabled",disabled}});
    };
    panel->onStop=[panel]{if(panel->onMessage)panel->onMessage("返回源数据可取消当前裁剪构建；关闭文档会等待结果资源释放。");};
    return panel;
}
}
