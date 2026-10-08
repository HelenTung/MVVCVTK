#include "AlignmentReport.h"
#include "AlignmentData.h"
#include "AlignmentInspectionData.h"
#include "AlignmentDeviationData.h"
#include "AlignmentVoxelVolumeData.h"
#include <set>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool value,const char *message) { if (!value) throw std::invalid_argument(message); }
struct Budget final {
    std::size_t limit,used=0;
    void Add(std::size_t count,std::size_t width=1) {
        if (count>(limit-used)/width) throw std::length_error("Report metadata exceeds working budget.");
        used+=count*width;
    }
    void Text(const std::string &text) { Add(text.size(),4); }
};
void MetadataBudget(Budget &budget,const ImageMetadata &m) {
    budget.Text(m.identity.datasetId);
    for (const auto *v:{&m.identity.inspectionId,&m.identity.objectId,&m.identity.batchId,&m.source.digest}) if (*v) budget.Text(**v);
    budget.Text(m.source.uri); budget.Text(m.scalar.quantity); budget.Text(m.scalar.unit);
    budget.Add(m.attributes.size(),4*sizeof(MetadataAttribute));
    for (const auto &a:m.attributes) { budget.Text(a.key); if (const auto *text=std::get_if<std::string>(&a.value)) budget.Text(*text); }
}
bool Current(const DataGraphSnapshot &graph,const DataRevisionRef &ref) {
    return ref.generation==std::numeric_limits<DataGeneration>::max() || !graph.view->GetData({ref.entityId,ref.generation+1});
}
}

