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
