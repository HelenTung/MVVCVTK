#include "Model/PartLineageMatcher.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t cancelBatch = 4096;
constexpr std::size_t mapNodeOverhead = sizeof(void*) * 4U;
constexpr std::size_t maxPartCount = 4096;

bool GetProduct(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

bool GetSum(
    const std::size_t left,
    const std::size_t right,
    std::size_t& sum) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    sum = left + right;
    return true;
}

class WorkingBudget final {
public:
    explicit WorkingBudget(const std::size_t limit) noexcept
        : m_limit(limit)
    {
    }

    bool Add(const std::size_t bytes) noexcept
    {
        std::size_t next = 0;
        if (!GetSum(m_current, bytes, next)) {
            m_required = std::numeric_limits<std::size_t>::max();
            return false;
        }
        m_required = std::max(m_required, next);
        if (next > m_limit) return false;
        m_current = next;
        return true;
    }

    void SetOverflow() noexcept
    {
        m_required = std::numeric_limits<std::size_t>::max();
    }

    std::size_t GetRequired() const noexcept
    {
        return std::max(m_required, m_current);
    }

private:
    std::size_t m_limit = 0;
    std::size_t m_current = 0;
    std::size_t m_required = 0;
};

struct OverlapCount final {
    PartLabelId previousLabel = 0;
    PartLabelId currentLabel = 0;
    std::uint64_t voxelCount = 0;
};

std::uint64_t GetOverlapKey(
    const PartLabelId previousLabel,
    const PartLabelId currentLabel) noexcept
{
    return (static_cast<std::uint64_t>(previousLabel) << 32)
        | static_cast<std::uint64_t>(currentLabel);
}

bool GetStopped(const std::function<bool()>& getStopRequested)
{
    return getStopRequested && getStopRequested();
}

bool GetOverlapLess(
    const OverlapCount& left,
    const OverlapCount& right) noexcept
{
    return left.currentLabel != right.currentLabel
        ? left.currentLabel < right.currentLabel
        : left.previousLabel < right.previousLabel;
}

bool SortOverlaps(
    std::vector<OverlapCount>& overlaps,
    const std::function<bool()>& getStopRequested)
{
    if (overlaps.size() < 2) return !GetStopped(getStopRequested);
    std::vector<OverlapCount> scratch(overlaps.size());
    std::size_t checkedCount = 0;
    for (std::size_t width = 1; width < overlaps.size();) {
        for (std::size_t begin = 0; begin < overlaps.size();) {
            const std::size_t middle = begin
                + std::min(width, overlaps.size() - begin);
            const std::size_t end = middle
                + std::min(width, overlaps.size() - middle);
            std::size_t left = begin;
            std::size_t right = middle;
            std::size_t write = begin;
            while (write < end) {
                if (checkedCount % cancelBatch == 0
                    && GetStopped(getStopRequested)) {
                    return false;
                }
                ++checkedCount;
                if (left < middle
                    && (right == end
                        || !GetOverlapLess(
                            overlaps[right], overlaps[left]))) {
                    scratch[write++] = overlaps[left++];
                }
                else {
                    scratch[write++] = overlaps[right++];
                }
            }
            begin = end;
        }
        overlaps.swap(scratch);
        width = width > overlaps.size() / 2U
            ? overlaps.size() : width * 2U;
    }
    return !GetStopped(getStopRequested);
}

