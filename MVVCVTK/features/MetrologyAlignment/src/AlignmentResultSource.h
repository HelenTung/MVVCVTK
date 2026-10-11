#pragma once
#include "AlignmentData.h"

// 对齐后的评价共用同一套固定输入/单位/当前性校验，不绑定活动显示入口。
struct AlignmentResultSource final {
    AlignmentInput input;
    DataRevisionRef alignment, nominal, transform;
    AlignmentMatrix sourceToTarget=alignmentIdentity;
    std::shared_ptr<const SurfaceMeshPayload> measuredMesh, referenceMesh;
    std::vector<DataExpectation> expectations;
    static std::optional<AlignmentResultSource> Read(const DataGraphSnapshot &graph,
        const DataRevisionRef &result,bool requiresReferenceSurface);
};
