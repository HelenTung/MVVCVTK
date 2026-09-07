#include "PartSegmentationTestCases.h"
#include "Algorithms/PartLabelEditor.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <limits>

namespace {

class EditCase final {
public:
    explicit EditCase(std::vector<PartLabelId> labels)
        : values(labels.size(), 1.0)
    {
        input.volume.dimensions = { static_cast<int>(labels.size()), 1, 1 };
        input.volume.extent = { 0, static_cast<int>(labels.size()) - 1, 0, 0, 0, 0 };
        input.volume.values = { values.data(), values.size(), PartScalarType::Float64 };
        input.maxWorkingBytes = 128U * 1024U * 1024U;
        input.previous.labels = std::make_shared<const std::vector<PartLabelId>>(std::move(labels));
        const auto count = *std::max_element(input.previous.labels->begin(), input.previous.labels->end());
        const auto metrics = ClassicalPartSegmenter::BuildLabelMetrics(input.volume, *input.previous.labels, count);
        auto catalog = std::make_shared<PartCatalog>();
        catalog->partSetId = { 1, 1 };
        catalog->resultRevision = 1;
        catalog->catalogRevision = 1;
        catalog->partsByLabel.resize(static_cast<std::size_t>(count) + 1);
        for (PartLabelId label = 1; label <= count; ++label) {
            auto& entry = catalog->partsByLabel[label];
            entry.labelId = label;
            entry.objectId = { 7, label };
            entry.metrics = (*metrics)[label];
            entry.metrics.confidence = 0.95;
            entry.userState = { "Original " + std::to_string(label), true };
            entry.presentation.color = GetPartStableColor(entry.objectId);
            catalog->labelByObject.emplace(entry.objectId, label);
        }
        input.previous.catalog = catalog;
        input.request.expectedCatalogRevision = 1;
    }

    PartBindingRef GetPart(PartLabelId label) const
    {
        const auto& c = *input.previous.catalog;
        return { { c.partSetId, c.partsByLabel[label].objectId }, c.resultRevision };
    }
    PartEditBuildResult Build() { return PartLabelEditor::BuildLabels(input, identities); }
    std::vector<double> values;
    PartEditInput input;
    PartIdentityFactory identities;
};

bool GetSameObjects(const PartCatalog& a, const PartCatalog& b)
{
    return a.labelByObject == b.labelByObject;
}

} // namespace

