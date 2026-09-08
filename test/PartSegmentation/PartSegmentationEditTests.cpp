// 测试用途：验证涂绘、擦除、填充、生长、孤岛、拆分、合并及保护范围和掩码约束。
#include "PartSegmentationTestCases.h"
#include "Algorithms/PartLabelEditor.h"
#include "Data/DataGraphStore.h"
#include "Geometry/RoiEvaluator.h"
#include <cstring>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

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
    RoiReadSnapshot GetRegion(RoiNode node, std::shared_ptr<const IDataPayload> mask = {})
    {
        GridGeometry3D grid{input.volume.extent,input.volume.dimensions,input.volume.spacing,
            input.volume.origin,input.volume.direction,input.coordinateFrame};
        if (!GetDataRevisionRefValid(input.sourceRevision)) {
            input.sourceRevision={roiStore.CreateDataEntityId(),1};
            auto bytes=std::make_shared<std::vector<std::uint8_t>>(values.size()*sizeof(double));
            std::memcpy(bytes->data(),values.data(),bytes->size());
            DataTransaction source;
            source.outputs.push_back({input.sourceRevision.entityId,0,DataTypes::imageGrid3D,{},
                std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::Float64,1,bytes)});
            if (roiStore.SetDataCommit(source).status!=DataCommitStatus::Succeeded) return {};
        }
        DataTransaction transaction;
        if (mask) {
            const DataRevisionRef ref{roiStore.CreateDataEntityId(),1};
            transaction.outputs.push_back({ref.entityId,0,mask->GetDataType(),{},mask});
            node.primitive.shape=RoiShape::MaskReference; node.primitive.mask=ref;
        }
        const DataRevisionRef ref{roiStore.CreateDataEntityId(),1};
        RoiDefinition definition{input.sourceRevision,{node}};
        transaction.outputs.push_back({ref.entityId,0,DataTypes::roiGeometry,RoiEvaluator::GetInputs(definition),
            std::make_shared<const RoiGeometryPayload>(definition)});
        const auto committed=roiStore.SetDataCommit(transaction);
        return committed.status==DataCommitStatus::Succeeded
            ? RoiEvaluator::GetRoi(committed.graph,ref,input.sourceRevision).roi : RoiReadSnapshot{};
    }
    void SetExtentRoi(const std::array<int,6>& extent)
    {
        RoiNode node;
        for (int r=0;r<3;++r) {
            node.primitive.localToSource[r*4+3]=input.volume.origin[r];
            for (int a=0;a<3;++a) {
                const double center=(static_cast<double>(extent[a*2])+extent[a*2+1])*0.5;
                const double half=std::max(0.25,(static_cast<double>(extent[a*2+1])-extent[a*2])*0.5);
                node.primitive.localToSource[r*4+a]=input.volume.direction[r*3+a]*input.volume.spacing[a]*half;
                node.primitive.localToSource[r*4+3]+=input.volume.direction[r*3+a]*input.volume.spacing[a]*center;
            }
        }
        input.editRoi=GetRegion(node);
        input.request.scope.editRoi=input.editRoi ? std::optional<DataRevisionRef>{input.editRoi->GetRevision()}:std::nullopt;
    }
    PartEditBuildResult Build() { return PartLabelEditor::BuildLabels(input, identities); }
    DataGraphStore roiStore;
    void SetGeometryMetrics()
    {
        // 几何夹具修改网格后同步原目录指标，保持与真实不可变输入相同的约束。
        auto catalog = std::make_shared<PartCatalog>(*input.previous.catalog);
        const auto metrics = ClassicalPartSegmenter::BuildLabelMetrics(input.volume, *input.previous.labels,
            static_cast<std::uint32_t>(catalog->partsByLabel.size()-1));
        if (!metrics) throw std::runtime_error("Invalid edit fixture geometry");
        for (std::size_t i = 1; i < catalog->partsByLabel.size(); ++i) {
            const auto confidence = catalog->partsByLabel[i].metrics.confidence;
            catalog->partsByLabel[i].metrics = (*metrics)[i];
            catalog->partsByLabel[i].metrics.confidence = confidence;
        }
        input.previous.catalog = std::move(catalog);
    }
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
    splitCase.SetExtentRoi({0,3,0,0,0,0});
    check(splitCase.Build().failureReason == PartFailureReason::ConstraintConflict,
        "ROI-truncated split does not relabel outside its scope");
    splitCase.input.request.scope.editRoi.reset(); splitCase.input.editRoi.reset();
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
    slab.SetGeometryMetrics();
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
    masked.input.editRoi = masked.GetRegion({}, std::make_shared<const LabelMap3DPayload>(grid, LabelMapValues{roiValues}));
    masked.input.protectionRoi = masked.GetRegion({}, std::make_shared<const LabelMap3DPayload>(grid, LabelMapValues{protectedValues}));
    masked.input.request.scope.editRoi = masked.input.editRoi->GetRevision();
    masked.input.request.scope.protectionRoi = masked.input.protectionRoi->GetRevision();
    masked.input.request.operation = PartFillEdit{ masked.GetPart(1), { 1, 0, 0 } };
    const auto protectedFill = masked.Build();
    check(protectedFill.labels && *protectedFill.labels == std::vector<PartLabelId>({ 1, 1, 0, 0, 0 }),
        "Typed ROI/protection masks stop propagation without overwriting protected voxels");
    masked.input.request.operation = PartFillEdit{ masked.GetPart(1), { 2, 0, 0 } };
    check(masked.Build().failureReason == PartFailureReason::ConstraintConflict,
        "A seed in a protected voxel is rejected");
    grid.spacing[0] = 2;
    check(!masked.GetRegion({}, std::make_shared<const LabelMap3DPayload>(grid, LabelMapValues{roiValues})),
        "Mask geometry mismatch is rejected without implicit resampling");

    EditCase anisotropic(std::vector<PartLabelId>(9, 1));
    anisotropic.input.volume.dimensions = { 3, 3, 1 };
    anisotropic.input.volume.extent = { 0, 2, 0, 2, 0, 0 };
    anisotropic.input.volume.spacing = { 10, 1, 1 };
    anisotropic.SetGeometryMetrics();
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
    islandCase.SetExtentRoi({2,2,0,0,0,0});
    check(islandCase.Build().failureReason == PartFailureReason::NoChange,
        "ROI-truncated large object is not a small island");

    EditCase geometryCase({ 1, 0, 0 });
    geometryCase.input.volume.extent = { 10, 12, -2, -2, 3, 3 };
    geometryCase.input.volume.spacing = { 2, 3, 4 };
    geometryCase.input.volume.direction = { 0, -1, 0, 1, 0, 0, 0, 0, 1 };
    geometryCase.SetGeometryMetrics();
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
    if (physical.labels) {
        const auto measured = ClassicalPartSegmenter::BuildLabelMetrics(geometryCase.input.volume, *physical.labels, 1);
        check(measured && (*measured)[1] == physical.catalog->partsByLabel[1].metrics,
            "Bounded edited metrics equal a full-grid recomputation in transformed coordinates");
    }
    std::vector<PartLabelId> sparseLabels(64U*64U*64U, 0);
    for (int z = 28; z <= 35; ++z) for (int y = 28; y <= 35; ++y) for (int x = 28; x <= 35; ++x)
        sparseLabels[x+64*(y+64*z)] = 1;
    EditCase sparse(std::move(sparseLabels));
    sparse.input.volume.dimensions = {64,64,64}; sparse.input.volume.extent = {0,63,0,63,0,63};
    sparse.SetGeometryMetrics(); sparse.input.maxWorkingBytes = 18U*1024U*1024U;
    PartSplitEdit localSplit; localSplit.target = sparse.GetPart(1);
    localSplit.seeds = {{{28,28,28},1},{{35,35,35},2}}; sparse.input.request.operation = localSplit;
    const auto local = sparse.Build();
    check(local.labels && local.catalog->partsByLabel.size() == 3 && local.requiredBytes <= sparse.input.maxWorkingBytes
        && local.catalog->partsByLabel[1].metrics.voxelCount + local.catalog->partsByLabel[2].metrics.voxelCount == 512,
        "Full-grid labels split a bounded parent within a budget that cannot hold whole-grid split arrays");
    auto badBounds = std::make_shared<PartCatalog>(*sparse.input.previous.catalog);
    badBounds->partsByLabel[1].metrics.voxelExtent = {29,34,29,34,29,34}; sparse.input.previous.catalog = badBounds;
    check(sparse.Build().failureReason == PartFailureReason::InvalidGeometry,
        "Incomplete catalog bounds cannot silently omit source voxels during a bounded split");
    return failures;
}
