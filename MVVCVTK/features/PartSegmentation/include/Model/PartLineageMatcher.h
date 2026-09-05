#pragma once

#include "Model/PartCatalog.h"
#include "Model/PartIdentityFactory.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct PartHistorySnapshot final {
    std::shared_ptr<const std::vector<PartLabelId>> labels;
    std::shared_ptr<const PartCatalog> catalog;
};

struct PartLineageRequest final {
    PartHistorySnapshot previous;
    std::shared_ptr<const std::vector<PartLabelId>> currentLabels;
    std::vector<PartMetrics> currentMetricsByLabel;
    std::uint64_t nextResultRevision = 0;
    std::size_t maxWorkingBytes = 0;
};

struct PartLineageResult final {
    PartFailureReason failureReason = PartFailureReason::InternalError;
    std::shared_ptr<const PartCatalog> catalog;
    std::size_t requiredBytes = 0;
};

class PartLineageMatcher final {
public:
    static PartLineageResult BuildCatalog(
        PartLineageRequest request,
        PartIdentityFactory& identities,
        const std::function<bool()>& getStopRequested = nullptr);
};
