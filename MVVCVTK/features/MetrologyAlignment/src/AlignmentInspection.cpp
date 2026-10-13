#include "AlignmentInspection.h"
#include "AlignmentSection.h"
#include "AlignmentMath.h"
#include <set>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
void Require(bool value,const char *message) { if (!value) throw std::invalid_argument(message); }
void Check(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work)!=AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Inspection cancelled.");
}
bool Round(AlignmentGeometryKind kind) {
    return kind==AlignmentGeometryKind::Circle || kind==AlignmentGeometryKind::Sphere || kind==AlignmentGeometryKind::Cylinder;
}
bool Axis(AlignmentGeometryKind kind) { return kind==AlignmentGeometryKind::Line || kind==AlignmentGeometryKind::Cylinder; }
bool Center(AlignmentGeometryKind kind) {
    return kind==AlignmentGeometryKind::Point || kind==AlignmentGeometryKind::Circle || kind==AlignmentGeometryKind::Sphere;
}
bool HasDirection(AlignmentGeometryKind kind) {
    return Axis(kind) || kind==AlignmentGeometryKind::Plane || kind==AlignmentGeometryKind::Circle;
}
bool MetadataFits(const InspectionPlan &plan,const AlignmentConfig &config) {
    const auto count=std::numeric_limits<std::size_t>::max()/4096;
    return plan.geometries.size()<=count && plan.items.size()<=count-plan.geometries.size();
}
bool MeasureValid(const InspectionMeasure &m,const InspectionPlan &plan,bool nominal) {
    if (static_cast<unsigned>(m.metric)>9 || m.first.geometry>=plan.geometries.size() ||
        (m.second && m.second->geometry>=plan.geometries.size()) ||
        !Positive(m.maxParallelAngleDegrees) || m.maxParallelAngleDegrees>=90) return false;
    const bool pair=m.metric==InspectionMetric::CenterDistance ||
        m.metric==InspectionMetric::OrientedDirectionAngleDegrees || m.metric==InspectionMetric::SignedParallelPlaneDistance;
    if (pair!=m.second.has_value()) return false;
    const bool point=m.metric==InspectionMetric::CenterX || m.metric==InspectionMetric::CenterY ||
        m.metric==InspectionMetric::CenterZ || m.metric==InspectionMetric::CenterDistance;
    const auto operandValid=[&](const InspectionOperand &o) {
        const auto &spec=plan.geometries[o.geometry];
        if (nominal && spec.source==InspectionGeometrySource::MeasuredSurface) return false;
        if (o.sectionPlane && (!point || !Axis(spec.fit.kind) || *o.sectionPlane>=plan.geometries.size() ||
            plan.geometries[*o.sectionPlane].fit.kind!=AlignmentGeometryKind::Plane ||
            (nominal && plan.geometries[*o.sectionPlane].source==InspectionGeometrySource::MeasuredSurface))) return false;
        return !point || Center(spec.fit.kind) || (Axis(spec.fit.kind) && o.sectionPlane);
    };
    if (!operandValid(m.first) || (m.second && !operandValid(*m.second))) return false;
    const auto kind=plan.geometries[m.first.geometry].fit.kind;
    switch (m.metric) {
    case InspectionMetric::Radius: case InspectionMetric::Diameter: return Round(kind);
    case InspectionMetric::OrientedDirectionAngleDegrees:
        return HasDirection(kind) && HasDirection(plan.geometries[m.second->geometry].fit.kind);
    case InspectionMetric::SignedParallelPlaneDistance:
        return kind==AlignmentGeometryKind::Plane && plan.geometries[m.second->geometry].fit.kind==AlignmentGeometryKind::Plane;
    case InspectionMetric::FitRms: case InspectionMetric::FitMaximum:
        return plan.geometries[m.first.geometry].source!=InspectionGeometrySource::NominalDefinition;
    default: return true;
    }
}
const InspectionGeometry &Geometry(const std::vector<InspectionGeometry> &geometries,std::size_t index) {
    const auto &g=geometries.at(index);
    Require(g.status==AlignmentStatus::FullyDetermined,"A required geometry is unavailable.");
    return g;
}
Vec PointAt(const std::vector<InspectionGeometry> &geometries,const InspectionOperand &operand) {
    const auto &g=Geometry(geometries,operand.geometry);
    if (!operand.sectionPlane) return Vector(g.alignedCenter);
    const auto &plane=Geometry(geometries,*operand.sectionPlane);
    const auto axis=Vector(g.alignedDirection), normal=Vector(plane.alignedDirection);
    const double denominator=axis.dot(normal);
    Require(std::abs(denominator)>1e-8,"Axis is parallel or numerically tangent to its construction section.");
    return Vector(g.alignedCenter)+axis*(Vector(plane.alignedCenter)-Vector(g.alignedCenter)).dot(normal)/denominator;
}
double Value(const InspectionMeasure &m,const std::vector<InspectionGeometry> &geometries) {
    const auto &g=Geometry(geometries,m.first.geometry);
    switch (m.metric) {
    case InspectionMetric::Radius: return g.fitted.radius;
    case InspectionMetric::Diameter: return 2*g.fitted.radius;
    case InspectionMetric::CenterX: return PointAt(geometries,m.first)[0];
    case InspectionMetric::CenterY: return PointAt(geometries,m.first)[1];
    case InspectionMetric::CenterZ: return PointAt(geometries,m.first)[2];
    case InspectionMetric::CenterDistance: return cv::norm(PointAt(geometries,m.first)-PointAt(geometries,*m.second));
    case InspectionMetric::FitRms: return g.fitted.fitRms;
    case InspectionMetric::FitMaximum: return g.fitted.fitMax;
    case InspectionMetric::OrientedDirectionAngleDegrees:
        return std::acos(std::clamp(Vector(g.alignedDirection).dot(Vector(Geometry(geometries,m.second->geometry).alignedDirection)),-1.0,1.0))*180/std::acos(-1.0);
    case InspectionMetric::SignedParallelPlaneDistance: {
        const auto &other=Geometry(geometries,m.second->geometry);
        const auto n=Vector(g.alignedDirection);
        const double angle=std::acos(std::clamp(std::abs(n.dot(Vector(other.alignedDirection))),0.0,1.0))*180/std::acos(-1.0);
        Require(angle<=m.maxParallelAngleDegrees,"Plane separation requires parallel planes within the declared angular gate.");
        return (Vector(other.alignedCenter)-Vector(g.alignedCenter)).dot(n);
    }
    }
    throw std::invalid_argument("Unknown inspection quantity.");
}
AlignmentInspectionVerdict Overall(const std::vector<InspectionItemResult> &items) {
    auto verdict=AlignmentInspectionVerdict::Pass;
    for (const auto &item:items) {
        if (item.verdict==AlignmentInspectionVerdict::Fail) return AlignmentInspectionVerdict::Fail;
        if (item.verdict==AlignmentInspectionVerdict::Indeterminate) verdict=item.verdict;
        else if (item.verdict==AlignmentInspectionVerdict::NotEvaluated && verdict==AlignmentInspectionVerdict::Pass) verdict=item.verdict;
    }
    return verdict;
}
} // namespace

