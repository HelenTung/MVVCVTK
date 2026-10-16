#include "AlignmentReportParameters.h"
#include "JsonInput.h"
#include <QJsonArray>
#include <initializer_list>
#include <stdexcept>

namespace Manual {
namespace {
// 条件来自冻结请求；64 位编号使用十进制文本，不经 double 丢失精度。
class ParametersJson final {
public:
    explicit ParametersJson(const ReportCheckpoint& checkpoint):m_checkpoint(checkpoint){}
    template<class T> QJsonObject Get(const T& value) { Check();return Value(value).toObject(); }
private:
    void Check() const { if(m_checkpoint)m_checkpoint(); }
    QJsonValue Value(const DataRevisionRef& v) { return GetRefText(v); }
    QJsonValue Value(const std::string& v) { return QString::fromStdString(v); }
    QJsonValue Value(bool v) { return v; }
    QJsonValue Value(int v) { return v; }
    QJsonValue Value(double v) { return v; }
    QJsonValue Value(std::uint64_t v) { return QString::number(v); }
    template<class T> QJsonValue EnumValue(T v,std::initializer_list<const char*> names) {
        const auto index=static_cast<std::size_t>(v);
        if(index>=names.size())throw std::invalid_argument("冻结报告参数包含未知枚举");
        return QString::fromLatin1(*(names.begin()+index));
    }
    QJsonValue Value(AlignmentGeometryKind v) { return EnumValue(v,{"Point","Line","Plane","Circle","Sphere","Cylinder"}); }
    QJsonValue Value(AlignmentAssociation v) { return EnumValue(v,{"LeastSquares","SequentialLeastSquares","Contact"}); }
    QJsonValue Value(InspectionGeometrySource v) { return EnumValue(v,{"MeasuredSurface","ReferenceSurface","NominalDefinition"}); }
    QJsonValue Value(InspectionMetric v) { return EnumValue(v,{"Radius","Diameter","CenterX","CenterY","CenterZ","CenterDistance","OrientedDirectionAngleDegrees","SignedParallelPlaneDistance","FitRms","FitMaximum"}); }
    QJsonValue Value(AlignmentComputeBackend v) { return EnumValue(v,{"Cpu","Cuda"}); }
    QJsonValue Value(AlignmentDeviationMetric v) { return EnumValue(v,{"UnsignedDistance","NominalNormalProjection"}); }
    QJsonValue Value(VoxelVolumeMethod v) { return EnumValue(v,{"StoredValueRange","Labels"}); }
    template<class T> QJsonValue Value(const std::optional<T>& v) { return v?Value(*v):QJsonValue(); }
    template<class T,std::size_t N> QJsonValue Value(const std::array<T,N>& v) {
        QJsonArray result;for(const auto& item:v)result.append(Value(item));return result;
    }
    template<class T> QJsonValue Value(const std::vector<T>& v) {
        QJsonArray result;for(const auto& item:v){Check();result.append(Value(item));}return result;
    }
    QJsonValue Value(const AlignmentRegion& v) {
        return QJsonObject{{"pinnedMesh",Value(v.pinnedMesh)},{"vertexIds",Value(v.vertexIds)},
            {"targetBounds",Value(v.targetBounds)},{"roi",Value(v.roi)},{"meshToRoi",Value(v.meshToRoi)}};
    }
    QJsonValue Value(const AlignmentGeometrySpec& v) {
        return QJsonObject{{"id",Value(v.id)},{"kind",Value(v.kind)},{"association",Value(v.association)},
            {"region",Value(v.region)},{"targetDirection",Value(v.targetDirection)},{"radiusRange",Value(v.radiusRange)},
            {"maxFitRms",Value(v.maxFitRms)},{"minCoverage",Value(v.minCoverage)},{"minDirectionCosine",Value(v.minDirectionCosine)}};
    }
    QJsonValue Value(const InspectionIdealGeometry& v) {
        return QJsonObject{{"center",Value(v.center)},{"direction",Value(v.direction)},{"radius",Value(v.radius)}};
    }
    QJsonValue Value(const InspectionSection& v) {
        return QJsonObject{{"pinnedMesh",Value(v.pinnedMesh)},{"planePoint",Value(v.planePoint)},
            {"planeNormal",Value(v.planeNormal)},{"triangles",Value(v.triangles)},{"sampleSpacing",Value(v.sampleSpacing)},
            {"planeTolerance",Value(v.planeTolerance)},{"selectionPoint",Value(v.selectionPoint)},{"maxSelectionDistance",Value(v.maxSelectionDistance)}};
    }
    QJsonValue Value(const InspectionGeometrySpec& v) {
        return QJsonObject{{"fit",Value(v.fit)},{"source",Value(v.source)},{"ideal",Value(v.ideal)},
            {"definitionSource",Value(v.definitionSource)},{"section",Value(v.section)}};
    }
    QJsonValue Value(const InspectionOperand& v) {
        return QJsonObject{{"geometry",Value(v.geometry)},{"sectionPlane",Value(v.sectionPlane)}};
    }
    QJsonValue Value(const InspectionMeasure& v) {
        return QJsonObject{{"metric",Value(v.metric)},{"first",Value(v.first)},{"second",Value(v.second)},
            {"maxParallelAngleDegrees",Value(v.maxParallelAngleDegrees)}};
    }
    QJsonValue Value(const InspectionItem& v) {
        return QJsonObject{{"id",Value(v.id)},{"name",Value(v.name)},{"measured",Value(v.measured)},
            {"nominal",Value(v.nominal)},{"nominalMeasure",Value(v.nominalMeasure)},
            {"nominalSource",Value(v.nominalSource)},{"deviationLimits",Value(v.deviationLimits)}};
    }
    QJsonValue Value(const InspectionPlan& v) {
        return QJsonObject{{"id",Value(v.id)},{"alignmentResult",Value(v.alignmentResult)},
            {"geometries",Value(v.geometries)},{"items",Value(v.items)},{"lengthScale",Value(v.lengthScale)},
            {"rankTolerance",Value(v.rankTolerance)},{"conditionLimit",Value(v.conditionLimit)},
            {"solveTolerance",Value(v.solveTolerance)},{"iterationLimit",Value(v.iterationLimit)},
            {"requiresMeasurementQuality",Value(v.requiresMeasurementQuality)},{"minSupportRatio",Value(v.minSupportRatio)},
            {"maxLocalizationSigma",Value(v.maxLocalizationSigma)},{"maxSurfaceFitResidual",Value(v.maxSurfaceFitResidual)}};
    }
    QJsonValue Value(const AlignmentDeviationRequest& v) {
        return QJsonObject{{"computeBackend",Value(v.computeBackend)},{"alignmentResult",Value(v.alignmentResult)},
            {"evaluationRoi",Value(v.evaluationRoi)},{"metric",Value(v.metric)},{"sourceTriangles",Value(v.sourceTriangles)},
            {"referenceTriangles",Value(v.referenceTriangles)},{"maxDistance",Value(v.maxDistance)},
            {"minNormalCosine",Value(v.minNormalCosine)},{"tolerance",Value(v.tolerance)},
            {"reverseCoverage",Value(v.reverseCoverage)},{"requiresMeasurementQuality",Value(v.requiresMeasurementQuality)},
            {"minSupportRatio",Value(v.minSupportRatio)},{"maxLocalizationSigma",Value(v.maxLocalizationSigma)},
            {"maxSurfaceFitResidual",Value(v.maxSurfaceFitResidual)}};
    }
    QJsonValue Value(const VoxelVolumeRequest& v) {
        return QJsonObject{{"id",Value(v.id)},{"source",Value(v.source)},{"method",Value(v.method)},
            {"extent",Value(v.extent)},{"storedValueRange",Value(v.storedValueRange)},{"labels",Value(v.labels)},
            {"validityImage",Value(v.validityImage)},{"requireEnclosedSelection",Value(v.requireEnclosedSelection)}};
    }
    const ReportCheckpoint& m_checkpoint;
};
}
QJsonObject GetAlignmentReportParameters(const InspectionPlan& value,const ReportCheckpoint& checkpoint) { return ParametersJson(checkpoint).Get(value); }
QJsonObject GetAlignmentReportParameters(const AlignmentDeviationRequest& value,const ReportCheckpoint& checkpoint) { return ParametersJson(checkpoint).Get(value); }
QJsonObject GetAlignmentReportParameters(const VoxelVolumeRequest& value,const ReportCheckpoint& checkpoint) { return ParametersJson(checkpoint).Get(value); }
}
