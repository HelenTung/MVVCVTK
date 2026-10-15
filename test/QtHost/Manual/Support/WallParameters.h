#pragma once
#include "Host/WallThicknessHostTypes.h"
#include <QJsonObject>
namespace Manual {
QJsonObject GetWallMeasurementJson(const ThicknessInput& input,const ThicknessParams& parameters);
QJsonObject GetWallEvaluationJson(const ThicknessEvaluation& evaluation);
}
