// 测试用途：通过既有公共入口显示 ROI 草稿、提交结果、取消及显隐。
#include "ModuleFactories.h"
#include "Host/RoiEditingHostFeature.h"
#include <vtkMatrix4x4.h>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
namespace Manual {
namespace {
std::array<double,16> GetRoiBox(const QJsonObject& p,const ImageDescriptor& source,const std::optional<std::array<double,16>>& existing={})
{
    if(!p["boxToSource"].isNull())return GetAffineMatrix(p["boxToSource"]);
    std::array<double,3> center{},size{};auto direction=source.direction;
    if(existing)for(int row=0;row<3;++row)center[row]=(*existing)[row*4+3];
    else {center=source.origin;for(int row=0;row<3;++row)for(int axis=0;axis<3;++axis)center[row]+=direction[row*3+axis]*source.spacing[axis]*(source.extent[2*axis]+source.extent[2*axis+1])*0.5;}
    for(int axis=0;axis<3;++axis){if(existing){double length=0;for(int row=0;row<3;++row)length+=(*existing)[row*4+axis]*(*existing)[row*4+axis];size[axis]=2*std::sqrt(length);for(int row=0;row<3;++row)direction[row*3+axis]=(*existing)[row*4+axis]/(size[axis]*0.5);}else size[axis]=(source.extent[2*axis+1]-source.extent[2*axis]+1)*source.spacing[axis]*0.5;}
    if(!p["centerMM"].isNull())center=GetArray<double,3>(p["centerMM"]);
    if(!p["sizeMM"].isNull())size=GetArray<double,3>(p["sizeMM"]);
    if(!p["direction3D"].isNull())direction=GetDirectionMatrix(p["direction3D"]);
    std::array<double,16> matrix=roiIdentityMatrix;
    for(int axis=0;axis<3;++axis)if(!(size[axis]>0) || !std::isfinite(size[axis]))throw std::invalid_argument("区域尺寸必须为有限正数");
    for(int row=0;row<3;++row){matrix[row*4+3]=center[row];for(int axis=0;axis<3;++axis)matrix[row*4+axis]=direction[row*3+axis]*size[axis]*0.5;}
    return GetAffineMatrix(GetValues(matrix));
}
QJsonObject RoiBoxParameters(const std::array<double,16>& matrix,const QString& name)
{
    std::array<double,3> center{},size{};std::array<double,9> direction{};
    for(int row=0;row<3;++row)center[row]=matrix[row*4+3];
    for(int axis=0;axis<3;++axis){double length=0;for(int row=0;row<3;++row)length+=matrix[row*4+axis]*matrix[row*4+axis];size[axis]=2*std::sqrt(length);for(int row=0;row<3;++row)direction[row*3+axis]=matrix[row*4+axis]/(size[axis]*0.5);}
    return {{"name",name},{"centerMM",GetValues(center)},{"sizeMM",GetValues(size)},{"direction3D",GetValues(direction)},{"boxToSource",QJsonValue()}};
}
bool SameRoiBox(const std::array<double,16>& a,const std::array<double,16>& b){for(int i=0;i<16;++i)if(std::abs(a[i]-b[i])>1e-9*std::max({1.,std::abs(a[i]),std::abs(b[i])}))return false;return true;}
}
ModulePanel* CreateRoiTest(TestContext context,std::shared_ptr<RoiEditingHostFeature> feature,QWidget* parent)
{
    auto* panel=new ModulePanel(context,"Roi",parent);
    panel->SetNotice("黄色是可拖动草稿，青色是已提交的感兴趣区域。取消草稿保留已提交几何；坐标使用源模型空间。");
    const QJsonObject parameters{{"name","ROI"},{"centerMM",QJsonValue()},{"sizeMM",QJsonValue()},{"direction3D",QJsonValue()},{"boxToSource",QJsonValue()}};
    auto appliedName=std::make_shared<QString>("ROI");auto observedBox=std::make_shared<std::optional<std::array<double,16>>>();
    const auto begin=[panel,feature](const std::array<double,16>& matrix,const QString& name) {
        const auto source=panel->GetSession()->GetImageDescriptor();if(!source)throw std::invalid_argument("没有当前体数据");
        RoiRequest draft;draft.definition.source=source->dataRevision;draft.metadata.name=name.toStdString();
        const auto catalog=panel->GetSession()->GetRoiDescriptors(true);draft.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;
        RoiNode node;node.primitive.localToSource=matrix;draft.definition.nodes.push_back(node);
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);return feature->SendRequest(request);
    };
    panel->AttachAction("Begin",parameters,[panel,feature,begin,appliedName,observedBox](auto id,const auto& p) {
        const auto source=panel->GetSession()->GetImageDescriptor();
        if (!source) throw std::invalid_argument("没有当前体数据");
        const auto matrix=GetRoiBox(p,*source);const auto name=GetText(p,"name");const auto result=begin(matrix,name);
        if(result.error==RoiError::None){*appliedName=name;*observedBox=matrix;panel->SetParameterPatch("SetDraft",RoiBoxParameters(matrix,name));}
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",static_cast<int>(result.error)}});
    },TestPolicy::Interaction,true);
    panel->AttachAction("SetDraft",parameters,[panel,feature,appliedName,observedBox](auto id,const auto& p) {
        const auto state=feature->GetState();const auto source=panel->GetSession()->GetImageDescriptor();
        if(!source || !state.draft || state.draft->source!=source->dataRevision || state.draft->nodes.size()!=1)throw std::invalid_argument("请先建立当前输入的区域草稿");
        const auto matrix=GetRoiBox(p,*source,state.draft->nodes[0].primitive.localToSource);const auto name=GetText(p,"name");
        RoiEditingRequest request; request.action=RoiEditingAction::SetDraft;
        request.boxToSource=matrix;RoiRequest metadata;metadata.definition.source=state.draft->source;metadata.metadata.name=name.toStdString();request.draft=std::move(metadata);
        const auto result=feature->SendRequest(request);
        if(result.error==RoiError::None){*appliedName=name;*observedBox=matrix;panel->SetParameterPatch("SetDraft",RoiBoxParameters(matrix,name));}
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",static_cast<int>(result.error)}});
    },TestPolicy::Interaction,true);
    for (const auto& action:std::vector<std::pair<QString,RoiEditingAction>>{
        {"Commit",RoiEditingAction::Commit},{"Cancel",RoiEditingAction::Cancel},{"Visibility",RoiEditingAction::SetVisible}}) {
        panel->AttachAction(action.first,action.second==RoiEditingAction::SetVisible?QJsonObject{{"isVisible",true}}:QJsonObject{},
            [panel,feature,action](auto id,const auto& p) {
                RoiEditingRequest request;request.action=action.second;
                if(request.action==RoiEditingAction::SetVisible)request.isVisible=GetBool(p,"isVisible");
                const auto result=feature->SendRequest(request);
                panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",
                    {{"error",static_cast<int>(result.error)},{"roi",result.roi?GetRefText(result.roi->revision):QString()}});
            },action.second==RoiEditingAction::Cancel?TestPolicy::Stop:TestPolicy::View);
    }
    panel->onObserve=[panel,feature,appliedName,observedBox] {
        const auto state=feature->GetState();
        const auto input=panel->GetSession()->GetImageDescriptor();
        const auto roi=state.committedRoi ? panel->GetSession()->GetRoiDescriptor(*state.committedRoi) : std::optional<RoiDescriptor>{};
        bool dirty=false;
        if(state.draft && input && state.draft->source==input->dataRevision && state.draft->nodes.size()==1) {
            const auto matrix=state.draft->nodes[0].primitive.localToSource;
            try {auto p=panel->GetActionParameters("SetDraft");
                if(*observedBox && !SameRoiBox(**observedBox,matrix) && GetText(p,"name")==*appliedName && SameRoiBox(GetRoiBox(p,*input,**observedBox),**observedBox)) {panel->SetParameterPatch("SetDraft",RoiBoxParameters(matrix,*appliedName));p=panel->GetActionParameters("SetDraft");}
                dirty=GetText(p,"name")!=*appliedName || !SameRoiBox(GetRoiBox(p,*input,matrix),matrix);
            }catch(const std::exception&){dirty=true;}
            *observedBox=matrix;
        }
        panel->SetState({{"hasDraft",state.hasDraft},{"isVisible",state.isVisible},{"isDragging",state.isDragging},
            {"source",state.draft ? GetRefText(state.draft->source) : roi ? GetRefText(roi->definition.source) : QString()},
            {"isRoiCurrent",input && roi && roi->definition.source==input->dataRevision},
            {"committedRoi",state.committedRoi?GetRefText(*state.committedRoi):QString()},{"draftParametersChanged",dirty}});
    };
