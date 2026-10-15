#pragma once
#include "ReportDocument.h"
#include "Host/MetrologyAlignmentHostTypes.h"
namespace Manual {
ReportDocument CreateInspectionReport(const QJsonObject& record, const InspectionSnapshot& snapshot, const QString& source);
}
