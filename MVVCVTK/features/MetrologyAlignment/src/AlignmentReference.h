#pragma once
#include "AlignmentGeometry.h"

struct AlignmentReferenceData final {
    std::shared_ptr<const SurfaceMeshPayload> mesh;
    AlignmentReferenceSnapshot metadata;
    AlignmentStatus status = AlignmentStatus::InvalidInput;
    std::string message;
};

class AlignmentReferenceLoader final {
  public:
    static bool GetRequestValid(const AlignmentReferenceRequest &request);
    static AlignmentReferenceData Load(const AlignmentReferenceRequest &request,
                                        const AlignmentWork &work);
};