int GetPartEditFailCount()
{
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!passed) ++failures;
    };
    EditCase splitCase({ 1, 1, 1, 1, 1 });
    PartSplitEdit split;
    split.target = splitCase.GetPart(1);
    split.seeds = { { { 0, 0, 0 }, 1 }, { { 4, 0, 0 }, 2 } };
    split.barriers = { { { 2, 0, 0 }, 0 } };
    splitCase.input.request.operation = split;
    auto divided = splitCase.Build();
    check(divided.failureReason == PartFailureReason::None && divided.labels
        && *divided.labels == std::vector<PartLabelId>({ 1, 1, 1, 2, 2 })
        && divided.catalog->retiredFromPrevious.size() == 1
        && divided.catalog->relationsFromPrevious.size() == 2
        && std::all_of(divided.catalog->relationsFromPrevious.begin(), divided.catalog->relationsFromPrevious.end(),
            [](const auto& r) { return r.kind == PartRelationKind::SplitFrom; }),
        "Constrained split conserves all voxels and retires the parent");
    std::reverse(split.seeds.begin(), split.seeds.end());
    splitCase.input.request.operation = split;
    auto reversed = splitCase.Build();
    check(reversed.labels && divided.labels && *reversed.labels == *divided.labels,
        "Split labels do not depend on seed input order");
    split.seeds = { { { 0, 0, 0 }, 1 }, { { 1, 0, 0 }, 2 } };
    splitCase.input.request.operation = split;
    check(splitCase.Build().failureReason == PartFailureReason::UnassignedVoxels,
        "Barrier-isolated region without seeds is rejected");
    split.seeds = { { { 0, 0, 0 }, 1 }, { { 0, 0, 0 }, 2 } };
    splitCase.input.request.operation = split;
    check(splitCase.Build().failureReason == PartFailureReason::ConstraintConflict,
        "Different seed owners at one voxel are rejected");
    split.seeds = { { { 0, 0, 0 }, 1 }, { { 4, 0, 0 }, 3 } };
    splitCase.input.request.operation = split;
    check(splitCase.Build().failureReason == PartFailureReason::InvalidEdit,
        "Missing target number is rejected");
    split.seeds.back().target = 2;
    splitCase.input.request.operation = split;
    splitCase.input.request.scope.extent = std::array<int, 6>{ 0, 3, 0, 0, 0, 0 };
    check(splitCase.Build().failureReason == PartFailureReason::ConstraintConflict,
        "ROI-truncated split does not relabel outside its scope");
    splitCase.input.request.scope.extent.reset();
    splitCase.input.request.scope.protectedParts = { splitCase.GetPart(1) };
    check(splitCase.Build().failureReason == PartFailureReason::ConstraintConflict,
        "Protected parent cannot be split");
    splitCase.input.request.scope.protectedParts.clear();
    check(PartLabelEditor::BuildLabels(splitCase.input, splitCase.identities, [] { return true; }).failureReason
        == PartFailureReason::Cancelled, "Cancelled edit leaves the source unchanged");
    splitCase.input.maxWorkingBytes = 1;
    check(splitCase.Build().failureReason == PartFailureReason::BudgetExceeded,
        "Budget failure precedes candidate allocation");
    check(*splitCase.input.previous.labels == std::vector<PartLabelId>({ 1, 1, 1, 1, 1 }),
        "Failed edits never mutate input labels");

    EditCase mergeCase({ 1, 1, 0, 2, 2, 2 });
    mergeCase.input.request.operation = PartMergeEdit{ { mergeCase.GetPart(1), mergeCase.GetPart(2) } };
    auto merged = mergeCase.Build();
    check(merged.labels && *merged.labels == std::vector<PartLabelId>({ 1, 1, 0, 1, 1, 1 })
        && merged.catalog->retiredFromPrevious.size() == 2
        && merged.catalog->relationsFromPrevious.size() == 2
        && merged.catalog->partsByLabel[1].metrics.voxelCount == 5
        && !merged.catalog->partsByLabel[1].userState.isReviewed
        && !mergeCase.input.previous.catalog->labelByObject.count(merged.catalog->partsByLabel[1].objectId),
        "Merge writes real labels, preserves background, and creates a new identity");
    if (merged.labels) {
        const PartHistorySnapshot mergeHistory{ merged.labels, merged.catalog };
        auto undone = PartLabelEditor::BuildRestore(mergeHistory, mergeCase.input.previous, 64U * 1024U * 1024U);
        check(undone.labels && *undone.labels == *mergeCase.input.previous.labels
            && GetSameObjects(*undone.catalog, *mergeCase.input.previous.catalog)
            && undone.catalog->partsByLabel[1].userState == mergeCase.input.previous.catalog->partsByLabel[1].userState
            && undone.catalog->resultRevision > merged.catalog->resultRevision,
            "Undo restores labels, original identities, metadata in a new revision");
        if (undone.labels) {
            auto redone = PartLabelEditor::BuildRestore({ undone.labels, undone.catalog }, mergeHistory, 64U * 1024U * 1024U);
            check(redone.labels && *redone.labels == *merged.labels
                && GetSameObjects(*redone.catalog, *merged.catalog)
                && redone.catalog->resultRevision > undone.catalog->resultRevision,
                "Redo restores the exact merged identity without generating another ID");
        }
    }

    EditCase brushCase({ 1, 0, 0, 0, 2 });
    PartBrushEdit brush;
    brush.target = brushCase.GetPart(1);
    brush.radiusMM = 0.1;
    brush.sourcePoints = { { 0, 0, 0 }, { 4, 0, 0 } };
    brushCase.input.request.operation = brush;
    const auto painted = brushCase.Build();
    check(painted.labels && *painted.labels == std::vector<PartLabelId>({ 1, 1, 1, 1, 2 })
        && painted.catalog->partsByLabel[1].objectId == brushCase.GetPart(1).object.objectId
        && !painted.catalog->partsByLabel[1].userState.isReviewed
        && !painted.catalog->partsByLabel[1].metrics.confidence
        && painted.catalog->partsByLabel[2].userState.isReviewed,
        "Continuous brush bridges sparse samples and preserves untouched identity/review");
    brushCase.input.request.scope.protectedParts = { brushCase.GetPart(1) };
    check(brushCase.Build().failureReason == PartFailureReason::ConstraintConflict,
        "A protected target cannot expand into background");
    brushCase.input.request.scope.protectedParts.clear();
    brush.isErase = true;
    brushCase.input.request.operation = brush;
    const auto erased = brushCase.Build();
    check(erased.labels && *erased.labels == std::vector<PartLabelId>({ 0, 0, 0, 0, 1 })
        && erased.catalog->partsByLabel[1].objectId == brushCase.GetPart(2).object.objectId,
        "Eraser only removes the target and compaction preserves the other ID");

    EditCase fillCase({ 1, 0, 0, 2, 0 });
    fillCase.input.request.operation = PartFillEdit{ fillCase.GetPart(1), { 1, 0, 0 } };
    const auto filled = fillCase.Build();
    check(filled.labels && *filled.labels == std::vector<PartLabelId>({ 1, 1, 1, 2, 0 }),
        "Fill changes only the connected seed-label region");
    PartGrowEdit grow;
    grow.target = fillCase.GetPart(1);
    grow.seeds = { { 1, 0, 0 } };
    grow.minimum = 0.5; grow.maximum = 1.0;
    fillCase.values[2] = 2.0;
    fillCase.input.request.operation = grow;
    const auto grown = fillCase.Build();
    check(grown.labels && *grown.labels == std::vector<PartLabelId>({ 1, 1, 0, 2, 0 }),
        "Seed growth obeys gray limits and does not cross other labels");
    check(fillCase.values == std::vector<double>({ 1, 1, 2, 1, 1 }),
        "Label operations leave source gray values unchanged");
    EditCase wide({ 1, 0, 0 });
    const std::vector<std::uint64_t> wideValues{ 0, 9007199254740992ULL, 9007199254740993ULL };
    wide.input.volume.values = { wideValues.data(), wideValues.size(), PartScalarType::UInt64 };
    PartGrowEdit wideGrow;
    wideGrow.target = wide.GetPart(1);
    wideGrow.minimum = wideGrow.maximum = 9007199254740992.0;
    wideGrow.seeds = { { 1, 0, 0 } };
    wide.input.request.operation = wideGrow;
    const auto precise = wide.Build();
    check(precise.labels && *precise.labels == std::vector<PartLabelId>({ 1, 1, 0 }),
        "UInt64 growth does not round an out-of-range voxel into the threshold interval");

    EditCase slab(std::vector<PartLabelId>{ 1, 0, 0, 0, 0, 0 });
    slab.input.volume.dimensions = { 3, 1, 2 };
    slab.input.volume.extent = { 0, 2, 0, 0, 0, 1 };
    slab.input.volume.spacing = { 1, 1, 4 };
    PartBrushEdit slabBrush;
    slabBrush.target = slab.GetPart(1);
    slabBrush.radiusMM = 0.1;
    slabBrush.sourcePoints = { { 0, 0, 0 }, { 2, 0, 0 } };
    slabBrush.slice = PartBrushPlane{ { 0, 0, 0 }, { 0, 0, 7 }, 1.0 };
    slab.input.request.operation = slabBrush;
    const auto sliced = slab.Build();
    check(sliced.labels && *sliced.labels == std::vector<PartLabelId>({ 1, 1, 1, 0, 0, 0 }),
        "Slice brush normalizes the plane and respects explicit physical thickness");

    EditCase masked({ 1, 0, 0, 0, 0 });
    GridGeometry3D grid;
    grid.extent = masked.input.volume.extent;
    grid.dimensions = masked.input.volume.dimensions;
    auto roiValues = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 1, 1, 1, 1, 0 });
    auto protectedValues = std::make_shared<const std::vector<std::int16_t>>(
        std::vector<std::int16_t>{ 0, 0, 1, 0, 0 });
    masked.input.roiMask = std::make_shared<const LabelMap3DPayload>(grid, LabelMapValues{roiValues});
    masked.input.protectionMask = std::make_shared<const LabelMap3DPayload>(grid, LabelMapValues{protectedValues});
    masked.input.request.scope.roiMask = DataRevisionRef{};
    masked.input.request.scope.protectionMask = DataRevisionRef{};
    masked.input.request.operation = PartFillEdit{ masked.GetPart(1), { 1, 0, 0 } };
    const auto protectedFill = masked.Build();
    check(protectedFill.labels && *protectedFill.labels == std::vector<PartLabelId>({ 1, 1, 0, 0, 0 }),
        "Typed ROI/protection masks stop propagation without overwriting protected voxels");
    masked.input.request.operation = PartFillEdit{ masked.GetPart(1), { 2, 0, 0 } };
    check(masked.Build().failureReason == PartFailureReason::ConstraintConflict,
        "A seed in a protected voxel is rejected");
    grid.spacing[0] = 2;
    masked.input.roiMask = std::make_shared<const LabelMap3DPayload>(grid, LabelMapValues{roiValues});
    check(masked.Build().failureReason == PartFailureReason::InvalidGeometry,
        "Mask geometry mismatch is rejected without implicit resampling");

    EditCase anisotropic(std::vector<PartLabelId>(9, 1));
    anisotropic.input.volume.dimensions = { 3, 3, 1 };
    anisotropic.input.volume.extent = { 0, 2, 0, 2, 0, 0 };
    anisotropic.input.volume.spacing = { 10, 1, 1 };
    PartSplitEdit metricSplit;
    metricSplit.target = anisotropic.GetPart(1);
    metricSplit.seeds = { { { 2, 0, 0 }, 1 }, { { 0, 2, 0 }, 2 } };
    anisotropic.input.request.operation = metricSplit;
    const auto metricResult = anisotropic.Build();
    check(metricResult.labels && (*metricResult.labels)[0] == 2
        && (*metricResult.labels)[2] == 1 && (*metricResult.labels)[6] == 2,
        "Seed competition uses physical anisotropic edge lengths and fixes original seeds");
    anisotropic.input.volume.direction[1] = 0.2;
    check(anisotropic.Build().failureReason == PartFailureReason::InvalidGeometry,
        "Sheared directions are rejected by the physical edit contract");

    EditCase islandCase({ 1, 0, 1, 1, 1 });
    islandCase.input.request.operation = PartIslandEdit{ islandCase.GetPart(1), 2 };
    const auto cleaned = islandCase.Build();
    check(cleaned.labels && *cleaned.labels == std::vector<PartLabelId>({ 0, 0, 1, 1, 1 }),
        "Island cleanup removes only small complete components");
    islandCase.input.request.scope.extent = std::array<int, 6>{ 2, 2, 0, 0, 0, 0 };
    check(islandCase.Build().failureReason == PartFailureReason::NoChange,
        "ROI-truncated large object is not a small island");

    EditCase geometryCase({ 1, 0, 0 });
    geometryCase.input.volume.extent = { 10, 12, -2, -2, 3, 3 };
    geometryCase.input.volume.spacing = { 2, 3, 4 };
    geometryCase.input.volume.direction = { 0, -1, 0, 1, 0, 0, 0, 0, 1 };
    PartBrushEdit physicalBrush;
    physicalBrush.target = geometryCase.GetPart(1);
    physicalBrush.radiusMM = 0.1;
    physicalBrush.sourcePoints = { { 6, 20, 12 }, { 6, 24, 12 } };
    geometryCase.input.request.operation = physicalBrush;
    auto physical = geometryCase.Build();
    check(physical.labels && *physical.labels == std::vector<PartLabelId>({ 1, 1, 1 })
        && std::abs(physical.catalog->partsByLabel[1].metrics.physicalVolumeMM3 - 72.0) < 1e-9
        && physical.catalog->partsByLabel[1].metrics.centroidInputPhysical == std::array<double, 3>{ 6, 22, 12 },
        "Brush and metrics support nonzero extent, anisotropy, and rotated direction");
    // 独立全网格参考用例：投影端点和体素后求线段距离，不使用候选包围范围。
    // 重点检出局部化遗漏：斜切片、非零extent、各向异性、反射/旋转direction和离面笔迹。
    for (int variant = 0; variant < 4; ++variant) {
        constexpr std::size_t n = 8U * 7U * 6U;
        std::vector<PartLabelId> source(n, 0);
        source[0] = 1;
        EditCase swept(source);
        auto& volume = swept.input.volume;
        volume.dimensions = {8, 7, 6};
        volume.extent = {-4, 3, 10, 16, -3, 2};
        volume.origin = {17, -29, 11};
        volume.spacing = {0.3, 1.7, 2.5};
        volume.direction = {0.6, -0.8, 0, 0.8, 0.6, 0, 0, 0, variant == 3 ? -1.0 : 1.0};
        const auto physicalPoint = [&](std::size_t offset) {
            const std::array<double, 3> index{
                static_cast<double>(offset % 8) - 4,
                static_cast<double>((offset / 8) % 7) + 10,
                static_cast<double>(offset / 56) - 3};
            auto point = volume.origin;
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t axis = 0; axis < 3; ++axis)
                    point[row] += volume.direction[row * 3 + axis] * volume.spacing[axis] * index[axis];
            return point;
        };
        PartBrushEdit sweep;
        sweep.target = swept.GetPart(1);
        sweep.radiusMM = 1.8;
        sweep.sourcePoints = {physicalPoint(65), physicalPoint(179), physicalPoint(260)};
        std::array<double, 3> normal{1.0/3, 2.0/3, 2.0/3};
        if (variant != 0) {
            sweep.slice = PartBrushPlane{physicalPoint(179), normal, 2.1};
            if (variant == 2) {
                for (auto& point : sweep.sourcePoints)
                    for (std::size_t row = 0; row < 3; ++row) point[row] += normal[row] * 1000.0;
            }
        }
        const auto project = [&](std::array<double, 3> point) {
            if (sweep.slice) {
                double height = 0;
                for (std::size_t row = 0; row < 3; ++row) height += (point[row] - sweep.slice->origin[row]) * normal[row];
                for (std::size_t row = 0; row < 3; ++row) point[row] -= height * normal[row];
            }
            return point;
        };
        auto expected = source;
        for (std::size_t voxel = 0; voxel < n; ++voxel) {
            const auto physicalPointValue = physicalPoint(voxel);
            if (sweep.slice) {
                double height = 0;
                for (std::size_t row = 0; row < 3; ++row)
                    height += (physicalPointValue[row] - sweep.slice->origin[row]) * normal[row];
                if (std::abs(height) > sweep.slice->thicknessMM / 2) continue;
            }
            const auto point = project(physicalPointValue);
            for (std::size_t segment = 0; segment < sweep.sourcePoints.size(); ++segment) {
                const auto a = project(sweep.sourcePoints[segment == 0 ? 0 : segment - 1]);
                const auto b = project(sweep.sourcePoints[segment]);
                double denominator = 0, numerator = 0;
                for (std::size_t row = 0; row < 3; ++row) {
                    denominator += (b[row]-a[row]) * (b[row]-a[row]);
                    numerator += (point[row]-a[row]) * (b[row]-a[row]);
                }
                const double parameter = denominator == 0 ? 0 : std::clamp(numerator / denominator, 0.0, 1.0);
                double distance2 = 0;
                for (std::size_t row = 0; row < 3; ++row) {
                    const double difference = point[row] - (a[row] + parameter * (b[row]-a[row]));
                    distance2 += difference * difference;
                }
                if (distance2 <= sweep.radiusMM * sweep.radiusMM) { expected[voxel] = 1; break; }
            }
        }
        swept.input.request.operation = sweep;
        const auto result = swept.Build();
        check(result.labels && *result.labels == expected && *swept.input.previous.labels == source,
            "Bounded brush matches a full-grid physical reference without altering source");
        if (result.labels) {
            swept.input.maxWorkingBytes = result.requiredBytes;
            check(swept.Build().failureReason == PartFailureReason::None, "Exact editor budget succeeds");
            --swept.input.maxWorkingBytes;
            check(swept.Build().failureReason == PartFailureReason::BudgetExceeded, "One byte below editor budget is rejected");
        }
        swept.input.maxWorkingBytes = 128U * 1024U * 1024U;
        sweep.sourcePoints = {{1e8, 1e8, 1e8}};
        sweep.slice.reset();
        swept.input.request.operation = sweep;
        check(swept.Build().failureReason == PartFailureReason::NoChange,
            "Disjoint brush publishes no empty candidate");
    }
    return failures;
}

