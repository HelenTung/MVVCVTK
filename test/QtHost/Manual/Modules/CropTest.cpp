// 测试用途：验证裁剪控件调整、预览确认、历史导航、发布结果及恢复源数据的业务闭环。
#include "ModuleFactories.h"
#include "Host/CropHostFeature.h"
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <vtkNew.h>
#include <vtkPLYReader.h>
#include <vtkPolyData.h>
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
    for (const auto* id : {"primary-3d", "composite-volume", "slice-top-down", "slice-front-back", "slice-left-right"}) {
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
    DataRevisionRef input, editSource, source, output;
    bool isConfirmed = false;
    std::uint64_t pending = 0;
    std::optional<std::size_t> expectedNode;
    std::uint64_t deletedOperation = 0;
    CropRemovalMode preferredMode = CropRemovalMode::KeepInside;
    void InvalidatePreview() { isConfirmed = false; source = {}; output = {}; }
};
}
ModulePanel* CreateCropTest(TestContext context, std::shared_ptr<CropHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Crop", parent);
    auto flow = std::make_shared<CropFlow>();
    panel->SetNotice("开始后在主三维画面拖动盒或平面；每次有效拖动生成一个历史节点。确认结束控件编辑，发布生成数据，使用结果改变后续业务输入。撤销全部保留重做历史。");
    context.workflow.AttachExit("Crop", [owner = QPointer<ModulePanel>(panel), feature, flow] {
        if (!feature->GetState().isActive) return true;
        if (!owner || flow->pending || !GetFramesReady(owner)) return false;
        CropHostRequest request; request.action = CropHostAction::Exit;
        return feature->SendRequest(std::move(request));
    });
    // 只用于单个历史提交的超时，不承担刷新或推进任务。
    auto* deadline = new QTimer(panel); deadline->setSingleShot(true);
    QObject::connect(deadline, &QTimer::timeout, panel, [panel, flow] {
        const auto id = flow->pending; flow->pending = 0;
        if (id && !panel->GetContext().workflow.GetIsClosing()) panel->SetComplete(id, "Failed", {{"message", "预览提交超时，请检查视图后重试；未确认或发布结果"}});
    });
    panel->AttachAction("Start", {{"shape", "Box"}, {"removalMode", "KeepInside"}}, [panel, feature, flow](auto id, const auto& p) {
        const auto shape = GetEnum<CropHostAction>(p, "shape", {{"Box", CropHostAction::Box}, {"Plane", CropHostAction::Plane}});
        const auto mode = GetEnum<CropRemovalMode>(p, "removalMode", {{"None", CropRemovalMode::None},
            {"KeepInside", CropRemovalMode::KeepInside}, {"RemoveInside", CropRemovalMode::RemoveInside}});
        const bool first = flow->editSource != panel->GetSession()->GetImageDescriptor()->dataRevision;
        CropHostRequest request; request.action = shape; request.target = GetCropTarget();
        if (!feature->SendRequest(std::move(request))) { panel->SetComplete(id, "Rejected"); return; }
        ResetInitialCamera(panel, first);
        flow->editSource = panel->GetSession()->GetImageDescriptor()->dataRevision; flow->InvalidatePreview();
        flow->preferredMode = mode;
        request = {}; request.action = CropHostAction::Mode; request.target = GetCropTarget(); request.removalMode = mode;
        panel->SetComplete(id, feature->SendRequest(std::move(request)) ? "Succeeded" : "Rejected",
            {{"message", "请在主三维画面拖动控件，再确认裁剪预览"}});
    }, TestPolicy::Interaction, true);
    for (const auto& action : std::vector<std::pair<QString, CropHostAction>>{{"Box", CropHostAction::Box},
        {"Plane", CropHostAction::Plane}, {"Mode", CropHostAction::Mode}, {"Previous", CropHostAction::Previous},
        {"Next", CropHostAction::Next}, {"Node", CropHostAction::Node}, {"ResetPreview", CropHostAction::Node}}) {
        QJsonObject defaults;
        if (action.first == "Mode") defaults = {{"removalMode", "KeepInside"}};
        if (action.first == "Node") defaults = {{"nodeCount", "0"}};
        panel->AttachAction(action.first, defaults, [panel, feature, flow, deadline, action](auto id, const auto& p) {
            const auto before = feature->GetState();
            const bool first = flow->editSource != panel->GetSession()->GetImageDescriptor()->dataRevision;
            CropHostRequest request; request.action = action.second;
            if (action.second == CropHostAction::Box || action.second == CropHostAction::Plane || action.second == CropHostAction::Mode)
                request.target = GetCropTarget();
            if (action.second == CropHostAction::Mode) request.removalMode = GetEnum<CropRemovalMode>(p, "removalMode", {
                {"None", CropRemovalMode::None}, {"KeepInside", CropRemovalMode::KeepInside}, {"RemoveInside", CropRemovalMode::RemoveInside}});
            std::optional<std::size_t> node;
            if (action.second == CropHostAction::Previous) node = before.history.nodeCount - 1;
            if (action.second == CropHostAction::Next) node = before.history.nodeCount + 1;
            if (action.second == CropHostAction::Node) request.nodeCount = node = action.first == "ResetPreview" ? 0 : GetId(p["nodeCount"]);
            const auto requestedMode = request.removalMode;
            bool accepted = feature->SendRequest(std::move(request));
            if (accepted) {
                flow->InvalidatePreview();
                if (!node) {
                    ResetInitialCamera(panel, first);
                    flow->editSource = panel->GetSession()->GetImageDescriptor()->dataRevision;
                    if (requestedMode) flow->preferredMode = *requestedMode;
                    else if (feature->GetState().history.editMode == CropRemovalMode::None && flow->preferredMode != CropRemovalMode::None) {
                        CropHostRequest mode; mode.action = CropHostAction::Mode; mode.target = GetCropTarget(); mode.removalMode = flow->preferredMode;
                        accepted = feature->SendRequest(std::move(mode));
                    }
                }
                if (!accepted) { panel->SetComplete(id, "Rejected", {{"message", "工具已启用，但保留模式设置失败"}}); return; }
                if (node) { flow->pending = id; flow->expectedNode = node; flow->deletedOperation = 0; deadline->start(15000); panel->SetAdmission(id, true); }
                else panel->SetComplete(id, "Accepted", {{"message", "工具设置已接纳，请拖动控件并检查预览"}});
            } else panel->SetComplete(id, "Rejected");
        }, TestPolicy::Compute, action.second == CropHostAction::Box || action.second == CropHostAction::Plane || action.second == CropHostAction::Mode);
    }
    panel->AttachAction("DeleteNode", {{"operationIndex", "0"}}, [panel, feature, flow, deadline](auto id, const auto& p) {
        const auto operation = GetId(p["operationIndex"]); const auto before = feature->GetState().history;
        const auto found = std::find(before.operationIndices.begin(), before.operationIndices.end(), operation);
        if (found == before.operationIndices.end()) { panel->SetComplete(id, "Rejected", {{"message", "所选预览节点不存在，原始数据与已物化基线不可删除"}}); return; }
        const auto index = static_cast<std::size_t>(std::distance(before.operationIndices.begin(), found));
        CropHostRequest request; request.action = CropHostAction::DeleteNode; request.operationIndex = operation;
        if (!feature->SendRequest(std::move(request))) { panel->SetComplete(id, "Rejected", {{"message", "节点正忙或已失效，请等待当前预览提交"}}); return; }
        flow->InvalidatePreview(); flow->pending = id; flow->deletedOperation = operation;
        flow->expectedNode = before.nodeCount - (index < before.nodeCount ? 1 : 0);
        deadline->start(15000); panel->SetAdmission(id, true);
    }, TestPolicy::Compute);
    for (const auto& mode : std::vector<std::pair<QString, CropRemovalMode>>{{"KeepInside", CropRemovalMode::KeepInside}, {"RemoveInside", CropRemovalMode::RemoveInside}, {"PositionOnly", CropRemovalMode::None}})
        panel->AttachAction(mode.first, {}, [panel, feature, flow, mode](auto id, const auto&) {
            const bool first = flow->editSource != panel->GetSession()->GetImageDescriptor()->dataRevision;
            CropHostRequest request; request.action = CropHostAction::Mode; request.target = GetCropTarget(); request.removalMode = mode.second;
            const auto accepted = feature->SendRequest(std::move(request));
            if (accepted) { ResetInitialCamera(panel, first); flow->preferredMode = mode.second; flow->editSource = panel->GetSession()->GetImageDescriptor()->dataRevision; flow->InvalidatePreview(); }
            panel->SetComplete(id, accepted ? "Accepted" : "Rejected");
        }, TestPolicy::Interaction, true);
    for (const QString action : {QString("FinishEditing")})
        panel->AttachAction(action, {}, [panel, feature, flow](auto id, const auto&) {
            CropHostRequest request; request.action = CropHostAction::Exit;
            const bool accepted = feature->SendRequest(std::move(request));
            if (accepted) flow->isConfirmed = feature->GetState().history.nodeCount > 0;
            panel->SetComplete(id, !accepted ? "Rejected" : flow->isConfirmed ? "PreviewConfirmed" : "Exited",
                {{"message", flow->isConfirmed ? "预览已确认，下一步发布裁剪结果" : "已退出工具，当前没有可发布的裁剪历史"}});
        }, TestPolicy::Change);
    panel->AttachAction("Exit", {}, [panel, feature, flow](auto id, const auto&) {
        CropHostRequest request; request.action = CropHostAction::Exit;
        const bool accepted = feature->SendRequest(std::move(request));
        if (accepted) flow->isConfirmed = false;
        panel->SetComplete(id, accepted ? "Exited" : "Rejected", {{"message", "已退出工具；保留已提交历史，未提交的拖动不作确认"}});
    }, TestPolicy::Stop);
    panel->AttachAction("BuildResult", {}, [panel, feature, flow](auto id, const auto&) {
        CropHostRequest request; request.action = CropHostAction::BuildResult; request.target = GetCropTarget();
        const auto accepted = feature->SendRequest(std::move(request), [owner = QPointer<ModulePanel>(panel), flow, id](CropBuildResult result) {
            if (!owner) return;
            if (result.isSucceeded) { flow->source = result.sourceRevision; flow->output = result.outputRevision; }
            owner->SetComplete(id, result.isSucceeded ? "Published" : "Failed", {
                {"failureReason", static_cast<int>(result.failureReason)}, {"message", QString::fromStdString(result.message)},
                {"source", GetRefText(result.sourceRevision)}, {"output", GetRefText(result.outputRevision)},
                {"recipe", GetRefText(result.recipeRevision)}, {"commitId", QString::number(result.commitId)}});
        });
        panel->SetAdmission(id, accepted);
    }, TestPolicy::Compute);
    for (const bool restore : {false, true}) panel->AttachAction(restore ? "RestoreSource" : "SelectOutput", {},
        [panel, flow, restore](auto id, const auto&) { panel->SetInput(id, restore ? flow->source : flow->output); }, TestPolicy::Input, true);
    panel->AttachAction("SetPolyData", {{"plyPath", ""}}, [panel, feature, flow](auto id, const auto& p) {
        vtkNew<vtkPLYReader> reader; const auto path = GetText(p, "plyPath").toUtf8(); reader->SetFileName(path.constData()); reader->Update();
        if (!reader->GetOutput() || !reader->GetOutput()->GetNumberOfPoints()) throw std::invalid_argument("PLY 网格为空");
        CropHostRequest request; request.action = CropHostAction::SetPolyData; request.polyData = reader->GetOutput();
        const auto accepted = feature->SendRequest(std::move(request));
        if (accepted) { flow->InvalidatePreview(); flow->editSource = {}; }
        panel->SetComplete(id, accepted ? "Succeeded" : "Rejected", {{"message", "网格接口输入已设置；体数据引导流程请重新开始裁剪"}});
    });
    panel->AttachAction("ClearPolyData", {}, [panel, feature, flow](auto id, const auto&) {
        CropHostRequest request; request.action = CropHostAction::ClearPolyData;
        const auto accepted = feature->SendRequest(std::move(request));
        if (accepted) { flow->InvalidatePreview(); flow->editSource = {}; }
        panel->SetComplete(id, accepted ? "Succeeded" : "Rejected");
    });
    panel->validateAction = [panel, feature](const QString& action, const QJsonObject& p) -> QString {
        if (action == "Node" && GetId(p["nodeCount"]) > feature->GetState().history.operationCount) return "目标节点超出当前历史范围。";
        if (action == "Box" || action == "Plane" || action == "KeepInside" || action == "RemoveInside"
            || ((action == "Start" || action == "Mode") && p["removalMode"].toString() != "None")) return GetDisplayProblem(panel);
        return {};
    };
    panel->observeInBackground = true;
    panel->onObserve = [panel, feature, flow, deadline] {
        const auto input = panel->GetSession()->GetImageDescriptor();
        const auto current = input ? input->dataRevision : DataRevisionRef{};
        if (current != flow->input) {
            flow->input = current; flow->editSource = {}; flow->isConfirmed = false;
            if (current != flow->source && current != flow->output) { flow->source = {}; flow->output = {}; }
            if (flow->pending) { const auto id = flow->pending; flow->pending = 0; deadline->stop(); panel->SetComplete(id, "SourceChanged"); }
        }
        const auto state = feature->GetState();
        const bool framesReady = GetFramesReady(panel);
        const bool deletionComplete = !flow->deletedOperation || std::find(state.history.operationIndices.begin(), state.history.operationIndices.end(), flow->deletedOperation) == state.history.operationIndices.end();
        if (flow->pending && framesReady && flow->expectedNode == state.history.nodeCount && deletionComplete) {
            const auto id = flow->pending; flow->pending = 0; deadline->stop();
            panel->SetComplete(id, "Succeeded", {{"nodeCount", QString::number(state.history.nodeCount)}, {"message", flow->deletedOperation ? "已删除所选预览节点，其余节点已保留并更新画面" : "历史与视图已更新"}});
            flow->deletedOperation = 0;
        }
        const bool editingSource = GetDataRevisionRefValid(flow->editSource) && current == flow->editSource;
        const bool published = GetDataRevisionRefValid(flow->output);
        const bool selected = published && current == flow->output;
        const bool canSelect = published && current == flow->source;
        QJsonObject disabled;
        if (flow->pending || !framesReady) for (const auto* action : {"Previous", "Next", "Node", "ResetPreview", "FinishEditing", "DeleteNode"}) disabled[action] = "请先松开控件并等待裁剪预览提交；隐藏视图需恢复显示。";
        const auto primary = panel->GetSession()->GetRenderViewState({"primary-3d"});
        if (flow->pending || (primary && primary->isInteracting)) for (const auto* action : {"Start", "Box", "Plane", "Mode", "KeepInside", "RemoveInside", "PositionOnly"}) disabled[action] = "请先松开正在拖动的控件并等待该次提交。";
        if (!editingSource) for (const auto* action : {"Previous", "Next", "Node", "ResetPreview", "FinishEditing", "DeleteNode"}) disabled[action] = "当前输入没有本页的裁剪历史。";
        if (state.history.nodeCount == 0) for (const auto* action : {"Previous", "ResetPreview", "FinishEditing"}) disabled[action] = "请先有效拖动控件，生成裁剪历史。";
        if (state.history.nodeCount >= state.history.operationCount) disabled["Next"] = "当前已在最新历史节点。";
        if (!editingSource || !flow->isConfirmed || state.history.nodeCount == 0) disabled["BuildResult"] = "请先调整并确认当前输入的裁剪预览。";
        if (!canSelect) disabled["SelectOutput"] = "当前没有匹配源数据的已发布裁剪结果。";
        if (!selected) disabled["RestoreSource"] = "仅在使用本次裁剪结果时可以恢复其源数据。";
        QJsonArray indices; for (const auto index : state.history.operationIndices) indices.append(QString::number(index));
        panel->SetState({{"operationIndices", indices}, {"isActive", state.isActive}, {"isBusy", flow->pending != 0 || state.isPublishing},
            {"hasHistory", editingSource}, {"historySource", GetRefText(flow->editSource)}, {"isOutputCurrent", selected},
            {"nodeCount", QString::number(state.history.nodeCount)}, {"operationCount", QString::number(state.history.operationCount)},
            {"editMode", static_cast<int>(state.history.editMode)}, {"framesReady", framesReady}, {"isConfirmed", flow->isConfirmed},
            {"source", GetRefText(flow->source)}, {"output", GetRefText(flow->output)}, {"disabled", disabled}});
    };
    panel->onStop = [panel] { if (panel->onMessage) panel->onMessage("裁剪发布不支持单独取消，请等待完成；停止会话将统一结束任务。"); };
    return panel;
}
}
