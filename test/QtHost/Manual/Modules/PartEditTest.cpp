// 测试用途：通过编辑页面测试标签候选、确认、丢弃、撤销重做及编辑约束。
#include "ModuleFactories.h"
#include "PartInput.h"
#include "Support/ReferenceDataSource.h"
#include <QPointer>
#include <type_traits>
namespace Manual {
ModulePanel* CreatePartEditTest(TestContext context, std::shared_ptr<PartSegmentationHostFeature> feature, QWidget* parent)
{
    auto* panel = new ModulePanel(context, "PartEdit", parent);
    panel->SetNotice("种子使用源网格体素索引，笔刷点和半径以毫米计。高亮零件、编辑对象和候选在顶部单列，可按名称或标签检索。单次编辑的 120 秒预算包含表面重建及标签冻结；候选需确认后才替换正式结果。");
    const auto complete = [owner = QPointer<ModulePanel>(panel), feature](std::uint64_t id, PartSegmentationResult result) {
        if (!owner) return;
        auto summary = GetPartResult(result);
        if (result.status == PartResultStatus::PreviewReady) {
            const auto preview = feature->GetEditPreview();
            if (preview) summary["previewId"] = QString::number(preview->previewId);
            if (preview && preview->parts) if (const auto current = feature->GetPartSetSnapshot()) {
                QJsonArray outputs;
                for (const auto value : GetPreviewChanges(*current, *preview->parts)["changed"].toArray()) outputs.append(value.toObject()["binding"]);
                owner->GetContext().workflow.partEditContext["candidateTargets"] = outputs;
            }
        }
        auto& editContext = owner->GetContext().workflow.partEditContext;
        const bool unchanged = result.failureReason == PartFailureReason::NoChange;
        editContext["status"] = result.status == PartResultStatus::PreviewReady ? "候选待确认"
            : result.status == PartResultStatus::Succeeded ? "已完成" : unchanged ? "无标签变化" : "本次编辑未完成";
        if (result.status == PartResultStatus::Succeeded) if (const auto catalog = feature->GetPartSetSnapshot()) {
            if (owner->GetContext().records.GetRecord(id)["action"] == "Commit")
                editContext["targets"] = editContext["candidateTargets"];
            const auto currentParts = GetEditingParts(editContext, *catalog); QJsonArray targets;
            for (const auto value : currentParts) targets.append(value.toObject()["binding"]);
            editContext["targets"] = targets;
            editContext.remove("candidateTargets");
        }
        if (unchanged) summary["message"] = "没有标签发生变化：区域可能已属于目标零件，或没有符合操作条件的体素。正式结果保持不变。";
        owner->SetComplete(id, unchanged ? "Unchanged" : result.status == PartResultStatus::PreviewReady ? "PreviewReady"
            : result.status == PartResultStatus::Succeeded ? "Succeeded"
            : result.status == PartResultStatus::SucceededWithDisplayFailure ? "SucceededWithDisplayFailure"
            : result.status == PartResultStatus::Cancelled ? "Cancelled" : "Failed", summary);
        owner->Observe();
        owner->SelectNodeGroup(result.status == PartResultStatus::PreviewReady
            ? "edit-preview:" + summary["previewId"].toString() : "part-edit-targets");
    };
    for (const QString operation : {QString("Paint"), QString("Erase"), QString("Fill"), QString("Island"),
        QString("Grow"), QString("Split"), QString("Merge"), QString("Undo"), QString("Redo")}) {
        auto defaults = GetJson(R"({"target":"selected","expectedLabelMap":"current","expectedCatalogRevision":"current","extent":null,"roiMask":null,"protectionMask":null,"protectedParts":[]})");
        if (operation == "Undo" || operation == "Redo") defaults = {{"expectedLabelMap", "current"}, {"expectedCatalogRevision", "current"}};
        if (operation == "Merge") defaults.remove("target");
        if (operation == "Paint" || operation == "Erase") {
            defaults["sourcePointsMM"] = QJsonArray{QJsonValue(QJsonArray{0, 0, 0})}; defaults["radiusMM"] = 1.0;
            defaults["slice"] = QJsonValue(); defaults["overwriteParts"] = QJsonArray{}; defaults["isBackgroundAllowed"] = true;
        } else if (operation == "Fill") defaults["seed"] = QJsonArray{0, 0, 0};
        else if (operation == "Island") defaults["minIslandVoxels"] = "1";
        else if (operation == "Grow") {
            defaults["seeds"] = QJsonArray{QJsonValue(QJsonArray{0, 0, 0})}; defaults["minimum"] = 0.0; defaults["maximum"] = 1.0;
            defaults["overwriteParts"] = QJsonArray{}; defaults["isBackgroundAllowed"] = true;
        } else if (operation == "Split") {
            defaults["seeds"] = GetJson(R"({"value":[{"imageIndex":[0,0,0],"target":"1"},{"imageIndex":[1,1,1],"target":"2"}]})")["value"];
            defaults["barriers"] = QJsonArray{};
        } else if (operation == "Merge") defaults["parts"] = QJsonArray{};
        panel->AttachAction(operation, defaults, [panel, feature, complete, operation](auto id, const auto& p) {
            const auto catalog = feature->GetPartSetSnapshot();
            if (!catalog) throw std::invalid_argument("需要有效零件目录");
            PartEditRequest request;
            request.expectedLabelMap = GetText(p, "expectedLabelMap") == "current" ? feature->GetState().labelMap : GetRef(p["expectedLabelMap"]);
            request.expectedCatalogRevision = GetText(p, "expectedCatalogRevision") == "current" ? catalog->catalogRevision : GetId(p["expectedCatalogRevision"]);
            if (operation != "Undo" && operation != "Redo") {
                request.scope.editRoi = CreateInputRoi(*panel->GetSession(), feature->GetState().sourceRevision,
                    p["extent"], p["roiMask"], "Part edit region");
                request.scope.protectionRoi = CreateInputRoi(*panel->GetSession(), feature->GetState().sourceRevision,
                    QJsonValue(), p["protectionMask"], "Part protection region");
                request.scope.protectedParts = GetParts(p["protectedParts"], *catalog);
            }
            if (operation == "Paint" || operation == "Erase") {
                PartBrushEdit edit; edit.target = GetPart(p["target"], *catalog); edit.isErase = operation == "Erase";
                edit.radiusMM = GetNumber(p, "radiusMM");
                for (const auto point : p["sourcePointsMM"].toArray()) edit.sourcePoints.push_back(GetArray<double, 3>(point));
                if (!p["slice"].isNull()) {
                    const auto slice = p["slice"].toObject();
                    edit.slice = PartBrushPlane{GetArray<double, 3>(slice["origin"]), GetArray<double, 3>(slice["normal"]), GetNumber(slice, "thicknessMM")};
                }
                edit.overwriteParts = GetParts(p["overwriteParts"], *catalog); edit.isBackgroundAllowed = GetBool(p, "isBackgroundAllowed");
                request.operation = edit;
            } else if (operation == "Fill") request.operation = PartFillEdit{GetPart(p["target"], *catalog), GetArray<int, 3>(p["seed"])};
            else if (operation == "Island") request.operation = PartIslandEdit{GetPart(p["target"], *catalog), GetId(p["minIslandVoxels"])};
            else if (operation == "Grow") {
                PartGrowEdit edit; edit.target = GetPart(p["target"], *catalog);
                for (const auto seed : p["seeds"].toArray()) edit.seeds.push_back(GetArray<int, 3>(seed));
                edit.minimum = GetNumber(p, "minimum"); edit.maximum = GetNumber(p, "maximum");
                edit.overwriteParts = GetParts(p["overwriteParts"], *catalog); edit.isBackgroundAllowed = GetBool(p, "isBackgroundAllowed");
                request.operation = edit;
            } else if (operation == "Split") {
                PartSplitEdit edit; edit.target = GetPart(p["target"], *catalog);
                for (const auto seed : p["seeds"].toArray()) {
                    const auto value = seed.toObject(); const auto target = GetId(value["target"]);
                    if (target > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("拆分目标编号越界");
                    edit.seeds.push_back({GetArray<int, 3>(value["imageIndex"]), static_cast<std::uint32_t>(target)});
                }
                for (const auto barrier : p["barriers"].toArray()) {
                    const auto value = barrier.toObject(); const auto axis = GetNumber(value, "axis");
                    if (axis < 0 || axis > 2 || std::trunc(axis) != axis) throw std::invalid_argument("屏障轴必须为 0、1 或 2");
                    edit.barriers.push_back({GetArray<int, 3>(value["imageIndex"]), static_cast<std::uint8_t>(axis)});
                }
                request.operation = edit;
            } else if (operation == "Merge") request.operation = PartMergeEdit{GetParts(p["parts"], *catalog)};
            else request.operation = PartHistoryEdit{operation == "Redo"};
            panel->GetContext().records.SetAdmission(id, {{"expectedLabelMap", GetRefText(request.expectedLabelMap)},
                {"expectedCatalogRevision", QString::number(request.expectedCatalogRevision)}});
            QJsonArray targets;
            std::visit([&](const auto& edit) {
                using Edit = std::decay_t<decltype(edit)>;
                if constexpr (std::is_same_v<Edit, PartMergeEdit>) for (const auto& part : edit.parts) targets.append(GetPartRef(part));
                else if constexpr (!std::is_same_v<Edit, PartHistoryEdit>) targets.append(GetPartRef(edit.target));
            }, request.operation);
            const auto admission = feature->SendEditRequest(std::move(request), [complete, id](auto result) { complete(id, result); });
            if (admission.status == PartAdmissionStatus::Accepted && !targets.isEmpty())
                panel->GetContext().workflow.partEditContext = {{"source", GetRefText(catalog->sourceRevision)},
                    {"targets", targets}, {"status", "正在计算"}, {"operation", operation}};
            else if (admission.status == PartAdmissionStatus::Accepted)
                panel->GetContext().workflow.partEditContext["status"] = "正在计算";
            panel->SetAdmission(id, admission.status == PartAdmissionStatus::Accepted,
                {{"status", static_cast<int>(admission.status)}, {"requestId", QString::number(admission.requestId)}});
        }, TestPolicy::Compute, true);
    }
    for (const bool commit : {true, false}) panel->AttachAction(commit ? "Commit" : "Discard", {{"previewId", "current"}},
        [panel, feature, complete, commit](auto id, const auto& p) {
            const auto preview = feature->GetEditPreview();
            const auto previewId = GetText(p, "previewId") == "current" ? (preview ? preview->previewId : 0) : GetId(p["previewId"]);
            if (commit) {
                const auto admission = feature->SetEditCommit(previewId, [complete, id](auto result) { complete(id, result); });
                panel->SetAdmission(id, admission.status == PartAdmissionStatus::Accepted,
                    {{"status", static_cast<int>(admission.status)}, {"previewId", QString::number(previewId)}});
            } else {
                const auto result = feature->ClearEditPreview(previewId);
                panel->SetComplete(id, result.status == PartMutationStatus::Succeeded ? "Discarded" : "Failed", {{"status", static_cast<int>(result.status)}});
                if (result.status == PartMutationStatus::Succeeded) {
                    panel->GetContext().workflow.partEditContext["status"] = "候选已丢弃";
                    panel->GetContext().workflow.partEditContext.remove("candidateTargets");
                }
                panel->Observe(); panel->SelectNodeGroup("part-edit-targets");
            }
        });
    panel->onObserve = [panel, feature] {
        const auto preview = feature->GetEditPreview();
        const auto catalog = feature->GetPartSetSnapshot();
        const auto input = panel->GetSession()->GetImageDescriptor();
        const auto current = feature->GetState();
        QJsonObject summary{{"hasPreview", preview && input && preview->sourceRevision == input->dataRevision}, {"formalLabelMap", GetRefText(feature->GetState().labelMap)}};
        const auto detail = GetCatalog(*feature);
        summary["parts"] = detail["parts"]; summary["relations"] = detail["relations"];
        summary["source"] = detail["source"]; summary["isOverlayVisible"] = current.isOverlayVisible;
        summary["editingParts"] = catalog ? GetEditingParts(panel->GetContext().workflow.partEditContext, *catalog) : QJsonArray{};
        summary["editingStatus"] = panel->GetContext().workflow.partEditContext["status"];
        summary["hasCurrentParts"] = catalog && !catalog->isStale && !catalog->parts.empty() && input && catalog->sourceRevision == input->dataRevision;
        summary["isBusy"] = current.status == PartSegmentationStatus::Running || current.status == PartSegmentationStatus::Stopping || current.status == PartSegmentationStatus::Committing;
        summary["requestId"] = QString::number(current.requestId); summary["progress"] = current.progress;
        if (summary["hasPreview"].toBool()) {
            summary["previewId"] = QString::number(preview->previewId);
            summary["source"] = GetRefText(preview->sourceRevision);
            summary["baseLabels"] = GetRefText(preview->baseLabels);
            summary["candidatePartCount"] = QString::number(preview->parts ? preview->parts->parts.size() : 0);
            if (preview->parts && catalog) summary["previewChanges"] = GetPreviewChanges(*catalog, *preview->parts);
        }
        panel->SetState(summary);
    };
    panel->onStop = [feature] { PartSegmentationRequest request; request.action = PartSegmentationAction::Stop; feature->SendRequest(std::move(request)); };
    return panel;
}
}
