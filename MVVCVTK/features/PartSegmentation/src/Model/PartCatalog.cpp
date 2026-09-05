#include "Model/PartCatalog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace {

constexpr std::size_t partNameByteLimit = 255;
constexpr std::size_t cancelBatch = 4096;
constexpr std::size_t mapNodeOverhead = sizeof(void*) * 4U;

bool AddStorageBytes(
    const std::size_t count,
    const std::size_t itemBytes,
    std::size_t& total) noexcept
{
    if (count != 0
        && itemBytes > std::numeric_limits<std::size_t>::max() / count) {
        return false;
    }
    const std::size_t bytes = count * itemBytes;
    if (bytes > std::numeric_limits<std::size_t>::max() - total) {
        return false;
    }
    total += bytes;
    return true;
}

std::uint64_t GetMixed(std::uint64_t value) noexcept
{
    // SplitMix64 finalizer：只用于稳定展示色与哈希，不承担身份生成。
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool GetUnitValid(const double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool GetMetricsValid(const PartMetrics& metrics) noexcept
{
    if (metrics.voxelCount == 0
        || !std::isfinite(metrics.physicalVolumeMM3)
        || metrics.physicalVolumeMM3 <= 0.0) {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (metrics.voxelExtent[axis * 2]
                > metrics.voxelExtent[axis * 2 + 1]
            || !std::isfinite(metrics.inputPhysicalBounds[axis * 2])
            || !std::isfinite(metrics.inputPhysicalBounds[axis * 2 + 1])
            || metrics.inputPhysicalBounds[axis * 2]
                > metrics.inputPhysicalBounds[axis * 2 + 1]
            || !std::isfinite(metrics.centroidInputPhysical[axis])) {
            return false;
        }
    }
    return !metrics.confidence
        || GetUnitValid(*metrics.confidence);
}

bool GetPresentationValid(
    const PartObjectId& objectId,
    const PartPresentation& presentation) noexcept
{
    if (!GetUnitValid(presentation.opacity)
        || !std::all_of(
            presentation.color.begin(), presentation.color.end(),
            GetUnitValid)) {
        return false;
    }
    return presentation.colorUse != PartColorUse::Stable
        || presentation.color == GetPartStableColor(objectId);
}

bool GetBindingValid(
    const PartBindingRef& binding,
    const PartSetId& partSetId,
    const std::uint64_t resultRevision,
    const bool isCurrent) noexcept
{
    return binding.object.partSetId == partSetId
        && GetPartObjectIdValid(binding.object.objectId)
        && binding.resultRevision != 0
        && (isCurrent
            ? binding.resultRevision == resultRevision
            : binding.resultRevision < resultRevision);
}

bool GetPatchValid(const PartStatePatch& patch) noexcept
{
    const bool hasValue = patch.name
        || patch.isVisible
        || patch.isSelected
        || patch.isReviewed
        || patch.opacity
        || patch.color;
    if (!hasValue
        || (patch.name
            && (patch.name->empty()
                || patch.name->size() > partNameByteLimit))
        || (patch.opacity && !GetUnitValid(*patch.opacity))) {
        return false;
    }
    if (!patch.color) return true;
    if (patch.color->colorUse != PartColorUse::Stable
        && patch.color->colorUse != PartColorUse::Custom) {
        return false;
    }
    return std::all_of(
        patch.color->color.begin(), patch.color->color.end(),
        GetUnitValid);
}

} // namespace

std::size_t PartObjectIdHash::operator()(
    const PartObjectId& value) const noexcept
{
    const std::uint64_t mixed = GetMixed(value.high)
        ^ GetMixed(value.low + 0x9e3779b97f4a7c15ULL);
    if constexpr (sizeof(std::size_t) >= sizeof(mixed)) {
        return static_cast<std::size_t>(mixed);
    }
    return static_cast<std::size_t>(mixed ^ (mixed >> 32));
}

bool GetPartSetIdValid(const PartSetId& value) noexcept
{
    return value.high != 0 || value.low != 0;
}

bool GetPartObjectIdValid(const PartObjectId& value) noexcept
{
    return value.high != 0 || value.low != 0;
}

std::array<double, 4> GetPartStableColor(
    const PartObjectId& value) noexcept
{
    const std::uint64_t hash = GetMixed(value.high)
        ^ GetMixed(value.low + 0x9e3779b97f4a7c15ULL);
    const auto getChannel = [hash](const unsigned shift) {
        const auto byte = static_cast<unsigned>((hash >> shift) & 0xffULL);
        return 0.25 + 0.75 * static_cast<double>(byte) / 255.0;
    };
    return { getChannel(16), getChannel(8), getChannel(0), 0.85 };
}

bool GetPartCatalogValid(
    const PartCatalog& catalog,
    const std::vector<PartLabelId>& labels,
    const std::function<bool()>& getStopRequested) noexcept
{
    try {
        if (!GetPartSetIdValid(catalog.partSetId)
            || catalog.resultRevision == 0
            || catalog.catalogRevision == 0
            || catalog.partsByLabel.empty()
            || catalog.partsByLabel[0].labelId != 0
            || GetPartObjectIdValid(catalog.partsByLabel[0].objectId)
            || catalog.labelByObject.size() + 1
                != catalog.partsByLabel.size()) {
            return false;
        }

        std::vector<std::uint64_t> histogram(
            catalog.partsByLabel.size(), 0);
        for (std::size_t index = 0; index < labels.size(); ++index) {
            if (index % cancelBatch == 0
                && getStopRequested && getStopRequested()) {
                return false;
            }
            const PartLabelId label = labels[index];
            if (label >= histogram.size()
                || histogram[label]
                    == std::numeric_limits<std::uint64_t>::max()) {
                return false;
            }
            ++histogram[label];
        }

        std::size_t selectedCount = 0;
        for (std::size_t index = 1;
            index < catalog.partsByLabel.size(); ++index) {
            if (index % cancelBatch == 0
                && getStopRequested && getStopRequested()) {
                return false;
            }
            const auto& entry = catalog.partsByLabel[index];
            if (entry.labelId != static_cast<PartLabelId>(index)
                || !GetPartObjectIdValid(entry.objectId)
                || histogram[index] != entry.metrics.voxelCount
                || !GetMetricsValid(entry.metrics)
                || !GetPresentationValid(entry.objectId, entry.presentation)) {
                return false;
            }
            selectedCount += entry.presentation.isSelected ? 1U : 0U;
            const auto found = catalog.labelByObject.find(entry.objectId);
            if (found == catalog.labelByObject.end()
                || found->second != entry.labelId) {
                return false;
            }
        }
        if (selectedCount > 1) return false;

        for (std::size_t index = 0;
            index < catalog.relationsFromPrevious.size(); ++index) {
            if (index % cancelBatch == 0
                && getStopRequested && getStopRequested()) {
                return false;
            }
            const auto& relation = catalog.relationsFromPrevious[index];
            if (!GetBindingValid(
                    relation.current,
                    catalog.partSetId,
                    catalog.resultRevision,
                    true)
                || !GetBindingValid(
                    relation.previous,
                    catalog.partSetId,
                    catalog.resultRevision,
                    false)
                || !GetUnitValid(relation.overlapScore)
                || catalog.labelByObject.find(
                    relation.current.object.objectId)
                    == catalog.labelByObject.end()) {
                return false;
            }
        }

        std::unordered_set<PartObjectId, PartObjectIdHash> retired;
        for (std::size_t index = 0;
            index < catalog.retiredFromPrevious.size(); ++index) {
            if (index % cancelBatch == 0
                && getStopRequested && getStopRequested()) {
                return false;
            }
            const auto& binding = catalog.retiredFromPrevious[index];
            if (!GetBindingValid(
                    binding,
                    catalog.partSetId,
                    catalog.resultRevision,
                    false)
                || !retired.insert(binding.object.objectId).second) {
                return false;
            }
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool GetPartCatalogStorageBytes(
    const PartCatalog& catalog,
    std::size_t& storageBytes) noexcept
{
    std::size_t total = sizeof(PartCatalog);
    if (!AddStorageBytes(
            catalog.partsByLabel.capacity(), sizeof(PartEntry), total)
        || !AddStorageBytes(
            catalog.labelByObject.size(),
            sizeof(std::pair<const PartObjectId, PartLabelId>)
                + mapNodeOverhead,
            total)
        || !AddStorageBytes(
            catalog.labelByObject.bucket_count(), sizeof(void*), total)
        || !AddStorageBytes(
            catalog.relationsFromPrevious.capacity(),
            sizeof(PartRelation),
            total)
        || !AddStorageBytes(
            catalog.retiredFromPrevious.capacity(),
            sizeof(PartBindingRef),
            total)) {
        storageBytes = std::numeric_limits<std::size_t>::max();
        return false;
    }
    for (const auto& entry : catalog.partsByLabel) {
        if (!AddStorageBytes(
                entry.userState.name.capacity(), sizeof(char), total)) {
            storageBytes = std::numeric_limits<std::size_t>::max();
            return false;
        }
    }
    storageBytes = total;
    return true;
}

std::shared_ptr<const PartSetSnapshot> BuildPartSetSnapshot(
    const PartCatalog& catalog,
    const DataVersion sourceVersion,
    const bool isStale)
{
    try {
        if (!GetPartSetIdValid(catalog.partSetId)
            || catalog.resultRevision == 0
            || catalog.catalogRevision == 0
            || catalog.partsByLabel.empty()) {
            return {};
        }

        auto snapshot = std::make_shared<PartSetSnapshot>();
        snapshot->partSetId = catalog.partSetId;
        snapshot->sourceVersion = sourceVersion;
        snapshot->resultRevision = catalog.resultRevision;
        snapshot->catalogRevision = catalog.catalogRevision;
        snapshot->isStale = isStale;
        snapshot->parts.reserve(catalog.partsByLabel.size() - 1);
        for (std::size_t index = 1;
            index < catalog.partsByLabel.size(); ++index) {
            const auto& entry = catalog.partsByLabel[index];
            PartSnapshot part;
            part.binding.object = { catalog.partSetId, entry.objectId };
            part.binding.resultRevision = catalog.resultRevision;
            part.labelId = entry.labelId;
            part.metrics = entry.metrics;
            part.userState = entry.userState;
            part.presentation = entry.presentation;
            snapshot->parts.push_back(std::move(part));
        }
        snapshot->relationsFromPrevious = catalog.relationsFromPrevious;
        snapshot->retiredFromPrevious = catalog.retiredFromPrevious;
        return snapshot;
    }
    catch (...) {
        return {};
    }
}

PartMutationResult SetPartCatalogState(
    PartCatalog& catalog,
    const PartBindingRef& part,
    const PartStatePatch& patch,
    const std::uint64_t expectedCatalogRevision)
{
    PartMutationResult result;
    result.catalogRevision = catalog.catalogRevision;
    if (!GetPatchValid(patch)) return result;
    if (part.object.partSetId != catalog.partSetId) {
        result.status = PartMutationStatus::NotFound;
        return result;
    }
    if (part.resultRevision != catalog.resultRevision) {
        result.status = PartMutationStatus::StaleReference;
        return result;
    }
    if (expectedCatalogRevision != catalog.catalogRevision) {
        result.status = PartMutationStatus::RevisionConflict;
        return result;
    }
    const auto found = catalog.labelByObject.find(part.object.objectId);
    if (found == catalog.labelByObject.end()
        || found->second == 0
        || found->second >= catalog.partsByLabel.size()) {
        result.status = PartMutationStatus::NotFound;
        return result;
    }

    auto& entry = catalog.partsByLabel[found->second];
    if (entry.objectId != part.object.objectId
        || static_cast<std::size_t>(entry.labelId) != found->second) {
        result.status = PartMutationStatus::NotFound;
        return result;
    }

    // 先完成唯一可能分配的 name 候选，再计算并一次性提交无抛出字段。
    std::optional<std::string> nextName;
    if (patch.name && entry.userState.name != *patch.name) {
        nextName = *patch.name;
    }
    const auto resolvedColor = patch.color
        ? std::optional<std::array<double, 4>>{
            patch.color->colorUse == PartColorUse::Stable
                ? GetPartStableColor(entry.objectId) : patch.color->color }
        : std::nullopt;
    bool isChanged = nextName.has_value()
        || (patch.isReviewed
            && entry.userState.isReviewed != *patch.isReviewed)
        || (patch.isVisible
            && entry.presentation.isVisible != *patch.isVisible)
        || (patch.opacity
            && entry.presentation.opacity != *patch.opacity)
        || (patch.color
            && (entry.presentation.colorUse != patch.color->colorUse
                || entry.presentation.color != *resolvedColor));
    if (patch.isSelected) {
        if (*patch.isSelected) {
            for (std::size_t index = 1;
                index < catalog.partsByLabel.size(); ++index) {
                const bool nextSelected = index == found->second;
                isChanged = isChanged
                    || catalog.partsByLabel[index].presentation.isSelected
                        != nextSelected;
            }
        }
        else {
            isChanged = isChanged || entry.presentation.isSelected;
        }
    }

    if (!isChanged) {
        result.status = PartMutationStatus::Succeeded;
        return result;
    }
    if (catalog.catalogRevision
        == std::numeric_limits<std::uint64_t>::max()) {
        result.status = PartMutationStatus::RevisionConflict;
        return result;
    }

    if (nextName) entry.userState.name.swap(*nextName);
    if (patch.isReviewed) entry.userState.isReviewed = *patch.isReviewed;
    if (patch.isVisible) entry.presentation.isVisible = *patch.isVisible;
    if (patch.opacity) entry.presentation.opacity = *patch.opacity;
    if (patch.color) {
        entry.presentation.colorUse = patch.color->colorUse;
        entry.presentation.color = *resolvedColor;
    }
    if (patch.isSelected) {
        if (*patch.isSelected) {
            for (std::size_t index = 1;
                index < catalog.partsByLabel.size(); ++index) {
                catalog.partsByLabel[index].presentation.isSelected =
                    index == found->second;
            }
        }
        else {
            entry.presentation.isSelected = false;
        }
    }
    ++catalog.catalogRevision;
    result.status = PartMutationStatus::Succeeded;
    result.catalogRevision = catalog.catalogRevision;
    return result;
}
