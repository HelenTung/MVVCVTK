#include "../TestDataPort.h"
#include "PartSegmentationTestCases.h"

#include "Model/PartCatalog.h"
#include "Model/PartIdentityFactory.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool GetCaseResult(const bool passed, const std::string_view name)
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

PartEntry BuildEntry(
    const PartLabelId labelId,
    const std::uint64_t objectLow,
    const std::uint64_t voxelCount)
{
    PartEntry entry;
    entry.objectId = { 0xabcU, objectLow };
    entry.labelId = labelId;
    entry.metrics.voxelCount = voxelCount;
    entry.metrics.physicalVolumeMM3 = static_cast<double>(voxelCount);
    entry.metrics.voxelExtent = {
        static_cast<int>(labelId), static_cast<int>(labelId),
        0, 0, 0, 0
    };
    entry.metrics.inputPhysicalBounds = {
        static_cast<double>(labelId), static_cast<double>(labelId),
        0.0, 0.0, 0.0, 0.0
    };
    entry.metrics.centroidInputPhysical = {
        static_cast<double>(labelId), 0.0, 0.0
    };
    entry.presentation.color = GetPartStableColor(entry.objectId);
    return entry;
}

PartCatalog BuildCatalog()
{
    PartCatalog catalog;
    catalog.partSetId = { 0x123U, 0x456U };
    catalog.resultRevision = 1;
    catalog.catalogRevision = 1;
    catalog.partsByLabel.resize(3);
    catalog.partsByLabel[1] = BuildEntry(1, 1, 2);
    catalog.partsByLabel[2] = BuildEntry(2, 2, 1);
    catalog.labelByObject.emplace(catalog.partsByLabel[1].objectId, 1);
    catalog.labelByObject.emplace(catalog.partsByLabel[2].objectId, 2);
    return catalog;
}

} // namespace