#if defined(MANUAL_CROP)
    panel->AttachAction("OpenCrop",{},[panel,feature](auto id,const auto&) {
        const auto state=feature->GetState();const auto input=panel->GetSession()->GetImageDescriptor();
        const auto roi=state.committedRoi?panel->GetSession()->GetRoiDescriptor(*state.committedRoi):std::optional<RoiDescriptor>{};
        if(!input || !roi || roi->definition.source!=input->dataRevision)throw std::invalid_argument("请先保存当前输入的正式区域");
        panel->GetContext().workflow.onNavigate("Crop","PrepareRoi",{{"inputRoi",GetRefText(roi->revision)}});panel->SetComplete(id,"ParametersCopied",{{"roi",GetRefText(roi->revision)}});
    },TestPolicy::Read);
#endif
#if defined(MANUAL_GAP)
    panel->AttachAction("OpenGap",{},[panel](auto id,const auto&) {if(panel->onMessage)panel->onMessage("孔隙分析使用当前采用体的有效域。需要限定此 ROI 时，先按正式 ROI 生成并采用裁剪体。");if(panel->GetContext().workflow.onNavigate)panel->GetContext().workflow.onNavigate("Gap","Start",{});panel->SetComplete(id,"ParametersCopied");},TestPolicy::Read);
