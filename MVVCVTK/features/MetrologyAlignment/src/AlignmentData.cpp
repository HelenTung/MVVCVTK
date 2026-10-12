#include "AlignmentData.h"
#include "AlignmentMath.h"
#include "AlignmentReference.h"
#include <limits>
#include <set>
#include <stdexcept>

namespace AlignmentData {
namespace {
bool GetGeometryValid(const AlignmentGeometry &g) {
    return !g.id.empty() && static_cast<unsigned>(g.kind) <= 5 &&
           AlignmentMath::Finite(AlignmentMath::Vector(g.sourceCenter)) &&
           AlignmentMath::Finite(AlignmentMath::Vector(g.sourceDirection)) &&
           std::isfinite(g.radius) && g.radius >= 0 && AlignmentMath::GetFitStatisticsValid(g.fitRms, g.fitMax) &&
           g.sampleCount > 0 && AlignmentGeometryFit::GetSupportValid(g);
}
bool GetResultValid(const ResultRecord &r) {
    if (r.algorithmVersion != "metrology-alignment-1" && r.algorithmVersion != "metrology-alignment-2" && r.algorithmVersion != "metrology-alignment-3") return false;
    if (r.diagnostics.prealignment) {
        const auto &p = *r.diagnostics.prealignment;
        if (p.sourceFeatures > p.sourceSamples || p.referenceFeatures > p.referenceSamples ||
            p.candidates == 0 || p.candidates > 8 || !std::isfinite(p.bestCoverage) ||
            p.bestCoverage < 0 || p.bestCoverage > 1 || !std::isfinite(p.sourceSelectedArea) ||
            !std::isfinite(p.sourceRejectedArea) || p.sourceRejectedArea < 0 ||
            p.sourceSelectedArea < p.sourceRejectedArea) return false;
    }
    if (r.algorithmVersion != "metrology-alignment-3" && !r.diagnostics.surfaceStages.empty()) return false;
    if (r.diagnostics.surfaceStages.size() > 16) return false;
    for (const auto &stage : r.diagnostics.surfaceStages) {
        if (!std::isfinite(stage.voxelSize) || stage.voxelSize < 0 ||
            !std::isfinite(stage.maxPairDistance) || stage.maxPairDistance <= 0 ||
            stage.samples == 0 || stage.samples > stage.selectedVertices ||
            stage.rejectedSamples > stage.samples || stage.iterations == 0 || stage.iterations > 1000 ||
            !std::isfinite(stage.selectedArea) || !std::isfinite(stage.acceptedArea) ||
            stage.selectedArea <= 0 || stage.acceptedArea < 0 || stage.acceptedArea > stage.selectedArea)
            return false;
    }
    return GetInputValid(r.input) && GetDataRevisionRefValid(r.recipe) &&
           GetDataRevisionRefValid(r.transform) && GetDataRevisionRefValid(r.geometry) &&
           GetDataRevisionRefValid(r.residuals) && r.diagnostics.hardRemaining <= 6 &&
           r.diagnostics.remaining <= r.diagnostics.hardRemaining &&
           std::isfinite(r.diagnostics.rms) && std::isfinite(r.diagnostics.maximum) &&
           !r.initialPoses.empty() && r.initialPoses.size() <= 8 &&
           std::all_of(r.initialPoses.begin(), r.initialPoses.end(), AlignmentMath::Rigid);
}
} // namespace
bool SetTypes(TrustedDataPort &data) {
    const std::vector<DataTypeDescriptor> types{
        {referenceType,
         {{"org.mvvcvtk.metrology-alignment.reference"}},
         [](const IDataPayload &p, std::string &message) {
             const auto *value = dynamic_cast<const ReferencePayload *>(&p);
             if (!value) { message = "Invalid reference payload type."; return false; }
             const auto &r = value->metadata;
             const bool valid = AlignmentReferenceLoader::GetRequestValid(
                 {r.filePath, r.frameId, r.coordinateFrame, r.fileUnit, r.unit}) &&
                 GetDataRevisionRefValid(r.mesh) && r.sourceBytes > 0 && r.pointCount >= 3 &&
                 r.triangleCount > 0 && r.sha256.size() == 64 &&
                 std::all_of(r.sha256.begin(), r.sha256.end(), [](char c) {
                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                 });
             if (!valid) message = "Invalid STL reference provenance.";
             return valid;
         }},
        {recipeType,
         {{"org.mvvcvtk.metrology-alignment.recipe"}},
         [](const IDataPayload &p, std::string &message) {
             const auto *value = dynamic_cast<const RecipePayload *>(&p);
             const bool valid =
                 value && AlignmentGeometryFit::GetRecipeValid(value->recipe, AlignmentConfig{});
             if (!valid)
                 message = "Invalid alignment recipe schema.";
             return valid;
         }},
        {geometryType,
         {{"org.mvvcvtk.metrology-alignment.geometry"}},
         [](const IDataPayload &p, std::string &message) {
             const auto *value = dynamic_cast<const GeometryPayload *>(&p);
             const bool valid = value && std::all_of(value->geometries.begin(),
                                                     value->geometries.end(), GetGeometryValid);
             if (!valid)
                 message = "Invalid alignment geometry schema.";
             return valid;
         }},
        {resultType,
         {{"org.mvvcvtk.metrology-alignment.result"}},
         [](const IDataPayload &p, std::string &message) {
             const auto *value = dynamic_cast<const ResultPayload *>(&p);
             const bool valid = value && GetResultValid(value->record);
             if (!valid)
                 message = "Invalid alignment result schema.";
             return valid;
         }}};
    for (const auto &type : types) {
        if (data.SetDataType(type))
            continue;
        const auto graph = data.GetDataGraph();
        if (!graph.view || graph.view->GetDataFacets(type.id) != type.facets)
            return false;
    }
    return true;
}
std::shared_ptr<const AlignmentReferenceSnapshot> GetReferenceShared(const DataGraphSnapshot &graph,
                                                                    const DataRevisionRef &ref) {
    const auto data = graph.view ? graph.view->GetData(ref) : nullptr;
    const auto mesh = data ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(data->payload) : nullptr;
    if (!mesh || data->type != DataTypes::surfaceMesh) return {};
    const auto input = std::find_if(data->inputs.begin(), data->inputs.end(), [](const DataInputRef &i) {
        return i.role == "reference-source";
    });
    if (input == data->inputs.end()) return {};
    const auto source = graph.view->GetData(input->source);
    const auto reference = source ? std::dynamic_pointer_cast<const ReferencePayload>(source->payload) : nullptr;
    if (!reference || source->type != referenceType || reference->metadata.mesh != ref ||
        reference->metadata.coordinateFrame != mesh->GetCoordinateFrame() ||
        reference->metadata.pointCount != mesh->GetVertices().size() / 3 ||
        reference->metadata.triangleCount != mesh->GetTriangles().size() / 3) return {};
    return std::shared_ptr<const AlignmentReferenceSnapshot>(reference,&reference->metadata);
}
std::optional<AlignmentReferenceSnapshot> GetReference(const DataGraphSnapshot &graph,const DataRevisionRef &ref) {
    const auto reference=GetReferenceShared(graph,ref);
    return reference ? std::optional<AlignmentReferenceSnapshot>{*reference} : std::nullopt;
}
bool GetInputValid(const AlignmentInput &i) {
    if (!GetDataRevisionRefValid(i.source) || !GetDataRevisionRefValid(i.mesh) ||
        !GetDataRevisionRefValid(i.scopeData) || i.sourceFrameId.empty() ||
        i.sourceFrameId.size() > 128 || i.coordinateFrame.empty() ||
        i.coordinateFrame.size() > 128 || i.scope.empty() || i.scope.size() > 128 ||
        static_cast<unsigned>(i.unit) > 2)
        return false;
    return std::all_of(i.scope.begin(), i.scope.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}
bool GetExpectationsCurrent(const DataGraphSnapshot &graph,
                            const std::vector<DataExpectation> &expectations) {
    if (!graph.view)
        return false;
    for (const auto &e : expectations) {
        if (e.kind == DataExpectationKind::EntityHead) {
            const DataRevisionRef ref{e.entityId, e.expectedGeneration};
            if (!graph.view->GetData(ref))
                return false;
            if (ref.generation < std::numeric_limits<DataGeneration>::max() &&
                graph.view->GetData({ref.entityId, ref.generation + 1}))
                return false;
        } else {
            const auto binding =
                graph.view->GetDataBinding(e.binding).value_or(DataBinding{e.binding, {}, 0});
            if (binding.revision != e.expectedBindingRevision ||
                (e.isTargetChecked && binding.target != e.expectedTarget))
                return false;
        }
    }
    return true;
}
std::vector<DataInputRef> BuildInputs(const AlignmentInput &input, const DataRevisionRef &recipe,
                                      const DataRevisionRef &nominal) {
    return {{"source", input.source},
            {"mesh", input.mesh},
            {"scope", input.scopeData},
            {"recipe", recipe},
            {"nominal", nominal}};
}
std::vector<DataExpectation> BuildExpectations(const DataGraphSnapshot &graph,
                                               const AlignmentInput &input,
                                               const DataRevisionRef &recipe,
                                               const DataRevisionRef &nominal) {
    std::vector<DataExpectation> result;
    for (const auto &i : BuildInputs(input, recipe, nominal)) {
        // Restore 的配方尚未创建，只有显式映射的数据修订参与校验。
        if (i.role == "recipe" && !GetDataRevisionRefValid(recipe))
            continue;
        DataExpectation e;
        e.entityId = i.source.entityId;
        e.expectedGeneration = i.source.generation;
        result.push_back(e);
    }
    const std::vector<std::pair<std::string, DataRevisionRef>> bindings{
        {input.sourceBinding, input.source},
        {input.meshBinding, input.mesh},
        {input.scopeBinding, input.scopeData}};
    for (const auto &b : bindings)
        if (!b.first.empty()) {
            const auto current = graph.view->GetDataBinding(b.first);
            if (!current || current->target != b.second)
                throw std::invalid_argument("Input binding does not match the supplied revision.");
            DataExpectation e;
            e.kind = DataExpectationKind::Binding;
            e.binding = b.first;
            e.expectedBindingRevision = current->revision;
            e.isTargetChecked = true;
            e.expectedTarget = current->target;
            result.push_back(e);
        }
    if (!GetExpectationsCurrent(graph, result))
        throw std::invalid_argument("Input revision is stale or missing.");
    return result;
}
std::string GetBindingName(const std::string &scope) {
    return "metrology-alignment.active." + scope;
}
DataBinding GetBinding(const DataGraphSnapshot &graph, const std::string &scope) {
    const auto name = GetBindingName(scope);
    return graph.view->GetDataBinding(name).value_or(DataBinding{name, {}, 0});
}
DataTransaction BuildTransaction(TrustedDataPort &data, const AlignmentWork &work,
                                 const DataRevisionRef &recipe, const AlignmentCandidate &candidate,
                                 const std::vector<DataExpectation> &expectations,
                                 const DataBinding &active, bool isActivationRequested,
                                 DataRevisionRef &resultRef, DataRevisionRef &transformRef) {
    DataTransaction tx;
    tx.expectations = expectations;
    transformRef = {data.CreateDataEntityId(), 1};
    const DataRevisionRef geometryRef{data.CreateDataEntityId(), 1},
        residualRef{data.CreateDataEntityId(), 1};
    resultRef = {data.CreateDataEntityId(), 1};
    const auto inputs = BuildInputs(work.input, recipe, work.recipe.nominalData);
    const DataProvenance provenance{"metrology-alignment", "align", "metrology-alignment-3",
                                    "parameters: recipe input; initialization: result payload"};
    tx.outputs.push_back({transformRef.entityId, 0, DataTypes::transform3D, inputs,
                          std::make_shared<const Transform3DPayload>(candidate.sourceToTarget,
                                                                     work.input.sourceFrameId,
                                                                     work.recipe.targetFrameId),
                          provenance});
    tx.outputs.push_back({geometryRef.entityId, 0, geometryType, inputs,
                          std::make_shared<const GeometryPayload>(candidate.geometries),
                          provenance});
    std::vector<std::string> ids;
    std::vector<double> values, tolerances;
    std::vector<std::uint64_t> kinds, priorities;
    std::vector<std::uint8_t> used;
    const auto count = candidate.residuals.size();
    ids.reserve(count); values.reserve(count); tolerances.reserve(count);
    kinds.reserve(count); priorities.reserve(count); used.reserve(count);
    for (const auto &r : candidate.residuals) {
        ids.push_back(r.id);
        values.push_back(r.value);
        tolerances.push_back(r.tolerance);
        kinds.push_back(static_cast<std::uint64_t>(r.kind));
        priorities.push_back(r.priority);
        used.push_back(r.isUsed ? 1 : 0);
    }
    std::vector<RecordColumn> columns;
    columns.reserve(6);
    columns.push_back({"constraint", std::move(ids)});
    columns.push_back({"residual", std::move(values)});
    columns.push_back({"tolerance", std::move(tolerances)});
    columns.push_back({"kind", std::move(kinds)});
    columns.push_back({"priority", std::move(priorities)});
    columns.push_back({"used", std::move(used)});
    tx.outputs.push_back({residualRef.entityId, 0, DataTypes::recordTable, inputs,
                          std::make_shared<const RecordTablePayload>(
                              DataTypes::recordTable, "metrology-alignment-residuals-1", std::move(columns)),
                          provenance});
    ResultRecord record{work.input,  recipe,      transformRef,
                        geometryRef, residualRef, candidate.diagnostics,
                        candidate.initialPoses.empty() ? work.poses : candidate.initialPoses, expectations};
    auto resultInputs = inputs;
    resultInputs.push_back({"transform", transformRef});
    resultInputs.push_back({"geometry", geometryRef});
    resultInputs.push_back({"residuals", residualRef});
    tx.outputs.push_back({resultRef.entityId, 0, resultType, std::move(resultInputs),
                          std::make_shared<const ResultPayload>(std::move(record)), provenance});
    if (isActivationRequested && candidate.diagnostics.status == AlignmentStatus::FullyDetermined &&
        candidate.diagnostics.isQualityPassed)
        tx.bindings.push_back({active.name, active.revision, true, active.target, resultRef});
    return tx;
}
std::shared_ptr<const ResultPayload> GetResult(const DataGraphSnapshot &graph,
                                               const DataRevisionRef &ref) {
    if (!graph.view)
        return {};
    const auto data = graph.view->GetData(ref);
    if (!data || data->type != resultType)
        return {};
    const auto result = std::dynamic_pointer_cast<const ResultPayload>(data->payload);
    if (!result || !GetResultValid(result->record))
        return {};
    const auto transform = graph.view->GetData(result->record.transform),
               geometry = graph.view->GetData(result->record.geometry),
               residuals = graph.view->GetData(result->record.residuals),
               recipe = graph.view->GetData(result->record.recipe);
    if (!transform || !geometry || !residuals || !recipe ||
        transform->type != DataTypes::transform3D || geometry->type != geometryType ||
        residuals->type != DataTypes::recordTable || recipe->type != recipeType)
        return {};
    const auto matrix = std::dynamic_pointer_cast<const Transform3DPayload>(transform->payload);
    const auto recipeData = std::dynamic_pointer_cast<const RecipePayload>(recipe->payload);
    if (!matrix || !recipeData || !AlignmentMath::Rigid(matrix->GetSourceToTarget()) ||
        matrix->GetSourceFrame() != result->record.input.sourceFrameId ||
        matrix->GetTargetFrame() != recipeData->recipe.targetFrameId ||
        recipeData->recipe.unit != result->record.input.unit)
        return {};
    auto dependencies =
        BuildInputs(result->record.input, result->record.recipe, recipeData->recipe.nominalData);
    for (const auto &input : dependencies) {
        const auto expected =
            std::find_if(result->record.expectations.begin(), result->record.expectations.end(),
                         [&](const DataExpectation &e) {
                             return e.kind == DataExpectationKind::EntityHead &&
                                    e.use == DataExpectationUse::Required &&
                                    e.entityId == input.source.entityId &&
                                    e.expectedGeneration == input.source.generation;
                         });
        if (expected == result->record.expectations.end())
            return {};
        for (const auto &node : {data, transform, geometry, residuals}) {
            const auto found =
                std::find_if(node->inputs.begin(), node->inputs.end(), [&](const DataInputRef &e) {
                    return e.role == input.role && e.source == input.source;
                });
            if (found == node->inputs.end())
                return {};
        }
    }
    dependencies = {{"transform", result->record.transform},
                    {"geometry", result->record.geometry},
                    {"residuals", result->record.residuals}};
    for (const auto &input : dependencies) {
        if (std::none_of(data->inputs.begin(), data->inputs.end(), [&](const DataInputRef &e) {
                return e.role == input.role && e.source == input.source;
            }))
            return {};
    }
    const std::vector<std::pair<std::string, DataRevisionRef>> bindings{
        {result->record.input.sourceBinding, result->record.input.source},
        {result->record.input.meshBinding, result->record.input.mesh},
        {result->record.input.scopeBinding, result->record.input.scopeData}};
    for (const auto &binding : bindings) {
        if (!binding.first.empty() &&
            std::none_of(result->record.expectations.begin(), result->record.expectations.end(),
                         [&](const DataExpectation &e) {
                             return e.kind == DataExpectationKind::Binding &&
                                    e.binding == binding.first && e.isTargetChecked &&
                                    e.expectedTarget == binding.second;
                         }))
            return {};
    }
    return result;
}
} // namespace AlignmentData
