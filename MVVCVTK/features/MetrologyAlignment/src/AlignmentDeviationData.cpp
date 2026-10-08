#include "AlignmentDeviationData.h"
#include "AlignmentData.h"
#include "AlignmentMath.h"
#include <algorithm>

namespace AlignmentDeviationDataStore {
namespace {
std::vector<DataInputRef> Inputs(const AlignmentDeviationSnapshot &s) {
    return {{"alignment",s.alignmentResult},{"mesh",s.sourceMesh},{"nominal",s.referenceMesh},{"transform",s.transform}};
}
bool FieldValid(const std::shared_ptr<const AlignmentDeviationField> &field) {
    if (!field || field->samples.empty() || !std::isfinite(field->statistics.selectedArea) ||
        field->statistics.selectedArea <= 0) return false;
    const auto &stats = field->statistics;
    if (!std::isfinite(stats.evaluatedArea) || stats.evaluatedArea < 0 || stats.evaluatedArea > stats.selectedArea ||
        !std::isfinite(stats.outOfToleranceArea) || stats.outOfToleranceArea < 0 || stats.outOfToleranceArea > stats.evaluatedArea ||
        !std::isfinite(stats.minimum) || !std::isfinite(stats.maximum) || !std::isfinite(stats.mean) ||
        !std::isfinite(stats.rms) || !std::isfinite(stats.absoluteP95)) return false;
    std::array<std::size_t,7> counts{};
    std::array<double,7> areas{};
    for (const auto &sample : field->samples) {
        if (static_cast<unsigned>(sample.state) > 6 || !std::isfinite(sample.area) || sample.area < 0 ||
            !std::isfinite(sample.distance) || sample.distance < 0 || !std::isfinite(sample.value) ||
            (sample.state == AlignmentDeviationState::NotSelected ? sample.area != 0 : sample.area <= 0)) return false;
        const auto state = static_cast<std::size_t>(sample.state);
        ++counts[state]; areas[state] += sample.area;
    }
    return counts == stats.counts && areas == stats.areas && stats.hasValues == (counts[1] > 0);
}
} // namespace
bool SetType(TrustedDataPort &data) {
    const DataTypeDescriptor descriptor{resultType, {{"org.mvvcvtk.metrology-alignment.deviation"}},
        [](const IDataPayload &input, std::string &message) {
            const auto *value = dynamic_cast<const Payload *>(&input);
            if (value) {
                const auto &s = value->snapshot;
                if (GetDataRevisionRefValid(s.result) && GetDataRevisionRefValid(s.alignmentResult) &&
                    GetDataRevisionRefValid(s.sourceMesh) && GetDataRevisionRefValid(s.referenceMesh) &&
                    GetDataRevisionRefValid(s.transform) && s.parameters.alignmentResult == s.alignmentResult &&
                    AlignmentDeviation::GetRequestValid(s.parameters) && AlignmentMath::Rigid(s.sourceToTarget) &&
                    static_cast<unsigned>(s.unit) <= 2 && static_cast<unsigned>(s.verdict) <= 3 &&
                    FieldValid(s.forward) && (s.parameters.reverseCoverage ? FieldValid(s.reverse) : !s.reverse)) return true;
            }
            message = "Invalid surface deviation result."; return false;
        }};
    if (data.SetDataType(descriptor)) return true;
    const auto graph = data.GetDataGraph();
    return graph.view && graph.view->GetDataFacets(resultType) == descriptor.facets;
}
std::optional<Context> Prepare(const DataGraphSnapshot &graph, const AlignmentDeviationRequest &request,
                              const AlignmentConfig &config) {
    if (!AlignmentDeviation::GetRequestValid(request)) return {};
    const auto alignment = AlignmentData::GetResult(graph, request.alignmentResult);
    if (!alignment || alignment->record.diagnostics.status != AlignmentStatus::FullyDetermined ||
        !alignment->record.diagnostics.isQualityPassed ||
        !AlignmentData::GetExpectationsCurrent(graph, alignment->record.expectations)) return {};
    const auto recipe = std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(
        graph.view->GetData(alignment->record.recipe)->payload);
    const auto matrix = std::dynamic_pointer_cast<const Transform3DPayload>(
        graph.view->GetData(alignment->record.transform)->payload);
    const auto measured = graph.view->GetData(alignment->record.input.mesh);
    const auto nominal = recipe ? graph.view->GetData(recipe->recipe.nominalData) : nullptr;
    if (!measured || !nominal || !matrix) return {};
    Context context;
    context.work.config = config;
    context.work.mesh = std::dynamic_pointer_cast<const SurfaceMeshPayload>(measured->payload);
    context.work.referenceMesh = std::dynamic_pointer_cast<const SurfaceMeshPayload>(nominal->payload);
    if (!context.work.mesh || !context.work.referenceMesh ||
        context.work.mesh->GetCoordinateFrame() != context.work.referenceMesh->GetCoordinateFrame()) return {};
    const auto imported = AlignmentData::GetReference(graph, recipe->recipe.nominalData);
    const bool hasSource = std::any_of(nominal->inputs.begin(), nominal->inputs.end(),
        [](const auto &input) { return input.role == "reference-source"; });
    if ((hasSource && !imported) || (imported && (imported->unit != recipe->recipe.unit ||
        imported->frameId != recipe->recipe.targetFrameId))) return {};
    auto &s = context.snapshot;
    s.alignmentResult = request.alignmentResult; s.sourceMesh = alignment->record.input.mesh;
    s.referenceMesh = recipe->recipe.nominalData; s.transform = alignment->record.transform;
    s.parameters = request; s.unit = alignment->record.input.unit; s.sourceToTarget = matrix->GetSourceToTarget();
    context.expectations = alignment->record.expectations;
    for (const auto &input : Inputs(s)) {
        DataExpectation expectation;
        expectation.entityId = input.source.entityId; expectation.expectedGeneration = input.source.generation;
        context.expectations.push_back(expectation);
    }
    if (!AlignmentData::GetExpectationsCurrent(graph, context.expectations)) return {};
    return context;
}
DataTransaction BuildTransaction(TrustedDataPort &data, Context &context, const AlignmentDeviationData &result,
                                 DataRevisionRef &resultRef) {
    auto &s = context.snapshot;
    resultRef = {data.CreateDataEntityId(),1}; s.result = resultRef;
    s.forward = result.forward; s.reverse = result.reverse; s.verdict = result.verdict;
    DataTransaction tx; tx.expectations = context.expectations;
    tx.outputs.push_back({resultRef.entityId,0,resultType,Inputs(s),
        std::make_shared<const Payload>(s, context.expectations),
        DataProvenance{"metrology-alignment","evaluate-surface","surface-deviation-1","vertex area quadrature; fixed alignment result"}});
    return tx;
}
std::optional<AlignmentDeviationSnapshot> GetResult(const DataGraphSnapshot &graph, const DataRevisionRef &ref) {
    const auto data = graph.view ? graph.view->GetData(ref) : nullptr;
    const auto payload = data && data->type == resultType ? std::dynamic_pointer_cast<const Payload>(data->payload) : nullptr;
    if (!payload || payload->snapshot.result != ref) return {};
    const auto &s = payload->snapshot;
    const auto alignment = AlignmentData::GetResult(graph, s.alignmentResult);
    if (!alignment || alignment->record.input.mesh != s.sourceMesh || alignment->record.transform != s.transform) return {};
    const auto recipe = std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(graph.view->GetData(alignment->record.recipe)->payload);
    const auto matrix = std::dynamic_pointer_cast<const Transform3DPayload>(graph.view->GetData(s.transform)->payload);
    if (!recipe || recipe->recipe.nominalData != s.referenceMesh || !matrix ||
        matrix->GetSourceToTarget() != s.sourceToTarget || alignment->record.input.unit != s.unit) return {};
    const auto sourceData = graph.view->GetData(s.sourceMesh), referenceData = graph.view->GetData(s.referenceMesh);
    const auto source = sourceData ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(sourceData->payload) : nullptr;
    const auto reference = referenceData ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(referenceData->payload) : nullptr;
    if (!source || !reference || !s.forward || s.forward->samples.size() != source->GetVertices().size()/3 ||
        (s.reverse && s.reverse->samples.size() != reference->GetVertices().size()/3)) return {};
    for (const auto &input : Inputs(s)) {
        if (std::none_of(data->inputs.begin(), data->inputs.end(), [&](const auto &i) { return i.role == input.role && i.source == input.source; }) ||
            std::none_of(payload->expectations.begin(), payload->expectations.end(), [&](const auto &e) {
                return e.kind == DataExpectationKind::EntityHead && e.use == DataExpectationUse::Required &&
                       e.entityId == input.source.entityId && e.expectedGeneration == input.source.generation;
            })) return {};
    }
    auto snapshot = s;
    snapshot.isCurrent = AlignmentData::GetExpectationsCurrent(graph, payload->expectations) &&
        AlignmentData::GetExpectationsCurrent(graph, alignment->record.expectations);
    return snapshot;
}
} // namespace AlignmentDeviationDataStore
