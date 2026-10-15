#include "RoiArchiveTestReader.h"
#include "Data/DataGraphStore.h"
#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"
#include "Support/LabelMapBuilder.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Fixture final {
    DataGraphStore store;
    RoiService service{store};
    GridGeometry3D grid;
    DataRevisionRef source{store.CreateDataEntityId(), 1};

    Fixture()
    {
        grid.extent = {2, 6, -3, -3, 4, 4};
        grid.dimensions = {5, 1, 1};
        grid.spacing = {.5, 2, 3};
        grid.origin = {7, 9, 11};
        grid.direction = {0, -1, 0, 1, 0, 0, 0, 0, 1};
        DataTransaction tx;
        tx.outputs.push_back({source.entityId, 0, DataTypes::imageGrid3D, {},
            std::make_shared<const ImageGrid3DPayload>(grid, ImageValueType::UInt8, 1,
                std::make_shared<const std::vector<std::uint8_t>>(5, 1))});
        Require(store.SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded,
            "mask archive source setup failed");
    }

    std::shared_ptr<const RoiArchiveReader> Archive(
        std::shared_ptr<const IDataPayload> payload, std::uint64_t label)
    {
        const DataRevisionRef mask{store.CreateDataEntityId(), 1};
        DataTransaction tx;
        tx.outputs.push_back({mask.entityId, 0, payload->GetDataType(), {}, std::move(payload)});
        Require(store.SetDataCommit(std::move(tx)).status == DataCommitStatus::Succeeded,
            "mask archive payload setup failed");
        RoiRequest request;
        RoiNode node;
        node.primitive.shape = RoiShape::MaskReference;
        node.primitive.mask = mask;
        node.primitive.labels = {label};
        request.definition = {source, {node}};
        request.metadata.name = "first";
        const auto first = service.SetRoi(request);
        request.metadata.name = "second";
        request.definition.nodes[0].primitive.labels = {1};
        const auto second = service.SetRoi(request);
        Require(first.roi && second.roi, "shared label ROI setup failed");
        auto result = RoiService::GetArchiveReader(store.GetDataGraph(),
            {{first.roi->revision, "ct"}, {second.roi->revision, "ct"}});
        Require(result.error == RoiError::None && result.reader, "mask archive export failed");
        return result.reader;
    }
};