bool GetMetricsValid(
    const std::vector<PartLabelId>& labels,
    const std::vector<PartMetrics>& metricsByLabel,
    const std::function<bool()>& getStopRequested)
{
    if (metricsByLabel.empty()) return false;
    std::vector<std::uint64_t> histogram(metricsByLabel.size(), 0);
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (index % cancelBatch == 0
            && GetStopped(getStopRequested)) {
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
    if (metricsByLabel[0].voxelCount != 0) return false;
    for (std::size_t index = 1; index < metricsByLabel.size(); ++index) {
        if (index % cancelBatch == 0
            && GetStopped(getStopRequested)) {
            return false;
        }
        const auto& metrics = metricsByLabel[index];
        if (histogram[index] == 0
            || histogram[index] != metrics.voxelCount
            || !std::isfinite(metrics.physicalVolumeMM3)
            || metrics.physicalVolumeMM3 <= 0.0) {
            return false;
        }
    }
    return true;
}

bool AddItems(
    WorkingBudget& budget,
    const std::size_t count,
    const std::size_t itemBytes) noexcept
{
    std::size_t bytes = 0;
    if (!GetProduct(count, itemBytes, bytes)) {
        budget.SetOverflow();
        return false;
    }
    return budget.Add(bytes);
}

bool AddBaseBytes(
    const PartLineageRequest& request,
    WorkingBudget& budget) noexcept
{
    const std::size_t currentSlots =
        request.currentMetricsByLabel.size();
    const std::size_t currentParts = currentSlots == 0
        ? 0 : currentSlots - 1U;
    const std::size_t currentLabelCapacity = request.currentLabels
        ? request.currentLabels->capacity() : 0;
    constexpr std::size_t reverseNodeBytes =
        sizeof(std::pair<const PartObjectId, PartLabelId>)
        + mapNodeOverhead + sizeof(void*) * 2U;
    constexpr std::size_t identityNodeBytes =
        sizeof(PartObjectId) + mapNodeOverhead + sizeof(void*) * 2U;

    if (!AddItems(
            budget, currentLabelCapacity, sizeof(PartLabelId))
        || !AddItems(
            budget,
            request.currentMetricsByLabel.capacity(),
            sizeof(PartMetrics))
        // histogram、degree 与 continuation table 都按 dense N+1 计费。
        || !AddItems(budget, currentSlots, sizeof(std::uint64_t))
        || !AddItems(budget, currentSlots, sizeof(std::size_t))
        || !AddItems(budget, currentSlots, sizeof(PartLabelId))
        // 新 catalog 的 entry、反查表与 continued identity set 共存。
        || !budget.Add(sizeof(PartCatalog))
        || !AddItems(budget, currentSlots, sizeof(PartEntry))
        || !AddItems(budget, currentParts, reverseNodeBytes)
        || !AddItems(budget, currentParts, identityNodeBytes)
        || !budget.Add(
            sizeof(std::vector<OverlapCount>)
            + sizeof(std::unordered_map<std::uint64_t, std::size_t>)
            + sizeof(std::vector<std::size_t>) * 2U
            + sizeof(std::vector<PartLabelId>)
            + sizeof(std::unordered_set<PartObjectId, PartObjectIdHash>))) {
        return false;
    }

    if (request.previous.labels && request.previous.catalog) {
        const std::size_t previousSlots =
            request.previous.catalog->partsByLabel.size();
        const std::size_t previousParts = previousSlots == 0
            ? 0 : previousSlots - 1U;
        std::size_t previousCatalogBytes = 0;
        if (!GetPartCatalogStorageBytes(
                *request.previous.catalog, previousCatalogBytes)
            || !AddItems(
                budget,
                request.previous.labels->capacity(),
                sizeof(PartLabelId))
            || !budget.Add(previousCatalogBytes)
            || !AddItems(budget, previousSlots, sizeof(std::size_t))
            // previous validation histogram 与 retired uniqueness set。
            || !AddItems(budget, previousSlots, sizeof(std::uint64_t))
            || !AddItems(
                budget,
                request.previous.catalog->retiredFromPrevious.size(),
                identityNodeBytes)
            || !AddItems(budget, previousParts, identityNodeBytes)
            // 最坏情况下全部旧对象进入本次 retired vector。
            || !AddItems(
                budget, previousParts, sizeof(PartBindingRef))) {
            return false;
        }
        // Exact continuation 会复制旧 name；把堆字符串与旧 catalog 同时计费。
        for (const auto& entry :
            request.previous.catalog->partsByLabel) {
            if (!AddItems(
                    budget, entry.userState.name.capacity(), sizeof(char))) {
                return false;
            }
        }
    }
    return true;
}

PartBindingRef GetBinding(
    const PartSetId& partSetId,
    const PartObjectId& objectId,
    const std::uint64_t resultRevision) noexcept
{
    return { { partSetId, objectId }, resultRevision };
}

void SetFailure(
    PartLineageResult& result,
    const PartFailureReason reason,
    const WorkingBudget& budget) noexcept
{
    result.failureReason = reason;
    result.catalog.reset();
    result.requiredBytes = budget.GetRequired();
}

} // namespace

