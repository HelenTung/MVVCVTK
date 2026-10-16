#include "WallParameters.h"
#include "JsonInput.h"
#include <QJsonArray>
namespace Manual {
QJsonObject GetWallEvaluationJson(const ThicknessEvaluation& v)
{
    return {{"lower",v.lower},{"upper",v.upper},{"histogramRange",GetValues(v.histogramRange)},{"histogramBins",double(v.histogramBins)},{"minRegionArea",v.minRegionArea}};
}
QJsonObject GetWallMeasurementJson(const ThicknessInput& input,const ThicknessParams& p)
{
    QJsonArray labels;for(auto id:input.materialLabels)labels.append(QString::number(id));
    return {{"source",GetRefText(input.source)},{"labels",GetRefText(input.labels)},{"mesh",GetRefText(input.mesh)},{"materialLabels",labels},{"unit",int(input.unit)},
        {"maxDistance",p.maxDistance},{"sampleSpacing",p.sampleSpacing},{"materialThreshold",p.materialThreshold?QJsonValue(*p.materialThreshold):QJsonValue()},
        {"coneAngleDegrees",p.coneAngleDegrees},{"directionCount",int(p.directionCount)},{"boundaryPolicy",int(p.boundaryPolicy)},{"maxBoundaryError",p.maxBoundaryError},
        {"evaluationBounds",p.evaluationBounds?QJsonValue(GetValues(*p.evaluationBounds)):QJsonValue()},
        {"evaluationRoi",p.evaluationRoi?QJsonValue(GetRefText(*p.evaluationRoi)):QJsonValue()}};
}
}
