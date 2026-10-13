// 测试用途：通过对齐页面测试参考导入、方案保存、求解、结果应用与归档恢复。
#include "ModuleFactories.h"
#include "Support/ParameterEditor.h"
#include "Support/RoiSelection.h"
#include "AlignmentInput.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include <QPointer>
#include <algorithm>
namespace Manual {
ModulePanel* CreateAlignmentTest(TestContext context, std::shared_ptr<MetrologyAlignmentHostFeature> feature,
    std::shared_ptr<ReferenceDataSource> dataSource, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "Alignment", parent);
    panel->SetNotice("有效测量表面 → 导入参考 → 保存方案 → 对齐 → 检测 / 偏差 / 报告。检测选区独立于配准选区；截面使用目标坐标系。未提供名义值时只报告实测，不判合格。模型姿态只改变显示，不重采样。");
    struct State { std::optional<ReferenceInput> reference; DataRevisionRef recipe; DataRevisionRef result; QJsonObject savedParameters,startParameters; bool isReviewed = false; };
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
            const auto surface = panel->GetContext().workflow.GetMeasurementSurface();
            const auto mesh = GetText(p, "mesh") == "surface" ? (surface ? surface->mesh : DataRevisionRef{}) : GetRef(p["mesh"]);
            if (GetText(p, "mesh") == "surface" && (!surface || surface->source != source))
                throw std::invalid_argument("已生成表面不属于选定源修订");
            state->reference = dataSource->LoadReference(GetText(p, "filePath"), source, mesh);
            state->recipe = {}; state->result = {}; state->savedParameters={};state->startParameters={};state->isReviewed = false;
            panel->SetComplete(id, "ReferencePublished", {{"input", GetInputJson(GetAlignmentInput(*state->reference))},
                {"recipe", state->reference->document["recipe"]},
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
            : result.status == AlignmentStatus::NotConverged ? "NotConverged"
            : result.status == AlignmentStatus::Stale ? "Stale" : "Failed";
        owner->SetComplete(id, status, summary);
    };
    panel->AttachAction("ImportNominalMesh", GetJson(R"({"filePath":"","frameId":"nominal-frame","coordinateFrame":"RAS","fileUnit":"Millimeter","unit":"Millimeter"})"),
        [panel, state, feature, complete](auto id, const auto& p) {
            if (!state->reference) throw std::invalid_argument("先导入参考方案，再导入其名义网格");
            AlignmentReferenceRequest reference; reference.filePath = GetText(p, "filePath").toUtf8().toStdString();
            reference.frameId = GetText(p, "frameId").toStdString(); reference.coordinateFrame = GetText(p, "coordinateFrame").toStdString();
            const auto unit = [&](const char* key) { return GetEnum<AlignmentUnit>(p, key, {{"ModelUnit", AlignmentUnit::ModelUnit}, {"Millimeter", AlignmentUnit::Millimeter}, {"Meter", AlignmentUnit::Meter}}); };
            reference.fileUnit = unit("fileUnit"); reference.unit = unit("unit");
            if (reference.unit != GetAlignmentInput(*state->reference).unit || reference.coordinateFrame != state->reference->coordinateFrame.toStdString())
                throw std::invalid_argument("名义网格与参考方案的单位/坐标约定不一致");
            AlignmentRequest request; request.action = AlignmentAction::ImportReference; request.reference = reference;
            const auto admission = feature->SendRequest(std::move(request), [state, complete, id](auto result) {
                if (result.status == AlignmentStatus::FullyDetermined && GetDataRevisionRefValid(result.referenceMesh)) {
                    state->reference->nominal = result.referenceMesh; state->recipe = {}; state->result = {};state->savedParameters={};state->startParameters={}; state->isReviewed = false;
                }
                complete(id, result);
            });
            panel->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        }, TestPolicy::Change, true);
    panel->AttachAction("SaveRecipe", {{"recipe", QJsonValue()},{"inputRoi",""}}, [panel, state, feature, complete](auto id, const auto& p) {
        if (!state->reference) throw std::invalid_argument("请先导入参考文件");
        const auto json = p["recipe"].isNull() ? state->reference->document["recipe"].toObject() : p["recipe"].toObject();
        AlignmentRequest request; request.action = AlignmentAction::SaveRecipe;
        request.recipe = GetRecipe(json, state->reference->mesh, state->reference->nominal);
        const QJsonObject normalized{{"recipe",GetRecipeJson(*request.recipe)},{"inputRoi",p["inputRoi"]}};
        const auto input=GetAlignmentInput(*state->reference);
        if (const auto roi=GetSelectedRoi(*panel->GetSession(),p["inputRoi"],input.source)) {
            const auto vertices=panel->GetContext().workflow.getRoiVertices(*roi,input.source,input.mesh);
            for(auto& geometry:request.recipe->geometries)SetRegionRoi(geometry.region,vertices,input.mesh);
            if(request.recipe->surfaceFit) {
                if(!request.recipe->surfaceFit->sourceRegion)request.recipe->surfaceFit->sourceRegion=AlignmentRegion{};
                SetRegionRoi(*request.recipe->surfaceFit->sourceRegion,vertices,input.mesh);
            }
            if(!request.recipe->fitPairs.empty()) {
                auto& pairs=request.recipe->fitPairs;
                pairs.erase(std::remove_if(pairs.begin(),pairs.end(),[&](const auto& pair){
                    return !std::binary_search(vertices.begin(),vertices.end(),pair.vertexId);
                }),pairs.end());
                if(pairs.empty() && !request.recipe->surfaceFit)throw std::invalid_argument("区域内没有配准对应点");
            }
            panel->GetContext().records.SetAdmission(id,{{"inputRoi",GetRefText(*roi)}});
        }
        if (request.recipe->unit != GetAlignmentInput(*state->reference).unit) throw std::invalid_argument("配方与参考单位不一致");
        panel->GetContext().records.SetAdmission(id, {{"resolvedRecipe", GetRecipeJson(*request.recipe)}});
        const auto admission = feature->SendRequest(std::move(request), [complete, id,state,normalized](auto result) {if(result.status==AlignmentStatus::FullyDetermined)state->savedParameters=normalized; complete(id, result); });
        panel->SetAdmission(id, admission.status == AlignmentAdmissionStatus::Accepted,
            {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
    });
    panel->AttachAction("Start", {{"initialPoses", QJsonArray{QJsonValue(GetValues(alignmentIdentity))}}},
        [panel, feature, state, complete](auto id, const auto& p) {
            if (!state->reference || !GetDataRevisionRefValid(state->recipe)) throw std::invalid_argument("需要已保存配方和有效输入");
            AlignmentRequest request; request.action = AlignmentAction::Start; request.recipeRef = state->recipe;
            request.input = GetAlignmentInput(*state->reference); request.isActivationRequested = false;
            request.initialPoses.clear();
            for (const auto pose : p["initialPoses"].toArray()) request.initialPoses.push_back(GetAffineMatrix(pose, true));
            QJsonArray poses;for(const auto& pose:request.initialPoses)poses.append(GetValues(pose));const QJsonObject normalized{{"initialPoses",poses}};
            const auto admission = feature->SendRequest(std::move(request), [complete, id,state,normalized](auto result) {if(result.status==AlignmentStatus::FullyDetermined)state->startParameters=normalized; complete(id, result); });
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
                const auto active = feature->GetArchive(feature->GetState().activeResult);
                if (active) request.input = active->input;
                else if (state->reference) request.input = GetAlignmentInput(*state->reference);
                else throw std::invalid_argument("没有可停用的对齐结果输入");
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
        if (GetNumber(json, "schemaVersion") != archive.schemaVersion ||
            GetText(json, "algorithmVersion").toStdString() != archive.algorithmVersion)
            throw std::invalid_argument("归档版本不支持");
        archive.recipe = GetRecipe(json["recipe"].toObject(), {}, state->reference->nominal);
        archive.sourceToTarget = GetAffineMatrix(json["sourceToTarget"], true);
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
        DataRevisionRef desiredSource,desiredMesh;
        try {const auto target=panel->GetActionParameters("ImportReference");desiredSource=target["source"]=="current" && input?input->dataRevision:GetRef(target["source"]);
            const auto surface=panel->GetContext().workflow.GetMeasurementSurface();desiredMesh=target["mesh"]=="surface" ? surface?surface->mesh:DataRevisionRef{} : GetRef(target["mesh"]);
        }catch(const std::exception&){}
        const bool referenceCurrent = input && state->reference && state->reference->source==desiredSource && state->reference->mesh==desiredMesh && desiredSource==input->dataRevision;
        bool recipeChanged=false,poseChanged=false;
        if(referenceCurrent)try {const QJsonValue recipe=panel->GetActionParameters("SaveRecipe")["recipe"];const auto json=recipe.isNull()?state->reference->document["recipe"].toObject():recipe.toObject();recipeChanged=!state->savedParameters.isEmpty() && QJsonObject{{"recipe",GetRecipeJson(GetRecipe(json,state->reference->mesh,state->reference->nominal))},{"inputRoi",panel->GetActionParameters("SaveRecipe")["inputRoi"]}}!=state->savedParameters;QJsonArray poses;for(const auto pose:panel->GetActionParameters("Start")["initialPoses"].toArray())poses.append(GetValues(GetAffineMatrix(pose,true)));poseChanged=!state->startParameters.isEmpty() && QJsonObject{{"initialPoses",poses}}!=state->startParameters;}
        catch(const std::exception&){recipeChanged=true;}
        ParameterChoices results;
        const auto graph = panel->GetContext().workflow.getPublishedGraph ? panel->GetContext().workflow.getPublishedGraph() : QJsonObject();
        for (const auto value : graph["nodes"].toArray()) {
            const auto node = value.toObject();
            if (node["producer"] != "metrology-alignment") continue;
            const auto ref = GetRef(node["ref"]);
            if (feature->GetResult(ref)) results.append({GetRefText(ref),"对齐结果 · #" + node["order"].toString()});
        }
        QJsonObject selectedResults;
        for (const auto* action : {"Result","Activate","ExportArchive","InspectSection","InspectGeometry","EvaluateSurface","ApplyModelPose","SetSceneDisplay","ExportReport"})
            if (auto* form = panel->GetParameterEditor(action))
                if (auto* field = form->GetField("result")) {
                    field->SetReferenceChoices(results);
                    // 报告也可以仅包含独立体素体积等结果；完整请求由报告入口校验。
                    if (QString(action) == "ExportReport") continue;
                    const auto chosen = field->GetValue();
                    try { selectedResults[action] = feature->GetResult(chosen == "current" ? state->result : GetRef(chosen)).has_value(); }
                    catch (const std::invalid_argument&) { selectedResults[action] = false; }
                }
        const auto choice = panel->GetParameterEditor("Activate")->GetField("result")->GetValue();
        DataRevisionRef selectedRef;
        try { selectedRef = choice == "current" ? state->result : GetRef(choice); } catch (const std::invalid_argument&) {}
        const auto result = feature->GetResult(selectedRef);
        const bool active = current.isCurrent && GetDataRevisionRefValid(current.activeResult);
        const bool displayAvailable = active && (!current.isOverlayVisible || current.isDisplayReady);
        panel->GetParameterEditor("Visibility")->GetField("isVisible")->SetAppliedBoolean(displayAvailable ? QJsonValue(current.isOverlayVisible) : QJsonValue(), GetRefText(current.activeResult),
            displayAvailable ? QString() : active ? "叠加未就绪，请重新应用结果" : "请先应用当前对齐结果");
        panel->SetState({{"isBusy", current.isBusy}, {"isCurrent", current.isCurrent}, {"isDisplayReady", current.isDisplayReady},
            {"isOverlayVisible", current.isOverlayVisible},
            {"hasReference", state->reference.has_value()}, {"isReferenceCurrent", referenceCurrent},
            {"nominal", state->reference ? GetRefText(state->reference->nominal) : QString()},
            {"hasRecipe", GetDataRevisionRefValid(state->recipe) && referenceCurrent}, {"hasResult", GetDataRevisionRefValid(state->result) && referenceCurrent},
            {"recipeParametersChanged",recipeChanged},{"poseParametersChanged",poseChanged},{"targetSource",GetDataRevisionRefValid(desiredSource)?GetRefText(desiredSource):QString()},{"targetMesh",GetDataRevisionRefValid(desiredMesh)?GetRefText(desiredMesh):QString()},
            {"hasReadableResult", !results.isEmpty()},
            {"selectedResults", selectedResults},
            {"isResultCurrent", result && result->isCurrent}, {"isReviewed", state->isReviewed},
            {"isApplied", active},
            {"lastStatus", static_cast<int>(current.lastStatus)}, {"activeResult", GetRefText(current.activeResult)},
            {"savedRecipe", GetRefText(state->recipe)}, {"lastResult", GetRefText(state->result)}});
    };
    AttachInspectionActions(panel, feature, [state] { return state->result; });
    panel->onStop = [panel] { panel->SendAction("Cancel", {{"targetRequestId", "0"}}); };
    return panel;
}
}
