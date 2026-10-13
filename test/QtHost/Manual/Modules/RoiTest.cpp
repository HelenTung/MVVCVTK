// 测试用途：通过既有公共入口显示 ROI 草稿、提交结果、取消及显隐。
#include "ModuleFactories.h"
#include "Host/RoiEditingHostFeature.h"
#include "Support/RoiSelection.h"
#include "Support/ParameterEditor.h"
#include <vtkMatrix4x4.h>
#include <QJsonDocument>
#include <cmath>
namespace Manual {
ModulePanel* CreateRoiTest(TestContext context,std::shared_ptr<RoiEditingHostFeature> feature,QWidget* parent)
{
    auto* panel=new ModulePanel(context,"Roi",parent);
    panel->SetNotice("区域只保存范围，不改变当前输入。黄色为编辑草稿，青色为已保存区域；保存后可直接用于各业务。盒支持拖动；球和圆柱通过参数调整。");
    panel->AttachAction("Begin",{{"name","区域"},{"group",""},{"description",""},{"shape","Box"},
        {"boxToSource",QJsonValue()},{"center",QJsonArray{0,0,0}},{"axis",QJsonArray{0,0,1}},
        {"radius",1.0},{"height",2.0}},[panel,feature](auto id,const auto& p) {
        const auto source=panel->GetSession()->GetImageDescriptor();
        if (!source) throw std::invalid_argument("没有当前体数据");
        RoiRequest draft;draft.definition.source=source->dataRevision;
        draft.metadata={GetText(p,"name").toStdString(),GetText(p,"group").toStdString(),GetText(p,"description").toStdString(),false};
        const auto catalog=panel->GetSession()->GetRoiDescriptors(true);
        draft.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;
        RoiNode node;
        node.primitive.shape=GetEnum<RoiShape>(p,"shape",{{"Box",RoiShape::Box},{"Sphere",RoiShape::Sphere},{"Cylinder",RoiShape::Cylinder}});
        if (node.primitive.shape==RoiShape::Box) {
        if (!p["boxToSource"].isNull()) node.primitive.localToSource=GetAffineMatrix(p["boxToSource"]);
        else {
            auto& matrix=node.primitive.localToSource;
            for (int row=0;row<3;++row) {
                matrix[row*4+3]=source->origin[row];
                for (int axis=0;axis<3;++axis) {
                    const double half=(source->extent[axis*2+1]-source->extent[axis*2]+1)*0.25;
                    const double center=(source->extent[axis*2]+source->extent[axis*2+1])*0.5;
                    matrix[row*4+axis]=source->direction[row*3+axis]*source->spacing[axis]*half;
                    matrix[row*4+3]+=source->direction[row*3+axis]*source->spacing[axis]*center;
                }
            }
        }
        }
        if (node.primitive.shape!=RoiShape::Box) {
            node.primitive.localToSource=roiIdentityMatrix;
            node.primitive.origin=GetArray<double,3>(p["center"]);
            if(node.primitive.shape==RoiShape::Cylinder)node.primitive.normal=GetArray<double,3>(p["axis"]);
            node.primitive.radius=GetNumber(p,"radius");
            node.primitive.height=node.primitive.shape==RoiShape::Cylinder?GetNumber(p,"height"):0;
        }
        draft.definition.nodes.push_back(node);
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);
        const auto result=feature->SendRequest(request);
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",static_cast<int>(result.error)}});
    },TestPolicy::Interaction,true);
    panel->AttachAction("SetDraft",{{"boxToSource",QJsonValue()},{"center",QJsonArray{0,0,0}},
        {"axis",QJsonArray{0,0,1}},{"radius",1.0},{"height",2.0}},[panel,feature](auto id,const auto& p) {
        const auto state=feature->GetState();
        if (!state.draft) throw std::invalid_argument("请先创建或编辑区域");
        RoiEditingRequest request; request.action=RoiEditingAction::SetDraft;
        request.primitive=state.draft->nodes.front().primitive;
        if (request.primitive->shape==RoiShape::Box) {
            if (p["boxToSource"].isNull()) throw std::invalid_argument("请提供草稿区域框的变换矩阵");
            request.primitive->localToSource=GetAffineMatrix(p["boxToSource"]);
        } else {
            request.primitive->origin=GetArray<double,3>(p["center"]);
            if(request.primitive->shape==RoiShape::Cylinder)request.primitive->normal=GetArray<double,3>(p["axis"]);
            request.primitive->radius=GetNumber(p,"radius");
            request.primitive->height=request.primitive->shape==RoiShape::Cylinder?GetNumber(p,"height"):0;
        }
        const auto result=feature->SendRequest(request);
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
    panel->AttachAction("Edit",{{"roi",""}},[panel,feature](auto id,const auto& p) {
        const auto input=panel->GetSession()->GetImageDescriptor();
        if(!input)throw std::invalid_argument("请先选择源数据");
        const auto ref=GetSelectedRoi(*panel->GetSession(),p["roi"],input->dataRevision);
        if(!ref)throw std::invalid_argument("请选择已有区域");
        const auto roi=panel->GetSession()->GetRoiDescriptor(*ref);
        RoiRequest draft;draft.action=RoiAction::SetGeometry;draft.definition=roi->definition;
        draft.metadata=roi->metadata;draft.expectedRoi=roi->revision;draft.expectedCatalogRevision=roi->catalogRevision;
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);
        const auto result=feature->SendRequest(request);
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
    panel->onObserve=[panel,feature] {
        const auto state=feature->GetState();
        if(auto* form=panel->GetParameterEditor("SetDraft")) {
            const auto shape=state.draft?state.draft->nodes.front().primitive.shape:RoiShape::Box;
            form->SetFieldApplicability({{"boxToSource",shape==RoiShape::Box},{"center",shape!=RoiShape::Box},
                {"radius",shape!=RoiShape::Box},{"axis",shape==RoiShape::Cylinder},{"height",shape==RoiShape::Cylinder}});
        }
        const auto input=panel->GetSession()->GetImageDescriptor();
        const auto roi=state.committedRoi ? panel->GetSession()->GetRoiDescriptor(*state.committedRoi) : std::optional<RoiDescriptor>{};
        QJsonArray regions;
        for(const auto& item:panel->GetSession()->GetRoiDescriptors(true)) regions.append(QJsonObject{
            {"roi",GetRefText(item.revision)},{"name",QString::fromStdString(item.metadata.name)},
            {"group",QString::fromStdString(item.metadata.group)},{"isArchived",item.metadata.isArchived},
            {"description",QString::fromStdString(item.metadata.description)},
            {"source",GetRefText(item.definition.source)}});
        panel->SetState({{"regions",regions},{"hasDraft",state.hasDraft},{"isVisible",state.isVisible},{"isDragging",state.isDragging},
            {"source",state.draft ? GetRefText(state.draft->source) : roi ? GetRefText(roi->definition.source) : QString()},
            {"isRoiCurrent",input && roi && roi->definition.source==input->dataRevision},
            {"committedRoi",state.committedRoi?GetRefText(*state.committedRoi):QString()}});
    };
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
