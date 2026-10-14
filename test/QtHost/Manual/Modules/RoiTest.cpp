// 测试用途：通过既有公共入口显示 ROI 草稿、提交结果、取消及显隐。
#include "ModuleFactories.h"
#include "Host/RoiEditingHostFeature.h"
#include "Support/RoiSelection.h"
#include "Support/ParameterEditor.h"
#include <vtkMatrix4x4.h>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
namespace Manual {
namespace {
std::array<double,16> GetRoiBox(const QJsonObject& p,const ImageDescriptor& source,const std::optional<std::array<double,16>>& existing={})
{
    if(p.contains("boxToSource") && !p["boxToSource"].isNull())return GetAffineMatrix(p["boxToSource"]);
    std::array<double,3> center{},size{};auto direction=source.direction;
    if(existing)for(int row=0;row<3;++row)center[row]=(*existing)[row*4+3];
    else {center=source.origin;for(int row=0;row<3;++row)for(int axis=0;axis<3;++axis)center[row]+=direction[row*3+axis]*source.spacing[axis]*(source.extent[2*axis]+source.extent[2*axis+1])*0.5;}
    for(int axis=0;axis<3;++axis){if(existing){double length=0;for(int row=0;row<3;++row)length+=(*existing)[row*4+axis]*(*existing)[row*4+axis];size[axis]=2*std::sqrt(length);for(int row=0;row<3;++row)direction[row*3+axis]=(*existing)[row*4+axis]/(size[axis]*0.5);}else size[axis]=(source.extent[2*axis+1]-source.extent[2*axis]+1)*source.spacing[axis]*0.5;}
    if(p.contains("centerMM") && !p["centerMM"].isNull())center=GetArray<double,3>(p["centerMM"]);
    if(p.contains("sizeMM") && !p["sizeMM"].isNull())size=GetArray<double,3>(p["sizeMM"]);
    if(p.contains("direction3D") && !p["direction3D"].isNull())direction=GetDirectionMatrix(p["direction3D"]);
    std::array<double,16> matrix=roiIdentityMatrix;
    for(int axis=0;axis<3;++axis)if(!(size[axis]>0) || !std::isfinite(size[axis]))throw std::invalid_argument("区域尺寸必须为有限正数");
    for(int row=0;row<3;++row){matrix[row*4+3]=center[row];for(int axis=0;axis<3;++axis)matrix[row*4+axis]=direction[row*3+axis]*size[axis]*0.5;}
    return GetAffineMatrix(GetValues(matrix));
}
QJsonObject RoiGeometryParameters(const RoiPrimitive& primitive)
{
    const auto shape=[&]() -> QString {switch(primitive.shape){case RoiShape::Sphere:return "Sphere";case RoiShape::Cylinder:return "Cylinder";case RoiShape::Ellipsoid:return "Ellipsoid";case RoiShape::EllipticCylinder:return "EllipticCylinder";case RoiShape::HalfSpace:return "HalfSpace";case RoiShape::Contour:return "Contour";case RoiShape::ProjectedContour:return "ProjectedContour";case RoiShape::Brush:return "Brush";case RoiShape::MaskReference:return "MaskReference";default:return "Box";}}();
    if(primitive.shape!=RoiShape::Box && primitive.shape!=RoiShape::Ellipsoid) {
        QJsonArray rings,path,labels;for(const auto& ring:primitive.contours){QJsonArray points;for(const auto& point:ring)points.append(GetValues(point));rings.append(points);}for(const auto& point:primitive.path)path.append(GetValues(point));for(auto label:primitive.labels)labels.append(QString::number(label));
        return {{"shape",shape},{"centerMM",GetValues(primitive.origin)},{"axis",GetValues(primitive.normal)},
        {"radius",primitive.radius},{"height",primitive.height},{"boxToSource",QJsonValue()},
        {"planeToSource",GetValues(primitive.localToSource)},{"sourceToView",GetValues(primitive.sourceToView)},{"viewToClip",GetValues(primitive.viewToClip)},
        {"depthMM",GetValues(primitive.depth)},{"contours",rings},{"pathMM",path},{"labels",labels},{"mask",primitive.mask?GetRefText(*primitive.mask):QString()},{"isSliceBrush",primitive.height>0}};
    }
    const auto& matrix=primitive.localToSource;
    std::array<double,3> center{},size{};std::array<double,9> direction{};
    for(int row=0;row<3;++row)center[row]=matrix[row*4+3];
    for(int axis=0;axis<3;++axis){double length=0;for(int row=0;row<3;++row)length+=matrix[row*4+axis]*matrix[row*4+axis];size[axis]=2*std::sqrt(length);for(int row=0;row<3;++row)direction[row*3+axis]=matrix[row*4+axis]/(size[axis]*0.5);}
    return {{"shape",shape},{"centerMM",GetValues(center)},{"sizeMM",GetValues(size)},{"direction3D",GetValues(direction)},{"boxToSource",QJsonValue()}};
}
RoiPrimitive GetRoiPrimitive(const QJsonObject& p,const ImageDescriptor& source,RoiPrimitive primitive)
{
    const auto shape=p.contains("shape") ? GetEnum<RoiShape>(p,"shape",{{"Box",RoiShape::Box},{"Sphere",RoiShape::Sphere},{"Cylinder",RoiShape::Cylinder},{"Ellipsoid",RoiShape::Ellipsoid},{"EllipticCylinder",RoiShape::EllipticCylinder},{"HalfSpace",RoiShape::HalfSpace},{"Contour",RoiShape::Contour},{"ProjectedContour",RoiShape::ProjectedContour},{"Brush",RoiShape::Brush},{"MaskReference",RoiShape::MaskReference}}):primitive.shape;
    if (shape!=primitive.shape) {primitive={};primitive.shape=shape;}
    if(primitive.shape==RoiShape::Box || primitive.shape==RoiShape::Ellipsoid)primitive.localToSource=GetRoiBox(p,source,primitive.localToSource);
    else if (shape==RoiShape::Sphere || shape==RoiShape::Cylinder || shape==RoiShape::HalfSpace) {
        if(p.contains("centerMM") && !p["centerMM"].isNull())primitive.origin=GetArray<double,3>(p["centerMM"]);
        if (shape!=RoiShape::HalfSpace) primitive.radius=GetNumber(p,"radius");
        else primitive.normal=GetArray<double,3>(p["axis"]);
        if(primitive.shape==RoiShape::Cylinder){primitive.normal=GetArray<double,3>(p["axis"]);primitive.height=GetNumber(p,"height");}
    } else if (shape==RoiShape::Contour || shape==RoiShape::ProjectedContour) {
        primitive.contours.clear();for (const auto& ring:p["contours"].toArray()) {std::vector<std::array<double,2>> points;for (const auto& point:ring.toArray()) points.push_back(GetArray<double,2>(point));primitive.contours.push_back(std::move(points));}
        primitive.depth=GetArray<double,2>(p["depthMM"]);
        if (shape==RoiShape::Contour) primitive.localToSource=GetAffineMatrix(p["planeToSource"]);
        else {primitive.sourceToView=GetAffineMatrix(p["sourceToView"]);primitive.viewToClip=GetArray<double,16>(p["viewToClip"]);}
    } else if(shape==RoiShape::EllipticCylinder){primitive.localToSource=GetAffineMatrix(p["planeToSource"]);primitive.depth=GetArray<double,2>(p["depthMM"]);
    } else if (shape==RoiShape::Brush) {
        primitive.radius=GetNumber(p,"radius");primitive.path.clear();for (const auto& point:p["pathMM"].toArray()) primitive.path.push_back(GetArray<double,3>(point));
        primitive.height=GetBool(p,"isSliceBrush") ? 1:0;primitive.depth=GetArray<double,2>(p["depthMM"]);
        primitive.localToSource=GetAffineMatrix(p["planeToSource"]);
    } else if (shape==RoiShape::MaskReference) {
        primitive.mask=GetRef(p["mask"]);primitive.labels.clear();for (const auto& value:p["labels"].toArray()) primitive.labels.push_back(GetId(value));
    }
    return primitive;
}
bool SameRoiPrimitive(const RoiPrimitive& a,const RoiPrimitive& b)
{
    return a.shape==b.shape && a.localToSource==b.localToSource && a.origin==b.origin && a.normal==b.normal
        && a.boundaryPolicy==b.boundaryPolicy && a.radius==b.radius && a.height==b.height
        && a.contours==b.contours && a.path==b.path && a.depth==b.depth && a.labels==b.labels
        && a.mask==b.mask && a.voxels==b.voxels && a.sourceToView==b.sourceToView && a.viewToClip==b.viewToClip;
}
QJsonObject RoiMetadataParameters(const QJsonObject& p){return {{"name",GetText(p,"name")},{"group",GetText(p,"group")},{"description",GetText(p,"description")}};}
QJsonObject RoiToolParameters(QJsonObject parameters)
{
    for(const auto* key:{"name","group","description","outputMode"})parameters.remove(key);
    return parameters;
}
void SetRoiFields(ParameterEditor& form)
{
    const auto* field=form.GetField("shape");const auto shape=field?field->GetValue().toString():QString("Box");
    const bool affine=shape=="Box" || shape=="Ellipsoid";
    form.SetFieldApplicability({{"boxToSource",affine},{"sizeMM",affine},{"direction3D",affine},
        {"radius",shape=="Sphere" || shape=="Cylinder" || shape=="Brush"},
        {"axis",shape=="Cylinder" || shape=="HalfSpace"},{"height",shape=="Cylinder"},
        {"contours",shape=="Contour" || shape=="ProjectedContour"},{"planeToSource",shape=="Contour" || shape=="Brush" || shape=="EllipticCylinder"},
        {"depthMM",shape=="EllipticCylinder" || shape=="Contour" || shape=="ProjectedContour" || shape=="Brush"},
        {"sourceToView",shape=="ProjectedContour"},{"viewToClip",shape=="ProjectedContour"},
        {"pathMM",shape=="Brush"},{"isSliceBrush",shape=="Brush"},{"mask",shape=="MaskReference"},{"labels",shape=="MaskReference"}});
}
RoiMetadata GetRoiMetadata(const QJsonObject& p){return {GetText(p,"name").toStdString(),GetText(p,"group").toStdString(),GetText(p,"description").toStdString(),false};}

RoiOperationRequest GetOperationRequest(const QString& action,const QJsonObject& p)
{
    RoiOperationRequest request;request.output=GetEnum<RoiOutputMode>(p,"outputMode",{{"UpdateTarget",RoiOutputMode::UpdateTarget},{"CreateNew",RoiOutputMode::CreateNew}});
    const auto connectivity=[&]{return GetEnum<RoiConnectivity>(p,"connectivity",{{"Faces",RoiConnectivity::Faces},{"Full",RoiConnectivity::Full}});};
    if(QStringList{"Union","Intersection","Difference","Merge"}.contains(action)) {
        RoiBooleanParams params;params.operation=action=="Intersection"?RoiBooleanOperation::Intersection:action=="Difference"?RoiBooleanOperation::Difference:RoiBooleanOperation::Union;
        for(const auto& value:p["operands"].toArray())params.operands.push_back(GetRef(value));request.params=std::move(params);
        if(action=="Merge")request.output=RoiOutputMode::CreateNew;
    } else if(QStringList{"Dilate","Erode","Open","Close"}.contains(action)) {
        RoiMorphologyParams params;params.operation=action=="Erode"?RoiMorphologyOperation::Erode:action=="Open"?RoiMorphologyOperation::Open:action=="Close"?RoiMorphologyOperation::Close:RoiMorphologyOperation::Dilate;
        params.radius=GetNumber(p,"radius");params.unit=GetEnum<RoiRadiusUnit>(p,"radiusUnit",{{"Millimetres",RoiRadiusUnit::Millimetres},{"Voxels",RoiRadiusUnit::Voxels}});request.params=params;
    } else if(QStringList{"RemoveSmall","KeepLargest","KeepSelected","Split","FillHoles"}.contains(action)) {
        RoiComponentParams params;params.operation=action=="KeepLargest"?RoiComponentOperation::KeepLargest:action=="KeepSelected"?RoiComponentOperation::KeepSelected:action=="Split"?RoiComponentOperation::Split:action=="FillHoles"?RoiComponentOperation::FillHoles:RoiComponentOperation::RemoveSmall;
        params.connectivity=connectivity();params.minimumVolume=GetNumber(p,"minimumVolumeMM3");params.maximumHoleVolume=GetNumber(p,"maximumHoleVolumeMM3");params.keepInput=GetBool(p,"keepInput");
        for(const auto& value:p["componentLabels"].toArray()){const auto label=GetId(value);if(label>UINT32_MAX)throw std::invalid_argument("分量标签超出表示范围");params.labels.push_back(static_cast<std::uint32_t>(label));}request.params=std::move(params);
    } else if(action=="Threshold" || action=="Grow") {
        RoiThresholdParams params;params.mode=action=="Grow"?RoiThresholdMode::SeedGrow:RoiThresholdMode::Threshold;params.range=GetArray<double,2>(p["grayRange"]);params.connectivity=connectivity();if(action=="Grow")for(const auto& point:p["seedsMM"].toArray())params.seeds.push_back(GetArray<double,3>(point));request.params=std::move(params);
    } else if(action=="Invert") {RoiInvertParams params;if(!p["domainRoi"].toString().isEmpty())params.domain=GetRef(p["domainRoi"]);request.params=params;}
    else if(action=="Clear")request.params=RoiClearParams{};
    else if(action=="Transform") {RoiTransformParams params;params.sourceToTarget=GetAffineMatrix(p["sourceToTarget"]);params.rasterize=GetBool(p,"rasterize");if(!p["targetSource"].toString().isEmpty())params.targetSource=GetRef(p["targetSource"]);request.params=params;}
    else {RoiMeshParams params;params.mesh=GetRef(p["mesh"]);params.meshToSource=GetAffineMatrix(p["meshToSource"]);params.mode=action=="SurfaceInterior"?RoiMeshMode::Interior:RoiMeshMode::Layer;params.inward=GetNumber(p,"inwardMM");params.outward=GetNumber(p,"outwardMM");params.hasOrientation=GetBool(p,"hasOrientation");request.params=params;}
    return request;
}

}
ModulePanel* CreateRoiTest(TestContext context,std::shared_ptr<RoiEditingHostFeature> feature,QWidget* parent)
{
    auto* panel=new ModulePanel(context,"Roi",parent);
    panel->SetNotice("区域只保存范围，不改变当前输入。创建工具与添加、扣除、替换独立；当前预览、统计和保存共用同一范围。复杂区域按源网格近似显示，显示失败不阻止数据保存。");
    const QJsonObject parameters{{"name","区域"},{"group",""},{"description",""},{"centerMM",QJsonValue()},
        {"sizeMM",QJsonValue()},{"direction3D",QJsonValue()},{"boxToSource",QJsonValue()},
        {"axis",QJsonArray{0,0,1}},{"radius",1.0},{"height",2.0},{"shape","Box"},{"selectionMode","Replace"},{"outputMode","CreateNew"},
        {"planeToSource",GetValues(roiIdentityMatrix)},{"sourceToView",GetValues(roiIdentityMatrix)},{"viewToClip",GetValues(roiIdentityMatrix)},
        {"depthMM",QJsonArray{-0.5,0.5}},{"contours",QJsonArray{QJsonArray{QJsonArray{-1,-1},QJsonArray{1,-1},QJsonArray{1,1},QJsonArray{-1,1}}}},
        {"pathMM",QJsonArray{QJsonArray{0,0,0}}},{"isSliceBrush",false},{"mask",""},{"labels",QJsonArray{}}};
    auto appliedMetadata=std::make_shared<QJsonObject>();auto observedPrimitive=std::make_shared<std::optional<RoiPrimitive>>();
    auto appliedToolParameters=std::make_shared<QJsonObject>();
    auto pendingSave=std::make_shared<std::optional<std::uint64_t>>();auto pendingOperation=std::make_shared<std::optional<std::uint64_t>>();
    auto operationAction=std::make_shared<QString>();auto operationParametersSeen=std::make_shared<QJsonObject>();
    const auto remember=[panel,feature,appliedMetadata,observedPrimitive,appliedToolParameters](const RoiPrimitive& primitive,const QJsonObject& metadata) {
        *appliedMetadata=metadata;*observedPrimitive=primitive;
        auto patch=RoiGeometryParameters(primitive);for(auto it=metadata.begin();it!=metadata.end();++it)patch[it.key()]=it.value();
        const auto state=feature->GetState();patch["outputMode"]=state.output==RoiOutputMode::CreateNew?"CreateNew":"UpdateTarget";
        patch["selectionMode"]=state.selection==RoiSelectionMode::Add?"Add":state.selection==RoiSelectionMode::Subtract?"Subtract":state.selection==RoiSelectionMode::Intersect?"Intersect":"Replace";
        panel->SetParameterPatch("SetDraft",patch);
        // 以控件实际呈现的值记录对应关系；矩阵分解往返不代表用户编辑了几何。
        *appliedToolParameters=RoiToolParameters(panel->GetActionParameters("SetDraft"));
    };
    const auto finishSave=[panel](std::uint64_t id,const QString& status,RoiError error,std::optional<DataRevisionRef> saved) {
        panel->SetComplete(id,status,{{"error",int(error)},{"roi",saved?GetRefText(*saved):QString()},
            {"message",saved?"区域已保存，可用于分析":""}});
        auto& workflow=panel->GetContext().workflow;
        if(status=="Succeeded" && saved && !workflow.roiConsumer.isEmpty()){
            const auto consumer=workflow.roiConsumer;workflow.roiConsumer={};
            if(workflow.onNavigate)workflow.onNavigate(consumer["module"].toString(),consumer["action"].toString(),
                {{consumer["key"].toString(),GetRefText(*saved)}});
        }
    };
    auto beginParameters=parameters;beginParameters["shape"]="Box";
    panel->AttachAction("Begin",beginParameters,[panel,feature,remember](auto id,const auto& p) {
        const auto source=panel->GetSession()->GetImageDescriptor();if(!source)throw std::invalid_argument("没有当前体数据");
        RoiRequest draft;draft.definition.source=source->dataRevision;draft.metadata=GetRoiMetadata(p);
        RoiNode node;auto values=p;if(!values.contains("centerMM") || values["centerMM"].isNull()){const auto matrix=GetRoiBox({},*source);values["centerMM"]=QJsonArray{matrix[3],matrix[7],matrix[11]};}node.primitive=GetRoiPrimitive(values,*source,node.primitive);
        draft.definition.nodes.push_back(node);
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);const auto result=feature->SendRequest(request);
        if(result.error==RoiError::None)remember(node.primitive,RoiMetadataParameters(p));
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});
    },TestPolicy::Interaction,true);
    panel->AttachAction("SetDraft",parameters,[panel,feature,remember](auto id,const auto& p) {
        const auto state=feature->GetState();const auto source=panel->GetSession()->GetImageDescriptor();
        if(!source || !state.draft || state.draft->source!=source->dataRevision)throw std::invalid_argument("请先建立当前输入的区域草稿");
        RoiEditingRequest request;request.action=RoiEditingAction::SetDraft;request.primitive=GetRoiPrimitive(p,*source,state.tool?*state.tool:state.draft->nodes.front().primitive);
        request.selection=GetEnum<RoiSelectionMode>(p,"selectionMode",{{"Replace",RoiSelectionMode::Replace},{"Add",RoiSelectionMode::Add},{"Subtract",RoiSelectionMode::Subtract},{"Intersect",RoiSelectionMode::Intersect}});
        request.output=GetEnum<RoiOutputMode>(p,"outputMode",{{"CreateNew",RoiOutputMode::CreateNew},{"UpdateTarget",RoiOutputMode::UpdateTarget}});
        RoiRequest metadata;metadata.definition.source=state.draft->source;metadata.metadata=GetRoiMetadata(p);request.draft=std::move(metadata);
        const auto result=feature->SendRequest(request);if(result.error==RoiError::None)remember(*request.primitive,RoiMetadataParameters(p));
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});
    },TestPolicy::Interaction,true);
    for (const auto& action:std::vector<std::pair<QString,RoiEditingAction>>{
        {"Commit",RoiEditingAction::Commit},{"Cancel",RoiEditingAction::Cancel},{"Visibility",RoiEditingAction::SetVisible}}) {
        panel->AttachAction(action.first,action.second==RoiEditingAction::SetVisible?QJsonObject{{"isVisible",true},{"roi",""}}:QJsonObject{},
            [panel,feature,action,pendingSave,pendingOperation,observedPrimitive,appliedToolParameters,finishSave,operationAction,operationParametersSeen](auto id,const auto& p) {
                RoiEditingRequest request;request.action=action.second;
                if(request.action==RoiEditingAction::SetVisible){request.isVisible=GetBool(p,"isVisible");if(!p["roi"].toString().isEmpty())request.roi=GetRef(p["roi"]);}
                auto state=feature->GetState();
                if(request.action==RoiEditingAction::Commit && state.draft){
                    // GetActionParameters 直接解析当前控件，包含尚未失去焦点的文本。
                    const auto current=panel->GetActionParameters("SetDraft");
                    RoiRequest draft;draft.definition.source=state.draft->source;draft.metadata=GetRoiMetadata(current);request.draft=std::move(draft);
                    request.output=GetEnum<RoiOutputMode>(current,"outputMode",{{"CreateNew",RoiOutputMode::CreateNew},{"UpdateTarget",RoiOutputMode::UpdateTarget}});
                    const auto selection=GetEnum<RoiSelectionMode>(current,"selectionMode",{{"Replace",RoiSelectionMode::Replace},{"Add",RoiSelectionMode::Add},{"Subtract",RoiSelectionMode::Subtract},{"Intersect",RoiSelectionMode::Intersect}});
                    const auto source=panel->GetSession()->GetImageDescriptor();if(!source)throw std::invalid_argument("源数据不可用");
                    if((state.tool || *observedPrimitive) && RoiToolParameters(current)!=*appliedToolParameters){
                        request.primitive=GetRoiPrimitive(current,*source,state.tool?*state.tool:**observedPrimitive);request.selection=selection;}
                    if(!request.primitive && !operationAction->isEmpty() && state.operation.error!=RoiError::Unavailable){
                        const auto parameters=panel->GetActionParameters(*operationAction);
                        const auto operation=GetOperationRequest(*operationAction,parameters);
                        if(parameters!=*operationParametersSeen){RoiEditingRequest update;update.action=RoiEditingAction::SetOperation;update.operation=operation;
                            const auto changed=feature->SendRequest(update);if(changed.error!=RoiError::Pending)throw std::invalid_argument("运算参数无法更新");*operationParametersSeen=parameters;state=feature->GetState();}
                        request.output=state.output;
                    }
                }
                const auto result=feature->SendRequest(request);
                if(request.action==RoiEditingAction::Cancel){if(*pendingSave){const auto old=**pendingSave;pendingSave->reset();panel->SetComplete(old,"Cancelled");}if(*pendingOperation){const auto old=**pendingOperation;pendingOperation->reset();panel->SetComplete(old,"Cancelled");}}
                if(result.error==RoiError::Pending){*pendingSave=id;panel->SetAdmission(id,true);return;}
                finishSave(id,result.error==RoiError::None?"Succeeded":"Failed",result.error,
                    result.roi?std::optional<DataRevisionRef>{result.roi->revision}:std::nullopt);
                if(request.action==RoiEditingAction::Cancel)panel->GetContext().workflow.roiConsumer={};
            },action.second==RoiEditingAction::Cancel?TestPolicy::Stop:TestPolicy::View);
    }
    panel->AttachAction("Edit",{{"roi",""}},[panel,feature,remember](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();
        if(!input)throw std::invalid_argument("请先选择源数据");
        const auto ref=GetSelectedRoi(*panel->GetSession(),p["roi"],input->dataRevision);
        if(!ref)throw std::invalid_argument("请选择已有区域");
        const auto roi=panel->GetSession()->GetRoiDescriptor(*ref);
        RoiRequest draft;draft.action=RoiAction::SetGeometry;draft.definition=roi->definition;
        draft.metadata=roi->metadata;draft.expectedRoi=roi->revision;
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);
        const auto result=feature->SendRequest(request);
        if(result.error==RoiError::None)remember(roi->definition.nodes.front().primitive,
            {{"name",QString::fromStdString(roi->metadata.name)},{"group",QString::fromStdString(roi->metadata.group)},{"description",QString::fromStdString(roi->metadata.description)}});
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});
    },TestPolicy::Interaction,true);
    panel->AttachAction("SetMetadata",{{"roi",""},{"name","区域"},{"group",""},{"description",""},{"isArchived",false}},[panel](auto id,const auto& p) {
        const auto roi=panel->GetSession()->GetRoiDescriptor(GetRef(p["roi"]));
        if(!roi)throw std::invalid_argument("所选区域不可用");
        RoiRequest request;request.action=RoiAction::SetMetadata;request.expectedRoi=roi->revision;
        request.expectedCatalogRevision=roi->catalogRevision;
        request.metadata={GetText(p,"name").toStdString(),GetText(p,"group").toStdString(),GetText(p,"description").toStdString(),GetBool(p,"isArchived")};
        const auto result=panel->GetSession()->SetRoi(request);
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});
    });
    panel->AttachAction("Copy",{{"roi",""},{"name","区域副本"}},[panel](auto id,const auto& p) {
        const auto roi=panel->GetSession()->GetRoiDescriptor(GetRef(p["roi"]));
        if(!roi)throw std::invalid_argument("所选区域不可用");
        RoiRequest request;request.action=RoiAction::Copy;request.copyFrom=roi->revision;
        request.expectedCatalogRevision=roi->catalogRevision;request.metadata=roi->metadata;
        request.metadata.name=GetText(p,"name").toStdString();request.metadata.isArchived=false;
        const auto result=panel->GetSession()->SetRoi(request);
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)},
            {"roi",result.roi?GetRefText(result.roi->revision):QString()}});
    });
    const QJsonObject operationParameters{{"roi",""},{"outputMode","UpdateTarget"},{"operands",QJsonArray{}},
        {"radius",1.0},{"radiusUnit","Millimetres"},{"connectivity","Faces"},{"minimumVolumeMM3",0.0},{"maximumHoleVolumeMM3",0.0},
        {"componentLabels",QJsonArray{}},{"keepInput",true},{"grayRange",QJsonArray{0,1}},{"seedsMM",QJsonArray{}},
        {"domainRoi",""},{"rasterize",false},{"sourceToTarget",GetValues(roiIdentityMatrix)},{"targetSource",""},{"mesh",""},{"meshToSource",GetValues(roiIdentityMatrix)},
        {"inwardMM",0.0},{"outwardMM",1.0},{"hasOrientation",false}};
    const QStringList operationActions{"Union","Intersection","Difference","Merge","Dilate","Erode","Open","Close","RemoveSmall","KeepLargest","KeepSelected","Split","FillHoles","Threshold","Grow","Invert","Clear","Transform","SurfaceInterior","SurfaceLayer"};
    for(const auto& action:operationActions)panel->AttachAction(action,operationParameters,[panel,feature,pendingOperation,operationAction,operationParametersSeen,action](auto id,const auto& p) {
        if(!feature->GetState().hasDraft) {
            const auto input=panel->GetSession()->GetImageDescriptor();if(!input)throw std::invalid_argument("请先选择源数据");
            RoiRequest draft;draft.definition.source=input->dataRevision;draft.metadata.name="区域";
            const auto ref=GetSelectedRoi(*panel->GetSession(),p["roi"],input->dataRevision);
            if(ref){const auto selected=panel->GetSession()->GetRoiDescriptor(*ref);draft.action=RoiAction::SetGeometry;draft.expectedRoi=*ref;draft.definition=selected->definition;draft.metadata=selected->metadata;}
            else if(QStringList{"Threshold","Grow","SurfaceInterior","SurfaceLayer","Intersection"}.contains(action))draft.definition.nodes={{RoiNodeKind::SourceDomain}};
            else if(action=="Union" || action=="Merge")draft.definition.nodes={{RoiNodeKind::Empty}};
            else throw std::invalid_argument("请先选择正式区域或开始编辑草稿");
            RoiEditingRequest begin;begin.action=RoiEditingAction::Begin;begin.draft=draft;const auto ready=feature->SendRequest(begin);
            if(ready.error!=RoiError::None){panel->SetComplete(id,"Failed",{{"error",int(ready.error)}});return;}
        }
        RoiEditingRequest request;request.action=RoiEditingAction::BeginOperation;request.operation=GetOperationRequest(action,p);
        const auto result=feature->SendRequest(request);if(*pendingOperation){const auto old=**pendingOperation;pendingOperation->reset();panel->SetComplete(old,"Cancelled");}
        if(result.error==RoiError::Pending){panel->SetParameterPatch("SetDraft",{{"outputMode",feature->GetState().output==RoiOutputMode::CreateNew?"CreateNew":"UpdateTarget"}});*operationParametersSeen=p;*operationAction=action;*pendingOperation=id;panel->SetAdmission(id,true);}else panel->SetComplete(id,"Failed",{{"error",int(result.error)}});
    },TestPolicy::Interaction);
    for(const auto& entry:std::vector<std::pair<QString,RoiEditingAction>>{{"ApplySelection",RoiEditingAction::ApplySelection},{"ApplyOperation",RoiEditingAction::ApplyOperation},{"CancelOperation",RoiEditingAction::CancelOperation},{"Undo",RoiEditingAction::Undo},{"Redo",RoiEditingAction::Redo}})
        panel->AttachAction(entry.first,{},[panel,feature,entry,pendingOperation,pendingSave,appliedToolParameters](auto id,const auto&){RoiEditingRequest request;request.action=entry.second;const auto result=feature->SendRequest(request);if(result.error==RoiError::None && feature->GetState().hasDraft){const auto state=feature->GetState();QJsonObject patch{{"outputMode",state.output==RoiOutputMode::CreateNew?"CreateNew":"UpdateTarget"},{"selectionMode",state.selection==RoiSelectionMode::Add?"Add":state.selection==RoiSelectionMode::Subtract?"Subtract":state.selection==RoiSelectionMode::Intersect?"Intersect":"Replace"}};if(state.tool){const auto geometry=RoiGeometryParameters(*state.tool);for(auto it=geometry.begin();it!=geometry.end();++it)patch[it.key()]=it.value();}panel->SetParameterPatch("SetDraft",patch);*appliedToolParameters=RoiToolParameters(panel->GetActionParameters("SetDraft"));}
            if(entry.second==RoiEditingAction::CancelOperation){if(*pendingOperation){const auto old=**pendingOperation;pendingOperation->reset();panel->SetComplete(old,"Cancelled");}if(*pendingSave){const auto old=**pendingSave;pendingSave->reset();panel->SetComplete(old,"Cancelled");}}
            panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});});
    for(const auto& entry:std::vector<std::pair<QString,RoiDrawingMode>>{{"DrawRectangle2D",RoiDrawingMode::SliceRectangle},{"DrawCircle2D",RoiDrawingMode::SliceCircle},{"DrawEllipse2D",RoiDrawingMode::SliceEllipse},{"DrawContour2D",RoiDrawingMode::SliceContour},{"DrawBrush2D",RoiDrawingMode::SliceBrush},{"DrawContour3D",RoiDrawingMode::ProjectedContour},{"DrawBrush3D",RoiDrawingMode::VisibleBrush},{"PickSeed",RoiDrawingMode::Seed},{"EditControlPoint",RoiDrawingMode::EditControlPoint}})
        panel->AttachAction(entry.first,{},[panel,feature,entry](auto id,const auto&){RoiEditingRequest request;request.action=RoiEditingAction::BeginDrawing;request.drawing=entry.second;const auto result=feature->SendRequest(request);panel->SetComplete(id,result.error==RoiError::None?"PreviewReady":"Failed",{{"error",int(result.error)}});});
    panel->AttachAction("EndDrawing",{},[panel,feature](auto id,const auto&){RoiEditingRequest request;request.action=RoiEditingAction::EndDrawing;const auto result=feature->SendRequest(request);panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});});
    panel->AttachAction("Remove",{{"roi",""}},[panel](auto id,const auto& p){RoiRequest request;request.action=RoiAction::Remove;request.expectedRoi=GetRef(p["roi"]);const auto result=panel->GetSession()->SetRoi(request);panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});});
    panel->onParametersEdited=[panel,feature,remember,observedPrimitive,appliedToolParameters,operationActions,operationAction,operationParametersSeen](const QString& action,const QJsonObject& p) {
        const auto state=feature->GetState();if(!state.hasDraft || state.isDragging)return;
        RoiEditingRequest request;const auto currentSource=panel->GetSession()->GetImageDescriptor();
        if(action=="SetDraft"){
            if(!currentSource)return;
            const auto base=state.tool?*state.tool:*observedPrimitive?**observedPrimitive:state.draft->nodes.front().primitive;
            const auto selection=GetEnum<RoiSelectionMode>(p,"selectionMode",{{"Replace",RoiSelectionMode::Replace},{"Add",RoiSelectionMode::Add},{"Subtract",RoiSelectionMode::Subtract},{"Intersect",RoiSelectionMode::Intersect}});
            const auto output=GetEnum<RoiOutputMode>(p,"outputMode",{{"CreateNew",RoiOutputMode::CreateNew},{"UpdateTarget",RoiOutputMode::UpdateTarget}});
            RoiRequest draft;draft.definition.source=state.draft->source;draft.metadata=GetRoiMetadata(p);request.draft=std::move(draft);
            if(RoiToolParameters(p)!=*appliedToolParameters){
                const auto primitive=GetRoiPrimitive(p,*currentSource,base);
                request.action=RoiEditingAction::SetDraft;request.primitive=primitive;request.selection=selection;request.output=output;
                if(feature->SendRequest(request).error==RoiError::None)remember(primitive,RoiMetadataParameters(p));
            }else{request.action=RoiEditingAction::SetMetadata;request.output=output;(void)feature->SendRequest(request);}
        }else if(operationActions.contains(action) && *operationAction==action){
            request.action=RoiEditingAction::SetOperation;request.operation=GetOperationRequest(action,p);const auto result=feature->SendRequest(request);if(result.error==RoiError::Pending){*operationParametersSeen=p;panel->SetParameterPatch("SetDraft",{{"outputMode",feature->GetState().output==RoiOutputMode::CreateNew?"CreateNew":"UpdateTarget"}});}
        }
    };