int GetPartEditProfileFailCount()
{
    // 固定合成数据，仅用于同条件性能对照；不冒充真实 CT 或 Host 全链验收。
    constexpr int side = 128;
    constexpr std::size_t count = std::size_t{side} * side * side;
    std::vector<PartLabelId> labels(count, 0);
    labels[0] = 1;
    EditCase fixture(std::move(labels));
    fixture.input.volume.dimensions = {side, side, side};
    fixture.input.volume.extent = {0, side - 1, 0, side - 1, 0, side - 1};
    PartBrushEdit brush;
    brush.target = fixture.GetPart(1);
    brush.radiusMM = 2.0;
    for (int i = 0; i < 32; ++i) brush.sourcePoints.push_back({60.0 + i / 8.0, 64.0, 64.0});
    fixture.input.request.operation = brush;
    for (int run = 0; run < 4; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = fixture.Build();
        if (!result.labels || result.failureReason != PartFailureReason::None) return 1;
        const auto& p = result.profile;
        std::cout << "EDIT_PROFILE sample=synthetic-128-cube run=" << run
            << " voxels=" << count << " segments=" << brush.sourcePoints.size()
            << " visited=" << p.visitedVoxels << " required_bytes=" << result.requiredBytes
            << " input_ms=" << p.inputMs << " copy_ms=" << p.copyMs
            << " editable_ms=" << p.editableMs << " operation_ms=" << p.operationMs
            << " catalog_ms=" << p.catalogMs << " validation_ms=" << p.validationMs
            << " total_ms=" << std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count() << '\n';
    }
    return 0;
}
