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
    if(primitive.shape!=RoiShape::Box)return {{"centerMM",GetValues(primitive.origin)},{"axis",GetValues(primitive.normal)},
        {"radius",primitive.radius},{"height",primitive.height},{"boxToSource",QJsonValue()}};
    const auto& matrix=primitive.localToSource;
    std::array<double,3> center{},size{};std::array<double,9> direction{};
    for(int row=0;row<3;++row)center[row]=matrix[row*4+3];
    for(int axis=0;axis<3;++axis){double length=0;for(int row=0;row<3;++row)length+=matrix[row*4+axis]*matrix[row*4+axis];size[axis]=2*std::sqrt(length);for(int row=0;row<3;++row)direction[row*3+axis]=matrix[row*4+axis]/(size[axis]*0.5);}
    return {{"centerMM",GetValues(center)},{"sizeMM",GetValues(size)},{"direction3D",GetValues(direction)},{"boxToSource",QJsonValue()}};
}
RoiPrimitive GetRoiPrimitive(const QJsonObject& p,const ImageDescriptor& source,RoiPrimitive primitive)
{
    if(primitive.shape==RoiShape::Box)primitive.localToSource=GetRoiBox(p,source,primitive.localToSource);
    else {
        if(p.contains("centerMM") && !p["centerMM"].isNull())primitive.origin=GetArray<double,3>(p["centerMM"]);
        primitive.radius=GetNumber(p,"radius");
        if(primitive.shape==RoiShape::Cylinder){primitive.normal=GetArray<double,3>(p["axis"]);primitive.height=GetNumber(p,"height");}
    }
    return primitive;
}
bool SameRoiPrimitive(const RoiPrimitive& a,const RoiPrimitive& b)
{
    const auto same=[](double x,double y){return std::isfinite(x)&&std::isfinite(y)&&std::abs(x-y)<=1e-9*std::max({1.,std::abs(x),std::abs(y)});};
    if(a.shape!=b.shape)return false;
    if(a.shape==RoiShape::Box){for(int i=0;i<16;++i)if(!same(a.localToSource[i],b.localToSource[i]))return false;return true;}
    for(int i=0;i<3;++i)if(!same(a.origin[i],b.origin[i]) || (a.shape==RoiShape::Cylinder && !same(a.normal[i],b.normal[i])))return false;
    return same(a.radius,b.radius) && (a.shape!=RoiShape::Cylinder || same(a.height,b.height));
}
QJsonObject RoiMetadataParameters(const QJsonObject& p){return {{"name",GetText(p,"name")},{"group",GetText(p,"group")},{"description",GetText(p,"description")}};}
RoiMetadata GetRoiMetadata(const QJsonObject& p){return {GetText(p,"name").toStdString(),GetText(p,"group").toStdString(),GetText(p,"description").toStdString(),false};}

}
ModulePanel* CreateRoiTest(TestContext context,std::shared_ptr<RoiEditingHostFeature> feature,QWidget* parent)
{
    auto* panel=new ModulePanel(context,"Roi",parent);
    panel->SetNotice("区域只保存范围，不改变当前输入。黄色为草稿，青色为已保存区域；盒支持拖动和数值调整，球和圆柱通过参数调整。保存后可直接用于各业务。");
    const QJsonObject parameters{{"name","区域"},{"group",""},{"description",""},{"centerMM",QJsonValue()},
        {"sizeMM",QJsonValue()},{"direction3D",QJsonValue()},{"boxToSource",QJsonValue()},
        {"axis",QJsonArray{0,0,1}},{"radius",1.0},{"height",2.0}};
    auto appliedMetadata=std::make_shared<QJsonObject>();auto observedPrimitive=std::make_shared<std::optional<RoiPrimitive>>();
    const auto remember=[panel,appliedMetadata,observedPrimitive](const RoiPrimitive& primitive,const QJsonObject& metadata) {
        *appliedMetadata=metadata;*observedPrimitive=primitive;
        auto patch=RoiGeometryParameters(primitive);for(auto it=metadata.begin();it!=metadata.end();++it)patch[it.key()]=it.value();
        panel->SetParameterPatch("SetDraft",patch);
    };
    auto beginParameters=parameters;beginParameters["shape"]="Box";
    panel->AttachAction("Begin",beginParameters,[panel,feature,remember](auto id,const auto& p) {
        const auto source=panel->GetSession()->GetImageDescriptor();if(!source)throw std::invalid_argument("没有当前体数据");
        RoiRequest draft;draft.definition.source=source->dataRevision;draft.metadata=GetRoiMetadata(p);
        const auto catalog=panel->GetSession()->GetRoiDescriptors(true);draft.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;
        RoiNode node;node.primitive.shape=GetEnum<RoiShape>(p,"shape",{{"Box",RoiShape::Box},{"Sphere",RoiShape::Sphere},{"Cylinder",RoiShape::Cylinder}});
        if(node.primitive.shape==RoiShape::Box)node.primitive.localToSource=GetRoiBox(p,*source);
        else {auto values=p;if(!values.contains("centerMM") || values["centerMM"].isNull()){const auto matrix=GetRoiBox({},*source);values["centerMM"]=QJsonArray{matrix[3],matrix[7],matrix[11]};}node.primitive=GetRoiPrimitive(values,*source,node.primitive);}
        draft.definition.nodes.push_back(node);
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);const auto result=feature->SendRequest(request);
        if(result.error==RoiError::None)remember(node.primitive,RoiMetadataParameters(p));
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});
    },TestPolicy::Interaction,true);
    panel->AttachAction("SetDraft",parameters,[panel,feature,remember](auto id,const auto& p) {
        const auto state=feature->GetState();const auto source=panel->GetSession()->GetImageDescriptor();
        if(!source || !state.draft || state.draft->source!=source->dataRevision || state.draft->nodes.size()!=1)throw std::invalid_argument("请先建立当前输入的区域草稿");
        RoiEditingRequest request;request.action=RoiEditingAction::SetDraft;request.primitive=GetRoiPrimitive(p,*source,state.draft->nodes.front().primitive);
        RoiRequest metadata;metadata.definition.source=state.draft->source;metadata.metadata=GetRoiMetadata(p);request.draft=std::move(metadata);
        const auto result=feature->SendRequest(request);if(result.error==RoiError::None)remember(*request.primitive,RoiMetadataParameters(p));
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",int(result.error)}});
    },TestPolicy::Interaction,true);
    for (const auto& action:std::vector<std::pair<QString,RoiEditingAction>>{
        {"Commit",RoiEditingAction::Commit},{"Cancel",RoiEditingAction::Cancel},{"Visibility",RoiEditingAction::SetVisible}}) {
        panel->AttachAction(action.first,action.second==RoiEditingAction::SetVisible?QJsonObject{{"isVisible",true}}:QJsonObject{},
            [panel,feature,action](auto id,const auto& p) {
                RoiEditingRequest request;request.action=action.second;
                if(request.action==RoiEditingAction::SetVisible)request.isVisible=GetBool(p,"isVisible");
                const auto result=feature->SendRequest(request);
                panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",
                    {{"error",static_cast<int>(result.error)},{"roi",result.roi?GetRefText(result.roi->revision):QString()},
                     {"message",result.roi?"区域已保存，可用于分析":""}});
                auto& workflow=panel->GetContext().workflow;
                if (result.roi && !workflow.roiConsumer.isEmpty()) {
                    const auto consumer=workflow.roiConsumer;workflow.roiConsumer={};
                    if (workflow.onNavigate) workflow.onNavigate(consumer["module"].toString(),consumer["action"].toString(),
                        {{consumer["key"].toString(),GetRefText(result.roi->revision)}});
                } else if (request.action==RoiEditingAction::Cancel) workflow.roiConsumer={};
            },action.second==RoiEditingAction::Cancel?TestPolicy::Stop:TestPolicy::View);
    }
    panel->AttachAction("Edit",{{"roi",""}},[panel,feature,remember](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();
        if(!input)throw std::invalid_argument("请先选择源数据");
        const auto ref=GetSelectedRoi(*panel->GetSession(),p["roi"],input->dataRevision);
        if(!ref)throw std::invalid_argument("请选择已有区域");
        const auto roi=panel->GetSession()->GetRoiDescriptor(*ref);
        RoiRequest draft;draft.action=RoiAction::SetGeometry;draft.definition=roi->definition;
        draft.metadata=roi->metadata;draft.expectedRoi=roi->revision;draft.expectedCatalogRevision=roi->catalogRevision;
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
    panel->onObserve=[panel,feature,appliedMetadata,observedPrimitive] {
        const auto state=feature->GetState();
        if(auto* form=panel->GetParameterEditor("SetDraft")) {
            const auto shape=state.draft?state.draft->nodes.front().primitive.shape:RoiShape::Box;
            form->SetFieldApplicability({{"boxToSource",shape==RoiShape::Box},{"sizeMM",shape==RoiShape::Box},{"direction3D",shape==RoiShape::Box},
                {"radius",shape!=RoiShape::Box},{"axis",shape==RoiShape::Cylinder},{"height",shape==RoiShape::Cylinder}});
        }
        const auto input=panel->GetSession()->GetImageDescriptor();
        const auto roi=state.committedRoi?panel->GetSession()->GetRoiDescriptor(*state.committedRoi):std::optional<RoiDescriptor>{};
        bool dirty=false;
        if(state.draft && input && state.draft->source==input->dataRevision && state.draft->nodes.size()==1) {
            const auto primitive=state.draft->nodes.front().primitive;
            try {auto p=panel->GetActionParameters("SetDraft");
                if(*observedPrimitive && !SameRoiPrimitive(**observedPrimitive,primitive) && RoiMetadataParameters(p)==*appliedMetadata
                    && SameRoiPrimitive(GetRoiPrimitive(p,*input,**observedPrimitive),**observedPrimitive)) {
                    panel->SetParameterPatch("SetDraft",RoiGeometryParameters(primitive));p=panel->GetActionParameters("SetDraft");
                }
                dirty=RoiMetadataParameters(p)!=*appliedMetadata || !SameRoiPrimitive(GetRoiPrimitive(p,*input,primitive),primitive);
            }catch(const std::exception&){dirty=true;}
            *observedPrimitive=primitive;
        }
        QJsonArray regions;for(const auto& item:panel->GetSession()->GetRoiDescriptors(true))regions.append(QJsonObject{
            {"roi",GetRefText(item.revision)},{"name",QString::fromStdString(item.metadata.name)},
            {"group",QString::fromStdString(item.metadata.group)},{"isArchived",item.metadata.isArchived},
            {"description",QString::fromStdString(item.metadata.description)},{"source",GetRefText(item.definition.source)}});
        panel->SetState({{"regions",regions},{"hasDraft",state.hasDraft},{"isVisible",state.isVisible},{"isDragging",state.isDragging},
            {"source",state.draft?GetRefText(state.draft->source):roi?GetRefText(roi->definition.source):QString()},
            {"isRoiCurrent",input && roi && roi->definition.source==input->dataRevision},
            {"committedRoi",state.committedRoi?GetRefText(*state.committedRoi):QString()},{"draftParametersChanged",dirty}});
    };
#if defined(MANUAL_GAP)
    panel->AttachAction("OpenGap",{},[panel](auto id,const auto&) {if(panel->onMessage)panel->onMessage("孔隙分析使用当前采用体的有效域。需要限定此 ROI 时，先按正式 ROI 生成并采用裁剪体。");if(panel->GetContext().workflow.onNavigate)panel->GetContext().workflow.onNavigate("Gap","Start",{});panel->SetComplete(id,"ParametersCopied");},TestPolicy::Read);
#endif
    panel->onStop=[panel,feature] {
        RoiEditingRequest request;request.action=RoiEditingAction::Cancel;
        if(feature->SendRequest(request).error==RoiError::None)panel->GetContext().workflow.roiConsumer={};
    };
    panel->getGrayScope=[panel,feature] {
        GrayStatisticsScope scope;const auto state=feature->GetState();if(!state.draft)return scope;
        const auto& draft=*state.draft;
        if(draft.nodes.size()!=1 || draft.nodes[0].kind!=RoiNodeKind::Primitive)
            throw std::invalid_argument("当前草稿不属于可编辑区域盒，不能把整卷分布标成草稿范围");
        const auto primitive=draft.nodes[0].primitive;
        if(primitive.shape==RoiShape::Sphere || primitive.shape==RoiShape::Cylinder) {
            scope.source=draft.source;scope.key=GetJsonText(QJsonObject{{"shape",int(primitive.shape)},
                {"origin",GetValues(primitive.origin)},{"normal",GetValues(primitive.normal)},
                {"radius",primitive.radius},{"height",primitive.height}});
            scope.title="曲面区域草稿的有效灰度分布";scope.description="草稿范围 ∩ 源有效域；尚未保存";
            scope.filter=[primitive](const auto& points) {
                auto axis=primitive.normal;const auto length=std::hypot(axis[0],axis[1],axis[2]);
                for(auto& value:axis)value/=length;
                std::vector<bool> kept;kept.reserve(points.size());
                for(const auto& point:points) {
                    const std::array<double,3> offset{point[0]-primitive.origin[0],point[1]-primitive.origin[1],point[2]-primitive.origin[2]};
                    const auto axial=offset[0]*axis[0]+offset[1]*axis[1]+offset[2]*axis[2];
                    kept.push_back(primitive.shape==RoiShape::Sphere
                        ? std::hypot(offset[0],offset[1],offset[2])<=primitive.radius
                        : std::abs(axial)<=primitive.height*.5 && std::hypot(offset[0]-axial*axis[0],
                            offset[1]-axial*axis[1],offset[2]-axial*axis[2])<=primitive.radius);
                }
                return kept;
            };
            return scope;
        }
        scope.source=draft.source;scope.key=QString::fromUtf8(QJsonDocument(GetValues(primitive.localToSource)).toJson(QJsonDocument::Compact))+QString::number(int(primitive.boundaryPolicy));scope.title="ROI 草稿范围的有效灰度分布";scope.description="草稿区域盒 ∩ 源有效域；草稿尚未发布";
        std::array<double,16> inverse{};vtkMatrix4x4::Invert(primitive.localToSource.data(),inverse.data());
        scope.filter=[inverse,edge=primitive.boundaryPolicy==RoiBoundaryPolicy::CropV1 ? 1+1e-6 : 1+1e-12](const auto& points) {
            std::vector<bool> kept;kept.reserve(points.size());
            for(const auto& point:points){bool contains=true;for(int row=0;row<3;++row){double local=inverse[row*4+3];for(int axis=0;axis<3;++axis)local+=inverse[row*4+axis]*point[axis];contains=contains && std::isfinite(local) && std::abs(local)<=edge;}kept.push_back(contains);}return kept;
        };
        return scope;
    };
    context.workflow.AttachExit("Roi",[feature,&workflow=context.workflow] {
        RoiEditingRequest request;request.action=RoiEditingAction::Cancel;
        const bool cancelled=feature->SendRequest(request).error==RoiError::None;
        if(cancelled)workflow.roiConsumer={};
        return cancelled;
    });
    return panel;
}
}
