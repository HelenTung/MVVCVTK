#pragma once
#include "ReportDocument.h"
#include "Host/MetrologyAlignmentHostTypes.h"

namespace Manual {
QJsonObject GetAlignmentReportParameters(const InspectionPlan&, const ReportCheckpoint& checkpoint = {});
QJsonObject GetAlignmentReportParameters(const AlignmentDeviationRequest&, const ReportCheckpoint& checkpoint = {});
QJsonObject GetAlignmentReportParameters(const VoxelVolumeRequest&, const ReportCheckpoint& checkpoint = {});
}
