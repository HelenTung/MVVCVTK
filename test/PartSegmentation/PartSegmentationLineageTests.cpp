#include "PartSegmentationTestCases.h"

#include "Model/PartCatalog.h"
#include "Model/PartIdentityFactory.h"
#include "Model/PartLineageMatcher.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool GetCaseResult(const bool passed, const std::string_view name)
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

std::vector<PartMetrics> BuildMetrics(
    const std::vector<PartLabelId>& labels)
{
    const auto maxLabel = labels.empty()
        ? PartLabelId{} : *std::max_element(labels.begin(), labels.end());
    std::vector<PartMetrics> metrics(
        static_cast<std::size_t>(maxLabel) + 1U);
    for (const PartLabelId label : labels) {
        if (label != 0) ++metrics[label].voxelCount;
    }
    for (PartLabelId label = 1; label <= maxLabel; ++label) {
        auto& value = metrics[label];
        value.physicalVolumeMM3 = static_cast<double>(value.voxelCount);
        value.voxelExtent = {
            static_cast<int>(label), static_cast<int>(label),
            0, 0, 0, 0
        };
        value.inputPhysicalBounds = {
            static_cast<double>(label), static_cast<double>(label),
            0.0, 0.0, 0.0, 0.0
        };
        value.centroidInputPhysical = {
            static_cast<double>(label), 0.0, 0.0
        };
    }
    return metrics;
}

std::shared_ptr<const PartCatalog> BuildPrevious(
    const std::vector<PartLabelId>& labels,
    const std::vector<PartObjectId>& ids)
{
    auto catalog = std::make_shared<PartCatalog>();
    catalog->partSetId = { 7, 8 };
    catalog->resultRevision = 1;
    catalog->catalogRevision = 3;
    const auto metrics = BuildMetrics(labels);
    catalog->partsByLabel.resize(metrics.size());
    for (std::size_t index = 1; index < metrics.size(); ++index) {
        auto& entry = catalog->partsByLabel[index];
        entry.objectId = ids[index - 1];
        entry.labelId = static_cast<PartLabelId>(index);
        entry.metrics = metrics[index];
        entry.userState.name = "part-" + std::to_string(index);
        entry.userState.isReviewed = true;
        entry.presentation.color = GetPartStableColor(entry.objectId);
        catalog->labelByObject.emplace(entry.objectId, entry.labelId);
    }
    return catalog;
}

PartIdentityFactory BuildFactory(const std::uint64_t first)
{
    auto next = std::make_shared<std::uint64_t>(first);
    return PartIdentityFactory([next] {
        const std::uint64_t value = (*next)++;
        return PartObjectId{ value, value + 1000U };
    });
}

PartLineageResult BuildLineage(
    const std::vector<PartLabelId>& current,
    const std::shared_ptr<const std::vector<PartLabelId>>& previousLabels,
    const std::shared_ptr<const PartCatalog>& previousCatalog,
    PartIdentityFactory& factory,
    const std::size_t maxWorkingBytes = 1024U * 1024U,
    const std::function<bool()>& getStopRequested = nullptr)
{
    PartLineageRequest request;
    request.previous = { previousLabels, previousCatalog };
    request.currentLabels =
        std::make_shared<const std::vector<PartLabelId>>(current);
    request.currentMetricsByLabel = BuildMetrics(current);
    request.nextResultRevision = previousCatalog
        ? previousCatalog->resultRevision + 1U : 1U;
    request.maxWorkingBytes = maxWorkingBytes;
    return PartLineageMatcher::BuildCatalog(
        std::move(request), factory, getStopRequested);
}

std::size_t GetRelationCount(
    const PartCatalog& catalog,
    const PartRelationKind kind)
{
    return static_cast<std::size_t>(std::count_if(
        catalog.relationsFromPrevious.begin(),
        catalog.relationsFromPrevious.end(),
        [kind](const PartRelation& relation) {
            return relation.kind == kind;
        }));
}

} // namespace