bool AlignmentInspection::GetPlanValid(const InspectionPlan &p,const AlignmentConfig &config) {
    if (p.id.empty() || p.id.size()>128 || !GetDataRevisionRefValid(p.alignmentResult) || p.geometries.empty() || p.items.empty() ||
        !MetadataFits(p,config) ||
        !Positive(p.lengthScale) || !Positive(p.rankTolerance) || p.rankTolerance>=.01 ||
        !Positive(p.conditionLimit) || p.conditionLimit<=1 || !Positive(p.solveTolerance) || p.iterationLimit==0 || p.iterationLimit>1000 ||
        !std::isfinite(p.minSupportRatio) || p.minSupportRatio<0 || p.minSupportRatio>1 ||
        !Positive(p.maxLocalizationSigma) || !Positive(p.maxSurfaceFitResidual)) return false;
    std::set<std::string> ids;
    for (const auto &g:p.geometries) {
        if (g.fit.id.empty() || g.fit.id.size()>128 || !ids.insert(g.fit.id).second || static_cast<unsigned>(g.source)>2 ||
            static_cast<unsigned>(g.fit.kind)>5 || g.definitionSource.size()>1024 || g.fit.association!=AlignmentAssociation::LeastSquares) return false;
        if (g.source==InspectionGeometrySource::NominalDefinition) {
            if (g.section || !g.ideal || g.definitionSource.empty() || !Finite(Vector(g.ideal->center)) || !GetUnitDirectionValid(g.ideal->direction) ||
                !g.fit.region.vertexIds.empty() || g.fit.region.targetBounds || g.fit.region.roi ||
                (Round(g.fit.kind) ? !Positive(g.ideal->radius) : g.ideal->radius!=0)) return false;
        } else if (g.ideal || !AlignmentGeometryFit::GetSpecValid(g.fit,!g.section || g.fit.region.roi.has_value() || g.fit.region.targetBounds.has_value()) ||
            (g.section && (!g.fit.region.vertexIds.empty() || g.fit.kind!=AlignmentGeometryKind::Circle || !AlignmentSection::GetValid(*g.section)))) return false;
    }
    ids.clear();
    for (const auto &item:p.items) {
        if (item.id.empty() || item.id.size()>128 || !ids.insert(item.id).second || item.name.size()>1024 || item.nominalSource.size()>1024 ||
            !MeasureValid(item.measured,p,false) ||
            p.geometries[item.measured.first.geometry].source!=InspectionGeometrySource::MeasuredSurface ||
            (item.nominal && (!std::isfinite(*item.nominal) || item.nominalSource.empty())) || (item.nominal && item.nominalMeasure) ||
            (item.nominalMeasure && (item.nominalMeasure->metric!=item.measured.metric || !MeasureValid(*item.nominalMeasure,p,true))) ||
            (item.deviationLimits && ((!item.nominal && !item.nominalMeasure) || !std::isfinite((*item.deviationLimits)[0]) ||
             !std::isfinite((*item.deviationLimits)[1]) || (*item.deviationLimits)[0]>(*item.deviationLimits)[1]))) return false;
    }
    return true;
}
AlignmentInspectionVerdict AlignmentInspection::GetVerdict(const std::vector<InspectionItemResult> &items) {
    return Overall(items);
}
InspectionData AlignmentInspection::Build(const AlignmentWork &input,const InspectionPlan &plan,const AlignmentMatrix &pose) {
    InspectionData output;
    try {
        Check(input);
        if (!MetadataFits(plan,input.config)) throw std::length_error("Inspection metadata size accounting overflows.");
        Require(GetPlanValid(plan,input.config) && Rigid(pose) && input.mesh,"Invalid inspection input.");
        const auto limit=std::numeric_limits<std::size_t>::max();
        constexpr std::size_t metadataBytes=4096;
        if (plan.geometries.size()>std::numeric_limits<std::size_t>::max()-plan.items.size() ||
            plan.geometries.size()+plan.items.size()>limit/metadataBytes) throw std::length_error("Inspection metadata size accounting overflows.");
        AlignmentWork work; work.config=input.config; work.cancelled=input.cancelled;
        work.recipe.lengthScale=plan.lengthScale; work.recipe.rankTolerance=plan.rankTolerance; work.recipe.conditionLimit=plan.conditionLimit;
        work.recipe.solveTolerance=plan.solveTolerance; work.recipe.iterationLimit=plan.iterationLimit;
        work.recipe.minSupportRatio=plan.minSupportRatio; work.recipe.maxLocalizationSigma=plan.maxLocalizationSigma;
        work.recipe.maxSurfaceFitResidual=plan.maxSurfaceFitResidual;
        auto report=std::make_shared<InspectionReport>(); report->geometries.reserve(plan.geometries.size()); report->items.reserve(plan.items.size());
        std::size_t total=0;
        for (const auto &spec:plan.geometries) {
            Check(work);
            InspectionGeometry geometry; geometry.source=spec.source; geometry.fitted.id=spec.fit.id; geometry.fitted.kind=spec.fit.kind;
            const bool nominal=spec.source==InspectionGeometrySource::NominalDefinition;
            const bool reference=spec.source==InspectionGeometrySource::ReferenceSurface;
            work.mesh=reference ? input.referenceMesh : input.mesh;
            work.input.mesh=reference ? input.recipe.nominalData : input.input.mesh;
            work.recipe.requiresMeasurementQuality=!reference && plan.requiresMeasurementQuality;
            const auto transform=reference || nominal ? alignmentIdentity : pose;
            if (!nominal) {
                Require(work.mesh!=nullptr,"Reference surface for nominal fitting is unavailable.");
                Require((spec.fit.region.vertexIds.empty() && !spec.fit.region.roi) || spec.fit.region.pinnedMesh==work.input.mesh,"Inspection ROI belongs to another mesh revision.");
                Require(!spec.section || spec.section->pinnedMesh==work.input.mesh,"Inspection section belongs to another mesh revision.");
            }
            try {
                if (nominal) {
                    geometry.fitted.sourceCenter=spec.ideal->center; geometry.fitted.sourceDirection=spec.ideal->direction;
                    geometry.fitted.radius=spec.ideal->radius;
                } else {
                    AlignmentSamples samples;
                    std::optional<AlignmentGeometrySpec> sectionFit;
                    const auto *fit=&spec.fit;
                    if (spec.section) {
                        auto section=AlignmentSection::Build(work,*spec.section,transform);
                        samples=std::move(section.samples); geometry.section=std::move(section.trace);
                        if(spec.fit.region.roi || spec.fit.region.targetBounds){
                            auto trace=std::make_shared<InspectionSectionTrace>(*geometry.section);AlignmentSamples selected;trace->parentSampleCount=trace->samples.size();trace->samples.clear();
                            RoiReadSnapshot roi;if(spec.fit.region.roi){const auto found=work.rois.find(*spec.fit.region.roi);Require(found!=work.rois.end(),"Inspection section ROI is unavailable.");roi=found->second;}
                            for(std::size_t i=0;i<samples.points.size();++i){const auto& p=samples.points[i];bool inside=!roi || roi->GetContains(Point(Transform(spec.fit.region.meshToRoi,Vector(p))));
                                if(spec.fit.region.targetBounds){const auto q=Transform(transform,Vector(p));for(int a=0;a<3;++a)if(q[a]<(*spec.fit.region.targetBounds)[a*2] || q[a]>(*spec.fit.region.targetBounds)[a*2+1])inside=false;}
                                if(inside){selected.points.push_back(p);trace->samples.push_back(geometry.section->samples[i]);}else ++selected.rejected;
                            }
                            trace->isPartialSelection=trace->samples.size()!=trace->parentSampleCount;samples=std::move(selected);geometry.section=std::move(trace);
                        }
                        sectionFit=spec.fit; sectionFit->targetDirection=spec.section->planeNormal;
                        fit=&*sectionFit;
                    } else samples=AlignmentGeometryFit::BuildSamples(work,fit->region,transform);
                    if (samples.points.size()>std::numeric_limits<std::size_t>::max()-total) throw std::length_error("Inspection sample count overflows.");
                    total+=samples.points.size();
                    geometry.fitted.sampleCount=samples.points.size(); geometry.fitted.rejectedCount=samples.rejected;
                    geometry.fitted=AlignmentGeometryFit::BuildGeometry(work,*fit,samples,transform,
                        geometry.section ? std::optional<AlignmentPoint>{geometry.section->sourcePlaneNormal} : std::nullopt,
                        bool(geometry.section));
                }
                geometry.alignedCenter=Point(Transform(transform,Vector(geometry.fitted.sourceCenter)));
                geometry.alignedDirection=Point(Unit(Rotate(transform,Vector(geometry.fitted.sourceDirection))));
                if (!nominal && HasDirection(spec.fit.kind)) {
                    const double cosine=Vector(geometry.alignedDirection).dot(Vector(spec.section ? spec.section->planeNormal : spec.fit.targetDirection));
                    Require(std::abs(cosine)>=spec.fit.minDirectionCosine,"Inspection direction sign is ambiguous against its declared hint.");
                    if (cosine<0) {
                        geometry.alignedDirection=Point(-Vector(geometry.alignedDirection));
                        geometry.fitted.sourceDirection=Point(-Vector(geometry.fitted.sourceDirection));
                    }
                }
                Require(Finite(Vector(geometry.alignedCenter)) && Finite(Vector(geometry.alignedDirection)),"Inspection geometry overflows.");
                geometry.status=AlignmentStatus::FullyDetermined;
            } catch (const std::invalid_argument &e) { geometry.status=AlignmentStatus::Degenerate; geometry.message=e.what(); }
            report->geometries.push_back(std::move(geometry));
        }
        for (const auto &spec:plan.items) {
            Check(work); InspectionItemResult row; row.id=spec.id; row.nominal=spec.nominal;
            try {
                const double value=Value(spec.measured,report->geometries); Require(std::isfinite(value),"Inspection value overflows."); row.measured=value;
                row.nominal=spec.nominal;
                if (spec.nominalMeasure) row.nominal=Value(*spec.nominalMeasure,report->geometries);
                if (row.nominal) {
                    Require(std::isfinite(*row.nominal) && std::isfinite(value-*row.nominal),"Nominal value or deviation overflows.");
                    row.deviation=value-*row.nominal;
                }
                row.verdict=!spec.deviationLimits ? AlignmentInspectionVerdict::NotEvaluated
                    : *row.deviation>=(*spec.deviationLimits)[0] && *row.deviation<=(*spec.deviationLimits)[1]
                        ? AlignmentInspectionVerdict::Pass : AlignmentInspectionVerdict::Fail;
            } catch (const std::invalid_argument &e) {
                if (row.nominal && !std::isfinite(*row.nominal)) row.nominal.reset();
                row.message=e.what(); row.verdict=AlignmentInspectionVerdict::Indeterminate;
            }
            report->items.push_back(std::move(row));
        }
        report->verdict=GetVerdict(report->items);
        report->evaluatedUnixMilliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        output.report=std::move(report); output.status=AlignmentStatus::FullyDetermined;
    } catch (const std::length_error &e) { output.status=AlignmentStatus::ResourceExhausted; output.message=e.what(); }
      catch (const std::bad_alloc &) { output.status=AlignmentStatus::ResourceExhausted; }
      catch (const std::invalid_argument &e) { output.status=AlignmentStatus::InvalidInput; output.message=e.what(); }
      catch (const std::exception &e) {
        const auto status=AlignmentGeometryFit::GetWorkStatus(input); output.status=status==AlignmentStatus::FullyDetermined ? AlignmentStatus::InternalError : status;
        output.message=e.what();
    }
    return output;
}