MetrologyReportResult AlignmentReport::Capture(const DataGraphSnapshot &graph,const MetrologyReportRequest &request,
                                              const AlignmentConfig &config) {
    MetrologyReportResult output;
    try {
        Require(graph.view && !request.id.empty() && request.id.size()<=128 && request.title.size()<=1024 &&
            (!request.alignments.empty() || !request.inspections.empty() || !request.deviations.empty() || !request.voxelVolumes.empty()),"Invalid report request.");
        Budget budget{config.workingBytes.value_or(std::numeric_limits<std::size_t>::max())};
        budget.Add(4096); budget.Text(request.id); budget.Text(request.title);
        // 每项预留包括增长中同时存活的 vector 缓冲、去重节点和小快照。
        for (const auto *refs:{&request.alignments,&request.inspections,&request.deviations,&request.voxelVolumes}) {
            budget.Add(refs->size(),4096); std::set<DataRevisionRef> unique;
            for (const auto &ref:*refs) Require(GetDataRevisionRefValid(ref) && unique.insert(ref).second,"Invalid or duplicate report result revision.");
        }
        auto report=std::make_shared<MetrologyReport>(); report->id=request.id; report->title=request.title;
        report->capturedCommit=graph.commitId;
        report->capturedUnixMilliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::set<DataRevisionRef> pendingAlignments(request.alignments.begin(),request.alignments.end());
        std::set<DataRevisionRef> visited;
        std::vector<DataRevisionRef> pendingInputs;
        const auto addInput=[&](const DataRevisionRef &ref) {
            if (visited.find(ref)!=visited.end()) return;
            Require(GetDataRevisionRefValid(ref),"Report lineage contains an invalid revision.");
            budget.Add(4096); visited.insert(ref); pendingInputs.push_back(ref);
        };
        report->inspections.reserve(request.inspections.size()); report->deviations.reserve(request.deviations.size());
        for (const auto &ref:request.inspections) {
            const auto snapshot=AlignmentInspectionData::GetResult(graph,ref);
            Require(snapshot.has_value(),"Inspection report source is missing or inconsistent.");
            pendingAlignments.insert(snapshot->alignmentResult); addInput(ref);
            report->wereInputsCurrentAtCapture=report->wereInputsCurrentAtCapture && snapshot->isCurrent;
            report->inspections.push_back(*snapshot);
        }
        for (const auto &ref:request.deviations) {
            const auto snapshot=AlignmentDeviationDataStore::GetResult(graph,ref);
            Require(snapshot.has_value(),"Deviation report source is missing or inconsistent.");
            pendingAlignments.insert(snapshot->alignmentResult); addInput(ref);
            report->wereInputsCurrentAtCapture=report->wereInputsCurrentAtCapture && snapshot->isCurrent;
            report->deviations.push_back(*snapshot);
        }
        report->voxelVolumes.reserve(request.voxelVolumes.size());
        for (const auto &ref:request.voxelVolumes) {
            const auto snapshot=AlignmentVoxelVolumeData::GetResult(graph,ref);
            Require(snapshot.has_value(),"Voxel volume report source is missing or inconsistent.");
            budget.Text(snapshot->geometry.coordinateFrame); budget.Text(snapshot->algorithmVersion);
            addInput(ref);
            report->wereInputsCurrentAtCapture=report->wereInputsCurrentAtCapture && snapshot->isCurrent;
            report->voxelVolumes.push_back(*snapshot);
        }
        for (const auto &ref:pendingAlignments) {
            budget.Add(4096); addInput(ref);
            const auto result=AlignmentData::GetResult(graph,ref);
            Require(bool(result),"Alignment report source is missing or inconsistent.");
            const auto &r=result->record;
            const auto recipe=std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(graph.view->GetData(r.recipe)->payload);
            const auto geometry=std::dynamic_pointer_cast<const AlignmentData::GeometryPayload>(graph.view->GetData(r.geometry)->payload);
            const auto residuals=std::dynamic_pointer_cast<const RecordTablePayload>(graph.view->GetData(r.residuals)->payload);
            const auto transform=std::dynamic_pointer_cast<const Transform3DPayload>(graph.view->GetData(r.transform)->payload);
            Require(recipe && geometry && residuals && transform,"Alignment report payload is unavailable.");
            for (const auto *s:{&r.algorithmVersion,&r.input.sourceFrameId,&r.input.coordinateFrame,&r.input.scope,
                               &r.input.sourceBinding,&r.input.meshBinding,&r.input.scopeBinding}) budget.Text(*s);
            MetrologyReportAlignment entry; entry.result=ref; entry.recipeRevision=r.recipe; entry.transform=r.transform;
            entry.input=r.input; entry.sourceToTarget=transform->GetSourceToTarget(); entry.algorithmVersion=r.algorithmVersion;
            entry.recipe=std::shared_ptr<const AlignmentRecipe>(recipe,&recipe->recipe);
            entry.geometries=std::shared_ptr<const std::vector<AlignmentGeometry>>(geometry,&geometry->geometries);
            entry.residuals=residuals;
            entry.diagnostics=std::shared_ptr<const AlignmentDiagnostics>(result,&r.diagnostics);
            entry.initialPoses=std::shared_ptr<const std::vector<AlignmentMatrix>>(result,&r.initialPoses);
            entry.reference=AlignmentData::GetReferenceShared(graph,recipe->recipe.nominalData);
            entry.wasCurrentAtCapture=AlignmentData::GetExpectationsCurrent(graph,r.expectations);
            report->wereInputsCurrentAtCapture=report->wereInputsCurrentAtCapture && entry.wasCurrentAtCapture;
            report->alignments.push_back(std::move(entry));
        }
        for (std::size_t index=0;index<pendingInputs.size();++index) {
            const auto node=graph.view->GetData(pendingInputs[index]);
            Require(node && node->payload,"Report source lineage is incomplete.");
            budget.Text(node->type.name); budget.Add(node->inputs.size(),4*sizeof(DataInputRef));
            for (const auto &input:node->inputs) { budget.Text(input.role); addInput(input.source); }
            if (node->provenance) for (const auto *s:{&node->provenance->producerId,&node->provenance->operationId,
                &node->provenance->algorithmVersion,&node->provenance->canonicalParameters}) budget.Text(*s);
            MetrologyReportInput entry; entry.revision=node->self; entry.type=node->type;
            entry.inputs=node->inputs; entry.provenance=node->provenance; entry.wasCurrentAtCapture=Current(graph,node->self);
            if (const auto *image=dynamic_cast<const ImageGrid3DPayload *>(node->payload.get())) {
                MetadataBudget(budget,image->GetMetadata()); budget.Text(image->GetGeometry().coordinateFrame);
                entry.image=MetrologyReportImage{image->GetGeometry(),image->GetMetadata(),image->GetValueType(),
                    image->GetComponentCount(),image->GetValues() ? image->GetValues()->size() : 0,
                    image->GetScalarRange(),bool(image->GetValidityMask())};
            } else if (const auto *mesh=dynamic_cast<const SurfaceMeshPayload *>(node->payload.get())) {
                budget.Text(mesh->GetCoordinateFrame());
                entry.mesh=MetrologyReportMesh{mesh->GetVertices().size()/3,mesh->GetTriangles().size()/3,mesh->GetCoordinateFrame()};
            }
            report->wereInputsCurrentAtCapture=report->wereInputsCurrentAtCapture && entry.wasCurrentAtCapture;
            report->inputs.push_back(std::move(entry));
        }
        report->estimatedMetadataBytes=budget.used; output.report=std::move(report); output.status=AlignmentStatus::FullyDetermined;
    } catch (const std::length_error &e) { output.status=AlignmentStatus::BudgetExceeded; output.message=e.what(); }
      catch (const std::bad_alloc &) { output.status=AlignmentStatus::BudgetExceeded; }
      catch (const std::invalid_argument &e) { output.status=AlignmentStatus::InvalidInput; output.message=e.what(); }
      catch (const std::exception &e) { output.status=AlignmentStatus::InternalError; output.message=e.what(); }
    return output;
}
