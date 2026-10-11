#pragma once
#include "Host/MetrologyReport.h"
namespace AlignmentReportDisplay {
MetrologyReportDeviationDisplay
BuildDeviation(const AlignmentDeviationDisplay &display,
               const AlignmentDeviationSnapshot &result,
               const std::optional<AlignmentPoint> &sourcePoint = {});
bool GetValid(const MetrologyReport &report);
} // namespace AlignmentReportDisplay
