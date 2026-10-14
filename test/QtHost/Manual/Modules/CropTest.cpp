// 测试用途：验证裁剪控件调整、预览确认、历史导航、发布结果及恢复源数据的业务闭环。
#include "ModuleFactories.h"
#include "Host/CropHostFeature.h"
#include "Support/RoiSelection.h"
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
        return QString("主三维显示等值阈值 %1 未落在当前灰度范围 (%2, %3) 内，无法提交裁剪预览。请在“显示与导航”设置有效的显示等值阈值，或切换为体渲染。")
            .arg(primary->isoThreshold).arg(primary->scalarRange[0]).arg(primary->scalarRange[1]);
    if(primary && (primary->viewMode==HostRenderMode::IsoSurface || primary->viewMode==HostRenderMode::CompositeIsoSurface) && primary->material.opacity<=0)
        return "主三维灰度等值面当前隐藏，裁切预览无法完成。请在“显示与导航”恢复三维体数据显示，或明确切换为体渲染；正式分析结果会保留。";
    return {};
}
void ResetInitialCamera(ModulePanel* panel, bool first)
{
    if (!first) return;
    HostViewResetRequest request; request.targetView.viewId = "primary-3d";
    (panel->GetSession()->SendRequest(std::move(request), [owner = QPointer<ModulePanel>(panel)](HostResult result) {
        if (owner && !result.isSucceeded && owner->onMessage) owner->onMessage("裁剪工具已启用，但主三维定位失败，请重置视图后拖动。");
    }).status == HostAdmissionStatus::Accepted);
}
struct CropFlow {
    DataRevisionRef input;
    DataRevisionRef viewedOutput;
    bool roiOnly=false;
    bool isConfirmed=false;
    CropDocumentId confirmedDocument=0;
    CropNodeId confirmedNode=0;
    std::uint64_t confirmedRevision=0;
    QString confirmedInputRoi;
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
    panel->SetNotice("开始会为当前数据创建裁剪文档。盒、平面、球和圆柱的每次有效拖动生成稳定节点；可选择分支、裁除子树、保存 ROI 或发布结果。返回源数据会尝试释放本次裁剪结果；仍被下游引用时保留结果，新的裁剪使用独立文档。");
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
        flow->pending=0;flow->preferredMode=mode;flow->isConfirmed=false;flow->viewedOutput={};flow->roiOnly=false;
        panel->SetParameterPatch("BuildResult",{{"inputRoi",""}});
        const QString hint=shape==CropHostAction::Sphere
            ? "球裁剪：暖白色手柄移动球心，浅金色手柄调整半径；三圆环表示空间范围，亮色外轮廓随视角更新。"
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
        if(history.documentId&&history.sourceRevision==input->dataRevision&&feature->GetState().blockers.empty()){startWidget(id,shape,mode);return;}
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
    panel->AttachAction("PrepareRoi",{{"inputRoi",""}},[panel,feature,flow](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();if(!input)throw std::invalid_argument("请先选择源体数据");
        const auto roi=GetSelectedRoi(*panel->GetSession(),p["inputRoi"],input->dataRevision);if(!roi)throw std::invalid_argument("请选择已有区域");
        if(panel->GetContext().workflow.ensureCropViews)panel->GetContext().workflow.ensureCropViews();
        panel->SetParameterPatch("BuildResult",{{"inputRoi",GetRefText(*roi)},{"nodeId","0"}});
        flow->roiOnly=true;flow->isConfirmed=false;flow->viewedOutput={};
        const auto history=feature->GetHistory();
        if(history.documentId && history.sourceRevision==input->dataRevision && feature->GetState().blockers.empty()
            && (history.appliedHead==history.rootNodeId || history.results.empty())) {
            const auto ready=[owner=QPointer<ModulePanel>(panel),flow,id,roi=*roi] {
                if(!owner)return;flow->pending=0;
                owner->SetComplete(id,"ParametersCopied",{{"roi",GetRefText(roi)},{"message","按文档源数据的正式区域生成；保留交互历史，不叠加历史裁剪范围。"}});
            };
            if(history.appliedHead==history.rootNodeId){ready();return;}
            // 只切回源预览节点，保留全部历史分支；确认与正式 ROI 构建使用同一 Root。
            CropEditRequest request;request.kind=CropEditKind::Select;request.documentId=history.documentId;
            request.nodeId=history.rootNodeId;request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;
            flow->pending=id;const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),flow,id,ready](CropEditOutcome outcome) {
                if(!owner)return;if(outcome.status==CropEditStatus::Succeeded)ready();
                else{flow->pending=0;owner->SetComplete(id,"Failed",{{"failureReason",int(outcome.failureReason)}});}
            });if(!admission)flow->pending=0;panel->SetAdmission(id,admission.isAccepted);return;
        }
        // 现存非 Root 文档若有生成结果，另建文档，避免 Root 返回事务释放原结果。
        CropDocumentRequest request;request.action=CropDocumentAction::CreateDocument;request.requestId=CropHostFeature::CreateRequestId();request.target=GetCropTarget();request.sourceRevision=input->dataRevision;
        flow->pending=id;const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),flow,id](CropDocumentOutcome outcome) {
            if(!owner)return;flow->pending=0;owner->SetComplete(id,outcome.status==CropEditStatus::Succeeded?"ParametersCopied":"Failed",{{"documentId",QString::number(outcome.documentId)}});
        });if(!admission)flow->pending=0;panel->SetAdmission(id,admission.isAccepted);
    },TestPolicy::Compute,true);
    for(const auto& action:std::vector<std::pair<QString,CropHostAction>>{{"Box",CropHostAction::Box},{"Plane",CropHostAction::Plane},{"Sphere",CropHostAction::Sphere},{"Cylinder",CropHostAction::Cylinder}})
        panel->AttachAction(action.first,{},[startWidget,flow,action](auto id,const auto&){startWidget(id,action.second,flow->preferredMode);},TestPolicy::Interaction,true);
    panel->AttachAction("Mode",{{"removalMode","KeepInside"}},[panel,feature,flow](auto id,const auto& p){
        CropHostRequest request;request.action=CropHostAction::Mode;request.target=GetCropTarget();
        request.removalMode=GetEnum<CropRemovalMode>(p,"removalMode",{{"None",CropRemovalMode::None},{"KeepInside",CropRemovalMode::KeepInside},{"RemoveInside",CropRemovalMode::RemoveInside}});
        const auto accepted=feature->SendRequest(request);if(accepted){flow->preferredMode=*request.removalMode;flow->isConfirmed=false;flow->viewedOutput={};}
        panel->SetComplete(id,accepted?"Succeeded":"Rejected");
    },TestPolicy::Interaction,true);
    panel->AttachAction("AppendExact",GetJson(R"({"shape":"Box","removalMode":"KeepInside","matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"center":[0,0,0],"axis":[0,0,1],"radius":1,"height":1})"),[panel,feature,flow](auto id,const auto& p){
        const auto history=feature->GetHistory();
        CropEditRequest request;request.documentId=history.documentId;request.nodeId=history.rootNodeId;
        request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;request.kind=CropEditKind::Append;
        auto& op=request.operation;
        op.geometryType=GetEnum<CropShape>(p,"shape",{{"Box",CropShape::Box},{"Plane",CropShape::Plane},{"Sphere",CropShape::Sphere},{"Cylinder",CropShape::Cylinder}});
        op.removalMode=GetEnum<CropRemovalMode>(p,"removalMode",{{"KeepInside",CropRemovalMode::KeepInside},{"RemoveInside",CropRemovalMode::RemoveInside}});
        op.boxToInputModelMatrix=GetAffineMatrix(p["matrix"]);
        op.centerInInputModel=GetArray<double,3>(p["center"]);op.planeCenterInInputModel=op.centerInInputModel;
        op.axisInInputModel=GetArray<double,3>(p["axis"]);op.planeNormalInInputModel=op.axisInInputModel;
        op.radius=GetNumber(p,"radius");op.height=GetNumber(p,"height");
        const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),id](CropEditOutcome result){
            if(owner && result.status!=CropEditStatus::Queued)owner->SetComplete(id,result.status==CropEditStatus::Succeeded?"Succeeded":"Failed",{{"nodeId",QString::number(result.nodeId)},{"failureReason",int(result.failureReason)}});
        });
        panel->SetAdmission(id,admission.isAccepted);
        if(admission.isAccepted){flow->isConfirmed=false;flow->viewedOutput={};}
    },TestPolicy::Compute);
    for(const auto& mode:std::vector<std::pair<QString,CropRemovalMode>>{{"KeepInside",CropRemovalMode::KeepInside},{"RemoveInside",CropRemovalMode::RemoveInside},{"PositionOnly",CropRemovalMode::None}})
        panel->AttachAction(mode.first,{},[panel,feature,flow,mode](auto id,const auto&){
            CropHostRequest request;request.action=CropHostAction::Mode;request.target=GetCropTarget();request.removalMode=mode.second;
            const bool accepted=feature->SendRequest(request);if(accepted){flow->preferredMode=mode.second;flow->isConfirmed=false;flow->viewedOutput={};}
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
            flow->pending=id;flow->expectedNode.reset();flow->isConfirmed=false;flow->viewedOutput={};
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
                QJsonArray blockers;QStringList references;
                for(const auto& blocker:value.blockers){
                    const auto revision=GetRefText(blocker.revision);
                    const auto name=blocker.owner=="DerivedRevision"?QString("下游结果"):QString::fromStdString(blocker.owner);
                    blockers.append(QJsonObject{{"revision",revision},{"owner",QString::fromStdString(blocker.owner)}});
                    references.append(name+" "+revision);
                }
                const auto message=value.failureReason==CropFailure::ResultInUse
                    ? "裁剪输出仍被引用，已保留，暂不能释放："+references.join("、")+"。对应源数据和下游结果仍可查询；结束会话会统一清理资源。" : QString();
                owner->SetComplete(id,value.status==CropEditStatus::Succeeded?"Succeeded":"Failed",{{"documentId",QString::number(value.documentId)},{"failureReason",int(value.failureReason)},{"blockerCount",int(value.blockers.size())},{"blockers",blockers},{"message",message}});
            });
            if(!admission)flow->pending=previousPending;panel->SetAdmission(id,admission.isAccepted);
        },action.second==CropDocumentAction::CloseDocument||action.second==CropDocumentAction::ReturnToSource?TestPolicy::Stop:TestPolicy::Compute);
    }
    for(const QString action:{QString("FinishEditing"),QString("Exit")})panel->AttachAction(action,{},[panel,feature,flow,action](auto id,const auto&){
        CropHostRequest request;request.action=CropHostAction::Exit;
        const bool accepted=(action=="FinishEditing" && !feature->GetState().isActive) || feature->SendRequest(request);
        if(accepted) {
            const auto history=feature->GetHistory();
            const auto roiText=panel->GetActionParameters("BuildResult")["inputRoi"].toString();
            const auto roi=roiText.isEmpty()?std::optional<RoiDescriptor>{}:panel->GetSession()->GetRoiDescriptor(GetRef(roiText));
            const bool roiRoot=roi && !roi->metadata.isArchived && roi->definition.source==history.sourceRevision;
            flow->isConfirmed=action=="FinishEditing"&&(feature->GetState().history.nodeCount>0 || roiRoot);
            flow->confirmedDocument=history.documentId;flow->confirmedNode=history.appliedHead;flow->confirmedRevision=history.stateRevision;
            flow->confirmedInputRoi=panel->GetActionParameters("BuildResult")["inputRoi"].toString();
        }
        panel->SetComplete(id,accepted?(flow->isConfirmed?"PreviewConfirmed":"Exited"):"Rejected");
    },TestPolicy::Change);
    panel->AttachAction("BuildResult",{{"nodeId","0"},{"inputRoi",""}},[panel,feature,flow](auto id,const auto& p){
        const auto history=feature->GetHistory();CropBuildRequest request;
        request.documentId=history.documentId;request.nodeId=GetId(p["nodeId"]);if(!request.nodeId)request.nodeId=history.appliedHead;
        request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=history.stateRevision;
        request.inputRoi=GetSelectedRoi(*panel->GetSession(),p["inputRoi"],history.sourceRevision);
        if(request.inputRoi)request.nodeId=history.rootNodeId;
        flow->pending=id;
        const auto admission=feature->SendRequest(request,[owner=QPointer<ModulePanel>(panel),flow,id](CropBuildResult result){
            if(!owner)return;if(flow->pending==id)flow->pending=0;if(result.isSucceeded)flow->viewedOutput=result.outputRevision;
            owner->SetComplete(id,result.isSucceeded?"Published":result.failureReason==CropFailure::Cancelled ? "Cancelled":"Failed",{{"failureReason",int(result.failureReason)},{"message",QString::fromStdString(result.message)},
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
    panel->AttachAction("ViewOutput",{{"revision",""}},[panel,flow](auto id,const auto& p){const auto ref=GetRef(p["revision"]);if(!panel->GetSession()->GetImageDescriptor(ref))throw std::invalid_argument("所选裁剪结果已不可读");flow->viewedOutput=ref;panel->SetComplete(id,"Observed",{{"output",GetRefText(ref)}});},TestPolicy::Read);
    panel->AttachAction("SelectOutput",{{"revision",""}},[panel,feature,flow](auto id,const auto& p){const auto explicitRef=GetText(p,"revision");panel->SetInput(id,!explicitRef.isEmpty()?GetRef(p["revision"]):GetDataRevisionRefValid(flow->viewedOutput)?flow->viewedOutput:feature->GetState().outputRevision);},TestPolicy::Input,true);
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
    panel->validateAction=[panel,flow,feature](const QString& action,const QJsonObject& p)->QString{
        if(flow->pending && action!="RestoreSource" && action!="CloseDocument")return "请等待当前裁剪请求完成。";
        if(action=="Start"||action=="Box"||action=="Plane"||action=="Sphere"||action=="Cylinder"||action=="KeepInside"||action=="RemoveInside")return GetDisplayProblem(panel);
        if(action=="BuildResult") {
            const auto history=feature->GetHistory(); const auto node=GetId(p["nodeId"]);
            if(!flow->isConfirmed || history.documentId!=flow->confirmedDocument || history.appliedHead!=flow->confirmedNode
                || history.stateRevision!=flow->confirmedRevision || (node && node!=flow->confirmedNode) || p["inputRoi"].toString()!=flow->confirmedInputRoi)
                return "请重新确认本次文档、源数据和预览节点，再生成裁剪结果。";
        }
        return {};
    };
    panel->observeInBackground=true;
    panel->onObserve=[panel,feature,flow,deadline]{
        const auto input=panel->GetSession()->GetImageDescriptor();const auto current=input?input->dataRevision:DataRevisionRef{};
        if(current!=flow->input){flow->input=current;flow->isConfirmed=false;}
        const auto state=feature->GetState();const auto history=feature->GetHistory(0,flow->historyAfter);const bool framesReady=GetFramesReady(panel);
        if(flow->isConfirmed && (history.documentId!=flow->confirmedDocument || history.appliedHead!=flow->confirmedNode
            || history.stateRevision!=flow->confirmedRevision || panel->GetActionParameters("BuildResult")["inputRoi"].toString()!=flow->confirmedInputRoi)) flow->isConfirmed=false;
        if(flow->pending&&flow->expectedNode&&framesReady&&state.history.appliedHead==*flow->expectedNode&&state.history.renderedHead==*flow->expectedNode){
            const auto id=flow->pending;flow->pending=0;flow->expectedNode.reset();deadline->stop();
            panel->SetComplete(id,"Succeeded",{{"nodeId",QString::number(state.history.appliedHead)},{"message","历史与已呈现画面均已更新。"}});
        }
        QJsonObject disabled;
        const auto displayProblem=GetDisplayProblem(panel);
        if(!displayProblem.isEmpty())for(const auto* action:{"Start","Box","Plane","Sphere","Cylinder","KeepInside","RemoveInside"})disabled[action]=displayProblem;
        const bool busy=flow->pending||state.isPublishing||state.history.pendingRequestCount||state.history.isDragging;
        if(busy||!framesReady)for(const auto* action:{"Start","Previous","Next","Node","ResetPreview","FinishEditing","PruneSubtree","PruneDescendants","PruneOutsidePaths","BuildResult","SaveRoi"})disabled[action]="请等待当前操作与视图完成。";
        if(!history.documentId)for(const auto* action:{"Box","Plane","Sphere","Cylinder","Previous","Next","Node","BuildResult","SaveRoi","CloseDocument","RestoreSource"})disabled[action]="请先创建裁剪文档。";
        if(!GetDataRevisionRefValid(state.outputRevision) && !GetDataRevisionRefValid(flow->viewedOutput))disabled["SelectOutput"]="当前文档没有已发布结果。";
        if(!flow->isConfirmed)disabled["BuildResult"]="请先确认当前裁剪预览；切换节点或来源后须重新确认。";
        const auto roiText=panel->GetActionParameters("BuildResult")["inputRoi"].toString();
        const auto roi=roiText.isEmpty()?std::optional<RoiDescriptor>{}:panel->GetSession()->GetRoiDescriptor(GetRef(roiText));
        const bool roiRoot=roi && !roi->metadata.isArchived && roi->definition.source==history.sourceRevision;
        if(!state.history.nodeCount && !roiRoot)disabled["FinishEditing"]="当前路径没有裁剪操作；请调整控件，或选择同源正式 ROI。";
        bool outputMatches=false;QJsonArray results;
        for(const auto& result:history.results)if(result.status==CropResultStatus::Published) {
            const auto inputRoi=result.inputRoi?GetRefText(*result.inputRoi):QString();
            if(result.outputRevision==state.outputRevision)outputMatches=result.nodeId==(roiRoot?history.rootNodeId:history.appliedHead) && inputRoi==roiText && result.sourceRevision==history.sourceRevision;
            results.append(QJsonObject{{"node",QString::number(result.nodeId)},{"output",GetRefText(result.outputRevision)},{"source",GetRefText(result.sourceRevision)},{"inputRoi",inputRoi}});
        }
        QJsonArray nodes;for(const auto& node:history.nodes)nodes.append(QJsonObject{{"nodeId",QString::number(node.nodeId)},{"parentNodeId",QString::number(node.parentNodeId)}});
        QJsonObject appliedOperation;
        for (const auto& node : history.nodes) if (node.nodeId == history.appliedHead && node.operation) {
            const auto& op = *node.operation;
            const auto shape = op.geometryType == CropShape::Box ? "Box" : op.geometryType == CropShape::Plane ? "Plane"
                : op.geometryType == CropShape::Sphere ? "Sphere" : "Cylinder";
            appliedOperation = {{"shape", shape}, {"removalMode", int(op.removalMode)},
                {"matrix", GetValues(op.boxToInputModelMatrix)},
                {"center", GetValues(op.geometryType == CropShape::Plane ? op.planeCenterInInputModel : op.centerInInputModel)},
                {"axis", GetValues(op.geometryType == CropShape::Plane ? op.planeNormalInInputModel : op.axisInInputModel)},
                {"radius", op.radius}, {"height", op.height}};
        }
        QJsonArray documents;for(const auto document:feature->GetDocuments())documents.append(QString::number(document));
        panel->SetState({{"nextPageAfter",QString::number(history.nextPageAfter)},{"documents",documents},{"documentId",QString::number(history.documentId)},{"nodes",nodes},
            {"appliedHead",QString::number(state.history.appliedHead)},{"renderedHead",QString::number(state.history.renderedHead)},
            {"isActive",state.isActive},{"isBusy",busy},{"nodeCount",QString::number(state.history.nodeCount)},{"operationCount",QString::number(state.history.operationCount)},
            {"framesReady",framesReady},{"editMode",int(state.history.editMode)},{"isConfirmed",flow->isConfirmed},{"source",GetDataRevisionRefValid(state.sourceRevision)?GetRefText(state.sourceRevision):QString()},{"output",GetDataRevisionRefValid(state.outputRevision)?GetRefText(state.outputRevision):QString()},
            {"documentStatus",int(state.documentStatus)},{"failureReason",int(state.failureReason)},{"blockerCount",int(state.blockers.size())},{"disabled",disabled},
            {"outputMatchesPreview",outputMatches},{"viewingOutput",GetDataRevisionRefValid(flow->viewedOutput)},{"viewedOutput",GetDataRevisionRefValid(flow->viewedOutput)?GetRefText(flow->viewedOutput):QString()},
            {"hasCurrentPreview",state.history.nodeCount>0 || roiRoot},{"roiOnly",roiRoot},{"results",results},{"appliedOperation",appliedOperation}});
    };
    panel->onStop=[panel]{panel->SendAction("RestoreSource",{});};
    panel->stopDescription="返回源体、结束预览并尝试释放本次裁剪输出；下游仍引用时保留结果。已有生成任务需要先结束，此动作不是独立取消生成。";
    panel->getGrayScope=[panel,feature,flow] {
        GrayStatisticsScope scope;const auto state=feature->GetState();const auto history=feature->GetHistory();
        if(GetDataRevisionRefValid(flow->viewedOutput)){scope.source=flow->viewedOutput;scope.title="正在查看的已发布裁剪结果";scope.key=GetRefText(flow->viewedOutput);return scope;}
        if(!history.documentId)return scope;
        const auto roi=panel->GetActionParameters("BuildResult")["inputRoi"].toString();
        if(!roi.isEmpty()) {scope.source=history.sourceRevision;scope.roi=GetRef(roi);
            scope.title="用于生成裁剪结果的已有区域灰度分布";scope.description="区域 ∩ 文档源有效域；交互裁剪历史不叠加";return scope;}
        if(!state.history.nodeCount)return scope;
        scope.source=history.sourceRevision;scope.title="当前裁剪预览保留区的灰度分布";
        scope.key=QString("preview:%1:%2:%3").arg(history.documentId).arg(history.renderedHead).arg(history.stateRevision);
        scope.description=QString("预览文档 %1 · 节点 %2 · 确定保留区 ∩ 源有效域；边界带与无法保证精度的样本单列排除").arg(history.documentId).arg(history.renderedHead);
        if(!roi.isEmpty())scope.roi=GetRef(roi);
        if(!state.history.nodeCount)return scope;
        scope.filter=[feature,document=history.documentId,node=history.renderedHead,revision=history.stateRevision](const auto& points) {
            const auto precision=feature->GetPreviewPrecision(document,"primary-3d",points);
            if(precision.failureReason!=CropFailure::None || precision.renderedHead!=node || precision.stateRevision!=revision)throw std::runtime_error("预览节点或已呈现范围已变化，等待重新统计");
            std::vector<bool> kept;kept.reserve(precision.samples.size());for(auto value:precision.samples)kept.push_back(value==CropPointClassification::Kept);return kept;
        };
        return scope;
    };
    return panel;
}
}
