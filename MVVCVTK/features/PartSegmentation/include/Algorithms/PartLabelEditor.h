#pragma once

#include "Algorithms/ClassicalPartSegmenter.h"
#include "Data/DataPayloads.h"
#include "Host/RoiReadTypes.h"
#include "Model/PartLineageMatcher.h"

std::optional<std::size_t> GetPartEditBytes(const PartEditRequest& request);

struct PartEditInput final {
    PartVolumeView volume;
    DataRevisionRef sourceRevision;
    std::string coordinateFrame = "RAS";
    PartHistorySnapshot previous;
    PartEditRequest request;
    RoiReadSnapshot editRoi;
    RoiReadSnapshot protectionRoi;
    std::size_t maxWorkingBytes = 0;
};

// 仓内诊断：分阶段耗时不属于 SDK/Feature 公共契约；不代表进程峰值内存。
struct PartEditProfile final {
    double inputMs = 0.0;
    double copyMs = 0.0;
    // 预先准备可编辑域的时间；改为按需检查后，其计算成本计入operationMs。
    double editableMs = 0.0;
    double operationMs = 0.0;
    double catalogMs = 0.0;
    double validationMs = 0.0;
    std::size_t visitedVoxels = 0;
};

struct PartEditBuildResult final {
    PartFailureReason failureReason = PartFailureReason::InvalidEdit;
    std::string message;
    std::size_t requiredBytes = 0;
    std::shared_ptr<const std::vector<PartLabelId>> labels;
    std::shared_ptr<const PartCatalog> catalog;
    PartEditProfile profile;
};

class PartLabelEditor final {
public:
    static PartEditBuildResult BuildLabels(
        const PartEditInput& input,
        PartIdentityFactory& identities,
        const std::function<bool()>& getStopRequested = nullptr);
    static PartEditBuildResult BuildRestore(
        const PartHistorySnapshot& current,
        const PartHistorySnapshot& restored,
        std::size_t maxWorkingBytes,
        const std::function<bool()>& getStopRequested = nullptr);
};
