// 测试用途：通过零件页面测试分割、目录查询、稳定身份与展示状态修改。
#include "ModuleFactories.h"
#include "PartInput.h"
#include "Support/ParameterEditor.h"
#include <QPointer>
#include <algorithm>
namespace Manual {
namespace {
// 分割预览与原始灰度对照使用同一视图；保存用户材质，只在业务状态改变时提交请求。
struct PartPreviewDisplay : std::enable_shared_from_this<PartPreviewDisplay> {
    struct View { std::optional<double> savedOpacity; std::optional<bool> requested; DataRevisionRef requestedSource; bool pending = false; QString error; };
    std::map<std::string, View> views;
    void Observe(ModulePanel* panel, bool enabled)
    {
        if (panel->GetContext().workflow.GetIsClosing()) return;
        for (const auto* id : {"primary-3d"}) {
            auto& view = views[id];
            if (view.pending) continue;
            const auto current = panel->GetSession()->GetRenderViewState({id});
            if (!current) continue;
            if (view.requested == enabled && view.requestedSource == current->dataRevision) continue;
            view.requested = enabled; view.requestedSource = current->dataRevision; view.error.clear();
            double opacity = 0;
            if (enabled) {
                if (!view.savedOpacity) view.savedOpacity = current->material.opacity;
            } else {
                if (!view.savedOpacity) continue;
                // 用户在预览期间主动修改了材质时，保留用户的新值。
                if (current->material.opacity != 0) { view.savedOpacity.reset(); continue; }
                opacity = *view.savedOpacity;
            }
            HostViewSetRequest request; request.targetView.viewId = id; request.opacity = opacity;
            view.pending = true;
            const QPointer<ModulePanel> owner(panel);
            // 回调持有显示状态；页面销毁后只释放状态，不再访问 UI。
            auto* state = &view;
            const auto complete = [owner, lifetime = shared_from_this(), state, enabled](HostResult result) {
                if (!owner) return;
                state->pending = false;
                if (result.isSucceeded) { if (!enabled) state->savedOpacity.reset(); }
                else {
                    state->error = QString::fromStdString(result.message);
                    if (owner->onMessage) owner->onMessage("切换零件预览显示失败：" + state->error);
                }
            };
            if (!panel->GetSession()->SendRequestResult(std::move(request), complete)) {
                view.pending = false; view.error = "视图请求未接受";
                if (panel->onMessage) panel->onMessage("切换零件预览显示失败：" + view.error);
            }
        }
    }
    bool GetReady() const
    {
        return views.size() == 1 && std::all_of(views.begin(), views.end(),
            [](const auto& entry) { return !entry.second.pending && entry.second.error.isEmpty(); });
    }
    QString GetError() const
    {
        for (const auto& entry : views) if (!entry.second.error.isEmpty())
            return QString::fromStdString(entry.first) + "：" + entry.second.error;
        return {};
    }
    void RetryFailed()
    {
        // 仅用户再次提交或输入修订改变时重试；稳定失败不持续投递重绘。
        for (auto& entry : views) if (!entry.second.pending && !entry.second.error.isEmpty())
            entry.second.requested.reset();
    }
};
}
ModulePanel* CreatePartTest(TestContext context, std::shared_ptr<PartSegmentationHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Part", parent);
    panel->observeInBackground = true;
    auto preview = std::make_shared<PartPreviewDisplay>();
    panel->SetNotice("主三维显示浅灰零件表面，体渲染保留原始灰度并叠加选中零件的半透明定位标记，切片保留灰度细节。选中节点后点击“高亮此零件”即可切换。隐藏零件只影响分割预览，不裁去原始体数据。");
    panel->AttachAction("Start", GetJson(R"({"threshold":0.5,"minPartVoxels":"1000"})"), [panel, feature, preview](auto id, const auto& params) {
        PartSegmentationRequest request; request.action = PartSegmentationAction::Start;
        request.start = PartSegmentationStartParams{GetPartViews(), GetNumber(params, "threshold"), GetId(params["minPartVoxels"])};
        const QPointer<ModulePanel> owner(panel);
        const auto admission = feature->SendRequest(std::move(request), [owner, id, preview](PartSegmentationResult result) {
            if (owner) {
                if (result.status == PartResultStatus::Succeeded) preview->RetryFailed();
                auto detail = GetPartResult(result);
                if (result.status == PartResultStatus::Failed) detail["hint"] = "工作集预算充足时仍可能超过零件数量上限；可调高最小零件体素数以过滤微小噪声，或调整分割阈值。";
                owner->SetComplete(id, result.status == PartResultStatus::Succeeded ? "Succeeded"
                    : result.status == PartResultStatus::SucceededWithDisplayFailure ? "SucceededWithDisplayFailure"
                    : result.status == PartResultStatus::Cancelled ? "Cancelled" : "Failed", detail);
            }
        });
        panel->SetAdmission(id, admission.status == PartAdmissionStatus::Accepted,
            {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
    }, TestPolicy::Compute, true);
    for (const auto& action : std::vector<std::pair<QString, PartSegmentationAction>>{
        {"Stop", PartSegmentationAction::Stop}, {"Visibility", PartSegmentationAction::SetVisibility}, {"Clear", PartSegmentationAction::Clear}}) {
        panel->AttachAction(action.first, action.second == PartSegmentationAction::SetVisibility ? QJsonObject{{"isVisible", true}} : QJsonObject{},
            [panel, feature, action, preview](auto id, const auto& params) {
                PartSegmentationRequest request; request.action = action.second;
                if (request.action == PartSegmentationAction::SetVisibility) request.isVisible = GetBool(params, "isVisible");
                const auto admission = feature->SendRequest(std::move(request));
                if (admission.status == PartAdmissionStatus::Accepted) preview->RetryFailed();
                panel->SetComplete(id, admission.status == PartAdmissionStatus::Accepted ? "Accepted" : "Rejected",
                    {{"admission", static_cast<int>(admission.status)}});
            }, action.second == PartSegmentationAction::Stop ? TestPolicy::Stop : TestPolicy::Change);
    }
    const auto patchDefaults = GetJson(R"({"target":"selected","expectedCatalogRevision":"current","name":null,"isVisible":null,"isSelected":true,"isReviewed":null,"opacity":null,"colorRGBA":null})");
    panel->AttachAction("SetState", patchDefaults, [panel, feature](auto id, const auto& params) {
        const auto catalog = feature->GetPartSetSnapshot();
        if (!catalog) throw std::invalid_argument("零件目录不存在");
        const auto target = GetPart(params["target"], *catalog);
        PartStatePatch patch;
        if (!params["name"].isNull()) patch.name = GetText(params, "name").toStdString();
        if (!params["isVisible"].isNull()) patch.isVisible = GetBool(params, "isVisible");
        if (!params["isSelected"].isNull()) patch.isSelected = GetBool(params, "isSelected");
        if (!params["isReviewed"].isNull()) patch.isReviewed = GetBool(params, "isReviewed");
        if (!params["opacity"].isNull()) patch.opacity = GetNumber(params, "opacity");
        if (!params["colorRGBA"].isNull()) patch.color = PartColorPatch{PartColorUse::Custom, GetArray<double, 4>(params["colorRGBA"])};
        const auto expected = GetText(params, "expectedCatalogRevision") == "current" ? catalog->catalogRevision : GetId(params["expectedCatalogRevision"]);
        const auto result = feature->SetPartState(target, patch, expected);
        panel->SetComplete(id, result.status == PartMutationStatus::Succeeded ? "Succeeded" : "Failed", {
            {"status", static_cast<int>(result.status)}, {"binding", GetPartRef(target)},
            {"expectedCatalogRevision", QString::number(expected)}, {"catalogRevision", QString::number(result.catalogRevision)}});
    });
    panel->AttachAction("Catalog", {}, [panel, feature](auto id, const auto&) { panel->SetComplete(id, "Observed", GetCatalog(*feature)); }, TestPolicy::Read);
    for (const auto& action : {QString("Highlight"), QString("ClearHighlight")}) {
        panel->AttachAction(action, {{"target", "selected"}}, [panel, feature, action](auto id, const auto& params) {
            const auto catalog = feature->GetPartSetSnapshot();
            if (!catalog || catalog->isStale) throw std::invalid_argument("请先生成有效零件目录");
            const auto target = GetPart(params["target"], *catalog);
            PartStatePatch patch; patch.isSelected = action == "Highlight";
            if (*patch.isSelected) patch.isVisible = true;
            const QPointer<ModulePanel> owner(panel);
            const auto apply = [owner, feature, id, target, patch, expected = catalog->catalogRevision] {
                if (!owner || owner->GetContext().workflow.GetIsClosing()) return;
                const auto result = feature->SetPartState(target, patch, expected);
                owner->SetComplete(id, result.status == PartMutationStatus::Succeeded ? "Succeeded" : "Failed", {
                    {"status", static_cast<int>(result.status)}, {"binding", GetPartRef(target)}, {"isSelected", *patch.isSelected},
                    {"catalogRevision", QString::number(result.catalogRevision)}});
            };
            if (*patch.isSelected && !feature->GetState().isOverlayVisible) {
                PartSegmentationRequest request; request.action = PartSegmentationAction::SetVisibility; request.isVisible = true;
                const auto admission = feature->SendRequest(std::move(request), [owner, id, apply](PartSegmentationResult result) {
                    if (!owner) return;
                    if (result.status == PartResultStatus::Succeeded) apply();
                    else owner->SetComplete(id, "Failed", GetPartResult(result));
                });
                panel->SetAdmission(id, admission.status == PartAdmissionStatus::Accepted,
                    {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
                return;
            }
            apply();
        });
    }
    panel->AttachAction("EditSelected", {{"target", "selected"}}, [panel, feature](auto id, const auto& p) {
        const auto catalog = feature->GetPartSetSnapshot();
        if (!catalog || catalog->isStale) throw std::invalid_argument("请先生成有效零件目录");
        const auto target = GetPart(p["target"], *catalog);
        // 目录在一次事务内清除旧选择并选中新目标，避免中途暴露零选择状态。
        PartStatePatch patch; patch.isSelected = true;
        const auto result = feature->SetPartState(target, patch, catalog->catalogRevision);
        if (result.status != PartMutationStatus::Succeeded) throw std::runtime_error("选择目标时目录已变化，请重新选择零件");
        if (panel->GetContext().workflow.onNavigate) panel->GetContext().workflow.onNavigate("PartEdit", "Paint", {{"target", "selected"}});
        panel->SetComplete(id, "ParametersCopied", {{"message", "已选择唯一编辑目标并进入零件编辑"}, {"binding", GetPartRef(target)}});
    });
    panel->onObserve = [panel, feature, preview] {
        auto summary = GetCatalog(*feature);
        const auto input = panel->GetSession()->GetImageDescriptor();
        summary["hasCurrentParts"] = summary["hasCurrentParts"].toBool() && input && GetRefText(input->dataRevision) == summary["source"].toString();
        const auto catalog = feature->GetPartSetSnapshot();
        auto* form = panel->GetParameterEditor("SetState");
        QJsonObject target; QString targetKey;
        if (catalog && summary["hasCurrentParts"].toBool()) try {
            const auto value = form->GetField("target")->GetValue();
            // 状态观察允许尚无唯一选择；只有提交操作才将缺少目标作为参数错误。
            const bool hasTarget = value != "selected" || std::count_if(catalog->parts.begin(), catalog->parts.end(),
                [](const auto& part) { return part.presentation.isSelected; }) == 1;
            if (hasTarget) {
                const auto binding = GetPart(value, *catalog); targetKey = GetJsonText(GetPartRef(binding));
                for (const auto& part : catalog->parts) if (part.binding == binding) target = {{"isVisible", part.presentation.isVisible}, {"isSelected", part.presentation.isSelected}, {"isReviewed", part.userState.isReviewed}};
            }
        } catch (const std::exception&) {}
        for (const auto* key : {"isVisible", "isSelected", "isReviewed"}) form->GetField(key)->SetAppliedBoolean(target.value(key), targetKey, target.isEmpty() ? "请选择当前零件" : QString());
        summary["isOverlayVisible"] = feature->GetState().isOverlayVisible;
        preview->Observe(panel, summary["hasCurrentParts"].toBool() && summary["isOverlayVisible"].toBool());
        summary["sourcePreviewReady"] = preview->GetReady();
        summary["sourcePreviewError"] = preview->GetError();
        panel->GetParameterEditor("Visibility")->GetField("isVisible")->SetAppliedBoolean(summary["hasCurrentParts"].toBool() ? summary["isOverlayVisible"] : QJsonValue(), summary["labelMap"].toString(), summary["hasCurrentParts"].toBool() ? QString() : "当前没有可用零件");
        panel->SetState(summary);
    };
    panel->onStop = [panel] { panel->SendAction("Stop", {}); };
    return panel;
}
}
