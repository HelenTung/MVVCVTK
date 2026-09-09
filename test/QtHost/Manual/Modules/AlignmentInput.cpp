// 测试用途：解析计量对齐参考与方案，校验几何约束并导出参数模板和结果摘要。
#include "AlignmentInput.h"
namespace Manual {
namespace {
const char* GetMethodName(AlignmentMethod method)
{
    switch (method) {
    case AlignmentMethod::SequentialPlanes: return "SequentialPlanes";
    case AlignmentMethod::PlaneTwoHoles: return "PlaneTwoHoles";
    case AlignmentMethod::Rps: return "Rps";
    case AlignmentMethod::ConstrainedBestFit: return "ConstrainedBestFit";
    }
    throw std::invalid_argument("对齐方法无效");
}
AlignmentUnit GetUnit(const QJsonObject& p)
{
    return GetEnum<AlignmentUnit>(p, "unit", {{"ModelUnit", AlignmentUnit::ModelUnit}, {"Millimeter", AlignmentUnit::Millimeter}, {"Meter", AlignmentUnit::Meter}});
}
const char* GetUnitName(AlignmentUnit unit)
{
    return unit == AlignmentUnit::Millimeter ? "Millimeter" : unit == AlignmentUnit::Meter ? "Meter" : "ModelUnit";
}
const char* GetKindName(AlignmentGeometryKind kind)
{
    switch (kind) {
    case AlignmentGeometryKind::Point: return "Point";
    case AlignmentGeometryKind::Line: return "Line";
    case AlignmentGeometryKind::Plane: return "Plane";
    case AlignmentGeometryKind::Circle: return "Circle";
    case AlignmentGeometryKind::Sphere: return "Sphere";
    case AlignmentGeometryKind::Cylinder: return "Cylinder";
    }
    throw std::invalid_argument("几何类型无效");
}
DataRevisionRef GetMeshRef(const QJsonValue& value, DataRevisionRef mesh)
{
    return value.toString() == "$mesh" ? mesh : GetRef(value);
}
}
AlignmentRecipe GetRecipe(const QJsonObject& p, const DataRevisionRef& mesh, const DataRevisionRef& nominal)
{
    AlignmentRecipe recipe;
    recipe.id = GetText(p, "id").toStdString();
    recipe.targetFrameId = GetText(p, "targetFrameId").toStdString();
    recipe.unit = GetUnit(p);
    recipe.method = GetEnum<AlignmentMethod>(p, "method", {{"SequentialPlanes", AlignmentMethod::SequentialPlanes},
        {"PlaneTwoHoles", AlignmentMethod::PlaneTwoHoles}, {"Rps", AlignmentMethod::Rps}, {"ConstrainedBestFit", AlignmentMethod::ConstrainedBestFit}});
    recipe.nominalData = nominal;
    if (p.contains("nominalData") && p["nominalData"].toString() != "$nominal") recipe.nominalData = GetRef(p["nominalData"]);
    if (p.contains("exactMesh") && !p["exactMesh"].isNull()) recipe.exactMesh = GetMeshRef(p["exactMesh"], mesh);
    if (!p["geometries"].isArray() || !p["constraints"].isArray() || !p["fitPairs"].isArray()) throw std::invalid_argument("配方需要几何、约束与对应点数组");
    for (const auto geometry : p["geometries"].toArray()) {
        const auto g = geometry.toObject();
        AlignmentGeometrySpec spec;
        spec.id = GetText(g, "id").toStdString();
        spec.kind = GetEnum<AlignmentGeometryKind>(g, "kind", {{"Point", AlignmentGeometryKind::Point}, {"Line", AlignmentGeometryKind::Line},
            {"Plane", AlignmentGeometryKind::Plane}, {"Circle", AlignmentGeometryKind::Circle}, {"Sphere", AlignmentGeometryKind::Sphere}, {"Cylinder", AlignmentGeometryKind::Cylinder}});
        spec.association = GetEnum<AlignmentAssociation>(g, "association", {{"LeastSquares", AlignmentAssociation::LeastSquares}, {"SequentialLeastSquares", AlignmentAssociation::SequentialLeastSquares}});
        const auto region = g["region"].toObject();
        spec.region.pinnedMesh = GetMeshRef(region["pinnedMesh"], mesh);
        for (const auto vertex : region["vertexIds"].toArray()) spec.region.vertexIds.push_back(GetId(vertex));
        if (!region["targetBounds"].isNull()) spec.region.targetBounds = GetArray<double, 6>(region["targetBounds"]);
        spec.targetDirection = GetArray<double, 3>(g["targetDirection"]);
        if (g.contains("radiusRange") && !g["radiusRange"].isNull()) spec.radiusRange = GetArray<double, 2>(g["radiusRange"]);
        if (g.contains("maxFitRms")) spec.maxFitRms = GetNumber(g, "maxFitRms");
        if (g.contains("minCoverage")) spec.minCoverage = GetNumber(g, "minCoverage");
        if (g.contains("minDirectionCosine")) spec.minDirectionCosine = GetNumber(g, "minDirectionCosine");
        recipe.geometries.push_back(std::move(spec));
    }
    for (const auto constraint : p["constraints"].toArray()) {
        const auto c = constraint.toObject();
        AlignmentConstraint value;
        value.geometryIndex = GetId(c["geometryIndex"]);
        value.nominalPoint = GetArray<double, 3>(c["nominalPoint"]);
        value.targetDirection = GetArray<double, 3>(c["targetDirection"]);
        value.kind = GetEnum<AlignmentConstraintKind>(c, "kind", {{"Hard", AlignmentConstraintKind::Hard}, {"Soft", AlignmentConstraintKind::Soft}, {"Check", AlignmentConstraintKind::Check}});
        const auto priority = GetId(c["priority"]);
        if (priority > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("priority 超界");
        value.priority = static_cast<std::uint32_t>(priority);
        value.weight = GetNumber(c, "weight"); value.tolerance = GetNumber(c, "tolerance");
        if (!c["sectionPlane"].isNull()) value.sectionPlane = GetId(c["sectionPlane"]);
        recipe.constraints.push_back(value);
    }
    for (const auto pair : p["fitPairs"].toArray()) {
        const auto f = pair.toObject();
        AlignmentFitPair value;
        value.vertexId = GetId(f["vertexId"]); value.nominalPoint = GetArray<double, 3>(f["nominalPoint"]); value.weight = GetNumber(f, "weight");
        if (!f["targetNormal"].isNull()) value.targetNormal = GetArray<double, 3>(f["targetNormal"]);
        recipe.fitPairs.push_back(value);
    }
    if (p.contains("datumCount")) recipe.datumCount = GetId(p["datumCount"]);
    if (p.contains("datumOffsets")) recipe.datumOffsets = GetArray<double, 3>(p["datumOffsets"]);
    if (p.contains("iterationLimit")) recipe.iterationLimit = GetId(p["iterationLimit"]);
    if (p.contains("requiresMeasurementQuality")) recipe.requiresMeasurementQuality = GetBool(p, "requiresMeasurementQuality");
    if (p.contains("minPairNormalCosine") && !p["minPairNormalCosine"].isNull()) recipe.minPairNormalCosine = GetNumber(p, "minPairNormalCosine");
#define MANUAL_RECIPE_NUMBER(field) if (p.contains(#field)) recipe.field = GetNumber(p, #field)
    MANUAL_RECIPE_NUMBER(lengthScale); MANUAL_RECIPE_NUMBER(rankTolerance); MANUAL_RECIPE_NUMBER(conditionLimit);
    MANUAL_RECIPE_NUMBER(solveTolerance); MANUAL_RECIPE_NUMBER(maxAlignmentRms); MANUAL_RECIPE_NUMBER(maxPairDistance);
    MANUAL_RECIPE_NUMBER(minPairCoverage); MANUAL_RECIPE_NUMBER(huberDistance); MANUAL_RECIPE_NUMBER(minSupportRatio);
    MANUAL_RECIPE_NUMBER(maxLocalizationSigma); MANUAL_RECIPE_NUMBER(maxSurfaceFitResidual);
#undef MANUAL_RECIPE_NUMBER
    return recipe;
}
QJsonObject GetRecipeJson(const AlignmentRecipe& recipe)
{
    QJsonObject value{{"id", QString::fromStdString(recipe.id)}, {"targetFrameId", QString::fromStdString(recipe.targetFrameId)},
        {"unit", GetUnitName(recipe.unit)}, {"method", GetMethodName(recipe.method)},
        {"exactMesh", GetDataRevisionRefValid(recipe.exactMesh) ? QJsonValue(GetRefText(recipe.exactMesh)) : QJsonValue()},
        {"nominalData", GetRefText(recipe.nominalData)}, {"datumCount", QString::number(recipe.datumCount)},
        {"datumOffsets", GetValues(recipe.datumOffsets)}, {"iterationLimit", QString::number(recipe.iterationLimit)},
        {"requiresMeasurementQuality", recipe.requiresMeasurementQuality},
        {"minPairNormalCosine", recipe.minPairNormalCosine ? QJsonValue(*recipe.minPairNormalCosine) : QJsonValue()}};
#define MANUAL_RECIPE_NUMBER(field) value[#field] = recipe.field
    MANUAL_RECIPE_NUMBER(lengthScale); MANUAL_RECIPE_NUMBER(rankTolerance); MANUAL_RECIPE_NUMBER(conditionLimit);
    MANUAL_RECIPE_NUMBER(solveTolerance); MANUAL_RECIPE_NUMBER(maxAlignmentRms); MANUAL_RECIPE_NUMBER(maxPairDistance);
    MANUAL_RECIPE_NUMBER(minPairCoverage); MANUAL_RECIPE_NUMBER(huberDistance); MANUAL_RECIPE_NUMBER(minSupportRatio);
    MANUAL_RECIPE_NUMBER(maxLocalizationSigma); MANUAL_RECIPE_NUMBER(maxSurfaceFitResidual);
#undef MANUAL_RECIPE_NUMBER
    QJsonArray geometries, constraints, pairs;
    for (const auto& g : recipe.geometries) {
        QJsonArray ids;
        for (const auto id : g.region.vertexIds) ids.append(QString::number(id));
        geometries.append(QJsonObject{{"id", QString::fromStdString(g.id)}, {"kind", GetKindName(g.kind)},
            {"association", g.association == AlignmentAssociation::SequentialLeastSquares ? "SequentialLeastSquares" : "LeastSquares"},
            {"region", QJsonObject{{"pinnedMesh", GetRefText(g.region.pinnedMesh)}, {"vertexIds", ids},
                {"targetBounds", g.region.targetBounds ? QJsonValue(GetValues(*g.region.targetBounds)) : QJsonValue()}}},
            {"targetDirection", GetValues(g.targetDirection)}, {"radiusRange", g.radiusRange ? QJsonValue(GetValues(*g.radiusRange)) : QJsonValue()},
            {"maxFitRms", g.maxFitRms}, {"minCoverage", g.minCoverage}, {"minDirectionCosine", g.minDirectionCosine}});
    }
    for (const auto& c : recipe.constraints) constraints.append(QJsonObject{{"geometryIndex", QString::number(c.geometryIndex)},
        {"nominalPoint", GetValues(c.nominalPoint)}, {"targetDirection", GetValues(c.targetDirection)},
        {"kind", c.kind == AlignmentConstraintKind::Hard ? "Hard" : c.kind == AlignmentConstraintKind::Soft ? "Soft" : "Check"},
        {"priority", QString::number(c.priority)}, {"weight", c.weight}, {"tolerance", c.tolerance},
        {"sectionPlane", c.sectionPlane ? QJsonValue(QString::number(*c.sectionPlane)) : QJsonValue()}});
    for (const auto& f : recipe.fitPairs) pairs.append(QJsonObject{{"vertexId", QString::number(f.vertexId)},
        {"nominalPoint", GetValues(f.nominalPoint)}, {"targetNormal", f.targetNormal ? QJsonValue(GetValues(*f.targetNormal)) : QJsonValue()}, {"weight", f.weight}});
    value["geometries"] = geometries; value["constraints"] = constraints; value["fitPairs"] = pairs;
    return value;
}
AlignmentInput GetAlignmentInput(const ReferenceInput& reference)
{
    AlignmentInput input;
    input.source = reference.source; input.mesh = reference.mesh; input.scopeData = reference.scope;
    input.sourceFrameId = GetText(reference.document, "sourceFrameId").toStdString();
    input.coordinateFrame = reference.coordinateFrame.toStdString();
    input.scope = GetText(reference.document, "scope").toStdString(); input.unit = GetUnit(reference.document);
    input.sourceBinding = std::string(primaryVolumeBinding);
    return input;
}
QJsonObject GetInputJson(const AlignmentInput& input)
{
    return {{"source", GetRefText(input.source)}, {"mesh", GetRefText(input.mesh)}, {"scopeData", GetRefText(input.scopeData)},
        {"sourceFrameId", QString::fromStdString(input.sourceFrameId)}, {"coordinateFrame", QString::fromStdString(input.coordinateFrame)},
        {"scope", QString::fromStdString(input.scope)}, {"unit", GetUnitName(input.unit)},
        {"sourceBinding", QString::fromStdString(input.sourceBinding)}, {"meshBinding", QString::fromStdString(input.meshBinding)},
        {"scopeBinding", QString::fromStdString(input.scopeBinding)}};
}
QJsonObject GetReferenceTemplate(AlignmentMethod method)
{
    AlignmentRecipe recipe;
    recipe.id = GetMethodName(method); recipe.targetFrameId = "nominal-frame"; recipe.unit = AlignmentUnit::Millimeter; recipe.method = method;
    recipe.datumCount = method == AlignmentMethod::ConstrainedBestFit || method == AlignmentMethod::Rps ? 0 : 3;
    if (method != AlignmentMethod::ConstrainedBestFit) for (int index = 0; index < 3; ++index) {
        AlignmentGeometrySpec g; g.id = "geometry-" + std::to_string(index);
        g.kind = method == AlignmentMethod::Rps ? AlignmentGeometryKind::Point
            : method == AlignmentMethod::PlaneTwoHoles && index > 0 ? AlignmentGeometryKind::Cylinder : AlignmentGeometryKind::Plane;
        if (method == AlignmentMethod::SequentialPlanes) g.association = AlignmentAssociation::SequentialLeastSquares;
        g.targetDirection = method == AlignmentMethod::SequentialPlanes ? AlignmentPoint{index == 2 ? 1.0 : 0, index == 1 ? 1.0 : 0, index == 0 ? 1.0 : 0} : AlignmentPoint{0, 0, 1};
        recipe.geometries.push_back(g);
        if (method == AlignmentMethod::Rps) for (int axis = 0; axis < 3; ++axis) {
            AlignmentConstraint c; c.geometryIndex = index; c.targetDirection = {0, 0, 0}; c.targetDirection[axis] = 1; recipe.constraints.push_back(c);
        }
    }
    if (method == AlignmentMethod::ConstrainedBestFit) for (int index = 0; index < 3; ++index) recipe.fitPairs.push_back({static_cast<std::uint64_t>(index), {}, {}, 1.0});
    auto json = GetRecipeJson(recipe);
    json["nominalData"] = "$nominal";
    if (method == AlignmentMethod::ConstrainedBestFit) json["exactMesh"] = "$mesh";
    QJsonArray geometries;
    for (const auto value : json["geometries"].toArray()) {
        auto g = value.toObject(); auto region = g["region"].toObject(); region["pinnedMesh"] = "$mesh"; g["region"] = region; geometries.append(g);
    }
    json["geometries"] = geometries;
    return {{"sourceFrameId", "source-model"}, {"coordinateFrame", "RAS"}, {"scope", "manual-alignment"},
        {"unit", "Millimeter"}, {"evidenceKind", "unconfigured-template"}, {"provenance", "请填写真实名义参考来源、几何区域/顶点与容差"}, {"recipe", json}};
}
QJsonObject GetAlignmentResult(const AlignmentResult& result)
{
    const auto& d = result.diagnostics;
    QJsonArray singular, stages, nullspace;
    for (auto value : d.singularValues) singular.append(value);
    for (auto value : d.stageRemaining) stages.append(QString::number(value));
    for (const auto& value : d.nullspace) nullspace.append(GetValues(value));
    return {{"status", static_cast<int>(result.status)}, {"requestId", QString::number(result.requestId)},
        {"recipe", GetRefText(result.recipe)}, {"result", GetRefText(result.result)}, {"transform", GetRefText(result.transform)},
        {"isActivated", result.isActivated}, {"isDisplayReady", result.isDisplayReady}, {"message", QString::fromStdString(result.message)},
        {"diagnostics", QJsonObject{{"status", static_cast<int>(d.status)}, {"hardRemaining", QString::number(d.hardRemaining)},
            {"remaining", QString::number(d.remaining)}, {"rms", d.rms}, {"maximum", d.maximum}, {"condition", d.condition},
            {"isQualityPassed", d.isQualityPassed}, {"singularValues", singular}, {"nullspace", nullspace}, {"stageRemaining", stages},
            {"iterations", QString::number(d.iterations)}, {"rejectedPairs", QString::number(d.rejectedPairs)}, {"message", QString::fromStdString(d.message)}}}};
}
}
