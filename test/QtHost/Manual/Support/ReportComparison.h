#pragma once
#include "ReportDocument.h"
namespace Manual {
enum class ReportMatch { Identity, Position, Distribution };
struct ReportComparisonOptions final {
    int baselineColumn = -1, currentColumn = -1;
    int baselineIdentity = -1, currentIdentity = -1;
    QString baselineUnit, currentUnit, baselineQuantity, currentQuantity;
    ReportMatch match = ReportMatch::Distribution;
    bool identitiesConfirmed = false, coordinatesConfirmed = false;
    bool mappingConfirmed = false;
    QString baselineCoordinateUnit, currentCoordinateUnit;
    double distanceTolerance = 0, valueTolerance = 0;
};
ReportDocument CompareReports(const ReportDocument& baseline, const ReportDocument& current,
    const ReportComparisonOptions& options, const ReportCheckpoint& checkpoint = {});
}