#if defined(MANUAL_CROP)
    panel->AttachAction("OpenCrop",{{"roi",""}},[panel,feature](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();
        if(!input)throw std::invalid_argument("请先选择源数据");
        auto value=p["roi"];
        if(value.toString().isEmpty() && feature->GetState().committedRoi)value=GetRefText(*feature->GetState().committedRoi);
        const auto roi=GetSelectedRoi(*panel->GetSession(),value,input->dataRevision);
        if(!roi)throw std::invalid_argument("请选择已有区域");
        if(panel->GetContext().workflow.onNavigate)panel->GetContext().workflow.onNavigate("Crop","PrepareRoi",{{"inputRoi",GetRefText(*roi)}});
        panel->SetComplete(id,"ParametersCopied",{{"roi",GetRefText(*roi)}});
    },TestPolicy::Read);
#endif
    panel->onObserve=[panel,feature,appliedMetadata,observedPrimitive,appliedToolParameters,pendingSave,pendingOperation,finishSave] {
        const auto state=feature->GetState();
        if(*pendingOperation && !state.isComputing){const auto id=**pendingOperation;pendingOperation->reset();panel->SetComplete(id,state.error==RoiError::None?"PreviewReady":"Failed",{{"error",int(state.error)},{"outputCount",int(state.operation.rois.size())}});}
        if(*pendingSave && !state.isComputing && !state.isSaving){const auto id=**pendingSave;pendingSave->reset();const bool saved=!state.hasDraft && state.error==RoiError::None;
            finishSave(id,saved?"Succeeded":state.hasDraft && state.error==RoiError::None?"Cancelled":"Failed",state.error,saved?state.committedRoi:std::nullopt);}
        // 控件形状草稿决定字段可用性；候选运算暂时没有原工具时也保留原表单。
        if(auto* form=panel->GetParameterEditor("SetDraft"))SetRoiFields(*form);
        const auto input=panel->GetSession()->GetImageDescriptor();
        const auto roi=state.committedRoi?panel->GetSession()->GetRoiDescriptor(*state.committedRoi):std::optional<RoiDescriptor>{};
        bool dirty=false;
        if(state.draft && input && state.draft->source==input->dataRevision) {
            try {auto p=panel->GetActionParameters("SetDraft");
                if(state.tool && (!*observedPrimitive || !SameRoiPrimitive(**observedPrimitive,*state.tool))
                    && RoiToolParameters(p)==*appliedToolParameters) {
                    panel->SetParameterPatch("SetDraft",RoiGeometryParameters(*state.tool));p=panel->GetActionParameters("SetDraft");
                    *appliedToolParameters=RoiToolParameters(p);*observedPrimitive=*state.tool;
                }
                dirty=RoiMetadataParameters(p)!=*appliedMetadata || RoiToolParameters(p)!=*appliedToolParameters;
            }catch(const std::exception&){dirty=true;}
        }
        QJsonArray regions;for(const auto& item:panel->GetSession()->GetRoiDescriptors(true))regions.append(QJsonObject{
            {"roi",GetRefText(item.revision)},{"name",QString::fromStdString(item.metadata.name)},
            {"group",QString::fromStdString(item.metadata.group)},{"isArchived",item.metadata.isArchived},
            {"description",QString::fromStdString(item.metadata.description)},{"source",GetRefText(item.definition.source)}});
        QJsonArray displayFailures;for(const auto& failure:state.displayFailures)displayFailures.append(QJsonObject{{"roi",GetRefText(failure.first)},{"error",int(failure.second)}});
        panel->SetState({{"displayFailures",displayFailures},{"regions",regions},{"hasDraft",state.hasDraft},{"isVisible",state.isVisible},{"isDragging",state.isDragging},
            {"isComputing",state.isComputing},{"isSaving",state.isSaving},{"undoCount",int(state.undoCount)},{"redoCount",int(state.redoCount)},{"displayError",int(state.displayError)},{"displayStatus",int(state.displayStatus)},{"previewAccuracy","复杂区域为源体素边界近似预览"},
            {"source",state.draft?GetRefText(state.draft->source):roi?GetRefText(roi->definition.source):QString()},
            {"isRoiCurrent",input && roi && roi->definition.source==input->dataRevision},
            {"committedRoi",state.committedRoi?GetRefText(*state.committedRoi):QString()},{"draftParametersChanged",dirty}});
    };
