#include "Data/LabelMapReader.h"
#include "Data/DataPayloads.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace {

struct LabelReadSnapshot final {
    DataSnapshot data;
    LabelMapDescriptor descriptor;
    std::shared_ptr<const LabelMap3DPayload> payload;
};
using LabelSnapshot = std::shared_ptr<const LabelReadSnapshot>;

std::shared_ptr<const ImageGrid3DPayload> GetSource(const DataGraphSnapshot& graph, DataRevisionRef& ref)
{
    const auto binding = graph.view ? graph.view->GetDataBinding(primaryVolumeBinding) : std::nullopt;
    const auto data = binding && binding->target ? graph.view->GetData(*binding->target) : nullptr;
    auto payload = data ? std::dynamic_pointer_cast<const ImageGrid3DPayload>(data->payload) : nullptr;
    if (!payload || !payload->GetValid() || payload->GetMetadata().identity.datasetId.empty()) return {};
    ref = data->self;
    return payload;
}

std::map<std::string, LabelSnapshot, std::less<>> GetLiveEntries(const DataGraphSnapshot& graph)
{
    std::map<std::string, LabelSnapshot, std::less<>> result;
    DataRevisionRef sourceRef;
    const auto source = GetSource(graph, sourceRef);
    if (!source) return result;
    const auto& geometry = source->GetGeometry();
    std::vector<DataRevisionRef> pending;
    for (const auto& binding : graph.view->GetDataBindings()) {
        if (binding.target) pending.push_back(*binding.target);
    }
    std::set<DataRevisionRef> visited;
    std::set<std::string> ambiguous;
    while (!pending.empty()) {
        const auto ref = pending.back();
        pending.pop_back();
        if (!visited.insert(ref).second) continue;
        const auto data = graph.view->GetData(ref);
        if (!data) continue;
        if (const auto collection = std::dynamic_pointer_cast<const DataCollectionPayload>(data->payload)) {
            for (const auto& entry : collection->GetItems()) pending.push_back(entry.data);
            continue;
        }
        const auto labels = std::dynamic_pointer_cast<const LabelMap3DPayload>(data->payload);
        if (!labels || !labels->GetValid() || !data->provenance || data->provenance->producerId.empty()) continue;
        const auto input = std::find_if(data->inputs.begin(), data->inputs.end(), [](const DataInputRef& value) {
            return value.role == "source-volume";
        });
        if (input == data->inputs.end() || input->source != sourceRef) continue;
        const auto& grid = labels->GetGeometry();
        if (grid.extent != geometry.extent || grid.dimensions != geometry.dimensions
            || grid.spacing != geometry.spacing || grid.origin != geometry.origin
            || grid.direction != geometry.direction || grid.coordinateFrame != geometry.coordinateFrame) continue;
        LabelMapDescriptor descriptor;
        descriptor.id = labels->GetId().empty() ? data->provenance->producerId + ".labels" : labels->GetId();
        descriptor.displayName = labels->GetDisplayName().empty() ? data->provenance->producerId + " labels" : labels->GetDisplayName();
        descriptor.producerFeatureId = data->provenance->producerId;
        descriptor.datasetId = source->GetMetadata().identity.datasetId;
        descriptor.sourceRevision = sourceRef;
        descriptor.dataRevision = data->self;
        descriptor.extent = grid.extent;
        descriptor.dims = grid.dimensions;
        descriptor.spacing = grid.spacing;
        descriptor.origin = grid.origin;
        descriptor.direction = grid.direction;
        descriptor.valueType = labels->GetValueType();
        descriptor.componentBytes = GetImageValueBytes(descriptor.valueType);
        descriptor.componentCount = 1;
        descriptor.voxelCount = labels->GetValueCount();
        descriptor.scalarRange = labels->GetScalarRange();
        const auto id = descriptor.id;
        if (result.find(id) != result.end()) { ambiguous.insert(id); continue; }
        result.emplace(id, std::make_shared<const LabelReadSnapshot>(LabelReadSnapshot{ data, std::move(descriptor), labels }));
    }
    for (const auto& id : ambiguous) result.erase(id);
    return result;
}

struct PublishedEntry final { LabelSnapshot snapshot; };
std::optional<PublishedEntry> GetLiveEntry(const DataGraphSnapshot& graph, std::string_view id, LabelMapError& error)
{
    DataRevisionRef sourceRef;
    error = LabelMapError::Unavailable;
    if (!GetSource(graph, sourceRef)) return std::nullopt;
    const auto entries = GetLiveEntries(graph);
    const auto found = entries.find(id);
    error = found == entries.end() ? LabelMapError::NotFound : LabelMapError::None;
    return found == entries.end() ? std::nullopt : std::optional<PublishedEntry>{ PublishedEntry{found->second} };
}

bool GetCheckedProduct(
    const std::size_t first,
    const std::size_t second,
    std::size_t& result) noexcept
{
    if (first != 0
        && second > std::numeric_limits<std::size_t>::max() / first) {
        result = 0;
        return false;
    }
    result = first * second;
    return true;
}

