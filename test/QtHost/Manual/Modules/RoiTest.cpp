// 测试用途：通过既有公共入口显示 ROI 草稿、提交结果、取消及显隐。
#include "ModuleFactories.h"
#include "Host/RoiEditingHostFeature.h"
namespace Manual {
ModulePanel* CreateRoiTest(TestContext context,std::shared_ptr<RoiEditingHostFeature> feature,QWidget* parent)
{
    auto* panel=new ModulePanel(context,"Roi",parent);
    panel->SetNotice("黄色是可拖动草稿，青色是已提交的感兴趣区域。取消草稿保留已提交几何；坐标使用源模型空间。");
    panel->AttachAction("Begin",{{"boxToSource",QJsonValue()}},[panel,feature](auto id,const auto& p) {
        const auto source=panel->GetSession()->GetImageDescriptor();
        if (!source) throw std::invalid_argument("没有当前体数据");
        RoiRequest draft;draft.definition.source=source->dataRevision;draft.metadata.name="ROI";
        const auto catalog=panel->GetSession()->GetRoiDescriptors(true);
        draft.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;
        RoiNode node;
        if (!p["boxToSource"].isNull()) node.primitive.localToSource=GetArray<double,16>(p["boxToSource"]);
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
        draft.definition.nodes.push_back(node);
        RoiEditingRequest request;request.action=RoiEditingAction::Begin;request.draft=std::move(draft);
        const auto result=feature->SendRequest(request);
        panel->SetComplete(id,result.error==RoiError::None?"Succeeded":"Failed",{{"error",static_cast<int>(result.error)}});
    },TestPolicy::Interaction,true);
    panel->AttachAction("SetDraft",{{"boxToSource",QJsonValue()}},[panel,feature](auto id,const auto& p) {
        if (p["boxToSource"].isNull()) throw std::invalid_argument("请提供草稿区域框的变换矩阵");
        RoiEditingRequest request; request.action=RoiEditingAction::SetDraft;
        request.boxToSource=GetArray<double,16>(p["boxToSource"]);
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
                    {{"error",static_cast<int>(result.error)},{"roi",result.roi?GetRefText(result.roi->revision):QString()}});
            },action.second==RoiEditingAction::Cancel?TestPolicy::Stop:TestPolicy::View);
    }
    panel->onObserve=[panel,feature] {
        const auto state=feature->GetState();
        panel->SetState({{"hasDraft",state.hasDraft},{"isVisible",state.isVisible},{"isDragging",state.isDragging},
            {"committedRoi",state.committedRoi?GetRefText(*state.committedRoi):QString()}});
    };
    panel->onStop=[feature] {RoiEditingRequest request;request.action=RoiEditingAction::Cancel;feature->SendRequest(request);};
    context.workflow.AttachExit("Roi",[feature] {
        RoiEditingRequest request;request.action=RoiEditingAction::Cancel;
        return feature->SendRequest(request).error==RoiError::None;
    });
    return panel;
}
}
