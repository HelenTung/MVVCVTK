#pragma once

#include "Algorithms/ClassicalPartSegmenter.h"
#include "Data/DataPayloads.h"
#include "Model/PartLineageMatcher.h"

std::optional<std::size_t> GetPartEditBytes(const PartEditRequest& request);

struct PartEditInput final {
    PartVolumeView volume;
    std::string coordinateFrame = "RAS";
    PartHistorySnapshot previous;
    PartEditRequest request;
    std::shared_ptr<const LabelMap3DPayload> roiMask;
    std::shared_ptr<const LabelMap3DPayload> protectionMask;
    std::size_t maxWorkingBytes = 0;
};

struct PartEditBuildResult final {
    PartFailureReason failureReason = PartFailureReason::InvalidEdit;
    std::string message;
    std::size_t requiredBytes = 0;
    std::shared_ptr<const std::vector<PartLabelId>> labels;
    std::shared_ptr<const PartCatalog> catalog;
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
