// 测试用途：通过对齐页面测试参考导入、方案保存、求解、结果应用与归档恢复。
#include "ModuleFactories.h"
#include "Support/ParameterEditor.h"
#include "AlignmentInput.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include <QPointer>
namespace Manual {
ModulePanel* CreateAlignmentTest(TestContext context, std::shared_ptr<MetrologyAlignmentHostFeature> feature,
    std::shared_ptr<ReferenceDataSource> dataSource, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Alignment", parent);
    panel->SetNotice("先选择当前源体，再导入名义参考 → 保存对齐方案 → 开始对齐 → 检查结果 → 应用对齐结果。模板须填写实测选区；接触关联尚未支持，最佳拟合需要明确的顶点对应。");
    struct State { std::optional<ReferenceInput> reference; DataRevisionRef recipe; DataRevisionRef result; bool isReviewed = false; };
    auto state = std::make_shared<State>();
    for (const auto& method : std::vector<std::pair<QString, AlignmentMethod>>{
        {"SequentialPlanes", AlignmentMethod::SequentialPlanes}, {"PlaneTwoHoles", AlignmentMethod::PlaneTwoHoles},
        {"Rps", AlignmentMethod::Rps}, {"ConstrainedBestFit", AlignmentMethod::ConstrainedBestFit}}) {
        panel->AttachAction("Export" + method.first + "Template", {{"outputPath", ""}, {"reference", GetReferenceTemplate(method.second)}},
            [panel](auto id, const auto& p) {
                ExportJson(GetText(p, "outputPath"), p["reference"].toObject());
                panel->SetComplete(id, "TemplateExported", {{"requiresInput", true}});
            }, TestPolicy::Read);
    }
    panel->AttachAction("ImportReference", {{"filePath", ""}, {"source", "current"}, {"mesh", "surface"}},
        [panel, dataSource, state](auto id, const auto& p) {
            const auto descriptor = panel->GetSession()->GetImageDescriptor();
            if (!descriptor) throw std::invalid_argument("需要源输入");
            const auto source = GetText(p, "source") == "current" ? descriptor->dataRevision : GetRef(p["source"]);
            // 本测试流程激活到当前主输入；历史体先经 Data.Select 显式切换。
            if (source != descriptor->dataRevision) throw std::invalid_argument("请先在“数据输入 → 选择当前输入”中选择参考源体");
            const auto mesh = GetText(p, "mesh") == "surface" ? panel->GetContext().workflow.GetSurfaceMesh() : GetRef(p["mesh"]);
            if (GetText(p, "mesh") == "surface" && panel->GetContext().workflow.GetSurfaceSource() != source)
                throw std::invalid_argument("已生成表面不属于选定源修订");
            state->reference = dataSource->LoadReference(GetText(p, "filePath"), source, mesh);
            state->recipe = {}; state->result = {}; state->isReviewed = false;
            panel->SetComplete(id, "ReferencePublished", {{"input", GetInputJson(GetAlignmentInput(*state->reference))},
                {"nominal", GetRefText(state->reference->nominal)}, {"evidenceKind", state->reference->document["evidenceKind"]},
                {"provenance", state->reference->document["provenance"]}, {"validationRoute", "trusted-input + public-feature"}});
        });
    const auto complete = [owner = QPointer<ModulePanel>(panel), state, feature](std::uint64_t id, AlignmentResult result) {
        if (!owner) return;
        const auto action = owner->GetContext().records.GetRecord(id)["action"].toString();
        if (result.status == AlignmentStatus::FullyDetermined && (action == "SaveRecipe" || action == "Restore")) {
            // 公开 Restore 恢复的是方案，必须重新求解，不能继续显示上一次结果为本方案结果。
            state->result = {}; state->isReviewed = false;
        }
        if (GetDataRevisionRefValid(result.recipe)) state->recipe = result.recipe;
        if (GetDataRevisionRefValid(result.result)) {
            if (state->result != result.result) state->isReviewed = false;
            state->result = result.result;
        }
        auto summary = GetAlignmentResult(result);
        if (const auto archive = feature->GetArchive(result.result)) summary["sourceToTarget"] = GetValues(archive->sourceToTarget);
        const auto status = result.status == AlignmentStatus::FullyDetermined ? "FullyDetermined"
            : result.status == AlignmentStatus::Cancelled ? "Cancelled"
            : result.status == AlignmentStatus::Underconstrained ? "Underconstrained"
            : result.status == AlignmentStatus::Degenerate ? "Degenerate"
            : result.status == AlignmentStatus::Conflicting ? "Conflicting"
            : result.status == AlignmentStatus::Stale ? "Stale" : "Failed";
        owner->SetComplete(id, status, summary);
    };
    panel->AttachAction("SaveRecipe", {{"recipe", QJsonValue()}}, [panel, state, feature, complete](auto id, const auto& p) {
        if (!state->reference) throw std::invalid_argument("请先导入参考文件");
        const auto json = p["recipe"].isNull() ? state->reference->document["recipe"].toObject() : p["recipe"].toObject();
        AlignmentRequest request; request.action = AlignmentAction::SaveRecipe;
        request.recipe = GetRecipe(json, state->reference->mesh, state->reference->nominal);
        if (request.recipe->unit != GetAlignmentInput(*state->reference).unit) throw std::invalid_argument("配方与参考单位不一致");
        panel->GetContext().records.SetAdmission(id, {{"resolvedRecipe", GetRecipeJson(*request.recipe)}});
        const auto admission = feature->SendRequest(std::move(request), [complete, id](auto result) { complete(id, result); });
        panel->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
            {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
    });
    panel->AttachAction("Start", {{"initialPoses", QJsonArray{QJsonValue(GetValues(alignmentIdentity))}}},
        [panel, feature, state, complete](auto id, const auto& p) {
            if (!state->reference || !GetDataRevisionRefValid(state->recipe)) throw std::invalid_argument("需要已保存配方和有效输入");
            AlignmentRequest request; request.action = AlignmentAction::Start; request.recipeRef = state->recipe;
            request.input = GetAlignmentInput(*state->reference); request.isActivationRequested = false;
            request.initialPoses.clear();
            for (const auto pose : p["initialPoses"].toArray()) request.initialPoses.push_back(GetArray<double, 16>(pose));
            const auto admission = feature->SendRequest(std::move(request), [complete, id](auto result) { complete(id, result); });
            panel->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        }, TestPolicy::Compute, true);
    for (const auto& action : std::vector<std::pair<QString, AlignmentAction>>{{"Cancel", AlignmentAction::Cancel},
        {"Activate", AlignmentAction::Activate}, {"Deactivate", AlignmentAction::Deactivate}, {"Visibility", AlignmentAction::SetVisibility}}) {
        QJsonObject defaults;
        if (action.second == AlignmentAction::Cancel) defaults["targetRequestId"] = "0";
        if (action.second == AlignmentAction::Activate) defaults["result"] = "current";
        if (action.second == AlignmentAction::SetVisibility) defaults["isVisible"] = true;
        panel->AttachAction(action.first, defaults, [panel, feature, state, complete, action](auto id, const auto& p) {
            AlignmentRequest request; request.action = action.second;
            if (request.action == AlignmentAction::Cancel) request.targetRequestId = GetId(p["targetRequestId"]);
            if (request.action == AlignmentAction::Activate) request.resultRef = GetText(p, "result") == "current" ? state->result : GetRef(p["result"]);
            if (request.action == AlignmentAction::Deactivate) {
                if (!state->reference) throw std::invalid_argument("没有当前作用域的参考输入");
                request.input = GetAlignmentInput(*state->reference);
            }
            if (request.action == AlignmentAction::SetVisibility) request.isVisible = GetBool(p, "isVisible");
            const auto admission = feature->SendRequest(std::move(request), [complete, id](auto result) { complete(id, result); });
            panel->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        }, action.second == AlignmentAction::Cancel ? TestPolicy::Stop : TestPolicy::Change);
    }
    panel->AttachAction("ExportArchive", {{"outputPath", ""}, {"result", "current"}}, [panel, feature, state](auto id, const auto& p) {
        const auto result = GetText(p, "result") == "current" ? state->result : GetRef(p["result"]);
        const auto archive = feature->GetArchive(result);
        if (!archive) throw std::invalid_argument("归档不存在");
        const QJsonObject json{{"schemaVersion", static_cast<int>(archive->schemaVersion)},
            {"algorithmVersion", QString::fromStdString(archive->algorithmVersion)}, {"recipe", GetRecipeJson(archive->recipe)},
            {"input", GetInputJson(archive->input)}, {"sourceToTarget", GetValues(archive->sourceToTarget)}};
        ExportJson(GetText(p, "outputPath"), json);
        panel->SetComplete(id, "ArchiveExported", json);
    }, TestPolicy::Read);
    panel->AttachAction("Restore", {{"archivePath", ""}}, [panel, state, feature, complete](auto id, const auto& p) {
        if (!state->reference) throw std::invalid_argument("恢复归档前必须导入本次映射参考");
        const auto json = LoadJson(GetText(p, "archivePath"));
        AlignmentArchive archive;
        if (GetNumber(json, "schemaVersion") != 1 || GetText(json, "algorithmVersion") != "metrology-alignment-1")
            throw std::invalid_argument("归档版本不支持");
        archive.recipe = GetRecipe(json["recipe"].toObject(), {}, state->reference->nominal);
        archive.sourceToTarget = GetArray<double, 16>(json["sourceToTarget"]);
        AlignmentRequest request; request.action = AlignmentAction::Restore; request.archive = archive;
        request.input = GetAlignmentInput(*state->reference); request.restoredNominal = state->reference->nominal;
        const auto admission = feature->SendRequest(std::move(request), [complete, id](auto result) { complete(id, result); });
        panel->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
            {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
    });
    panel->AttachAction("Result", {{"result", "current"}}, [panel, feature, state](auto id, const auto& p) {
        const auto ref = GetText(p, "result") == "current" ? state->result : GetRef(p["result"]);
        const auto result = feature->GetResult(ref);
        const auto archive = feature->GetArchive(ref);
        if (!result) throw std::invalid_argument("结果不存在");
        if (ref == state->result) state->isReviewed = true;
        QJsonArray residuals, geometries;
        for (const auto& r : result->residuals) residuals.append(QJsonObject{{"id", QString::fromStdString(r.id)},
            {"value", r.value}, {"tolerance", r.tolerance}, {"kind", static_cast<int>(r.kind)}, {"isUsed", r.isUsed}});
        for (const auto& g : result->geometries) geometries.append(QJsonObject{{"id", QString::fromStdString(g.id)},
            {"kind", static_cast<int>(g.kind)}, {"sourceCenter", GetValues(g.sourceCenter)}, {"sourceDirection", GetValues(g.sourceDirection)},
            {"radius", g.radius}, {"fitRms", g.fitRms}, {"fitMax", g.fitMax}, {"sampleCount", QString::number(g.sampleCount)}});
        panel->SetComplete(id, "Observed", {{"result", GetRefText(ref)}, {"isCurrent", result->isCurrent}, {"residuals", residuals}, {"geometries", geometries},
            {"sourceToTarget", archive ? QJsonValue(GetValues(archive->sourceToTarget)) : QJsonValue()}});
    }, TestPolicy::Read);
    panel->onObserve = [panel, feature, state] {
        const auto current = feature->GetState();
        const auto input = panel->GetSession()->GetImageDescriptor();
        const bool referenceCurrent = input && state->reference && GetAlignmentInput(*state->reference).source == input->dataRevision;
        const auto result = feature->GetResult(state->result);
        const bool active = referenceCurrent && current.isCurrent && current.activeResult == state->result;
        const bool displayAvailable = active && (!current.isOverlayVisible || current.isDisplayReady);
        panel->GetParameterEditor("Visibility")->GetField("isVisible")->SetAppliedBoolean(displayAvailable ? QJsonValue(current.isOverlayVisible) : QJsonValue(), GetRefText(current.activeResult),
            displayAvailable ? QString() : active ? "叠加未就绪，请重新应用结果" : "请先应用当前对齐结果");
        panel->SetState({{"isBusy", current.isBusy}, {"isCurrent", current.isCurrent}, {"isDisplayReady", current.isDisplayReady},
            {"isOverlayVisible", current.isOverlayVisible},
            {"hasReference", state->reference.has_value()}, {"isReferenceCurrent", referenceCurrent},
            {"nominal", state->reference ? GetRefText(state->reference->nominal) : QString()},
            {"hasRecipe", GetDataRevisionRefValid(state->recipe)}, {"hasResult", GetDataRevisionRefValid(state->result)},
            {"isResultCurrent", referenceCurrent && result && result->isCurrent}, {"isReviewed", state->isReviewed},
            {"isApplied", current.isCurrent && current.activeResult == state->result},
            {"lastStatus", static_cast<int>(current.lastStatus)}, {"activeResult", GetRefText(current.activeResult)},
            {"savedRecipe", GetRefText(state->recipe)}, {"lastResult", GetRefText(state->result)}});
    };
    panel->onStop = [panel] { panel->SendAction("Cancel", {{"targetRequestId", "0"}}); };
    return panel;
}
}
