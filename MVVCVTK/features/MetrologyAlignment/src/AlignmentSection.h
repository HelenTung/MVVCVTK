#pragma once
#include "AlignmentGeometry.h"

struct AlignmentSectionData final {
    AlignmentSamples samples;
    std::shared_ptr<const InspectionSectionTrace> trace;
};
class AlignmentSection final {
  public:
    static bool GetValid(const InspectionSection &section);
    static AlignmentSectionData Build(const AlignmentWork &work,const InspectionSection &section,
                                     const AlignmentMatrix &sourceToTarget);
};
