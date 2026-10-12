#include "AlignmentInspectionData.h"
#include "AlignmentResultSource.h"
#include "AlignmentMath.h"

namespace AlignmentInspectionData {
namespace {
std::vector<DataInputRef> Inputs(const InspectionSnapshot &s) {
    return {{"alignment",s.alignmentResult},{"mesh",s.sourceMesh},{"nominal",s.referenceData},{"transform",s.transform}};
}
bool ReportValid(const InspectionSnapshot &s) {
    if (!s.plan || !s.report || !AlignmentInspection::GetPlanValid(*s.plan,{}) || s.plan->alignmentResult!=s.alignmentResult ||
        (s.report->algorithmVersion!="metrology-inspection-1" && s.report->algorithmVersion!="metrology-inspection-2") || s.report->evaluatedUnixMilliseconds<=0 ||
        s.report->geometries.size()!=s.plan->geometries.size() || s.report->items.size()!=s.plan->items.size() ||
        static_cast<unsigned>(s.report->verdict)>3 || s.report->verdict!=AlignmentInspection::GetVerdict(s.report->items)) return false;
    for (std::size_t i=0;i<s.report->geometries.size();++i) {
        const auto &g=s.report->geometries[i]; const auto &spec=s.plan->geometries[i];
        if (g.source!=spec.source || g.fitted.id!=spec.fit.id || g.fitted.kind!=spec.fit.kind ||
            (g.status!=AlignmentStatus::FullyDetermined && g.status!=AlignmentStatus::Degenerate)) return false;
        if (!AlignmentGeometryFit::GetSupportValid(g.fitted) ||
            (g.source==InspectionGeometrySource::NominalDefinition && g.fitted.support)) return false;
        if ((spec.section && s.report->algorithmVersion!="metrology-inspection-2") || (g.section && !spec.section) ||
            (spec.section && g.status==AlignmentStatus::FullyDetermined && !g.section)) return false;
        if (g.section) {
            const auto &trace=*g.section;
            if (!AlignmentMath::Finite(AlignmentMath::Vector(trace.sourcePlanePoint)) ||
                !AlignmentMath::GetUnitDirectionValid(trace.sourcePlaneNormal) || trace.components==0 || trace.closedLoops==0 ||
                trace.closedLoops>trace.components || trace.selectedSegments<3 || trace.sourceTriangleCount==0 ||
                !AlignmentMath::Positive(trace.perimeter) || !AlignmentMath::Positive(trace.sampleSpacing) ||
                trace.samples.size()!=g.fitted.sampleCount || trace.samples.size()<8 ||
                !std::isfinite(trace.maxPlaneCorrection) || trace.maxPlaneCorrection<0 ||
                trace.maxPlaneCorrection>2*spec.section->planeTolerance) return false;
            for (const auto &sample:trace.samples) {
                double sum=0;
                for (auto weight:sample.barycentric) {
                    if (!std::isfinite(weight) || weight< -1e-10 || weight>1+1e-10) return false;
                    sum+=weight;
                }
                if (sample.triangle>=trace.sourceTriangleCount || std::abs(sum-1)>1e-10 || !std::isfinite(sample.planeCorrection) ||
                    std::abs(sample.planeCorrection)>trace.maxPlaneCorrection) return false;
            }
        }
        if (g.status==AlignmentStatus::FullyDetermined &&
            (!AlignmentMath::Finite(AlignmentMath::Vector(g.alignedCenter)) || !AlignmentMath::GetUnitDirectionValid(g.alignedDirection) ||
             !std::isfinite(g.fitted.radius) || g.fitted.radius<0 ||
             !AlignmentMath::GetFitStatisticsValid(g.fitted.fitRms, g.fitted.fitMax) ||
             (g.source!=InspectionGeometrySource::NominalDefinition && g.fitted.sampleCount==0))) return false;
    }
    for (std::size_t i=0;i<s.report->items.size();++i) {
        const auto &row=s.report->items[i]; const auto &spec=s.plan->items[i];
        if (row.id!=spec.id || static_cast<unsigned>(row.verdict)>3 ||
            (row.measured && !std::isfinite(*row.measured)) || (row.nominal && !std::isfinite(*row.nominal)) ||
            (row.deviation && (!row.measured || !row.nominal || !std::isfinite(*row.deviation) ||
                *row.deviation!=*row.measured-*row.nominal))) return false;
        if ((row.verdict==AlignmentInspectionVerdict::NotEvaluated && (!row.measured || spec.deviationLimits)) ||
            (row.verdict==AlignmentInspectionVerdict::Indeterminate && (row.deviation || row.message.empty()))) return false;
        if ((row.verdict==AlignmentInspectionVerdict::Pass || row.verdict==AlignmentInspectionVerdict::Fail) &&
            (!spec.deviationLimits || !row.deviation ||
             (row.verdict==AlignmentInspectionVerdict::Pass)!=(*row.deviation>=(*spec.deviationLimits)[0] && *row.deviation<=(*spec.deviationLimits)[1]))) return false;
    }
    return true;
}
} // namespace
bool SetType(TrustedDataPort &data) {
    const DataTypeDescriptor descriptor{resultType,{{"org.mvvcvtk.metrology-alignment.inspection"}},
        [](const IDataPayload &p,std::string &message) {
            const auto *value=dynamic_cast<const Payload *>(&p);
            if (value && GetDataRevisionRefValid(value->snapshot.result) && GetDataRevisionRefValid(value->snapshot.sourceMesh) &&
                GetDataRevisionRefValid(value->snapshot.referenceData) && GetDataRevisionRefValid(value->snapshot.transform) &&
                AlignmentMath::Rigid(value->snapshot.sourceToTarget) && static_cast<unsigned>(value->snapshot.unit)<=2 && ReportValid(value->snapshot)) return true;
            message="Invalid inspection report."; return false;
        }};
    if (data.SetDataType(descriptor)) return true;
    const auto graph=data.GetDataGraph(); return graph.view && graph.view->GetDataFacets(resultType)==descriptor.facets;
}
std::optional<Context> Prepare(const DataGraphSnapshot &graph,InspectionPlan plan,const AlignmentConfig &config) {
    if (!AlignmentInspection::GetPlanValid(plan,config)) return {};
    const bool needsSurface=std::any_of(plan.geometries.begin(),plan.geometries.end(),[](const auto &g) { return g.source==InspectionGeometrySource::ReferenceSurface; });
    const auto source=AlignmentResultSource::Read(graph,plan.alignmentResult,needsSurface);
    if (!source) return {};
    for (const auto &g:plan.geometries) if (g.source!=InspectionGeometrySource::NominalDefinition && !g.fit.region.vertexIds.empty() &&
        g.fit.region.pinnedMesh!=(g.source==InspectionGeometrySource::MeasuredSurface ? source->input.mesh : source->nominal)) return {};
    for (const auto &g:plan.geometries) if (g.section &&
        g.section->pinnedMesh!=(g.source==InspectionGeometrySource::MeasuredSurface ? source->input.mesh : source->nominal)) return {};
    Context context; context.work.input=source->input; context.work.config=config;
    context.work.mesh=source->measuredMesh; context.work.referenceMesh=source->referenceMesh; context.work.recipe.nominalData=source->nominal;
    context.expectations=source->expectations;
    auto &s=context.snapshot; s.alignmentResult=source->alignment; s.sourceMesh=source->input.mesh;
    s.referenceData=source->nominal; s.transform=source->transform; s.sourceToTarget=source->sourceToTarget; s.unit=source->input.unit;
    s.plan=std::make_shared<const InspectionPlan>(std::move(plan)); return context;
}
DataTransaction BuildTransaction(TrustedDataPort &data,Context &context,const InspectionData &result,DataRevisionRef &ref) {
    ref={data.CreateDataEntityId(),1}; context.snapshot.result=ref; context.snapshot.report=result.report;
    DataTransaction tx; tx.expectations=context.expectations;
    tx.outputs.push_back({ref.entityId,0,resultType,Inputs(context.snapshot),
        std::make_shared<const Payload>(context.snapshot,context.expectations),
        DataProvenance{"metrology-alignment","inspect",result.report->algorithmVersion,"independent fitting and typed scalar requirements"}});
    return tx;
}
std::optional<InspectionSnapshot> GetResult(const DataGraphSnapshot &graph,const DataRevisionRef &ref) {
    const auto node=graph.view ? graph.view->GetData(ref) : nullptr;
    const auto payload=node && node->type==resultType ? std::dynamic_pointer_cast<const Payload>(node->payload) : nullptr;
    if (!payload || payload->snapshot.result!=ref) return {};
    const auto &s=payload->snapshot; const auto alignment=AlignmentData::GetResult(graph,s.alignmentResult);
    if (!alignment || alignment->record.input.mesh!=s.sourceMesh || alignment->record.transform!=s.transform || alignment->record.input.unit!=s.unit) return {};
    const auto recipe=std::dynamic_pointer_cast<const AlignmentData::RecipePayload>(graph.view->GetData(alignment->record.recipe)->payload);
    const auto matrix=std::dynamic_pointer_cast<const Transform3DPayload>(graph.view->GetData(s.transform)->payload);
    if (!recipe || recipe->recipe.nominalData!=s.referenceData || !matrix || matrix->GetSourceToTarget()!=s.sourceToTarget) return {};
    // 逐样本范围在发布时验证；读取仅核对不可变来源规模，避免轮询反复遍历整条截线。
    for (const auto &g:s.report->geometries) if (g.section) {
        const auto meshRef=g.source==InspectionGeometrySource::MeasuredSurface ? s.sourceMesh : s.referenceData;
        const auto source=graph.view->GetData(meshRef);
        const auto *mesh=source ? dynamic_cast<const SurfaceMeshPayload *>(source->payload.get()) : nullptr;
        if (!mesh || g.section->sourceTriangleCount!=mesh->GetTriangles().size()/3) return {};
    }
    for (const auto &input:Inputs(s)) {
        if (std::none_of(node->inputs.begin(),node->inputs.end(),[&](const auto &i) { return i.role==input.role && i.source==input.source; }) ||
            std::none_of(payload->expectations.begin(),payload->expectations.end(),[&](const auto &e) {
                return e.kind==DataExpectationKind::EntityHead && e.use==DataExpectationUse::Required && e.entityId==input.source.entityId &&
                    e.expectedGeneration==input.source.generation;
            })) return {};
    }
    auto result=s; result.isCurrent=AlignmentData::GetExpectationsCurrent(graph,payload->expectations) &&
        AlignmentData::GetExpectationsCurrent(graph,alignment->record.expectations); return result;
}
} // namespace AlignmentInspectionData
