// 测试用途：显示输入、视图、裁剪历史、零件、候选、网格及对齐对象，供按钮选择准确目标。
#include "SceneNodes.h"
#include "UiText.h"
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <queue>
#include <algorithm>
namespace Manual {
namespace {
QJsonObject Node(QString id, QString title, QString status, QStringList actions = {}, QJsonObject patches = {}, QJsonArray children = {})
{
    return {{"id", id}, {"title", title}, {"status", status}, {"actions", QJsonArray::fromStringList(actions)}, {"patches", patches}, {"children", children}};
}
QString Ref(const QJsonValue& value) { return value.toString().endsWith(":0") ? QString() : value.toString(); }
QString Values(const QJsonValue& value)
{
    QStringList texts; for (const auto number : value.toArray()) texts.append(QString::number(number.toDouble(), 'g', 7));
    return texts.join(" × ");
}
QJsonArray Quality(const QJsonObject& quality)
{
    return {Node("quality-change", "变化体素", quality["changedCount"].toString()), Node("quality-mean", "平均灰度变化", QString::number(quality["meanDelta"].toDouble(), 'g', 6)),
        Node("quality-rms", "灰度变化 RMS", QString::number(quality["rmsDelta"].toDouble(), 'g', 6)), Node("quality-fidelity", "校正保真", quality["fidelityVerified"].toBool() ? "已核验" : "待核验")};
}
QJsonObject PartNode(const QJsonObject& part, bool current, bool edit, const QString& prefix = "part:")
{
    const auto binding = part["binding"].toObject();
    const auto id = QString::fromUtf8(QJsonDocument(binding).toJson(QJsonDocument::Compact));
    QJsonObject patches;
    const QStringList actions = edit ? QStringList{"Paint", "Erase", "Fill", "Island", "Grow", "Split", "Merge", "Undo", "Redo"} : QStringList{"Highlight", "ClearHighlight", "EditSelected", "SetState", "Catalog"};
    const QStringList targeted = edit ? QStringList{"Paint", "Erase", "Fill", "Island", "Grow", "Split"} : QStringList{"Highlight", "ClearHighlight", "EditSelected", "SetState"};
    for (const auto& action : targeted) patches[action] = QJsonObject{{"target", binding}};
    const auto name = part["name"].toString().isEmpty() ? "零件 " + part["labelId"].toString() : part["name"].toString();
    auto node = Node(prefix + id, name + " · 标签 " + part["labelId"].toString(),
        current ? (part["selected"].toBool() ? "视图中高亮" : part["voxelCount"].toString() + " 体素") : "已过期", actions, patches);
    node["binding"] = binding; return node;
}
QJsonObject PublishedNodes(const QString& module, const QJsonObject& graph, const QString& current)
{
    static const QMap<QString, QString> producers{{"Crop", "OrthogonalCrop"}, {"Gap", "GapAnalysis"}, {"Part", "part-segmentation"},
        {"PartEdit", "part-segmentation"}, {"Artifact", "artifact-reduction"}, {"Surface", "surface-determination"}, {"Alignment", "metrology-alignment"}};
    if (module != "Data" && !producers.contains(module)) return {};
    QMap<QString, QJsonObject> all; std::vector<std::pair<qulonglong, QString>> seeds;
    for (const auto value : graph["nodes"].toArray()) {
        const auto node = value.toObject(); const auto ref = node["ref"].toString(); all[ref] = node;
        if (module == "Data" || node["producer"] == producers.value(module)) seeds.emplace_back(node["order"].toString().toULongLong(), ref);
    }
    std::sort(seeds.rbegin(), seeds.rend());
    // 窗口内保留完整父关系；超过上限的来源明确标注为未展开，不制造伪分支。
    QSet<QString> included; QStringList pending; if (all.contains(current)) pending.append(current);
    for (const auto& seed : seeds) pending.append(seed.second);
    for (int i = 0; i < pending.size() && included.size() < 256; ++i) {
        const auto ref = pending[i]; if (included.contains(ref) || !all.contains(ref)) continue;
        included.insert(ref); for (const auto parent : all[ref]["parents"].toArray()) pending.append(parent.toString());
    }
    if (included.isEmpty()) return {};
    QMap<QString, int> children;
    for (const auto& ref : included) children[ref] = 0;
    for (const auto& ref : included) for (const auto parent : all[ref]["parents"].toArray()) if (included.contains(parent.toString())) ++children[parent.toString()];
    std::priority_queue<std::pair<qulonglong, QString>> ready;
    for (auto it = children.cbegin(); it != children.cend(); ++it) if (!it.value()) ready.emplace(all[it.key()]["order"].toString().toULongLong(), it.key());
    QJsonArray rows;
    while (!ready.empty()) {
        const auto ref = ready.top().second; ready.pop(); const auto data = all[ref];
        QString feature = "数据";
        for (auto it = producers.cbegin(); it != producers.cend(); ++it) if (data["producer"] == it.value() && it.key() != "PartEdit") feature = GetModuleText(it.key());
        const auto type = data["type"].toString();
        const QString kind = data["isVolume"].toBool() ? "体数据" : type == "org.mvvcvtk.surface-mesh" ? "表面网格" : type.contains("label", Qt::CaseInsensitive) ? "标签" : type.contains("mask", Qt::CaseInsensitive) ? "掩码" : type.contains("transform", Qt::CaseInsensitive) ? "变换" : "结果记录";
        QJsonArray parents; int omitted = 0;
        for (const auto parent : data["parents"].toArray()) {
            const auto parentRef = parent.toString(); if (!included.contains(parentRef)) { ++omitted; continue; }
            parents.append("published:" + parentRef);
            if (--children[parentRef] == 0) ready.emplace(all[parentRef]["order"].toString().toULongLong(), parentRef);
        }
        QStringList actions{"GraphInfo"}; if (data["isVolume"].toBool()) actions.prepend("UseData");
        const QJsonObject target{{"graphRevision", ref}};
        auto row = Node("published:" + ref, QString("%1 · %2 #%3").arg(feature, kind, data["order"].toString()),
            (ref == current ? "当前输入" : data["isVolume"].toBool() ? "可继续处理" : "已发布") + (omitted ? QString(" · %1 个来源未展开").arg(omitted) : QString()),
            actions, {{"UseData", target}, {"GraphInfo", target}});
        row["parents"] = parents; row["current"] = ref == current;
        row["description"] = QString("修订 %1；来源 %2；业务 %3 / %4。%5").arg(ref, QString::fromUtf8(QJsonDocument(data["parents"].toArray()).toJson(QJsonDocument::Compact)), data["producer"].toString(), data["operation"].toString(),
            data["isVolume"].toBool() ? "从此数据继续会切换当前输入；再次计算后保留已有发布结果。" : "只读发布记录；连线表示数据来源关系。");
        rows.append(row);
    }
    return Node("published-graph", "已发布数据关系", QString("%1 个节点").arg(rows.size()) + (included.size() == 256 ? " · 部分历史未展开" : ""), {}, {}, rows);
}
}
QJsonArray GetSceneNodes(const QString& module, const QJsonObject& s, const QJsonObject& input,
    const QStringList& availableActions, const QJsonObject& result, const QJsonObject& publishedGraph)
{
    auto actions = availableActions;
    actions.removeAll("UseData"); actions.removeAll("GraphInfo");
    if (module == "Crop") { actions.removeAll("Node"); actions.removeAll("DeleteNode"); }
    QJsonArray children;
    if (input["available"].toBool()) children.append(Node("input:" + input["dataRevision"].toString(), input["datasetId"].toString("当前体数据"), "当前输入",
        module == "Data" ? QStringList{"Descriptor", "ExportData", "ExportSlices", "CreateMask", "Select"} : actions,
        module == "Data" ? QJsonObject{{"Select", QJsonObject{{"revision", input["dataRevision"]}, {"expectedBindingRevision", "current"}}}} : QJsonObject{},
        module == "Data" ? QJsonArray{Node("input-dims", "体素尺寸", Values(input["dims"])), Node("input-spacing", "体素间距（RAS）", Values(input["spacingRAS"])),
            Node("input-origin", "原点（RAS）", Values(input["originRAS"])), Node("input-range", "灰度范围", Values(input["scalarRange"])), Node("input-file", "输入文件", input["uri"].toString())} : QJsonArray{}));
    if (module == "View") for (const auto v : s["views"].toArray()) {
        const auto view = v.toObject(); const auto id = view["viewId"].toString();
        children.append(Node("view:" + id, view["name"].toString(id), view["isCurrent"].toBool() ? "已显示" : "等待绘制",
            {"Set", "Visibility", "Reset", "Cursor", "State"}, {{"Set", QJsonObject{{"viewId", id}}}, {"Visibility", QJsonObject{{"viewScope", id}}}, {"Reset", QJsonObject{{"viewId", id}}}}));
    }
    if (module == "Crop") {
        children.append(Node("crop-tools", "裁剪工具", s["isActive"].toBool() ? "编辑中" : "未启用",
            {"Box", "Plane", "KeepInside", "RemoveInside", "PositionOnly", "Mode", "Exit"}));
        if (s["hasHistory"].toBool()) {
            QJsonArray history; const auto count = s["operationCount"].toString().toULongLong(); const auto cursor = s["nodeCount"].toString().toULongLong();
            const auto first = count > 200 && cursor > 100 ? cursor - 100 : 0;
            const auto end = qMin(count, first + 199);
            const auto indices = s["operationIndices"].toArray();
            for (auto i = first; i <= end && i <= static_cast<qulonglong>(indices.size()); ++i) {
                const auto operation = i ? indices[static_cast<int>(i-1)].toString() : QString("root");
                QStringList nodeActions{"Node", "Previous", "Next", "ResetPreview", "FinishEditing", "BuildResult"};
                QJsonObject patches{{"Node", QJsonObject{{"nodeCount", QString::number(i)}}}};
                if (i) { nodeActions.append("DeleteNode"); patches["DeleteNode"] = QJsonObject{{"operationIndex", operation}}; }
                const auto prefix = "crop-history:" + s["historySource"].toString() + ":";
                auto node = Node(prefix + operation, i == 0 ? "原始状态" : "裁剪节点 " + operation,
                    i == cursor ? "当前节点" : i < cursor ? "已生效" : "可重做", nodeActions, patches);
                node["parents"] = i > first ? QJsonArray{prefix + (i == 1 ? QString("root") : indices[static_cast<int>(i-2)].toString())} : QJsonArray{};
                node["current"] = i == cursor; node["pending"] = i > cursor;
                node["description"] = "裁剪预览按单线保存。回退后新增裁剪会替换后续可重做节点；已发布结果保留在数据关系中。";
                history.prepend(node);
            }
            children.append(Node("crop-history", QString("裁剪历史 · %1 / %2").arg(cursor).arg(count), (s["isConfirmed"].toBool() ? QString("已确认") : QString("预览")) + (first || end < count ? " · 部分历史未展开" : ""),
                {"Previous", "Next", "ResetPreview", "FinishEditing", "BuildResult"}, {}, history));
        }
        if (!Ref(s["output"]).isEmpty()) children.append(Node("crop-output:" + Ref(s["output"]), "裁剪结果", s["isOutputCurrent"].toBool() ? "当前输入" : "已发布", {"SelectOutput", "RestoreSource"}));
    }
    if (module == "Part" || module == "PartEdit") {
        const bool current = s["hasCurrentParts"].toBool();
        QJsonArray highlighted, editing;
        if (current) {
            for (const auto value : s["parts"].toArray()) if (value.toObject()["selected"].toBool())
                highlighted.append(PartNode(value.toObject(), true, module == "PartEdit", "highlight-part:"));
            for (const auto value : s["editingParts"].toArray())
                editing.append(PartNode(value.toObject(), true, module == "PartEdit", "editing-part:"));
        }
        children.append(Node("part-highlights", QString("高亮零件 · %1").arg(highlighted.size()),
            highlighted.isEmpty() ? "当前没有高亮" : s["isOverlayVisible"].toBool() ? "视图中高亮" : "预览已关闭", {}, {}, highlighted));
        auto editGroup = Node("part-edit-targets", QString("编辑对象 · %1").arg(editing.size()),
            editing.isEmpty() ? "尚无当前目标" : s["editingStatus"].toString(), {}, {}, editing);
        editGroup["description"] = "这里保留已进入编辑或已接纳操作的实际对象，与临时选中、高亮状态分别显示。";
        children.append(editGroup);
        QJsonArray parts;
        QMap<QString, QJsonObject> previous;
        QMap<QString, QJsonArray> parents;
        QMap<QString, QStringList> kinds;
        auto bindingId = [](const QJsonValue& value) { return "part:" + QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)); };
        for (const auto value : s["relations"].toArray()) {
            const auto relation = value.toObject(); const auto from = bindingId(relation["previous"]), to = bindingId(relation["current"]);
            if (!parents[to].contains(from)) parents[to].append(from);
            if (!kinds[to].contains(relation["kind"].toString())) kinds[to].append(relation["kind"].toString());
            auto node = Node(from, "上版零件 · " + relation["previous"].toObject()["objectLow"].toString(), "来源记录");
            node["parents"] = QJsonArray{};
            node["description"] = "上一版目录的来源零件，仅展示真实拆分、合并或延续关系；历史来源不支持单独恢复或编辑。";
            previous[from] = node;
        }
        for (const auto p : s["parts"].toArray()) {
            auto node = PartNode(p.toObject(), s["hasCurrentParts"].toBool(), module == "PartEdit"); const auto id = node["id"].toString();
            node["parents"] = parents.value(id); node["current"] = p.toObject()["selected"].toBool();
            if (!kinds.value(id).isEmpty()) node["description"] = "与上一版零件目录的关系：" + kinds.value(id).join("、") + "。当前零件可执行节点菜单中的业务操作。";
            parts.append(node);
        }
        for (const auto& node : previous) parts.append(node);
        if (s["hasPreview"].toBool()) {
            QJsonArray candidates;
            const QJsonObject patches{{"Commit", QJsonObject{{"previewId", s["previewId"]}}}, {"Discard", QJsonObject{{"previewId", s["previewId"]}}}};
            const auto append = [&](const QJsonArray& values, bool removed) {
                for (const auto value : values) {
                    const auto part = value.toObject();
                    auto node = PartNode(part, true, true, "candidate-part:" + s["previewId"].toString() + ":");
                    node.remove("binding"); node["candidateBinding"] = part["binding"];
                    node["actions"] = QJsonArray{"Commit", "Discard"}; node["patches"] = patches;
                    node["status"] = removed ? QString("将移除") : QString("候选 · %1 体素").arg(part["voxelCount"].toString());
                    node["pending"] = true;
                    node["description"] = "候选尚未替换正式结果；确认或丢弃作用于这一次完整编辑。";
                    candidates.append(node);
                }
            };
            append(s["previewChanges"].toObject()["changed"].toArray(), false);
            append(s["previewChanges"].toObject()["removed"].toArray(), true);
            children.append(Node("edit-preview:" + s["previewId"].toString(),
                QString("编辑候选 · 影响 %1 件 / 结果共 %2 件").arg(candidates.size()).arg(s["candidatePartCount"].toString()),
                "待确认", {"Commit", "Discard"}, patches, candidates));
        }
        if (!parts.isEmpty()) {
            auto all = Node("parts-all", QString("全部零件 · %1").arg(s["parts"].toArray().size()),
                current ? "当前目录与来源" : "已过期", actions, {}, parts);
            all["defaultExpanded"] = false; children.append(all);
        }
    }
    if (module == "Artifact") {
        if (s["hasCandidate"].toBool()) children.append(Node("artifact-candidate:" + s["requestId"].toString(), "校正候选", s["isCandidateCurrent"].toBool() ? "待发布" : "源输入已变化", {"Commit", "Discard"}, {{"Commit", QJsonObject{{"requestId", s["requestId"]}}}}, Quality(s["quality"].toObject())));
        if (!Ref(s["output"]).isEmpty()) children.append(Node("artifact-output:" + Ref(s["output"]), "校正体数据", s["isOutputCurrent"].toBool() ? "当前输入" : s["canSelectOutput"].toBool() ? "已发布" : "源输入已变化", {"SelectOutput", "RestoreSource"}));
    }
    if (module == "Surface" && !Ref(s["mesh"]).isEmpty()) {
        QJsonArray objects{Node("surface-accepted", "有效测量点", s["acceptedPoints"].toString()), Node("surface-rejected", "无效点", s["rejectedPoints"].toString())};
        for (const auto value : s["objects"].toArray()) { const auto object = value.toObject(); objects.append(Node("surface-object:" + Ref(s["mesh"]) + ":" + QString::number(object["objectIndex"].toInt()),
            "表面对象 " + QString::number(object["objectIndex"].toInt()), object["closed"].toBool() ? "闭合" : "开放")); }
        if (result["mesh"] == s["mesh"] && result["samples"].isArray()) {
            QJsonArray samples; const auto values = result["samples"].toArray();
            for (int i = 0; i < qMin(128, values.size()); ++i) { const auto point = values[i].toObject(); samples.append(Node("sample:" + Ref(s["mesh"]) + ":" + point["vertexId"].toString(),
                "采样点 " + point["vertexId"].toString(), Values(point["sourcePoint"]).replace(" × ", ", "))); }
            objects.append(Node("samples:" + Ref(s["mesh"]), QString("采样点 · 显示 %1 / %2").arg(samples.size()).arg(values.size()), "源坐标", {"SamplePoints"}, {}, samples));
        }
        children.append(Node("surface:" + Ref(s["mesh"]), "表面网格 · " + s["points"].toString() + " 点", !s["hasResult"].toBool() ? "已过期" : s["hasMeasurement"].toBool() ? "测量表面" : "预览网格",
            {"SamplePoints", "Visibility", "OpenAlignment", "Clear", "CopyIsoToDisplay", "CopyIsoToGap", "CopyIsoToPart"}, {}, objects));
    }
    if (module == "Surface" && s["hasIso"].toBool()) children.append(Node("surface-iso", "阈值估计 · " + QString::number(s["isoEstimate"].toObject()["iso"].toDouble()), "当前结果", {"LocalAdaptiveIso50", "GradientPeak", "CopyIsoToDisplay", "CopyIsoToGap", "CopyIsoToPart"},
        {{"LocalAdaptiveIso50", QJsonObject{{"initialIsoValue", s["isoEstimate"].toObject()["iso"]}}}, {"GradientPeak", QJsonObject{{"initialIsoValue", s["isoEstimate"].toObject()["iso"]}}}}));
    if (module == "Alignment") {
        QJsonArray details;
        if (result["result"] == s["lastResult"]) for (const auto value : result["residuals"].toArray()) { const auto residual = value.toObject();
            if (details.size() >= 128) break;
            details.append(Node("residual:" + Ref(s["lastResult"]) + ":" + residual["id"].toString(), "约束 " + residual["id"].toString(), QString("残差 %1 / 容差 %2").arg(residual["value"].toDouble(), 0, 'g', 6).arg(residual["tolerance"].toDouble(), 0, 'g', 6)));
        }
        if (s["hasReference"].toBool()) children.append(Node("alignment-reference:" + Ref(s["nominal"]), "名义参考", s["isReferenceCurrent"].toBool() ? "当前输入" : "已过期", {"ImportReference", "SaveRecipe", "Restore"}));
        if (s["hasRecipe"].toBool()) children.append(Node("alignment-recipe:" + Ref(s["savedRecipe"]), "对齐方案", s["isReferenceCurrent"].toBool() ? "已保存" : "源输入已变化", {"Start", "SaveRecipe", "Restore", "Cancel"}));
        if (s["hasResult"].toBool()) children.append(Node("alignment-result:" + Ref(s["lastResult"]), "对齐结果", !s["isResultCurrent"].toBool() ? "已过期" : s["isApplied"].toBool() ? "已应用" : "待应用",
            {"Result", "Activate", "Deactivate", "ExportArchive", "Visibility"}, {{"Result", QJsonObject{{"result", s["lastResult"]}}}, {"Activate", QJsonObject{{"result", s["lastResult"]}}}, {"ExportArchive", QJsonObject{{"result", s["lastResult"]}}}}, details));
    }
    if (module == "Gap" && s["hasResult"].toBool()) children.append(Node("gap-result:" + Ref(s["resultSet"]), "孔隙分析结果", s["isCurrent"].toBool() ? "孔隙率 " + QString::number(s["porosityRatio"].toDouble()*100., 'g', 5) + "%" : "已过期", {"Overlay", "Exit", "Start"}, {},
        {Node("gap-volume", "孔隙体积（mm³）", QString::number(s["voidVolumeMM3"].toDouble(), 'g', 7)), Node("gap-object-volume", "对象体积（mm³）", QString::number(s["objectVolumeMM3"].toDouble(), 'g', 7)), Node("gap-voxels", "孔隙体素", s["voidVoxels"].toString())}));
    if (module == "Rotation") children.append(Node("rotation", "模型变换", "可撤销 " + s["undoCount"].toString("0") + " 次", {"Rotate", "Undo", "SetEnabled", "Cancel"}));
    if (s["isBusy"].toBool()) children.append(Node("running", "正在处理", s.contains("progress") ? QString::number(s["progress"].toDouble()*100., 'f', 0) + "%" : "等待结果", {"Stop", "Cancel", "Exit"}));
    if (module == "Data") {
        for (const auto value : s["labels"].toArray()) { const auto label = value.toObject(); children.append(Node("label:" + Ref(label["revision"]), "标签 · " + label["id"].toString(), label["source"] == input["dataRevision"] ? "当前标签" : "旧输入标签", {"ReadLabelRegion", "LabelDescriptors"}, {{"ReadLabelRegion", QJsonObject{{"id", label["id"]}, {"revision", label["revision"]}}}})); }
        if (!Ref(result["mask"]).isEmpty() && result["source"] == input["dataRevision"]) children.append(Node("mask:" + Ref(result["mask"]), "测试掩码", "已发布", {"CreateMask"}));
    }
    const auto published = actions.isEmpty() ? QJsonObject() : PublishedNodes(module, publishedGraph, input["dataRevision"].toString());
    if (!published.isEmpty()) children.append(published);
    return {Node("root", GetModuleText(module), actions.isEmpty() ? "当前构建未启用" : input["available"].toBool() ? "场景" : "等待输入", actions, {}, children)};
}
}
