#pragma once
#include "AlignmentGeometry.h"

struct InspectionData final {
    AlignmentStatus status=AlignmentStatus::InvalidInput;
    std::string message;
    std::shared_ptr<const InspectionReport> report;
};
class AlignmentInspection final {
  public:
    static bool GetPlanValid(const InspectionPlan &plan,const AlignmentConfig &config);
    static AlignmentInspectionVerdict GetVerdict(const std::vector<InspectionItemResult> &items);
    static InspectionData Build(const AlignmentWork &input,const InspectionPlan &plan,const AlignmentMatrix &pose);
};
