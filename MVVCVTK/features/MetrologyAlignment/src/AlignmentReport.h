#pragma once
#include "Host/MetrologyReport.h"

namespace AlignmentReport {
struct CurrentDisplay final {
    bool supplied=false;
    std::optional<AlignmentDeviationDisplay> deviation;
    std::shared_ptr<const AlignmentSceneDisplay> scene;
};
MetrologyReportResult Capture(const DataGraphSnapshot &graph,const MetrologyReportRequest &request,
                              const AlignmentConfig &config,const CurrentDisplay &current = {});
}
