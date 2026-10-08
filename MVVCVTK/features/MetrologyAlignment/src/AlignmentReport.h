#pragma once
#include "Host/MetrologyReport.h"

namespace AlignmentReport {
MetrologyReportResult Capture(const DataGraphSnapshot &graph,const MetrologyReportRequest &request,
                              const AlignmentConfig &config);
}