template<class Value>
void RoundTrip(ImageValueType type)
{
    Fixture source;
    const std::vector<Value> expected{std::numeric_limits<Value>::min(), 0, 1,
        static_cast<Value>(std::numeric_limits<Value>::max() - 1), std::numeric_limits<Value>::max()};

    // 这两个断言分别验证外部可写别名隔离与内部独占存储移交，不能互相替代。
    auto shared = std::make_shared<std::vector<Value>>(expected);
    LabelMap3DPayload defensive(source.grid, LabelMapValues{shared});
    (*shared)[2] = 0;
    Require(defensive.GetValueData() != shared->data()
        && static_cast<const Value*>(defensive.GetValueData())[2] == 1,
        "public labels no longer isolate mutable aliases");
    auto owned = std::make_unique<std::vector<Value>>(expected);
    const auto* allocation = owned->data();
    auto frozen = LabelMapBuilder::Build(source.grid, std::move(owned));
    Require(!owned && frozen && frozen->GetValueData() == allocation,
        "exclusive label freeze copied the full array");
    const auto snapshot = std::dynamic_pointer_cast<const LabelMap3DPayload>(frozen->CreateSnapshot());
    frozen.reset();
    Require(snapshot && snapshot->GetValueData() == allocation
        && std::equal(expected.begin(), expected.end(), static_cast<const Value*>(snapshot->GetValueData())),
        "frozen labels lost their original storage or lifetime");

    const auto archive = source.Archive(snapshot, static_cast<std::uint64_t>(expected.back()));
    const auto& metadata = archive->GetMetadata();
    Require(roiSchemaVersion == 4 && metadata.schemaVersion == roiArchiveSchemaVersion
        && metadata.schemaVersion == 5 && metadata.masks.size() == 1
        && metadata.masks[0].grid.valueType == type
        && metadata.masks[0].grid.componentBytes == sizeof(Value)
        && archive->GetMaskCount(0) == expected.size(), "mask archive lost original integer storage type");
    RoiArchiveChunkRequest tail;
    tail.elementOffset = expected.size();
    const auto empty = archive->GetMaskChunk(tail);
    Require(empty.error == RoiError::None && empty.values.empty() && empty.isComplete
        && empty.nextOffset == expected.size(), "mask archive end chunk is inconsistent");

    for (std::size_t width : {1, 3, 64}) {
        Fixture target;
        RoiArchiveRestoreOptions options;
        options.chunkElements = width;
        const auto before = target.store.GetDataGraph().commitId;
        const auto restored = target.service.LoadArchiveReader(*archive, {{"ct", target.source}}, options);
        Require(restored.error == RoiError::None && restored.rois.size() == 2
            && target.store.GetDataGraph().commitId == before + 1, "mask restore did not publish one complete transaction");
        const auto mask = restored.rois[0].definition.nodes[0].primitive.mask;
        Require(mask && mask == restored.rois[1].definition.nodes[0].primitive.mask,
            "shared archive mask was restored more than once");
        const auto data = target.store.GetDataGraph().view->GetData(*mask);
        const auto* labels = dynamic_cast<const LabelMap3DPayload*>(data->payload.get());
        Require(labels && labels->GetValueType() == type && labels->GetGeometry().extent == source.grid.extent
            && labels->GetGeometry().direction == source.grid.direction
            && labels->GetValueCount() == expected.size()
            && std::equal(expected.begin(), expected.end(), static_cast<const Value*>(labels->GetValueData())),
            "mask restore changed typed values, order, or nonzero-extent geometry");
        for (std::size_t roi = 0; roi < 2; ++roi) {
            const auto view = RoiEvaluator::GetRoi(target.store.GetDataGraph(), restored.rois[roi].revision, target.source);
            Require(view.error == RoiError::None && view.roi, "restored mask is unreadable");
            for (std::size_t i = 0; i < expected.size(); ++i) {
                // 独立代入 p = origin + direction * (spacing * absoluteIndex)。
                const std::array<double, 3> point{13, 10 + .5 * i, 23};
                const auto selected = roi ? 1 : static_cast<std::uint64_t>(expected.back());
                Require(view.roi->GetContains(point) == (static_cast<std::uint64_t>(expected[i]) == selected),
                    "mask archive changed exact label selection membership");
            }
        }
    }
}

class MaskReader final : public RoiArchiveTestReader {
public:
    using RoiArchiveTestReader::RoiArchiveTestReader;
    bool invalidValue = false, interrupt = false;
    mutable bool isComplete = false;
    RoiArchiveMaskChunk GetMaskChunk(const RoiArchiveChunkRequest& request) const override
    {
        if (interrupt && request.elementOffset) return {RoiError::Cancelled};
        auto chunk = source->GetMaskChunk(request);
        if (invalidValue && !chunk.values.empty()) chunk.values[0] = 256;
        isComplete = chunk.isComplete;
        return chunk;
    }
};

