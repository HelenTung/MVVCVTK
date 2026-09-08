#pragma once

#include "Data/DataPayloads.h"
#include "Host/RoiReadTypes.h"

#include <functional>

// Host 私有实现；直接事务与服务入口共享同一校验，不能由 Feature 绕过。
class RoiEvaluator final {
public:
    using DataLookup = std::function<DataSnapshot(const DataRevisionRef&)>;
    static RoiError GetDefinitionError(const RoiDefinition& definition) noexcept;
    static RoiError GetRelationsError(const DataRevision& roi, const DataLookup& getData);
    static RoiReadResult GetRoi(const DataGraphSnapshot& graph,
        const DataRevisionRef& roiRef, const DataRevisionRef& sourceRef);
    static std::vector<DataInputRef> GetInputs(const RoiDefinition& definition);
};
