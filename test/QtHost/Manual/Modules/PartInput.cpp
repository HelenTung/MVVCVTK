// 测试用途：解析零件稳定绑定和编辑参数，并生成目录与结果摘要。
#include "PartInput.h"
#include <QMap>
#include <QSet>
namespace Manual {
QJsonObject GetPartRef(const PartBindingRef& part)
{
    return {{"setHigh", QString::number(part.object.partSetId.high)}, {"setLow", QString::number(part.object.partSetId.low)},
        {"objectHigh", QString::number(part.object.objectId.high)}, {"objectLow", QString::number(part.object.objectId.low)},
        {"resultRevision", QString::number(part.resultRevision)}};
}
PartBindingRef GetPart(const QJsonValue& value, const PartSetSnapshot& catalog)
{
    if (value.isString() && value.toString() == "selected") {
        std::optional<PartBindingRef> selected;
        for (const auto& part : catalog.parts) if (part.presentation.isSelected) {
            if (selected) throw std::invalid_argument("需要单个目标，当前选择了多个零件");
            selected = part.binding;
        }
        if (!selected) throw std::invalid_argument("请先在零件目录选择目标，或输入完整 PartBindingRef");
        return *selected;
    }
    const auto object = value.toObject();
    return {{{GetId(object["setHigh"]), GetId(object["setLow"])}, {GetId(object["objectHigh"]), GetId(object["objectLow"])}}, GetId(object["resultRevision"])};
}
std::vector<PartBindingRef> GetParts(const QJsonValue& value, const PartSetSnapshot& catalog)
{
    if (!value.isArray()) throw std::invalid_argument("零件列表必须是数组");
    std::vector<PartBindingRef> result;
    for (const auto part : value.toArray()) result.push_back(GetPart(part, catalog));
    return result;
}
QJsonObject GetPartResult(const PartSegmentationResult& result)
{
    return {{"status", static_cast<int>(result.status)}, {"failureReason", static_cast<int>(result.failureReason)},
        {"requestId", QString::number(result.requestId)}, {"resultRevision", QString::number(result.resultRevision)},
        {"catalogRevision", QString::number(result.catalogRevision)}, {"commitId", QString::number(result.commitId)},
        {"source", GetRefText(result.sourceRevision)}, {"labelMap", GetRefText(result.labelMap)},
        {"resultSet", GetRefText(result.resultSet)}, {"partCount", QString::number(result.partCount)}, {"message", QString::fromStdString(result.message)}};
}
QJsonObject GetPartJson(const PartSnapshot& part)
{
    return {{"binding", GetPartRef(part.binding)}, {"labelId", QString::number(part.labelId)},
        {"name", QString::fromStdString(part.userState.name)}, {"voxelCount", QString::number(part.metrics.voxelCount)},
        {"volumeMM3", part.metrics.physicalVolumeMM3}, {"centroidSourceMM", GetValues(part.metrics.centroidInputPhysical)},
        {"extent", GetValues(part.metrics.voxelExtent)}, {"visible", part.presentation.isVisible},
        {"selected", part.presentation.isSelected}, {"colorRGBA", GetValues(part.presentation.color)},
        {"opacity", part.presentation.opacity}, {"reviewed", part.userState.isReviewed}};
}
namespace {
QString PartKey(QJsonObject binding) { binding.remove("resultRevision"); return GetJsonText(binding); }
}
QJsonArray GetEditingParts(const QJsonObject& context, const PartSetSnapshot& catalog)
{
    if (context["source"] != GetRefText(catalog.sourceRevision) || catalog.isStale) return {};
    QSet<QString> wanted;
    for (const auto value : context["targets"].toArray()) wanted.insert(PartKey(value.toObject()));
    auto resolved = wanted;
    for (const auto& relation : catalog.relationsFromPrevious)
        if (wanted.contains(PartKey(GetPartRef(relation.previous)))) resolved.insert(PartKey(GetPartRef(relation.current)));
    QJsonArray result;
    for (const auto& part : catalog.parts) if (resolved.contains(PartKey(GetPartRef(part.binding)))) result.append(GetPartJson(part));
    return result;
}
QJsonObject GetPreviewChanges(const PartSetSnapshot& previous, const PartSetSnapshot& candidate)
{
    QMap<QString, const PartSnapshot*> before;
    for (const auto& part : previous.parts) before[PartKey(GetPartRef(part.binding))] = &part;
    QSet<QString> after; QJsonArray changed, removed;
    for (const auto& part : candidate.parts) {
        const auto key = PartKey(GetPartRef(part.binding)); after.insert(key);
        const auto old = before.value(key, nullptr);
        if (!old || old->metrics != part.metrics || old->userState != part.userState) changed.append(GetPartJson(part));
    }
    for (auto it = before.begin(); it != before.end(); ++it) if (!after.contains(it.key())) removed.append(GetPartJson(*it.value()));
    return {{"changed", changed}, {"removed", removed}};
}
QJsonObject GetCatalog(const PartSegmentationHostFeature& feature)
{
    const auto snapshot = feature.GetPartSetSnapshot();
    const auto state = feature.GetState();
    QJsonObject result{{"status", static_cast<int>(state.status)}, {"progress", state.progress}, {"requestId", QString::number(state.requestId)},
        {"isBusy", state.status == PartSegmentationStatus::Running || state.status == PartSegmentationStatus::Stopping || state.status == PartSegmentationStatus::Committing},
        {"hasCurrentParts", snapshot && !snapshot->isStale && !snapshot->parts.empty()},
        {"labelMap", GetRefText(state.labelMap)}, {"source", GetRefText(state.sourceRevision)},
        {"catalogRevision", QString::number(state.catalogRevision)}, {"resultRevision", QString::number(state.resultRevision)}};
    QJsonArray parts, relations;
    if (snapshot) {
        result["isStale"] = snapshot->isStale;
        for (const auto& relation : snapshot->relationsFromPrevious) relations.append(QJsonObject{{"current", GetPartRef(relation.current)},
            {"previous", GetPartRef(relation.previous)}, {"kind", relation.kind == PartRelationKind::SplitFrom ? "拆分" : relation.kind == PartRelationKind::MergedFrom ? "合并" : "延续"}});
        for (const auto& part : snapshot->parts) parts.append(GetPartJson(part));
    }
    result["parts"] = parts;
    result["relations"] = relations;
    result["partCount"] = parts.size();
    return result;
}
}
