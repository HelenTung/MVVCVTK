#include "AlignmentResultSource.h"
#include <algorithm>

std::optional<AlignmentResultSource> AlignmentResultSource::Read(const DataGraphSnapshot &graph,
    const DataRevisionRef &ref,bool requiresReferenceSurface) {
    const auto alignment=AlignmentData::GetResult(graph,ref);
    if (!alignment || alignment->record.diagnostics.status!=AlignmentStatus::FullyDetermined ||
        !alignment->record.diagnostics.isQualityPassed ||
        !AlignmentData::GetExpectationsCurrent(graph,alignment->record.expectations)) return {};
    const auto recipe=std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(graph.view->GetData(alignment->record.recipe)->payload);
    const auto matrix=std::dynamic_pointer_cast<const Transform3DPayload>(graph.view->GetData(alignment->record.transform)->payload);
    const auto source=graph.view->GetData(alignment->record.input.mesh);
    const auto nominal=recipe ? graph.view->GetData(recipe->recipe.nominalData) : nullptr;
    if (!source || !nominal || !matrix) return {};
    AlignmentResultSource value;
    value.measuredMesh=std::dynamic_pointer_cast<const SurfaceMeshPayload>(source->payload);
    value.referenceMesh=std::dynamic_pointer_cast<const SurfaceMeshPayload>(nominal->payload);
    if (!value.measuredMesh || value.measuredMesh->GetCoordinateFrame()!=alignment->record.input.coordinateFrame ||
        (requiresReferenceSurface && !value.referenceMesh) || (value.referenceMesh &&
        value.referenceMesh->GetCoordinateFrame()!=value.measuredMesh->GetCoordinateFrame())) return {};
    const auto imported=AlignmentData::GetReference(graph,recipe->recipe.nominalData);
    const bool hasSource=std::any_of(nominal->inputs.begin(),nominal->inputs.end(),[](const auto &i) { return i.role=="reference-source"; });
    if ((hasSource && !imported) || (imported && (imported->unit!=recipe->recipe.unit || imported->frameId!=recipe->recipe.targetFrameId))) return {};
    value.input=alignment->record.input; value.alignment=ref; value.nominal=recipe->recipe.nominalData;
    value.transform=alignment->record.transform; value.sourceToTarget=matrix->GetSourceToTarget();
    value.expectations=alignment->record.expectations;
    for (const auto &input:{value.alignment,value.transform}) {
        DataExpectation e; e.entityId=input.entityId; e.expectedGeneration=input.generation; value.expectations.push_back(e);
    }
    if (!AlignmentData::GetExpectationsCurrent(graph,value.expectations)) return {};
    return value;
}