struct ReadPlan final {
    LabelSnapshot snapshot;
    const void* values = nullptr;
    ImageReadRegion region;
    std::size_t regionVoxels = 0;
    std::size_t voxelBytes = 0;
    std::size_t requiredBytes = 0;
};

struct ReadPlanResult final {
    LabelMapError error = LabelMapError::InvalidData;
    std::size_t requiredBytes = 0;
    std::optional<ReadPlan> plan;
};

ReadPlanResult GetReadPlan(
    LabelSnapshot snapshot,
    const LabelMapReadRequest& request)
{
    ReadPlanResult result;
    if (!snapshot || !snapshot->payload) {
        result.error = LabelMapError::NotFound;
        return result;
    }
    if (request.expectedRevision
        && *request.expectedRevision
            != snapshot->descriptor.dataRevision) {
        result.error = LabelMapError::VersionMismatch;
        return result;
    }

    const auto* values = snapshot->payload->GetValueData();
    if (!snapshot->payload->GetValid() || !values) return result;

    ReadPlan plan;
    plan.snapshot = std::move(snapshot);
    plan.values = values;
    plan.voxelBytes = plan.snapshot->descriptor.componentBytes;
    if (plan.voxelBytes == 0) {
        result.error = LabelMapError::InvalidData;
        return result;
    }
    if (request.region) {
        plan.region = *request.region;
    }
    else {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            plan.region.size[axis] = static_cast<std::size_t>(
                plan.snapshot->descriptor.dims[axis]);
        }
    }

    plan.regionVoxels = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const int sourceDimension =
            plan.snapshot->descriptor.dims[axis];
        if (sourceDimension <= 0
            || plan.region.size[axis] == 0
            || plan.region.offset[axis]
                > static_cast<std::size_t>(sourceDimension)
            || plan.region.size[axis]
                > static_cast<std::size_t>(sourceDimension)
                    - plan.region.offset[axis]
            || !GetCheckedProduct(
                plan.regionVoxels,
                plan.region.size[axis],
                plan.regionVoxels)) {
            result.error = LabelMapError::InvalidRegion;
            return result;
        }
    }
    if (!GetCheckedProduct(
            plan.regionVoxels,
            plan.voxelBytes,
            plan.requiredBytes)) {
        result.error = LabelMapError::TooLarge;
        result.requiredBytes =
            std::numeric_limits<std::size_t>::max();
        return result;
    }
    result.error = LabelMapError::None;
    result.requiredBytes = plan.requiredBytes;
    result.plan = std::move(plan);
    return result;
}

LabelMapReadState GetReadState(
    const ReadPlan& plan,
    const std::size_t voxelOffset,
    const std::size_t voxelCount)
{
    LabelMapReadState state;
    state.descriptor = plan.snapshot->descriptor;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        state.dims[axis] = static_cast<int>(plan.region.size[axis]);
        state.extent[axis * 2] = 0;
        state.extent[axis * 2 + 1] = state.dims[axis] - 1;
        state.origin[axis] = state.descriptor.origin[axis];
        for (std::size_t column = 0; column < 3; ++column) {
            const double sourceIndex = static_cast<double>(
                static_cast<std::int64_t>(
                    state.descriptor.extent[column * 2])
                + static_cast<std::int64_t>(
                    plan.region.offset[column]));
            state.origin[axis] +=
                state.descriptor.direction[axis * 3 + column]
                * sourceIndex
                * state.descriptor.spacing[column];
        }
    }
    state.region = plan.region;
    state.voxelOffset = voxelOffset;
    state.voxelCount = voxelCount;
    return state;
}

ImageReadBytes GetReadBytes(
    const ReadPlan& plan,
    const std::size_t voxelOffset,
    const std::size_t voxelCount)
{
    if (!plan.values || voxelOffset > plan.regionVoxels
        || voxelCount > plan.regionVoxels - voxelOffset) {
        return {};
    }
    std::size_t byteCount = 0;
    if (!GetCheckedProduct(
            voxelCount, plan.voxelBytes, byteCount)) {
        return {};
    }
    try {
        auto bytes =
            std::make_shared<std::vector<std::uint8_t>>(byteCount);
        if (byteCount == 0) return bytes;
        const auto* source = static_cast<const std::uint8_t*>(
            plan.values);
        if (!source) return {};

        const std::size_t regionX = plan.region.size[0];
        const std::size_t regionY = plan.region.size[1];
        const std::size_t regionSlice = regionX * regionY;
        const std::size_t sourceXSize = static_cast<std::size_t>(
            plan.snapshot->descriptor.dims[0]);
        const std::size_t sourceYSize = static_cast<std::size_t>(
            plan.snapshot->descriptor.dims[1]);
        std::size_t copied = 0;
        while (copied < voxelCount) {
            const std::size_t regionIndex = voxelOffset + copied;
            const std::size_t localZ = regionIndex / regionSlice;
            const std::size_t sliceIndex = regionIndex % regionSlice;
            const std::size_t localY = sliceIndex / regionX;
            const std::size_t localX = sliceIndex % regionX;
            const std::size_t rowVoxels = std::min(
                voxelCount - copied, regionX - localX);
            const std::size_t sourceX =
                plan.region.offset[0] + localX;
            const std::size_t sourceY =
                plan.region.offset[1] + localY;
            const std::size_t sourceZ =
                plan.region.offset[2] + localZ;
            const std::size_t sourceIndex =
                (sourceZ * sourceYSize + sourceY)
                * sourceXSize + sourceX;
            const std::size_t sourceByte =
                sourceIndex * plan.voxelBytes;
            const std::size_t targetByte =
                copied * plan.voxelBytes;
            const std::size_t rowBytes =
                rowVoxels * plan.voxelBytes;
            std::memcpy(
                bytes->data() + targetByte,
                source + sourceByte,
                rowBytes);
            copied += rowVoxels;
        }
        return bytes;
    }
    catch (...) {
        return {};
    }
}

} // namespace

