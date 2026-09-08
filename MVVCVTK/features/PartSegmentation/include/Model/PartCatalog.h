#pragma once

#include "Host/PartSegmentationHostTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

struct PartObjectIdHash final {
    std::size_t operator()(const PartObjectId& value) const noexcept;
};

struct PartEntry final {
    PartObjectId objectId;
    PartLabelId labelId = 0;
    PartMetrics metrics;
    PartUserState userState;
    // 新生成零件采用实体预览，避免相邻标签的内部表面半透明叠加。
    PartPresentation presentation{true, false, 1.0};
    bool isEdited = false;
};

struct PartCatalog final {
    PartSetId partSetId;
    std::uint64_t resultRevision = 0;
    std::uint64_t catalogRevision = 0;
    std::vector<PartEntry> partsByLabel;
    std::unordered_map<PartObjectId, PartLabelId, PartObjectIdHash>
        labelByObject;
    std::vector<PartRelation> relationsFromPrevious;
    std::vector<PartBindingRef> retiredFromPrevious;
};

bool GetPartSetIdValid(const PartSetId& value) noexcept;
bool GetPartObjectIdValid(const PartObjectId& value) noexcept;
std::array<double, 4> GetPartStableColor(
    const PartObjectId& value) noexcept;
bool GetPartCatalogValid(
    const PartCatalog& catalog,
    const std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested = nullptr) noexcept;
bool GetPartCatalogStorageBytes(
    const PartCatalog& catalog,
    std::size_t& storageBytes) noexcept;
std::shared_ptr<const PartSetSnapshot> BuildPartSetSnapshot(
    const PartCatalog& catalog,
    DataRevisionRef sourceRevision,
    bool isStale);
PartMutationResult SetPartCatalogState(
    PartCatalog& catalog,
    const PartBindingRef& part,
    const PartStatePatch& patch,
    std::uint64_t expectedCatalogRevision);