PartLineageResult PartLineageMatcher::BuildCatalog(
    PartLineageRequest request,
    PartIdentityFactory& identities,
    const std::function<bool()>& getStopRequested)
{
    PartLineageResult result;
    WorkingBudget budget(request.maxWorkingBytes);
    if (!request.currentLabels
        || request.currentLabels->empty()
        || request.currentMetricsByLabel.empty()
        || request.currentMetricsByLabel.size() - 1U > maxPartCount
        || request.nextResultRevision == 0
        || request.maxWorkingBytes == 0) {
        SetFailure(result, PartFailureReason::InvalidGeometry, budget);
        return result;
    }
    if (GetStopped(getStopRequested)) {
        SetFailure(result, PartFailureReason::Cancelled, budget);
        return result;
    }

    const bool hasPrevious = request.previous.labels
        && request.previous.catalog;
    if (static_cast<bool>(request.previous.labels)
            != static_cast<bool>(request.previous.catalog)
        || (hasPrevious
            && (request.previous.labels->size()
                    != request.currentLabels->size()
                || request.previous.catalog->partsByLabel.empty()
                || request.previous.catalog->partsByLabel.size() - 1U
                    > maxPartCount
                || request.previous.catalog->resultRevision
                    == std::numeric_limits<std::uint64_t>::max()
                || request.previous.catalog->catalogRevision
                    == std::numeric_limits<std::uint64_t>::max()
                || request.nextResultRevision
                    != request.previous.catalog->resultRevision + 1U))
        || (!hasPrevious && request.nextResultRevision != 1)) {
        SetFailure(result, PartFailureReason::InvalidGeometry, budget);
        return result;
    }
    if (!AddBaseBytes(request, budget)) {
        SetFailure(result, PartFailureReason::BudgetExceeded, budget);
        return result;
    }

    try {
        if (!GetMetricsValid(
                *request.currentLabels,
                request.currentMetricsByLabel,
                getStopRequested)
            || (hasPrevious
                && !GetPartCatalogValid(
                    *request.previous.catalog,
                    *request.previous.labels,
                    getStopRequested))) {
            SetFailure(
                result,
                GetStopped(getStopRequested)
                    ? PartFailureReason::Cancelled
                    : PartFailureReason::InvalidGeometry,
                budget);
            return result;
        }

        auto catalog = std::make_shared<PartCatalog>();
        catalog->resultRevision = request.nextResultRevision;
        catalog->catalogRevision = hasPrevious
            ? request.previous.catalog->catalogRevision + 1U : 1U;

        if (hasPrevious) {
            catalog->partSetId = request.previous.catalog->partSetId;
        }
        else {
            const auto partSetId = identities.CreatePartSetId();
            if (!partSetId) {
                SetFailure(result, PartFailureReason::InternalError, budget);
                return result;
            }
            catalog->partSetId = *partSetId;
        }

        std::vector<OverlapCount> overlaps;
        std::unordered_map<std::uint64_t, std::size_t> overlapByPair;
        std::vector<std::size_t> previousDegree;
        std::vector<std::size_t> currentDegree(
            request.currentMetricsByLabel.size(), 0);
        if (hasPrevious) {
            previousDegree.resize(
                request.previous.catalog->partsByLabel.size(), 0);
            for (std::size_t index = 0;
                index < request.currentLabels->size(); ++index) {
                if (index % cancelBatch == 0
                    && GetStopped(getStopRequested)) {
                    SetFailure(
                        result, PartFailureReason::Cancelled, budget);
                    return result;
                }
                const PartLabelId previousLabel =
                    (*request.previous.labels)[index];
                const PartLabelId currentLabel =
                    (*request.currentLabels)[index];
                if (previousLabel == 0 || currentLabel == 0) continue;
                if (previousLabel >= previousDegree.size()
                    || currentLabel >= currentDegree.size()) {
                    SetFailure(
                        result, PartFailureReason::InvalidGeometry, budget);
                    return result;
                }

                const std::uint64_t key = GetOverlapKey(
                    previousLabel, currentLabel);
                const auto found = overlapByPair.find(key);
                if (found != overlapByPair.end()) {
                    if (overlaps[found->second].voxelCount
                        == std::numeric_limits<std::uint64_t>::max()) {
                        SetFailure(
                            result,
                            PartFailureReason::InvalidGeometry,
                            budget);
                        return result;
                    }
                    ++overlaps[found->second].voxelCount;
                    continue;
                }
                constexpr std::size_t overlapBytes =
                    // vector growth is at most doubled by the MSVC C++17
                    // implementation used by this product baseline；第三份
                    // slot 用于可取消 merge-sort scratch。
                    sizeof(OverlapCount) * 3U
                    + sizeof(std::pair<const std::uint64_t, std::size_t>)
                    + mapNodeOverhead + sizeof(void*) * 2U
                    // 每个实际 pair 最多投影 Split 与 Merge 两条关系。
                    + sizeof(PartRelation) * 2U;
                if (!budget.Add(overlapBytes)) {
                    SetFailure(
                        result,
                        PartFailureReason::BudgetExceeded,
                        budget);
                    return result;
                }
                const std::size_t overlapIndex = overlaps.size();
                overlaps.push_back({ previousLabel, currentLabel, 1 });
                overlapByPair.emplace(key, overlapIndex);
                if (previousDegree[previousLabel]
                        == std::numeric_limits<std::size_t>::max()
                    || currentDegree[currentLabel]
                        == std::numeric_limits<std::size_t>::max()) {
                    SetFailure(
                        result,
                        PartFailureReason::InvalidGeometry,
                        budget);
                    return result;
                }
                ++previousDegree[previousLabel];
                ++currentDegree[currentLabel];
            }
            if (!SortOverlaps(overlaps, getStopRequested)) {
                SetFailure(result, PartFailureReason::Cancelled, budget);
                return result;
            }
        }

        std::vector<PartLabelId> continuedFrom(
            request.currentMetricsByLabel.size(), 0);
        for (std::size_t index = 0; index < overlaps.size(); ++index) {
            if (index % cancelBatch == 0
                && GetStopped(getStopRequested)) {
                SetFailure(result, PartFailureReason::Cancelled, budget);
                return result;
            }
            const auto& overlap = overlaps[index];
            const auto oldCount = request.previous.catalog
                ->partsByLabel[overlap.previousLabel].metrics.voxelCount;
            const auto newCount = request.currentMetricsByLabel[
                overlap.currentLabel].voxelCount;
            if (overlap.voxelCount == oldCount
                && overlap.voxelCount == newCount
                && previousDegree[overlap.previousLabel] == 1
                && currentDegree[overlap.currentLabel] == 1) {
                continuedFrom[overlap.currentLabel] = overlap.previousLabel;
            }
        }

        std::size_t relationCapacity = 0;
        if (!GetProduct(overlaps.size(), 2U, relationCapacity)) {
            budget.SetOverflow();
            SetFailure(result, PartFailureReason::BudgetExceeded, budget);
            return result;
        }
        catalog->partsByLabel.resize(request.currentMetricsByLabel.size());
        catalog->labelByObject.reserve(
            request.currentMetricsByLabel.size() - 1U);
        catalog->relationsFromPrevious.reserve(relationCapacity);
        if (hasPrevious) {
            catalog->retiredFromPrevious.reserve(
                request.previous.catalog->partsByLabel.size() - 1U);
        }
        std::unordered_set<PartObjectId, PartObjectIdHash> continuedObjects;
        continuedObjects.reserve(
            request.currentMetricsByLabel.size() - 1U);
        for (std::size_t index = 1;
            index < request.currentMetricsByLabel.size(); ++index) {
            if (index % cancelBatch == 0
                && GetStopped(getStopRequested)) {
                SetFailure(result, PartFailureReason::Cancelled, budget);
                return result;
            }
            const PartLabelId currentLabel =
                static_cast<PartLabelId>(index);
            auto& entry = catalog->partsByLabel[index];
            const PartLabelId previousLabel = continuedFrom[index];
            if (previousLabel != 0) {
                entry = request.previous.catalog->partsByLabel[
                    previousLabel];
                continuedObjects.insert(entry.objectId);
            }
            else {
                const auto objectId = identities.CreatePartObjectId(
                    [&](const PartObjectId& candidate) {
                        return catalog->labelByObject.find(candidate)
                                != catalog->labelByObject.end()
                            || (hasPrevious
                                && request.previous.catalog->labelByObject.find(
                                    candidate)
                                    != request.previous.catalog
                                        ->labelByObject.end());
                    });
                if (!objectId) {
                    SetFailure(
                        result, PartFailureReason::InternalError, budget);
                    return result;
                }
                entry.objectId = *objectId;
                entry.presentation.color = GetPartStableColor(*objectId);
            }
            entry.labelId = currentLabel;
            entry.metrics = request.currentMetricsByLabel[index];
            if (!catalog->labelByObject.emplace(
                    entry.objectId, currentLabel).second) {
                SetFailure(result, PartFailureReason::InternalError, budget);
                return result;
            }

            if (previousLabel != 0) {
                catalog->relationsFromPrevious.push_back({
                    GetBinding(
                        catalog->partSetId,
                        entry.objectId,
                        catalog->resultRevision),
                    GetBinding(
                        catalog->partSetId,
                        entry.objectId,
                        request.previous.catalog->resultRevision),
                    PartRelationKind::ContinuedFrom,
                    1.0
                });
            }
        }

        for (std::size_t index = 0; index < overlaps.size(); ++index) {
            if (index % cancelBatch == 0
                && GetStopped(getStopRequested)) {
                SetFailure(result, PartFailureReason::Cancelled, budget);
                return result;
            }
            const auto& overlap = overlaps[index];
            if (continuedFrom[overlap.currentLabel]
                == overlap.previousLabel) {
                continue;
            }
            const auto& currentEntry =
                catalog->partsByLabel[overlap.currentLabel];
            const auto& previousEntry = request.previous.catalog
                ->partsByLabel[overlap.previousLabel];
            const auto oldCount = previousEntry.metrics.voxelCount;
            const auto newCount = currentEntry.metrics.voxelCount;
            if (overlap.voxelCount > oldCount
                || overlap.voxelCount > newCount
                || newCount
                    > std::numeric_limits<std::uint64_t>::max() - oldCount) {
                SetFailure(
                    result, PartFailureReason::InvalidGeometry, budget);
                return result;
            }
            const std::uint64_t unionCount =
                oldCount + newCount - overlap.voxelCount;
            const double score = unionCount == 0
                ? 0.0
                : static_cast<double>(overlap.voxelCount)
                    / static_cast<double>(unionCount);
            const auto current = GetBinding(
                catalog->partSetId,
                currentEntry.objectId,
                catalog->resultRevision);
            const auto previous = GetBinding(
                catalog->partSetId,
                previousEntry.objectId,
                request.previous.catalog->resultRevision);
            if (previousDegree[overlap.previousLabel] > 1) {
                catalog->relationsFromPrevious.push_back({
                    current, previous, PartRelationKind::SplitFrom, score
                });
            }
            if (currentDegree[overlap.currentLabel] > 1) {
                catalog->relationsFromPrevious.push_back({
                    current, previous, PartRelationKind::MergedFrom, score
                });
            }
        }

        if (hasPrevious) {
            for (std::size_t index = 1;
                index < request.previous.catalog->partsByLabel.size();
                ++index) {
                if ((index - 1U) % cancelBatch == 0
                    && GetStopped(getStopRequested)) {
                    SetFailure(
                        result, PartFailureReason::Cancelled, budget);
                    return result;
                }
                const auto& entry =
                    request.previous.catalog->partsByLabel[index];
                if (continuedObjects.find(entry.objectId)
                    != continuedObjects.end()) {
                    continue;
                }
                catalog->retiredFromPrevious.push_back(GetBinding(
                    catalog->partSetId,
                    entry.objectId,
                    request.previous.catalog->resultRevision));
            }
        }

        if (GetStopped(getStopRequested)) {
            SetFailure(result, PartFailureReason::Cancelled, budget);
            return result;
        }
        if (!GetPartCatalogValid(
                *catalog,
                *request.currentLabels,
                getStopRequested)) {
            SetFailure(
                result,
                GetStopped(getStopRequested)
                    ? PartFailureReason::Cancelled
                    : PartFailureReason::InternalError,
                budget);
            return result;
        }
        result.failureReason = PartFailureReason::None;
        result.catalog = std::move(catalog);
        result.requiredBytes = budget.GetRequired();
        return result;
    }
    catch (const std::bad_alloc&) {
        SetFailure(result, PartFailureReason::BudgetExceeded, budget);
        return result;
    }
    catch (...) {
        SetFailure(result, PartFailureReason::InternalError, budget);
        return result;
    }
}