#if defined(MANUAL_GAP)
    panel->AttachAction("OpenGap",{{"roi",""}},[panel,feature](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();if(!input)throw std::invalid_argument("请先选择源数据");
        QJsonValue selected=p["roi"];if(selected.toString().isEmpty() && feature->GetState().committedRoi)selected=GetRefText(*feature->GetState().committedRoi);
        const auto roi=GetSelectedRoi(*panel->GetSession(),selected,input->dataRevision);if(!roi)throw std::invalid_argument("请选择已保存的正式区域");
        if(panel->GetContext().workflow.onNavigate)panel->GetContext().workflow.onNavigate("Gap","Start",{{"evaluationRoi",GetRefText(*roi)}});panel->SetComplete(id,"ParametersCopied");
    },TestPolicy::Read);
#endif
    panel->onStop=[panel,feature,pendingSave,pendingOperation] {
        RoiEditingRequest request;request.action=RoiEditingAction::Cancel;
        if(feature->SendRequest(request).error==RoiError::None){panel->GetContext().workflow.roiConsumer={};if(*pendingSave){const auto id=**pendingSave;pendingSave->reset();panel->SetComplete(id,"Cancelled");}if(*pendingOperation){const auto id=**pendingOperation;pendingOperation->reset();panel->SetComplete(id,"Cancelled");}}
    };
    panel->getGrayScope=[feature] {
        GrayStatisticsScope scope;const auto state=feature->GetState();if(!state.draft)return scope;
        if(!state.preview)throw std::invalid_argument("当前区域预览不可读取");
        scope.source=state.draft->source;scope.key=QString::number(state.previewRevision);
        scope.title="当前区域预览的有效灰度分布";scope.description="公共 ROI 成员关系 ∩ 源有效域；保存使用同一预览";
        scope.filter=[read=state.preview](const auto& points) {
            const auto result=read->GetPoints(points);if(result.error!=RoiError::None)throw std::runtime_error("区域成员读取失败");
            std::vector<bool> kept;kept.reserve(result.values.size());for(auto value:result.values)kept.push_back(value!=0);return kept;
        };return scope;
    };
    context.workflow.AttachExit("Roi",[feature,&workflow=context.workflow] {
        RoiEditingRequest request;request.action=RoiEditingAction::Cancel;
        const bool cancelled=feature->SendRequest(request).error==RoiError::None;
        if(cancelled)workflow.roiConsumer={};
        return cancelled;
    });
    auto forms=operationActions;forms.push_back("SetDraft");
    for(const auto& action:forms)if(auto* form=panel->GetParameterEditor(action)){
        const auto onEdited=form->onEdited;
        // 未完成或非法输入也立即撤销旧保存意图，再让原通知链解析当前控件。
        form->onEdited=[panel,feature,pendingSave,onEdited,finishSave,form,action]{
            const auto state=feature->GetState();if(state.isSaving)(void)feature->SendRequest({RoiEditingAction::CancelSave});
            if(*pendingSave){const auto id=**pendingSave;pendingSave->reset();const bool saved=!state.hasDraft && state.error==RoiError::None;
                finishSave(id,saved?"Succeeded":"Cancelled",state.error,saved?state.committedRoi:std::nullopt);}
            if(action=="SetDraft")SetRoiFields(*form);
            if(onEdited)onEdited();
        };
    }
    return panel;
}
}
