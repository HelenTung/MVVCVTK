// 测试用途：材料等值面、结果用途、阈值复制与有界采样。
#include "ModuleFactories.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Support/ParameterEditor.h"
#include "Support/ReferenceDataSource.h"
#include "Support/RoiSelection.h"
#include "Support/ReportDocument.h"
#include "Support/SurfaceRead.h"
#include <QPointer>
#include <QComboBox>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QLocale>
#include <cmath>
#include <limits>
namespace Manual {
namespace {
QJsonObject GetIsoEstimate(const SurfaceIsoEstimate& value)
{
    QJsonObject result{{"iso",value.isoValue},{"samples",QString::number(value.sampleCount)},{"isAutomatic",value.sampleCount>0}};
    if(value.sampleCount){result["background"]=value.backgroundValue;result["material"]=value.materialValue;}
    else result["providedRange"]=QJsonArray{value.backgroundValue,value.materialValue};
    return result;
}
QJsonObject GetSurface(const std::shared_ptr<SurfaceDeterminationHostFeature>& feature,QJsonObject& formalStatistics,
    std::size_t objectOffset=0,std::size_t objectLimit=256,std::shared_ptr<const SurfaceGenerationSnapshot> anchored={})
{
    const auto state = feature->GetState();
    QJsonObject summary{{"stage", static_cast<int>(state.stage)}, {"failureReason", static_cast<int>(state.failureReason)},
        {"requestId", QString::number(state.requestId)}, {"source", GetRefText(state.sourceRevision)},
        {"resultRevision", QString::number(state.resultRevision)}, {"progress", state.progress01},
        {"points", QString::number(state.pointCount)}, {"acceptedPoints", QString::number(state.acceptedPointCount)},
        {"lowContrastPoints", QString::number(state.lowContrastPointCount)}, {"rejectedPoints", QString::number(state.rejectedPointCount)},
        {"truncatedPoints", QString::number(state.truncatedPointCount)}, {"error", QString::fromStdString(state.errorMessage)}};
    summary["purpose"] = static_cast<int>(state.purpose);
    summary["isDisplayComplete"]=state.isDisplayComplete;summary["hasDisplayFailure"]=state.hasDisplayFailure;
    summary["displayRequestedBatches"]=QString::number(state.displayRequestedBatches);summary["displayLoadedBatches"]=QString::number(state.displayLoadedBatches);
    summary["displayMessage"]=QString::fromStdString(state.displayMessage);
    summary["hasDisplayGeometry"]=state.displayLoadedBatches>0;
    summary["selectionGeneration"]=GetRefText(state.selectionGeneration);
    summary["selectedObject"]=state.selectedObject?QJsonValue(QString::number(*state.selectedObject)):QJsonValue();
    summary["isSelectionIsolated"]=state.isSelectionIsolated;summary["isObjectPickingEnabled"]=state.isObjectPickingEnabled;
    summary["taskDiagnostics"]=QJsonObject{{"points",summary["points"]},{"acceptedPoints",summary["acceptedPoints"]},{"rejectedPoints",summary["rejectedPoints"]},{"purpose",summary["purpose"]}};
    if (state.isoEstimate) summary["isoEstimate"] = GetIsoEstimate(*state.isoEstimate);
    const auto preview = feature->GetPreviewSnapshot();
    summary["previewRequestId"] = preview ? QString::number(preview->requestId) : QString();
    summary["previewPoints"]=preview && preview->points?QString::number(preview->points->size()):QString();
    // 只在当前调用范围持有正式网格；预览和估计不替换正式结果身份。
    const auto snapshot = anchored ? anchored : feature->GetSurfaceSnapshot();
    if (!snapshot) return summary;
    if(anchored) {
        const auto currentTask=summary;
        summary={{"currentTask",currentTask},{"requestId",QString::number(snapshot->requestId)},
            {"source",GetRefText(snapshot->sourceRevision)},{"purpose",static_cast<int>(snapshot->purpose)},
            {"stage",static_cast<int>(SurfaceDeterminationStage::Ready)},{"progress",1.0},
            {"failureReason",static_cast<int>(SurfaceFailureReason::None)},{"error",QString()}};
        if(snapshot->isoEstimate)summary["isoEstimate"]=GetIsoEstimate(*snapshot->isoEstimate);
    }
    summary["mesh"] = GetRefText(snapshot->meshRevision);
    summary["generation"]=GetRefText(snapshot->dataRevision);summary["selectedAreaMM2"]=snapshot->selectedAreaMM2;summary["selectionToleranceMM"]=snapshot->selectionToleranceMM;summary["selectionMesh"]=GetRefText(snapshot->selectionMeshRevision);
    summary["formalSource"]=GetRefText(snapshot->sourceRevision);
    summary["canonicalParameters"]=QString::fromStdString(snapshot->canonicalParameters);
    QJsonArray inputs;
    for(const auto& input:snapshot->inputs)inputs.append(QJsonObject{{"role",QString::fromStdString(input.role)},{"revision",GetRefText(input.source)}});
    summary["inputs"]=inputs;
    summary["points"]=snapshot->points?QString::number(snapshot->points->size()):QString();
    if(state.purpose==SurfaceTaskPurpose::Determine && state.requestId==snapshot->requestId)
        formalStatistics={{"mesh",summary["mesh"]},{"acceptedPoints",QString::number(state.acceptedPointCount)},{"lowContrastPoints",QString::number(state.lowContrastPointCount)},{"rejectedPoints",QString::number(state.rejectedPointCount)},{"truncatedPoints",QString::number(state.truncatedPointCount)}};
    for(const auto* key:{"acceptedPoints","lowContrastPoints","rejectedPoints","truncatedPoints"})summary[key]=formalStatistics["mesh"]==summary["mesh"]?formalStatistics[key]:QJsonValue();
    summary["method"] = static_cast<int>(snapshot->method);
    if (!state.isoEstimate && snapshot->isoEstimate) summary["isoEstimate"] = GetIsoEstimate(*snapshot->isoEstimate);
    QJsonArray objects;
    const auto objectCount=snapshot->objects?snapshot->objects->size():0;
    if(objectOffset>objectCount)throw std::invalid_argument("对象分页偏移超过完整对象数量");
    const auto objectEnd=objectOffset+std::min(objectLimit,objectCount-objectOffset);
    const auto page=snapshot->GetObjectPage(objectOffset,objectLimit);
    if(!page)throw std::invalid_argument("对象页不可读");
    for(const auto& object:page->objects) {
        objects.append(QJsonObject{{"objectIndex", QString::number(object.objectIndex)}, {"closed", object.isClosed},
            {"manifold", object.isManifold}, {"truncated", object.isTruncated}, {"orientationValid", object.isOrientationValid},
            {"areaValidity", static_cast<int>(object.areaValidity)}, {"volumeValidity", static_cast<int>(object.volumeValidity)},
            {"area", object.areaModelUnit2 ? QJsonValue(*object.areaModelUnit2) : QJsonValue()},
            {"volume", object.volumeModelUnit3 ? QJsonValue(*object.volumeModelUnit3) : QJsonValue()},
            {"firstPoint",QString::number(object.firstPoint)},{"pointCount",QString::number(object.pointCount)},
            {"firstTriangle",QString::number(object.firstTriangle)},{"triangleCount",QString::number(object.triangleCount)},
            {"boundsModel",GetValues(object.boundsModel)}});
    }
    summary["objects"] = objects;
    summary["objectCount"] = GetReportCountValue(static_cast<quint64>(objectCount));
    summary["objectSummaryLimit"] = static_cast<int>(objectLimit);
    summary["objectOffset"]=QString::number(objectOffset);summary["nextObjectOffset"]=QString::number(objectEnd);
    summary["objectsComplete"]=objectEnd==objectCount;
    return summary;
}
}
ModulePanel* CreateSurfaceTest(TestContext context, std::shared_ptr<SurfaceDeterminationHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Surface", parent);
    panel->SetNotice("选择体数据后，阈值留空时由当前表面算法估计背景/材料峰；显式阈值或材料范围直接按所填值处理。继续执行真实表面定位和质量检查，定位稳定性不等于计量不确定度。");
    const auto parameters = GetJson(R"({"componentSelection":"All","initialIsoValue":null,"materialRange":null,"seedModelPoint":null,"analysisRoi":"","parentGeneration":"","minimumObjectVoxels":"0"})");
    auto requestedParameters=std::make_shared<QJsonObject>();
    auto formalStatistics=std::make_shared<QJsonObject>();
    auto listedSurface=std::make_shared<std::shared_ptr<const SurfaceGenerationSnapshot>>();
    const auto getEstimateSnapshot=[panel,feature]() -> std::shared_ptr<const SurfaceGenerationSnapshot>{
        auto snapshot=feature->GetPreviewSnapshot();const auto formal=feature->GetSurfaceSnapshot();
        if(!snapshot||(formal&&formal->requestId>snapshot->requestId))snapshot=formal;
        const auto input=panel->GetSession()->GetImageDescriptor();if(!snapshot||!input||snapshot->sourceRevision!=input->dataRevision)return {};
        const auto roi=snapshot->requestedParams.analysisRoi?GetRefText(*snapshot->requestedParams.analysisRoi):QString();
        if(roi!=panel->GetActionParameters("GlobalAutomatic")["analysisRoi"].toString())return {};
        return snapshot;
    };
    panel->getGrayScope=[panel]{
        GrayStatisticsScope scope;const auto input=panel->GetSession()->GetImageDescriptor();if(!input)return scope;scope.source=input->dataRevision;
        const QJsonValue value=panel->GetActionParameters("GlobalAutomatic")["analysisRoi"];
        if(const auto roi=GetSelectedRoi(*panel->GetSession(),value,input->dataRevision)){scope.roi=*roi;scope.key=GetRefText(*roi);scope.title="当前表面分析范围的灰度分布";scope.description="按当前表面分析 ROI 统计；预览前不使用其他范围的旧估计。";}
        return scope;
    };
    panel->AttachAction("ReadObjects",{{"pageOffset","0"},{"pageSize","256"},{"generation","current"}},
        [panel,feature,formalStatistics,listedSurface](auto id,const auto& p) {
            const auto offset=GetId(p["pageOffset"]),count=GetId(p["pageSize"]);
            if(!count||count>4096)throw std::invalid_argument("对象分页大小必须为 1～4096");
            DataRevisionRef revision;
            if(!p["generation"].toString().isEmpty()&&p["generation"]!="current")revision=GetRef(p["generation"]);
            else if(offset)revision=GetRef(panel->property("surfaceObjectsGeneration").toString());
            else { const auto snapshot=feature->GetSurfaceSnapshot();if(snapshot)revision=snapshot->dataRevision; }
            const auto snapshot=*listedSurface&&(*listedSurface)->dataRevision==revision?*listedSurface:feature->GetSurfaceSnapshot(revision);
            if(!GetDataRevisionRefValid(revision)||!snapshot)throw std::invalid_argument("正式表面修订不可读；请从第一页显式刷新");
            *listedSurface=snapshot;
            panel->setProperty("surfaceObjectsGeneration",GetRefText(revision));
            const auto page=snapshot->GetObjectPage(offset,static_cast<std::size_t>(count));
            if(!page)throw std::invalid_argument("表面对象页不可读");
            QJsonArray rows,ids;
            for(const auto& object:page->objects) {
                rows.append(QJsonArray{QString::number(object.objectIndex),QString::number(object.pointCount),QString::number(object.triangleCount),
                    object.areaModelUnit2?QJsonValue(*object.areaModelUnit2):QJsonValue(),object.volumeModelUnit3?QJsonValue(*object.volumeModelUnit3):QJsonValue(),
                    static_cast<int>(object.areaValidity),static_cast<int>(object.volumeValidity)});
                ids.append(QString::number(object.objectIndex));
            }
            auto* widget=panel->GetStatisticsWidget();widget->SetTable({"对象编号","点数","三角形数","面积","体积","面积有效性","体积有效性"},rows);widget->ShowTable();
            widget->setProperty("surfaceObjectIds",ids);widget->setProperty("surfaceObjectNext",QString::number(page->next));widget->setProperty("surfaceObjectTotal",QString::number(page->total));
            widget->setProperty("surfaceObjectGeneration",GetRefText(revision));
            widget->onRow=[panel,widget](int row){const auto ids=widget->property("surfaceObjectIds").toJsonArray();if(row<0||row>=ids.size())return;
                panel->SendAction("SelectObject",{{"objectIndex",ids[row]},{"generation",widget->property("surfaceObjectGeneration").toString()}});};
            panel->SetComplete(id,"Observed",GetSurface(feature,*formalStatistics,static_cast<std::size_t>(offset),static_cast<std::size_t>(count),snapshot));
        },TestPolicy::Read);
    panel->AttachAction("NextObjectPage",{},[panel](auto id,const auto&){
        auto* widget=panel->GetStatisticsWidget();const auto next=widget->property("surfaceObjectNext").toString().toULongLong();
        if(next>=widget->property("surfaceObjectTotal").toString().toULongLong())throw std::invalid_argument("当前已是表面对象列表末页");
        auto parameters=panel->GetActionParameters("ReadObjects");parameters["pageOffset"]=QString::number(next);parameters["generation"]=widget->property("surfaceObjectGeneration").toString();
        panel->SendAction("ReadObjects",parameters);panel->SetComplete(id,"Observed");
    },TestPolicy::Read);
    const auto getObjectSnapshot=[panel,feature,listedSurface](const QJsonObject& p){
        const auto text=p["generation"].toString();
        const auto revision=text=="current"?DataRevisionRef{}:GetRef(text=="listed"?panel->property("surfaceObjectsGeneration").toString():text);
        const auto snapshot=text=="current"?feature->GetSurfaceSnapshot():*listedSurface&&(*listedSurface)->dataRevision==revision?*listedSurface:feature->GetSurfaceSnapshot(revision);
        if(!snapshot)throw std::invalid_argument("所选正式表面修订不可读");return snapshot;
    };
    const auto getObjectIndex=[](const QJsonObject& p){const auto index=GetId(p["objectIndex"]);if(index>UINT32_MAX)throw std::invalid_argument("表面对象编号越界");return static_cast<std::uint32_t>(index);};
    for(const bool isolated:{false,true})panel->AttachAction(isolated?"IsolateObject":"SelectObject",{{"generation","listed"},{"objectIndex","0"}},
        [panel,feature,getObjectSnapshot,getObjectIndex,isolated](auto id,const auto& p){
            const auto snapshot=getObjectSnapshot(p);const auto object=getObjectIndex(p);
            const auto admission=feature->SendRequest(SurfaceObjectSelectionRequest{snapshot->dataRevision,object,isolated});
            const bool accepted=admission.status==SurfaceAdmissionStatus::Accepted;
            panel->SetComplete(id,accepted?"Succeeded":"Rejected",{{"generation",GetRefText(snapshot->dataRevision)},{"objectIndex",QString::number(object)},{"isIsolated",isolated}});
            if(accepted)panel->SendAction("LocateObject",{{"generation",GetRefText(snapshot->dataRevision)},{"objectIndex",QString::number(object)}});
        },TestPolicy::View);
    panel->AttachAction("ClearObjectSelection",{},[panel,feature](auto id,const auto&){
        const auto snapshot=feature->GetSurfaceSnapshot();
        const bool accepted=snapshot&&feature->SendRequest(SurfaceObjectSelectionRequest{snapshot->dataRevision,{},false}).status==SurfaceAdmissionStatus::Accepted;
        panel->SetComplete(id,accepted?"Succeeded":"Rejected");
    },TestPolicy::View);
    panel->AttachAction("ObjectPicking",{{"isEnabled",false}},[panel,feature](auto id,const auto& p){
        const bool accepted=feature->SendRequest(SurfaceObjectPickModeRequest{GetBool(p,"isEnabled")}).status==SurfaceAdmissionStatus::Accepted;
        panel->SetComplete(id,accepted?"Succeeded":"Rejected",{{"isObjectPickingEnabled",feature->GetState().isObjectPickingEnabled}});
    },TestPolicy::View);
    panel->AttachAction("LocateObject",{{"generation","listed"},{"objectIndex","0"}},[panel,getObjectSnapshot,getObjectIndex](auto id,const auto& p){
        const auto snapshot=getObjectSnapshot(p);const auto page=snapshot->GetObjectPoints(getObjectIndex(p),0,1);
        if(!page||page->points.empty())throw std::invalid_argument("该对象没有可定位的正式网格点");
        const auto world=panel->GetContext().workflow.getModelWorldPosition(snapshot->sourceRevision,page->points[0].positionModel);
        HostSessionSetRequest request;request.cursor=HostCursorParams{world,-1};panel->SendHost(id,std::move(request));
    },TestPolicy::View);
    for(const bool points:{true,false})panel->AttachAction(points?"ReadObjectPoints":"ReadObjectTriangles",
        {{"generation","listed"},{"objectIndex","0"},{"pageOffset","0"},{"pageSize","128"}},
        [panel,getObjectSnapshot,getObjectIndex,points](auto id,const auto& p){
            const auto snapshot=getObjectSnapshot(p);const auto object=getObjectIndex(p);const auto offset=GetId(p["pageOffset"]),count=GetId(p["pageSize"]);
            if(!count||count>4096)throw std::invalid_argument("明细页大小必须为 1～4096");
            QJsonArray rows;std::uint64_t next=0,total=0,first=0;
            if(points){const auto page=snapshot->GetObjectPoints(object,offset,static_cast<std::size_t>(count));if(!page)throw std::invalid_argument("对象点范围不可读");
                first=page->firstPoint;next=page->next;total=page->total;
                for(std::size_t i=0;i<page->points.size();++i){const auto& point=page->points[i];rows.append(QJsonObject{{"pointId",QString::number(first+i)},{"position",GetValues(point.positionModel)},{"flags",QString::number(static_cast<std::uint32_t>(point.flags))}});}
            }else{const auto page=snapshot->GetObjectTriangles(object,offset,static_cast<std::size_t>(count));if(!page)throw std::invalid_argument("对象三角形范围不可读");
                first=page->firstTriangle;next=page->next;total=page->total;
                for(std::size_t i=0;i<page->triangles.size();++i){const auto& triangle=page->triangles[i];rows.append(QJsonObject{{"triangleId",QString::number(first+i)},
                    {"vertices",QJsonArray{QString::number(triangle.vertices[0]),QString::number(triangle.vertices[1]),QString::number(triangle.vertices[2])}},{"validity",triangle.validity}});}}
            panel->SetComplete(id,"Observed",{{"generation",GetRefText(snapshot->dataRevision)},{"objectIndex",QString::number(object)},
                {"first",QString::number(first)},{"next",QString::number(next)},{"total",QString::number(total)},{"rows",rows}});
        },TestPolicy::Read);
    context.workflow.highlightSurfaceObject=[feature](DataRevisionRef generation,std::uint32_t object) {
        return feature->SendRequest(SurfaceObjectSelectionRequest{generation,object,false,GetAllViews()}).status==SurfaceAdmissionStatus::Accepted;
    };
    panel->AttachAction("HighlightObject",{{"generation",""},{"objectIndex","0"}},[panel,feature,formalStatistics](auto id,const auto& p) {
        const auto snapshot=p["generation"].toString().isEmpty()?feature->GetSurfaceSnapshot():feature->GetSurfaceSnapshot(GetRef(p["generation"]));
        if(!snapshot)throw std::invalid_argument("指定修订没有正式表面对象");
        const auto index=GetId(p["objectIndex"]);if(index>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("对象编号超出声明类型");
        const QPointer<ModulePanel> owner(panel);const auto revision=snapshot->dataRevision;
        const auto admission=feature->SendRequest(SurfaceObjectSelectionRequest{revision,static_cast<std::uint32_t>(index),false,GetAllViews()},
            [owner,feature,formalStatistics,id,revision,index](SurfaceDeterminationResult result) {
                if(!owner)return;auto summary=GetSurface(feature,*formalStatistics,static_cast<std::size_t>(index),1,feature->GetSurfaceSnapshot(revision));
                summary["generation"]=GetRefText(revision);summary["formalSource"]=GetRefText(result.sourceRevision);
                summary["selectedObject"]=QString::number(index);summary["message"]=QString::fromStdString(result.message);
                owner->SetComplete(id,result.status==SurfaceResultStatus::Succeeded?"Succeeded":result.status==SurfaceResultStatus::Cancelled?"Cancelled":"Failed",summary);
            });
        panel->SetAdmission(id,admission.status==SurfaceAdmissionStatus::Accepted,{{"requestId",QString::number(admission.requestId)}});
    },TestPolicy::View);
    panel->AttachAction("ClearObjectHighlight",{},[panel,feature](auto id,const auto&) {
        const QPointer<ModulePanel> owner(panel);const auto admission=feature->SendRequest(SurfaceObjectSelectionRequest{},[owner,id](auto result){
            if(owner)owner->SetComplete(id,result.status==SurfaceResultStatus::Succeeded?"Succeeded":result.status==SurfaceResultStatus::Cancelled?"Cancelled":"Failed",{{"message",QString::fromStdString(result.message)}});
        });panel->SetAdmission(id,admission.status==SurfaceAdmissionStatus::Accepted,{{"requestId",QString::number(admission.requestId)}});
    },TestPolicy::View);
    {
        const auto send=[panel, feature,requestedParameters,formalStatistics](std::uint64_t id, const QJsonObject& params, SurfaceTaskPurpose purpose) {
            SurfaceDeterminationStartParams start;
            start.purpose=purpose;
            start.targetViews = GetAllViews();
            start.modelUnit = "mm";
            const auto source = panel->GetSession()->GetImageDescriptor();
            if (!source) throw std::invalid_argument("请先选择体数据输入");
            start.sourceVolume = source->dataRevision;
            if (params.contains("componentSelection")) start.componentSelection = GetEnum<SurfaceComponentSelection>(params, "componentSelection", {
                {"Largest", SurfaceComponentSelection::Largest}, {"Seeded", SurfaceComponentSelection::Seeded}, {"All", SurfaceComponentSelection::All}});
            if (params.contains("materialRange") && !params["materialRange"].isNull()) start.materialRange = GetArray<double, 2>(params["materialRange"]);
            if (!params["initialIsoValue"].isNull()) start.initialIsoValue = GetNumber(params, "initialIsoValue");
            if (start.componentSelection == SurfaceComponentSelection::Seeded && !params["seedModelPoint"].isNull()) start.seedModelPoint = GetArray<double, 3>(params["seedModelPoint"]);
            if(!params["parentGeneration"].toString().isEmpty())start.parentGeneration=GetRef(params["parentGeneration"]);
            start.analysisRoi = GetSelectedRoi(*panel->GetSession(), params["analysisRoi"], source->dataRevision);
            start.minimumObjectVoxels = GetId(params["minimumObjectVoxels"]);
            SurfaceStartRequest request{start};
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(std::move(request), [owner, feature, id,requestedParameters,params,purpose,formalStatistics](SurfaceDeterminationResult result) {
                if (!owner) return;
                if(result.status==SurfaceResultStatus::Succeeded && purpose==SurfaceTaskPurpose::Determine)*requestedParameters=params;
                auto summary = GetSurface(feature,*formalStatistics);
                // 取消后可保留旧正式快照，完成记录仍须属于本次回调请求。
                summary["requestId"] = QString::number(result.requestId);
                summary["failureReason"] = static_cast<int>(result.failureReason);
                summary["status"] = static_cast<int>(result.status);
                summary["message"] = result.status == SurfaceResultStatus::Cancelled
                    ? QString("表面测定已取消。") : QString::fromStdString(result.message);
                owner->SetComplete(id, result.status == SurfaceResultStatus::Succeeded ? "Succeeded"
                    : result.status == SurfaceResultStatus::Cancelled ? "Cancelled" : "Failed", summary);
            });
            panel->SetAdmission(id, admission.status == SurfaceAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        };
        panel->AttachAction("GlobalAutomatic",parameters,[send](auto id,const auto& p){send(id,p,SurfaceTaskPurpose::Determine);},TestPolicy::Compute,true);
        for(const auto& action:std::vector<std::pair<QString,SurfaceTaskPurpose>>{{"Preview",SurfaceTaskPurpose::Preview},{"Determine",SurfaceTaskPurpose::Determine}})
            panel->AttachAction(action.first,{},[panel,send,action](auto id,const auto& p){
                send(id,p.isEmpty() ? panel->GetActionParameters("GlobalAutomatic") : p,action.second);
            },TestPolicy::Compute,true);
    }
    for (const QString action : {QString("Stop"), QString("Visibility"), QString("Clear"), QString("ClearPreview")}) {
        QJsonObject defaults;
        if (action == "Visibility") defaults = {{"isVisible", true}};
        if (action == "Stop") defaults = {{"targetRequestId", "0"}};
        panel->AttachAction(action, defaults, [panel, feature, action](auto id, const auto& params) {
            SurfaceDeterminationRequest request = SurfaceClearRequest{};
            if (action == "Visibility") request = SurfaceVisibilityRequest{GetBool(params, "isVisible")};
            else if (action == "Stop") { panel->CancelReadTasks();request = SurfaceStopRequest{GetId(params["targetRequestId"])}; }
            else if (action == "ClearPreview") request = SurfaceClearPreviewRequest{};
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(std::move(request), [owner, id](SurfaceDeterminationResult result) {
                if (owner) owner->SetComplete(id, result.status == SurfaceResultStatus::Succeeded ? "Succeeded"
                    : result.status == SurfaceResultStatus::Cancelled ? "Cancelled" : "Failed",
                    {{"requestId", QString::number(result.requestId)}, {"message", QString::fromStdString(result.message)}});
            });
            panel->SetAdmission(id, admission.status == SurfaceAdmissionStatus::Accepted,
                {{"requestId", QString::number(admission.requestId)}, {"admission", static_cast<int>(admission.status)}});
        }, action == "Stop" ? TestPolicy::Stop : TestPolicy::Change);
    }
    for (const QString destination : {QString("Display"), QString("Gap"), QString("Part")})
        panel->AttachAction("CopyIsoTo" + destination, {}, [panel, feature, destination](auto id, const auto&) {
            const auto state = feature->GetState();
            const auto current = panel->GetSession()->GetImageDescriptor();
            if (!state.isoEstimate || !current || state.sourceRevision != current->dataRevision)
                throw std::invalid_argument("当前输入没有有效 ISO 估计");
            const auto iso = state.isoEstimate->isoValue;
            if (destination == "Display") {
                const auto view = panel->GetSession()->GetRenderViewState({"primary-3d"});
                if (!view || (view->viewMode != HostRenderMode::IsoSurface && view->viewMode != HostRenderMode::CompositeIsoSurface))
                    throw std::invalid_argument("主三维当前不是等值面模式，请先切换显示模式");
                HostViewSetRequest request; request.targetView.viewId = "primary-3d"; request.iso = iso;
                panel->SendHost(id, std::move(request));
            } else {
                const auto copy = panel->GetContext().workflow.onCopyParameters;
                if (!copy) throw std::invalid_argument("参数目标不可用");
                copy(destination, "Start", destination == "Gap" ? QJsonObject{{"isoMode", "AbsoluteValue"}, {"absoluteIsoValue", iso}} : QJsonObject{{"threshold", iso}});
                panel->SetComplete(id, "ParametersCopied", {{"iso", iso}});
            }
        });
    panel->AttachAction("SamplePoints", {{"maxPoints", "128"}}, [panel, feature](auto id, const auto& p) {
        const auto limit = GetId(p["maxPoints"]);
        if (limit == 0 || limit > 4096) throw std::invalid_argument("最大采样点数必须为 1～4096");
        const auto input = panel->GetSession()->GetImageDescriptor();
        auto snapshot = feature->GetSurfaceSnapshot();
        if (!snapshot || !input || snapshot->sourceRevision != input->dataRevision)
            snapshot = feature->GetPreviewSnapshot();
        if (!snapshot || !snapshot->points || !input || snapshot->sourceRevision != input->dataRevision)
            throw std::invalid_argument("当前输入没有网格点");
        const auto& points = *snapshot->points;
        const auto stride = std::max<std::size_t>(1, (points.size() + limit - 1) / limit);
        QJsonArray samples;
        for (std::size_t index = 0; index < points.size(); index += stride)
            samples.append(QJsonObject{{"vertexId", QString::number(index)}, {"sourcePoint", GetValues(points[index].positionModel)},
                {"normal", GetValues(points[index].normalModel)}, {"flags", static_cast<int>(points[index].flags)},
                {"validSupportRatio", points[index].validSupportRatio}, {"fitResidual", points[index].fitResidual},
                {"localizationSigma", points[index].estimatedLocalizationSigma}});
        QJsonArray inputs;
        for(const auto& input:snapshot->inputs)inputs.append(QJsonObject{{"role",QString::fromStdString(input.role)},{"revision",GetRefText(input.source)}});
        panel->SetComplete(id, "Observed", {{"mesh", GetRefText(snapshot->meshRevision)}, {"source", GetRefText(snapshot->sourceRevision)},
            {"generation",GetRefText(snapshot->dataRevision)},{"canonicalParameters",QString::fromStdString(snapshot->canonicalParameters)},{"inputs",inputs},
            {"samples", samples}, {"totalPoints", QString::number(points.size())}});
    }, TestPolicy::Read);
    panel->AttachAction("ExportNominalMeshFixture", {{"outputPath", ""}, {"transform", GetValues(roiIdentityMatrix)}, {"selectionOnly", true}},
        [panel, feature](auto id, const auto& p) {
            const auto input = panel->GetSession()->GetImageDescriptor();
            const auto snapshot = feature->GetSurfaceSnapshot();
            if (!snapshot || !input || snapshot->sourceRevision != input->dataRevision
                || !snapshot->points || !snapshot->triangleIndices || snapshot->triangleIndices->empty())
                throw std::invalid_argument("当前输入没有正式测量表面");
            const auto transform = GetAffineMatrix(p["transform"], true);
            const auto path = GetText(p, "outputPath");
            const auto selectionOnly=GetBool(p,"selectionOnly");
            panel->StartReadTask(id,"Exported",[snapshot,transform,path,selectionOnly](const ReadTaskContext& context){
                const auto count=ExportSurfaceFixture(path,*snapshot,transform,selectionOnly,context.checkpoint,context.commit);
                return QJsonObject{{"mesh",GetRefText(snapshot->meshRevision)},{"source",GetRefText(snapshot->sourceRevision)},
                    {"generation",GetRefText(snapshot->dataRevision)},{"triangleCount",QString::number(count)}};
            });
        }, TestPolicy::Read);
    struct MeasurementState final {
        std::uint64_t requested=0,completed=0;
        std::shared_ptr<const SurfaceMeasurementSelection> selection;
    };
    const auto measurement=std::make_shared<MeasurementState>();
    panel->AttachAction("CancelRead",{},[panel](auto id,const auto&){const auto accepted=panel->CancelReadTasks();panel->SetComplete(id,"Observed",{{"acceptedTasks",QString::number(accepted)}});},TestPolicy::Stop);
    panel->AttachAction("MeasurementSelection", {{"minSupportRatio", .5}, {"maxLocalizationSigma", .5}, {"maxSurfaceFitResidual", 1.}},
        [panel,feature,measurement](auto id,const auto& p) {
            const auto snapshot=feature->GetSurfaceSnapshot();const auto input=panel->GetSession()->GetImageDescriptor();
            if(!snapshot||!input||snapshot->sourceRevision!=input->dataRevision)
                throw std::invalid_argument("当前输入没有正式测量表面");
            SurfaceMeasurementCriteria criteria{GetNumber(p,"minSupportRatio"),GetNumber(p,"maxLocalizationSigma"),GetNumber(p,"maxSurfaceFitResidual")};
            auto output=std::make_shared<std::shared_ptr<const SurfaceMeasurementSelection>>();
            panel->StartReadTask(id,"Observed",[snapshot,criteria,p,id,output](const ReadTaskContext& context){
                const auto selected=ReadSurfaceMeasurement(*snapshot,criteria,context.checkpoint);*output=selected;
                QJsonArray vertices,triangles;
                for(std::size_t i=0;i<std::min<std::size_t>(256,selected->vertices.size());++i)vertices.append(QString::number(selected->vertices[i]));
                for(std::size_t i=0;i<std::min<std::size_t>(256,selected->triangles.size());++i)triangles.append(QString::number(selected->triangles[i]));
                return QJsonObject{{"source",GetRefText(selected->source)},{"mesh",GetRefText(selected->mesh)},
                    {"generation",GetRefText(selected->generation)},{"selectionId",QString::number(id)},
                    {"vertexIds",vertices},{"triangleIds",triangles},{"idPageLimit",256},
                    {"nextVertexOffset",QString::number(vertices.size())},{"nextTriangleOffset",QString::number(triangles.size())},
                    {"verticesComplete",static_cast<std::size_t>(vertices.size())==selected->vertices.size()},
                    {"trianglesComplete",static_cast<std::size_t>(triangles.size())==selected->triangles.size()},
                    {"totalVertices",QString::number(selected->totalVertices)},{"selectedVertices",QString::number(selected->vertices.size())},
                    {"totalTriangles",QString::number(selected->totalTriangles)},{"selectedTriangles",QString::number(selected->triangles.size())},
                    {"degenerateTriangles",QString::number(selected->degenerate)},{"rejectedQualityTriangles",QString::number(selected->rejectedQuality)},{"criteria",p}};
            },[measurement,output,id]{if(measurement->requested==id){measurement->completed=id;measurement->selection=*output;}});
            measurement->requested=id;
        },TestPolicy::Read);
    panel->AttachAction("ReadMeasurementSelection",{{"selectionId","latest"},{"kind","Triangles"},{"pageOffset","0"},{"pageSize","256"}},
        [panel,measurement](auto id,const auto& p){
            const auto selected=measurement->selection;const auto selectionId=p["selectionId"]=="latest"?measurement->completed:GetId(p["selectionId"]);
            if(!selected||selectionId!=measurement->completed)throw std::invalid_argument("筛选编号已失效，请显式重新读取测量选区");
            const auto kind=GetText(p,"kind");if(kind!="Triangles"&&kind!="Vertices")throw std::invalid_argument("编号种类无效");
            const auto& ids=kind=="Triangles"?selected->triangles:selected->vertices;
            const auto offset=GetId(p["pageOffset"]),count=GetId(p["pageSize"]);
            if(offset>ids.size()||!count||count>4096)throw std::invalid_argument("编号分页范围无效，页大小应为 1～4096");
            const auto end=offset+std::min<std::uint64_t>(count,ids.size()-offset);QJsonArray page;
            for(auto i=offset;i<end;++i)page.append(QString::number(ids[static_cast<std::size_t>(i)]));
            panel->SetComplete(id,"Observed",{{"selectionId",QString::number(selectionId)},{"generation",GetRefText(selected->generation)},
                {"kind",kind},{"ids",page},{"next",QString::number(end)},{"total",QString::number(ids.size())},{"complete",end==ids.size()}});
        },TestPolicy::Read);
    panel->AttachAction("ExportComparison", {{"outputPath",""}}, [panel,feature](auto id,const auto& p){
        auto snapshot=feature->GetSurfaceSnapshot();if(!snapshot)snapshot=feature->GetPreviewSnapshot();
        if(!snapshot||!snapshot->points)throw std::runtime_error("表面点不可用");
        const auto path=GetText(p,"outputPath");
        panel->StartReadTask(id,"Exported",[snapshot,path](const ReadTaskContext& context){
            ExportSurfaceComparison(path,*snapshot,context.checkpoint,context.commit);
            return QJsonObject{{"points",QString::number(snapshot->points->size())},{"triangles",QString::number(snapshot->triangleIndices?snapshot->triangleIndices->size()/3:0)},
                {"canonicalParameters",QString::fromStdString(snapshot->canonicalParameters)},{"objectCount",QString::number(snapshot->objects?snapshot->objects->size():0)},
                {"generation",GetRefText(snapshot->dataRevision)},{"coordinateFrame",QString::fromStdString(snapshot->coordinateFrame)}};
        });
    },TestPolicy::Read);
#if defined(MANUAL_ALIGNMENT)
    panel->AttachAction("OpenAlignment", {}, [panel,feature](auto id, const auto&) {
        const auto surface=feature->GetSurfaceSnapshot();if(!surface)throw std::invalid_argument("正式测量表面不可用");
        if (panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("Alignment", "ImportReference", {{"source",GetRefText(surface->sourceRevision)},{"mesh",GetRefText(surface->meshRevision)}});
        panel->SetComplete(id, "ParametersCopied", {{"source",GetRefText(surface->sourceRevision)},{"mesh",GetRefText(surface->meshRevision)},{"message", "已带入正式网格；请准备该目标的名义参考"}});
    }, TestPolicy::Read);
#endif
#if defined(MANUAL_WALL)
    panel->AttachAction("OpenWall",{},[panel,feature](auto id,const auto&) {
        const auto surface=feature->GetSurfaceSnapshot();
        if(!surface) throw std::invalid_argument("正式测量表面不可用");
        const auto labels=panel->GetContext().workflow.getPartLabels ? panel->GetContext().workflow.getPartLabels() : DataRevisionRef{};
        QJsonObject patch{{"source",GetRefText(surface->sourceRevision)},{"mesh",GetRefText(surface->meshRevision)}};
        if(GetDataRevisionRefValid(labels)) patch["labels"]=GetRefText(labels);
        if(surface->resolvedParams.initialIsoValue) patch["materialThreshold"]=*surface->resolvedParams.initialIsoValue;
        if(panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("Wall","Start",patch);
        panel->SetComplete(id,"ParametersCopied",{{"message",GetDataRevisionRefValid(labels) ? "已带入同源测量表面，请核对材料标签。" : "已带入测量表面，仍需生成同源零件标签。"}});
    },TestPolicy::Read);
#endif
    panel->observeInBackground = true;
    panel->onObserve = [panel, feature,requestedParameters,formalStatistics,getEstimateSnapshot] {
        const auto image = panel->GetSession()->GetImageDescriptor();
        if (image) {
            auto* form = panel->GetParameterEditor("GlobalAutomatic");
            auto range = image->scalarRange;
            try {
                const auto requested = form->GetField("materialRange")->GetValue();
                if (!requested.isNull()) range = GetArray<double,2>(requested);
            } catch (const std::exception&) { /* 未完成的手工输入留给提交校验。 */ }
            form->GetField("initialIsoValue")->SetDefaultValue(range[0] * 0.5 + range[1] * 0.5);
            form->GetField("materialRange")->SetDefaultValue(GetValues(image->scalarRange));
        }
        auto summary = GetSurface(feature,*formalStatistics);
        const auto state = feature->GetState();
        const auto snapshot = feature->GetSurfaceSnapshot();
        const auto input = panel->GetSession()->GetImageDescriptor();
        const auto preview = feature->GetPreviewSnapshot();
        const bool current = input && snapshot && snapshot->sourceRevision == input->dataRevision;
        const bool hasPreview = input && preview && preview->sourceRevision == input->dataRevision;
        const bool measured = current && GetDataRevisionRefValid(snapshot->meshRevision)
            && snapshot->purpose == SurfaceTaskPurpose::Determine && feature->GetResultValidity(snapshot->dataRevision).canMeasure;
        const auto estimateSnapshot=getEstimateSnapshot();
        summary["hasIso"] = estimateSnapshot&&(estimateSnapshot->isoEstimate.has_value()||estimateSnapshot->resolvedParams.initialIsoValue.has_value());
        if(estimateSnapshot){
            if(estimateSnapshot->isoEstimate)summary["isoEstimate"]=GetIsoEstimate(*estimateSnapshot->isoEstimate);
            else if(estimateSnapshot->resolvedParams.initialIsoValue)summary["isoEstimate"]=QJsonObject{{"iso",*estimateSnapshot->resolvedParams.initialIsoValue}};
        }else summary.remove("isoEstimate");
        const auto display = panel->GetSession()->GetRenderViewState({"primary-3d"});
        summary["canApplyIsoToDisplay"] = display && (display->viewMode == HostRenderMode::IsoSurface || display->viewMode == HostRenderMode::CompositeIsoSurface);
        summary["hasPreview"] = hasPreview;
        summary["hasResult"] = current || hasPreview || summary["hasIso"].toBool();
        summary["hasMesh"] = (current && GetDataRevisionRefValid(snapshot->meshRevision)) || hasPreview;
        summary["isOverlayVisible"] = state.isOverlayVisible;
        const auto displayKey = hasPreview ? QString::number(preview->requestId) : summary["mesh"].toString();
        panel->GetParameterEditor("Visibility")->GetField("isVisible")->SetAppliedBoolean(summary["hasMesh"].toBool() ? QJsonValue(state.isOverlayVisible) : QJsonValue(), displayKey, summary["hasMesh"].toBool() ? QString() : "当前没有可用表面");
        panel->GetParameterEditor("ObjectPicking")->GetField("isEnabled")->SetAppliedBoolean(state.isObjectPickingEnabled,GetRefText(state.selectionGeneration));
        summary["hasMeasurement"] = measured;
        summary["isFormalCurrent"]=current;
        try {
            const auto draft=panel->GetActionParameters("GlobalAutomatic");
            QJsonObject formal=*requestedParameters;
            if(snapshot) {
                const auto& p=snapshot->requestedParams;
                formal["componentSelection"]=p.componentSelection==SurfaceComponentSelection::Largest?"Largest":p.componentSelection==SurfaceComponentSelection::Seeded?"Seeded":"All";
                formal["initialIsoValue"]=p.initialIsoValue?QJsonValue(*p.initialIsoValue):QJsonValue();
                formal["materialRange"]=p.materialRange?QJsonValue(GetValues(*p.materialRange)):QJsonValue();
                if(p.componentSelection==SurfaceComponentSelection::Seeded)formal["seedModelPoint"]=p.seedModelPoint?QJsonValue(GetValues(*p.seedModelPoint)):QJsonValue();
                else formal.remove("seedModelPoint");
                formal["minimumObjectVoxels"]=QString::number(p.minimumObjectVoxels);
                formal["analysisRoi"]=p.analysisRoi?GetRefText(*p.analysisRoi):QString();
                summary["formalMesh"]=GetRefText(snapshot->meshRevision);summary["formalParameters"]=formal;
                summary["formalParameterFingerprint"]=QString::number(snapshot->parameterFingerprint);
            }
            summary["parametersChanged"]=current && draft!=formal;
        }
        catch(const std::exception&){summary["parametersChanged"]=true;}
        summary["isBusy"] = state.stage == SurfaceDeterminationStage::Preparing || state.stage == SurfaceDeterminationStage::ThresholdEstimation
            || state.stage == SurfaceDeterminationStage::SeedExtraction || state.stage == SurfaceDeterminationStage::SubvoxelRefinement
            || state.stage == SurfaceDeterminationStage::TopologyValidation || state.stage == SurfaceDeterminationStage::Committing || state.stage == SurfaceDeterminationStage::Stopping;
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Stop", {{"targetRequestId", "0"}}); };
    const QPointer<ModulePanel> grayOwner(panel);
    grayOwner->onGrayStatistics=[grayOwner,getEstimateSnapshot] {
        if(!grayOwner || grayOwner->GetContext().workflow.GetIsClosing())return false;
        const auto snapshot=getEstimateSnapshot();const auto estimate=snapshot?snapshot->isoEstimate:std::optional<SurfaceIsoEstimate>{};const auto source=snapshot?snapshot->sourceRevision:DataRevisionRef{};
        const auto input=grayOwner->GetSession()->GetImageDescriptor();
        if(!estimate || estimate->sampleCount==0 || !input || source!=input->dataRevision)return false;
        QJsonArray counts,edges;for(auto value:estimate->histogramCounts)counts.append(static_cast<double>(value));for(auto value:estimate->histogramEdges)edges.append(value);
        const auto estimateRoi=snapshot->requestedParams.analysisRoi?GetRefText(*snapshot->requestedParams.analysisRoi):QString();
        const auto description="来源 "+GetRefText(source)+" · 分析范围 "+(estimateRoi.isEmpty()?QString("全部有效域"):estimateRoi)+QString(" · 最近预览/测定的原始估计 · 512 桶最近中心分桶 · 采样步长 %1/%2/%3 · 有效 %4 · 排除 %5")
            .arg(estimate->histogramSampleSteps[0]).arg(estimate->histogramSampleSteps[1]).arg(estimate->histogramSampleSteps[2]).arg(estimate->sampleCount).arg(estimate->excludedSampleCount);
        const auto cacheMaterial=QJsonDocument(QJsonObject{{"source",GetRefText(source)},{"roi",estimateRoi},{"sampleCount",QString::number(estimate->sampleCount)},
            {"excluded",QString::number(estimate->excludedSampleCount)},{"steps",QJsonArray{QString::number(estimate->histogramSampleSteps[0]),QString::number(estimate->histogramSampleSteps[1]),QString::number(estimate->histogramSampleSteps[2])}},
            {"background",estimate->backgroundValue},{"material",estimate->materialValue},{"iso",estimate->isoValue},{"counts",counts},{"edges",edges}}).toJson(QJsonDocument::Compact);
        const auto cacheKey=QString::fromLatin1(QCryptographicHash::hash(cacheMaterial,QCryptographicHash::Sha256).toHex());
        const auto histogramKey=GetRefText(source)+":"+cacheKey;
        if(grayOwner->GetGrayStatisticsWidget()->GetHistogram()["cacheKey"]!=histogramKey)grayOwner->GetGrayStatisticsWidget()->SetHistogram({{"cacheKey",histogramKey},{"title","表面初始灰度估计分布"},{"description",description},{"counts",counts},{"edges",edges},
            {"xUnit","原始灰度"},{"yUnit","算法估计样本次数"},{"markers",QJsonArray{
                QJsonObject{{"label","BG"},{"value",estimate->backgroundValue}},QJsonObject{{"label","M"},{"value",estimate->materialValue}},QJsonObject{{"label","ISO"},{"value",estimate->isoValue}}}}});
        const auto parameters=grayOwner->GetActionParameters("GlobalAutomatic");
        const auto* sourceChoice=grayOwner->findChild<QComboBox*>("surfaceParameterSource");
        const bool manual=sourceChoice && sourceChoice->currentData().toBool();
        auto* statistics=grayOwner->GetGrayStatisticsWidget();statistics->SetInteractionModes({"编辑表面初值"});statistics->SelectInteractionMode("编辑表面初值");
        statistics->SetInteractionContext(GetRefText(source)+":"+QString::number(input->bindingRevision)+":"+grayOwner->GetParameterAction()+":"+parameters["analysisRoi"].toString()+":"+QString::number(manual)+":"+QString::number(snapshot->requestId));
        const auto iso=parameters["initialIsoValue"].isDouble()?parameters["initialIsoValue"].toDouble():estimate->isoValue;
        statistics->SetMarkers(QJsonArray{QJsonObject{{"label","BG"},{"value",estimate->backgroundValue}},QJsonObject{{"label","M"},{"value",estimate->materialValue}},
            QJsonObject{{"label",manual?"手动初值（待预览）":"自动初值"},{"key","initialIsoValue"},{"editable",true},{"value",iso}}});
        const auto sourceAtPresentation=source;const auto bindingAtPresentation=input->bindingRevision;const auto roiAtPresentation=parameters["analysisRoi"];const auto requestAtPresentation=snapshot->requestId;const QPointer<ModulePanel> owner(grayOwner);
        statistics->onMarker=[owner,sourceAtPresentation,bindingAtPresentation,roiAtPresentation,requestAtPresentation,getEstimateSnapshot](const QString& key,double value){
            try {
            if(key!="initialIsoValue" || !owner || owner->GetContext().workflow.GetIsClosing())return;
            const auto current=owner->GetSession()->GetImageDescriptor();if(!current || current->dataRevision!=sourceAtPresentation || current->bindingRevision!=bindingAtPresentation || owner->GetActionParameters("GlobalAutomatic")["analysisRoi"]!=roiAtPresentation)return;
            const auto presented=getEstimateSnapshot();if(!presented||presented->requestId!=requestAtPresentation)return;
            auto* sourceChoice=owner->findChild<QComboBox*>("surfaceParameterSource");
            if(sourceChoice && !sourceChoice->currentData().toBool()){const auto index=sourceChoice->findData(true);if(index>=0)sourceChoice->setCurrentIndex(index);}
            if(!owner || owner->GetContext().workflow.GetIsClosing())return;
            owner->SetParameterPatch("GlobalAutomatic",{{"initialIsoValue",value}});
            if(owner && owner->onMessage)owner->onMessage("已修改手动初值；BG/M 未改，尚未生成正式表面。");
            }catch(const std::exception& error){if(owner)owner->SetFeedback(QString::fromUtf8(error.what()),"error");}
        };
        statistics->onInterval={};
        return true;
    };
    panel->onSessionClosing=[listedSurface,measurement] {listedSurface->reset();measurement->selection.reset();};
    return panel;
}
}