std::vector<LabelMapDescriptor> LabelMapReader::GetDescriptors() const
{
    std::vector<LabelMapDescriptor> result;
    try { for (const auto& entry : GetLiveEntries(m_graph)) result.push_back(entry.second->descriptor); }
    catch (...) { result.clear(); }
    return result;
}

std::optional<LabelMapDescriptor> LabelMapReader::GetDescriptor(std::string_view id) const
{
    try {
        LabelMapError error;
        const auto entry = GetLiveEntry(m_graph, id, error);
        return entry ? std::optional<LabelMapDescriptor>{entry->snapshot->descriptor} : std::nullopt;
    }
    catch (...) { return std::nullopt; }
}

LabelMapReadResult LabelMapReader::GetReadResult(
    const LabelMapReadRequest& request) const try
{
    LabelMapReadResult result;
    if (request.id.empty() || request.id.size() > labelMapIdByteLimit) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    LabelMapError lookupError = LabelMapError::Unavailable;
    const auto entry = GetLiveEntry(m_graph, request.id, lookupError);
    if (!entry) {
        result.error = lookupError;
        return result;
    }
    auto planned = GetReadPlan(entry->snapshot, request);
    result.error = planned.error;
    result.requiredBytes = planned.requiredBytes;
    if (!planned.plan || planned.error != LabelMapError::None) {
        return result;
    }
    if (planned.plan->requiredBytes > request.maxBytes) {
        result.error = LabelMapError::TooLarge;
        return result;
    }
    auto state = GetReadState(
        *planned.plan, 0, planned.plan->regionVoxels);
    state.values = GetReadBytes(
        *planned.plan, 0, planned.plan->regionVoxels);
    if (!state.values) {
        result.error = LabelMapError::CopyFailed;
        return result;
    }
    result.error = LabelMapError::None;
    result.state = std::move(state);
    return result;
}
catch (...) { LabelMapReadResult failed; failed.error = LabelMapError::CopyFailed; return failed; }

LabelMapReadChunkResult LabelMapReader::GetReadChunk(
    const LabelMapReadRequest& request,
    const std::size_t voxelOffset) const try
{
    LabelMapReadChunkResult result;
    if (request.id.empty() || request.id.size() > labelMapIdByteLimit) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    LabelMapError lookupError = LabelMapError::Unavailable;
    const auto entry = GetLiveEntry(m_graph, request.id, lookupError);
    if (!entry) {
        result.error = lookupError;
        return result;
    }
    auto planned = GetReadPlan(entry->snapshot, request);
    result.error = planned.error;
    result.requiredBytes = planned.requiredBytes;
    if (!planned.plan || planned.error != LabelMapError::None) {
        return result;
    }
    const auto& plan = *planned.plan;
    if (voxelOffset > plan.regionVoxels) {
        result.error = LabelMapError::InvalidRegion;
        return result;
    }
    if (voxelOffset == plan.regionVoxels) {
        result.error = LabelMapError::None;
        result.nextVoxelOffset = voxelOffset;
        result.isDone = true;
        return result;
    }
    const std::size_t budget = std::min(
        request.maxBytes, imageChunkLimit);
    const std::size_t maxVoxels = budget / plan.voxelBytes;
    if (maxVoxels == 0) {
        result.error = LabelMapError::TooLarge;
        return result;
    }
    const std::size_t voxelCount = std::min(
        maxVoxels, plan.regionVoxels - voxelOffset);
    auto state = GetReadState(plan, voxelOffset, voxelCount);
    state.values = GetReadBytes(plan, voxelOffset, voxelCount);
    if (!state.values) {
        result.error = LabelMapError::CopyFailed;
        return result;
    }
    result.nextVoxelOffset = voxelOffset + voxelCount;
    result.isDone = result.nextVoxelOffset == plan.regionVoxels;
    result.error = LabelMapError::None;
    result.state = std::move(state);
    return result;
}
catch (...) { LabelMapReadChunkResult failed; failed.error = LabelMapError::CopyFailed; return failed; }
