#pragma once
#include "AlignmentGeometry.h"
class AlignmentSolver final {
  public:
    static AlignmentCandidate BuildResult(const AlignmentWork &work);
};