int GetPartLineageFailCount()
{
    int failureCount = 0;

    const std::vector<PartLabelId> initialLabels{ 1, 1, 0, 2 };
    auto initialFactory = BuildFactory(100);
    const auto initial = BuildLineage(
        initialLabels, {}, {}, initialFactory);
    failureCount += GetCaseResult(
        initial.failureReason == PartFailureReason::None
            && initial.catalog
            && initial.catalog->resultRevision == 1
            && initial.catalog->catalogRevision == 1
            && initial.catalog->partsByLabel.size() == 3
            && GetPartCatalogValid(*initial.catalog, initialLabels),
        "Initial lineage creates a valid PartSet and object identities")
        ? 0 : 1;

    const std::vector<PartLabelId> oldLabels{ 1, 1, 0, 2 };
    const auto oldLabelOwner =
        std::make_shared<const std::vector<PartLabelId>>(oldLabels);
    const std::vector<PartObjectId> oldIds{ { 11, 12 }, { 21, 22 } };
    const auto oldCatalog = BuildPrevious(oldLabels, oldIds);
    auto exactFactory = BuildFactory(200);
    const std::vector<PartLabelId> reordered{ 2, 2, 0, 1 };
    const auto exact = BuildLineage(
        reordered, oldLabelOwner, oldCatalog, exactFactory);
    failureCount += GetCaseResult(
        exact.failureReason == PartFailureReason::None
            && exact.catalog
            && exact.catalog->partSetId == oldCatalog->partSetId
            && exact.catalog->partsByLabel[1].objectId == oldIds[1]
            && exact.catalog->partsByLabel[2].objectId == oldIds[0]
            && exact.catalog->partsByLabel[1].userState.name == "part-2"
            && GetRelationCount(
                *exact.catalog, PartRelationKind::ContinuedFrom) == 2
            && exact.catalog->retiredFromPrevious.empty(),
        "Exact continuation survives a dense label reorder") ? 0 : 1;

    const std::vector<PartLabelId> splitOld{ 1, 1, 1, 1 };
    const auto splitOwner =
        std::make_shared<const std::vector<PartLabelId>>(splitOld);
    const auto splitCatalog = BuildPrevious(splitOld, { { 31, 32 } });
    auto splitFactory = BuildFactory(300);
    const auto split = BuildLineage(
        { 1, 1, 2, 2 }, splitOwner, splitCatalog, splitFactory);
    failureCount += GetCaseResult(
        split.catalog
            && split.catalog->partsByLabel[1].objectId
                != splitCatalog->partsByLabel[1].objectId
            && split.catalog->partsByLabel[2].objectId
                != splitCatalog->partsByLabel[1].objectId
            && GetRelationCount(
                *split.catalog, PartRelationKind::SplitFrom) == 2
            && split.catalog->retiredFromPrevious.size() == 1,
        "Split creates new identities and retires the old object") ? 0 : 1;

    const std::vector<PartLabelId> mergeOld{ 1, 1, 2, 2 };
    const auto mergeOwner =
        std::make_shared<const std::vector<PartLabelId>>(mergeOld);
    const auto mergeCatalog = BuildPrevious(
        mergeOld, { { 41, 42 }, { 51, 52 } });
    auto mergeFactory = BuildFactory(400);
    const auto merge = BuildLineage(
        { 1, 1, 1, 1 }, mergeOwner, mergeCatalog, mergeFactory);
    failureCount += GetCaseResult(
        merge.catalog
            && merge.catalog->partsByLabel[1].objectId != PartObjectId{ 41, 42 }
            && merge.catalog->partsByLabel[1].objectId != PartObjectId{ 51, 52 }
            && GetRelationCount(
                *merge.catalog, PartRelationKind::MergedFrom) == 2
            && merge.catalog->retiredFromPrevious.size() == 2,
        "Merge creates one new identity and retires every source") ? 0 : 1;

    auto newFactory = BuildFactory(500);
    const auto disjoint = BuildLineage(
        { 0, 0, 1, 1 },
        std::make_shared<const std::vector<PartLabelId>>(
            std::vector<PartLabelId>{ 1, 1, 0, 0 }),
        BuildPrevious({ 1, 1, 0, 0 }, { { 61, 62 } }),
        newFactory);
    failureCount += GetCaseResult(
        disjoint.catalog
            && disjoint.catalog->relationsFromPrevious.empty()
            && disjoint.catalog->retiredFromPrevious.size() == 1,
        "Disjoint replacement creates a new object and retires the old one")
        ? 0 : 1;

    auto manyFactory = BuildFactory(600);
    const auto many = BuildLineage(
        { 1, 2, 1, 2 }, mergeOwner, mergeCatalog, manyFactory);
    failureCount += GetCaseResult(
        many.catalog
            && GetRelationCount(
                *many.catalog, PartRelationKind::SplitFrom) == 4
            && GetRelationCount(
                *many.catalog, PartRelationKind::MergedFrom) == 4
            && std::all_of(
                many.catalog->relationsFromPrevious.begin(),
                many.catalog->relationsFromPrevious.end(),
                [&](const PartRelation& relation) {
                    return relation.current.object.partSetId
                            == many.catalog->partSetId
                        && relation.previous.object.partSetId
                            == many.catalog->partSetId
                        && relation.current.resultRevision == 2
                        && relation.previous.resultRevision == 1
                        && std::abs(
                            relation.overlapScore - (1.0 / 3.0)) < 1e-12;
                })
            && many.catalog->retiredFromPrevious.size() == 2,
        "Many-to-many overlap records directional split/merge IoU relations")
        ? 0 : 1;

    auto failureFactory = BuildFactory(700);
    const auto budget = BuildLineage(
        reordered, oldLabelOwner, oldCatalog, failureFactory, 1);
    const auto cancelled = BuildLineage(
        reordered,
        oldLabelOwner,
        oldCatalog,
        failureFactory,
        1024U * 1024U,
        [] { return true; });
    const auto wrongSize = BuildLineage(
        { 1 }, oldLabelOwner, oldCatalog, failureFactory);
    failureCount += GetCaseResult(
        budget.failureReason == PartFailureReason::BudgetExceeded
            && budget.requiredBytes > 1
            && !budget.catalog
            && cancelled.failureReason == PartFailureReason::Cancelled
            && !cancelled.catalog
            && wrongSize.failureReason == PartFailureReason::InvalidGeometry
            && !wrongSize.catalog,
        "Lineage failure is bounded, cancellable, and transactional")
        ? 0 : 1;

    auto measuredFactory = BuildFactory(800);
    const auto measured = BuildLineage(
        { 1, 2, 1, 2 }, mergeOwner, mergeCatalog, measuredFactory);
    auto exactBudgetFactory = BuildFactory(900);
    const auto exactBudget = measured.requiredBytes == 0
        ? PartLineageResult{}
        : BuildLineage(
            { 1, 2, 1, 2 },
            mergeOwner,
            mergeCatalog,
            exactBudgetFactory,
            measured.requiredBytes);
    auto belowBudgetFactory = BuildFactory(1000);
    const auto belowBudget = measured.requiredBytes == 0
        ? PartLineageResult{}
        : BuildLineage(
            { 1, 2, 1, 2 },
            mergeOwner,
            mergeCatalog,
            belowBudgetFactory,
            measured.requiredBytes - 1U);
    failureCount += GetCaseResult(
        measured.catalog
            && exactBudget.catalog
            && exactBudget.requiredBytes == measured.requiredBytes
            && belowBudget.failureReason
                == PartFailureReason::BudgetExceeded
            && !belowBudget.catalog
            && belowBudget.requiredBytes > measured.requiredBytes - 1U,
        "Lineage measured peak is an exact transactional budget boundary")
        ? 0 : 1;

    std::vector<PartLabelId> longLabels(8192, 1);
    const auto longPrevious =
        std::make_shared<const std::vector<PartLabelId>>(longLabels);
    const auto longCatalog = BuildPrevious(longLabels, { { 71, 72 } });
    int cancelChecks = 0;
    auto delayedCancelFactory = BuildFactory(1100);
    const auto delayedCancelled = BuildLineage(
        longLabels,
        longPrevious,
        longCatalog,
        delayedCancelFactory,
        4U * 1024U * 1024U,
        [&cancelChecks] { return ++cancelChecks >= 2; });
    failureCount += GetCaseResult(
        delayedCancelled.failureReason == PartFailureReason::Cancelled
            && !delayedCancelled.catalog
            && cancelChecks >= 2,
        "Lineage observes cancellation during a long validation scan")
        ? 0 : 1;

    PartIdentityFactory emptyIdentityFactory([] { return PartObjectId{}; });
    const auto identityFailure = BuildLineage(
        initialLabels, {}, {}, emptyIdentityFactory);
    auto overflowCatalog = std::make_shared<PartCatalog>(*oldCatalog);
    overflowCatalog->resultRevision =
        std::numeric_limits<std::uint64_t>::max();
    overflowCatalog->catalogRevision =
        std::numeric_limits<std::uint64_t>::max();
    PartLineageRequest overflowRequest;
    overflowRequest.previous = { oldLabelOwner, overflowCatalog };
    overflowRequest.currentLabels =
        std::make_shared<const std::vector<PartLabelId>>(reordered);
    overflowRequest.currentMetricsByLabel = BuildMetrics(reordered);
    overflowRequest.nextResultRevision = 1;
    overflowRequest.maxWorkingBytes = 1024U * 1024U;
    auto overflowFactory = BuildFactory(1200);
    const auto overflow = PartLineageMatcher::BuildCatalog(
        std::move(overflowRequest), overflowFactory);
    failureCount += GetCaseResult(
        identityFailure.failureReason == PartFailureReason::InternalError
            && !identityFailure.catalog
            && overflow.failureReason == PartFailureReason::InvalidGeometry
            && !overflow.catalog,
        "Identity exhaustion and revision overflow publish no partial catalog")
        ? 0 : 1;

    return failureCount;
}