#endif
    panel->onStop=[feature] {RoiEditingRequest request;request.action=RoiEditingAction::Cancel;feature->SendRequest(request);};
    panel->getGrayScope=[panel,feature] {
        GrayStatisticsScope scope;const auto state=feature->GetState();if(!state.draft)return scope;
        const auto& draft=*state.draft;
        if(draft.nodes.size()!=1 || draft.nodes[0].kind!=RoiNodeKind::Primitive || draft.nodes[0].primitive.shape!=RoiShape::Box)
            throw std::invalid_argument("当前草稿不属于可编辑区域盒，不能把整卷分布标成草稿范围");
        const auto primitive=draft.nodes[0].primitive;
        scope.source=draft.source;scope.key=QString::fromUtf8(QJsonDocument(GetValues(primitive.localToSource)).toJson(QJsonDocument::Compact))+QString::number(int(primitive.boundaryPolicy));scope.title="ROI 草稿范围的有效灰度分布";scope.description="草稿区域盒 ∩ 源有效域；草稿尚未发布";
        std::array<double,16> inverse{};vtkMatrix4x4::Invert(primitive.localToSource.data(),inverse.data());
        scope.filter=[inverse,edge=primitive.boundaryPolicy==RoiBoundaryPolicy::CropV1 ? 1+1e-6 : 1+1e-12](const auto& points) {
            std::vector<bool> kept;kept.reserve(points.size());
            for(const auto& point:points){bool contains=true;for(int row=0;row<3;++row){double local=inverse[row*4+3];for(int axis=0;axis<3;++axis)local+=inverse[row*4+axis]*point[axis];contains=contains && std::isfinite(local) && std::abs(local)<=edge;}kept.push_back(contains);}return kept;
        };
        return scope;
    };
    context.workflow.AttachExit("Roi",[feature] {
        RoiEditingRequest request;request.action=RoiEditingAction::Cancel;
        return feature->SendRequest(request).error==RoiError::None;
    });
    return panel;
}
}