void MaskFailuresAndLegacy()
{
    Fixture source;
    const auto archive = source.Archive(std::make_shared<const BinaryMask3DPayload>(source.grid,
        std::make_shared<const std::vector<std::uint8_t>>(5, 1)), 1);
    Require(archive->GetMetadata().masks[0].grid.valueType == ImageValueType::UInt8,
        "binary mask archive inflated its storage descriptor");
    Fixture target;
    const auto before = target.store.GetDataGraph().commitId;
    const auto reject = [&](const RoiArchiveReader& reader, RoiError error, RoiArchiveRestoreOptions options = {}) {
        const auto result = target.service.LoadArchiveReader(reader, {{"ct", target.source}}, options);
        Require(result.error == error && result.rois.empty() && result.revisionMapping.empty()
            && target.store.GetDataGraph().commitId == before
            && RoiService::GetDescriptors(target.store.GetDataGraph(), true).empty(),
            "failed mask restore published partial data or returned an incorrect error");
    };
    MaskReader bad(archive);
    bad.invalidValue = true;
    reject(bad, RoiError::InvalidRequest);
    // Int8 的 256 既不是合法正数，也不是负数的模 2^64 编码。
    bad.metadata.masks[0].grid.valueType = ImageValueType::Int8;
    for (auto& item : bad.metadata.rois) item.masks[0].grid.valueType = ImageValueType::Int8;
    reject(bad, RoiError::InvalidRequest);
    bad.metadata = archive->GetMetadata();
    bad.invalidValue = false;
    bad.interrupt = true;
    RoiArchiveRestoreOptions single;
    single.chunkElements = 1;
    reject(bad, RoiError::Cancelled, single);
    bad.interrupt = false;
    std::size_t afterLastRead = 0;
    single.getCancelled = [&] { return bad.isComplete && ++afterLastRead >= 2; };
    reject(bad, RoiError::Cancelled, single); // 最后一块消费后，在冻结扫描中取消。
    bad.isComplete = false;
    afterLastRead = 0;
    single.getCancelled = [&]() -> bool {
        if (bad.isComplete && ++afterLastRead >= 2) throw std::runtime_error("cancel freeze");
        return false;
    };
    reject(bad, RoiError::Cancelled, single);
    bad.metadata.masks[0].grid.componentBytes = 8;
    reject(bad, RoiError::SourceMismatch);
    bad.metadata = archive->GetMetadata();
    // 第二对象校验失败时，第一个对象及共享候选也必须保持未发布。
    bad.metadata.rois[1].masks[0].grid.valueType = ImageValueType::UInt16;
    reject(bad, RoiError::SourceMismatch);
    for (std::uint32_t version : {3, 4}) {
        bad.metadata = archive->GetMetadata();
        bad.metadata.schemaVersion = version;
        for (auto& item : bad.metadata.rois) item.schemaVersion = version;
        reject(bad, RoiError::SourceMismatch); // 旧版本不能悄悄接受新类型描述。
        bad.metadata.masks[0].grid.valueType = ImageValueType::UInt64;
        bad.metadata.masks[0].grid.componentBytes = 8;
        for (auto& item : bad.metadata.rois) item.masks[0].grid = bad.metadata.masks[0].grid;
        Fixture legacy;
        const auto loaded = legacy.service.LoadArchiveReader(bad, {{"ct", legacy.source}});
        Require(loaded.error == RoiError::None && loaded.rois.size() == 2, "legacy UInt64 masks no longer restore");
        const auto data = legacy.store.GetDataGraph().view->GetData(*loaded.rois[0].definition.nodes[0].primitive.mask);
        const auto* labels = dynamic_cast<const LabelMap3DPayload*>(data->payload.get());
        Require(labels && labels->GetValueType() == ImageValueType::UInt64
            && std::all_of(static_cast<const std::uint64_t*>(labels->GetValueData()),
                static_cast<const std::uint64_t*>(labels->GetValueData()) + 5, [](auto value) { return value == 1; }),
            "legacy mask values or storage changed");
    }
}
}

bool TestRoiMaskArchive()
{
    try {
        RoundTrip<std::int8_t>(ImageValueType::Int8);
        RoundTrip<std::uint8_t>(ImageValueType::UInt8);
        RoundTrip<std::int16_t>(ImageValueType::Int16);
        RoundTrip<std::uint16_t>(ImageValueType::UInt16);
        RoundTrip<std::int32_t>(ImageValueType::Int32);
        RoundTrip<std::uint32_t>(ImageValueType::UInt32);
        RoundTrip<std::int64_t>(ImageValueType::Int64);
        RoundTrip<std::uint64_t>(ImageValueType::UInt64);
        MaskFailuresAndLegacy();
        return true;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return false;
    }
}