int GetPartCatalogFailCount()
{
    int failureCount = 0;
    const std::vector<PartLabelId> labels{ 1, 1, 0, 2 };
    const auto catalog = BuildCatalog();

    failureCount += GetCaseResult(
        catalog.partsByLabel.size() == 3
            && catalog.partsByLabel[0].labelId == 0
            && !GetPartObjectIdValid(catalog.partsByLabel[0].objectId)
            && GetPartCatalogValid(catalog, labels),
        "Part catalog reserves label zero and forms a bijection") ? 0 : 1;

    const auto snapshot = BuildPartSetSnapshot(catalog, GetTestDataRef(42), false);
    failureCount += GetCaseResult(
        snapshot
            && snapshot->partSetId == catalog.partSetId
            && snapshot->sourceRevision == GetTestDataRef(42)
            && snapshot->resultRevision == 1
            && snapshot->catalogRevision == 1
            && !snapshot->isStale
            && snapshot->parts.size() == 2
            && snapshot->parts[0].labelId == 1
            && snapshot->parts[1].labelId == 2
            && snapshot->parts[0].binding.object.objectId
                == catalog.partsByLabel[1].objectId,
        "Part snapshot is an ordered immutable value projection") ? 0 : 1;

    auto duplicate = catalog;
    duplicate.partsByLabel[2].objectId = duplicate.partsByLabel[1].objectId;
    duplicate.labelByObject.clear();
    duplicate.labelByObject.emplace(duplicate.partsByLabel[1].objectId, 1);
    failureCount += GetCaseResult(
        !GetPartCatalogValid(duplicate, labels),
        "Part catalog rejects duplicate object identity") ? 0 : 1;

    auto labelGap = catalog;
    labelGap.partsByLabel[2].labelId = 3;
    auto zeroIdentity = catalog;
    zeroIdentity.partsByLabel[1].objectId = {};
    auto reverseMismatch = catalog;
    reverseMismatch.labelByObject[catalog.partsByLabel[1].objectId] = 2;
    const std::vector<PartLabelId> outOfRangeLabels{ 1, 1, 0, 3 };
    failureCount += GetCaseResult(
        !GetPartCatalogValid(labelGap, labels)
            && !GetPartCatalogValid(zeroIdentity, labels)
            && !GetPartCatalogValid(reverseMismatch, labels)
            && !GetPartCatalogValid(catalog, outOfRangeLabels),
        "Part catalog rejects label, identity, and reverse-map corruption")
        ? 0 : 1;

    constexpr PartLabelId maxPartCount = 4096;
    PartCatalog maximumCatalog;
    maximumCatalog.partSetId = { 1, 1 };
    maximumCatalog.resultRevision = 1;
    maximumCatalog.catalogRevision = 1;
    maximumCatalog.partsByLabel.resize(
        static_cast<std::size_t>(maxPartCount) + 1U);
    std::vector<PartLabelId> maximumLabels;
    maximumLabels.reserve(maxPartCount);
    for (PartLabelId label = 1; label <= maxPartCount; ++label) {
        maximumCatalog.partsByLabel[label] = BuildEntry(label, label, 1);
        maximumCatalog.labelByObject.emplace(
            maximumCatalog.partsByLabel[label].objectId, label);
        maximumLabels.push_back(label);
    }
    failureCount += GetCaseResult(
        maximumCatalog.partsByLabel.size() == 4097
            && GetPartCatalogValid(maximumCatalog, maximumLabels),
        "Part catalog accepts the exact 4096-entry boundary") ? 0 : 1;

    std::vector<PartObjectId> identities{
        {}, { 9, 9 }, { 9, 9 }, { 10, 10 }
    };
    auto identityIndex = std::make_shared<std::size_t>(0);
    PartIdentityFactory factory(
        [identities, identityIndex]() mutable {
            if (*identityIndex >= identities.size()) {
                return PartObjectId{};
            }
            return identities[(*identityIndex)++];
        });
    const auto objectId = factory.CreatePartObjectId(
        [](const PartObjectId& candidate) {
            return candidate == PartObjectId{ 9, 9 };
        });
    failureCount += GetCaseResult(
        objectId && *objectId == PartObjectId{ 10, 10 },
        "Identity factory rejects zero and catalog collisions") ? 0 : 1;

    auto collisionCount = std::make_shared<int>(0);
    PartIdentityFactory exhaustedFactory([collisionCount] {
        ++*collisionCount;
        return PartObjectId{ 9, 9 };
    });
    const auto exhausted = exhaustedFactory.CreatePartObjectId(
        [](const PartObjectId&) { return true; });
    failureCount += GetCaseResult(
        !exhausted && *collisionCount == 16,
        "Identity factory stops after sixteen consecutive collisions")
        ? 0 : 1;

    PartIdentityFactory throwingFactory([]() -> PartObjectId {
        throw std::runtime_error("random source failed");
    });
    failureCount += GetCaseResult(
        !throwingFactory.CreatePartSetId()
            && !throwingFactory.CreatePartObjectId({}),
        "Identity factory converts random-source failure to no identity")
        ? 0 : 1;

    const auto firstColor = GetPartStableColor({ 1, 2 });
    const auto sameColor = GetPartStableColor({ 1, 2 });
    const auto otherColor = GetPartStableColor({ 2, 1 });
    failureCount += GetCaseResult(
        firstColor == sameColor && firstColor != otherColor,
        "Stable color depends only on object identity") ? 0 : 1;

    auto mutableCatalog = catalog;
    PartStatePatch namePatch;
    namePatch.name = "reviewed part";
    const PartBindingRef firstBinding{
        { catalog.partSetId, catalog.partsByLabel[1].objectId },
        catalog.resultRevision
    };
    const auto named = SetPartCatalogState(
        mutableCatalog, firstBinding, namePatch, 1);
    const auto namedAgain = SetPartCatalogState(
        mutableCatalog, firstBinding, namePatch, named.catalogRevision);
    failureCount += GetCaseResult(
        named.status == PartMutationStatus::Succeeded
            && named.catalogRevision == 2
            && mutableCatalog.partsByLabel[1].userState.name
                == "reviewed part"
            && namedAgain.status == PartMutationStatus::Succeeded
            && namedAgain.catalogRevision == 2,
        "Catalog mutation increments once and identical state is idempotent")
        ? 0 : 1;

    PartStatePatch selectFirst;
    selectFirst.isSelected = true;
    const auto firstSelected = SetPartCatalogState(
        mutableCatalog,
        firstBinding,
        selectFirst,
        mutableCatalog.catalogRevision);
    const PartBindingRef secondBinding{
        { catalog.partSetId, catalog.partsByLabel[2].objectId },
        catalog.resultRevision
    };
    const auto secondSelected = SetPartCatalogState(
        mutableCatalog,
        secondBinding,
        selectFirst,
        mutableCatalog.catalogRevision);
    failureCount += GetCaseResult(
        firstSelected.status == PartMutationStatus::Succeeded
            && secondSelected.status == PartMutationStatus::Succeeded
            && !mutableCatalog.partsByLabel[1].presentation.isSelected
            && mutableCatalog.partsByLabel[2].presentation.isSelected,
        "Selecting one object atomically clears the previous selection")
        ? 0 : 1;

    PartStatePatch emptyPatch;
    PartStatePatch emptyName;
    emptyName.name = "";
    PartStatePatch longName;
    longName.name = std::string(256, 'x');
    PartStatePatch badOpacity;
    badOpacity.opacity = std::numeric_limits<double>::quiet_NaN();
    PartStatePatch badColor;
    badColor.color = PartColorPatch{
        PartColorUse::Custom, { 1.0, 0.0, 0.0, 1.5 }
    };
    failureCount += GetCaseResult(
        SetPartCatalogState(
            mutableCatalog,
            firstBinding,
            emptyPatch,
            mutableCatalog.catalogRevision).status
                == PartMutationStatus::InvalidRequest
            && SetPartCatalogState(
                mutableCatalog,
                firstBinding,
                emptyName,
                mutableCatalog.catalogRevision).status
                == PartMutationStatus::InvalidRequest
            && SetPartCatalogState(
                mutableCatalog,
                firstBinding,
                longName,
                mutableCatalog.catalogRevision).status
                == PartMutationStatus::InvalidRequest
            && SetPartCatalogState(
                mutableCatalog,
                firstBinding,
                badOpacity,
                mutableCatalog.catalogRevision).status
                == PartMutationStatus::InvalidRequest
            && SetPartCatalogState(
                mutableCatalog,
                firstBinding,
                badColor,
                mutableCatalog.catalogRevision).status
                == PartMutationStatus::InvalidRequest,
        "Catalog mutation rejects empty and non-finite patches") ? 0 : 1;

    PartStatePatch visibilityPatch;
    visibilityPatch.isVisible = false;
    auto wrongSet = firstBinding;
    wrongSet.object.partSetId.low += 1;
    auto oldRevision = firstBinding;
    oldRevision.resultRevision = 0;
    auto unknownObject = firstBinding;
    unknownObject.object.objectId = { 999, 999 };
    failureCount += GetCaseResult(
        SetPartCatalogState(
            mutableCatalog,
            wrongSet,
            visibilityPatch,
            mutableCatalog.catalogRevision).status
                == PartMutationStatus::NotFound
            && SetPartCatalogState(
                mutableCatalog,
                oldRevision,
                visibilityPatch,
                mutableCatalog.catalogRevision).status
                == PartMutationStatus::StaleReference
            && SetPartCatalogState(
                mutableCatalog,
                unknownObject,
                visibilityPatch,
                mutableCatalog.catalogRevision).status
                == PartMutationStatus::NotFound
            && SetPartCatalogState(
                mutableCatalog,
                firstBinding,
                visibilityPatch,
                mutableCatalog.catalogRevision - 1U).status
                == PartMutationStatus::RevisionConflict,
        "Catalog mutation rejects wrong identity and revision") ? 0 : 1;

    auto maxRevisionCatalog = catalog;
    maxRevisionCatalog.catalogRevision =
        std::numeric_limits<std::uint64_t>::max();
    PartStatePatch maxRevisionPatch;
    maxRevisionPatch.name = "must not commit";
    maxRevisionPatch.isSelected = true;
    const auto maxRevisionMutation = SetPartCatalogState(
        maxRevisionCatalog,
        firstBinding,
        maxRevisionPatch,
        maxRevisionCatalog.catalogRevision);
    failureCount += GetCaseResult(
        maxRevisionMutation.status == PartMutationStatus::RevisionConflict
            && maxRevisionCatalog.catalogRevision
                == std::numeric_limits<std::uint64_t>::max()
            && maxRevisionCatalog.partsByLabel[1].userState.name.empty()
            && !maxRevisionCatalog.partsByLabel[1]
                .presentation.isSelected
            && !maxRevisionCatalog.partsByLabel[2]
                .presentation.isSelected,
        "Revision overflow rejects mutation without changing the catalog")
        ? 0 : 1;

    return failureCount;
}
