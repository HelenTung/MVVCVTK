// 测试用途：通过旋转页面测试启用、旋转、取消、撤销和帧提交终态。
#include "ModuleFactories.h"
#include "Host/ModelRotationHostFeature.h"
#include "Support/ParameterEditor.h"
#include <QPointer>
namespace Manual {
ModulePanel* CreateRotationTest(TestContext context, std::shared_ptr<ModelRotationHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Rotation", parent);
    panel->SetNotice("旋转轴和旋转中心使用世界坐标。旋转请求接纳后仍需等待视图更新；相机导航和计量对齐是独立操作。");
    context.workflow.AttachExit("Rotation", [feature] {
        const auto state = feature->GetState();
        if (!state.isEnabled) return true;
        ModelRotationRequest request; request.action = ModelRotationAction::SetEnabled; request.isEnabled = false;
        return (feature->SendRequest(request).status == ModelRotationAdmissionStatus::Accepted);
    });
    for (const auto& action : std::vector<std::pair<QString, ModelRotationAction>>{
        {"SetEnabled", ModelRotationAction::SetEnabled}, {"Rotate", ModelRotationAction::Rotate},
        {"Cancel", ModelRotationAction::Cancel}, {"Undo", ModelRotationAction::Undo}}) {
        QJsonObject defaults;
        if (action.second == ModelRotationAction::SetEnabled) defaults = {{"isEnabled", true}};
        if (action.second == ModelRotationAction::Rotate) defaults = GetJson(R"({"worldAxis":[0,0,1],"angleDeg":10,"worldCenter":null})");
        panel->AttachAction(action.first, defaults, [panel, feature, action](auto id, const auto& params) {
            ModelRotationRequest request; request.action = action.second;
            if (request.action == ModelRotationAction::SetEnabled) request.isEnabled = GetBool(params, "isEnabled");
            if (request.action == ModelRotationAction::Rotate) {
                request.worldAxis = GetArray<double, 3>(params["worldAxis"]);
                request.angleDeg = GetNumber(params, "angleDeg");
                if (!params["worldCenter"].isNull()) request.worldCenter = GetArray<double, 3>(params["worldCenter"]);
            }
            const QPointer<ModulePanel> owner(panel);
            const auto admission = feature->SendRequest(request, [owner, id](ModelRotationResult result) {
                if (owner) owner->SetComplete(id, result.status == ModelRotationStatus::Succeeded ? "Succeeded"
                    : result.status == ModelRotationStatus::Cancelled ? "Cancelled" : "SourceChanged",
                    {{"requestId", QString::number(result.requestId)}, {"state", static_cast<int>(result.status)}});
            });
            panel->SetAdmission(id, admission.status == ModelRotationAdmissionStatus::Accepted,
                {{"requestId", QString::number(admission.requestId)}, {"admission", static_cast<int>(admission.status)}});
        }, action.second == ModelRotationAction::Cancel ? TestPolicy::Stop
            : action.second == ModelRotationAction::SetEnabled ? TestPolicy::Change : TestPolicy::Compute,
            action.second != ModelRotationAction::Cancel);
    }
    panel->onObserve = [panel, feature] {
        const auto state = feature->GetState();
        panel->GetParameterEditor("SetEnabled")->GetField("isEnabled")->SetAppliedBoolean(state.isEnabled, "rotation");
        QJsonObject summary{{"status", static_cast<int>(state.status)}, {"isEnabled", state.isEnabled}, {"undoCount", QString::number(state.undoCount)}};
        summary["isBusy"] = state.status == ModelRotationStatus::Pending || state.status == ModelRotationStatus::Dragging;
        summary["isInvalidated"] = state.status == ModelRotationStatus::Invalidated;
        summary["requestedEnabled"]=panel->GetActionParameters("SetEnabled")["isEnabled"];
        if (panel->GetContext().workflow.getViewTransforms)
            summary["projection"]=panel->GetContext().workflow.getViewTransforms();
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Cancel", {}); };
    return panel;
}
}
