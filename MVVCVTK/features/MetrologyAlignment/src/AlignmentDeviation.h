#pragma once
#include "AlignmentGeometry.h"

struct AlignmentDeviationData final {
    AlignmentStatus status = AlignmentStatus::InvalidInput;
    std::string message;
    std::shared_ptr<const AlignmentDeviationField> forward, reverse;
    AlignmentInspectionVerdict verdict = AlignmentInspectionVerdict::NotEvaluated;
};
class AlignmentDeviation final {
  public:
    static bool GetRequestValid(const AlignmentDeviationRequest &request);
    static AlignmentDeviationData Build(const AlignmentWork &work,
        const AlignmentDeviationRequest &request, const AlignmentMatrix &sourceToTarget);
};
