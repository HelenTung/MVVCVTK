#include "Data/LabelMapStore.h"

#include "Data/DataService.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkType.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

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

ImageValueType GetValueType(const int vtkType) noexcept
{
    switch (vtkType) {
    case VTK_CHAR:
        return std::numeric_limits<char>::is_signed
            ? ImageValueType::Int8 : ImageValueType::UInt8;
    case VTK_SIGNED_CHAR:
        return ImageValueType::Int8;
    case VTK_UNSIGNED_CHAR:
        return ImageValueType::UInt8;
    case VTK_SHORT:
        return ImageValueType::Int16;
    case VTK_UNSIGNED_SHORT:
        return ImageValueType::UInt16;
    case VTK_INT:
        return ImageValueType::Int32;
    case VTK_UNSIGNED_INT:
        return ImageValueType::UInt32;
    case VTK_LONG:
        return sizeof(long) == sizeof(std::int64_t)
            ? ImageValueType::Int64 : ImageValueType::Int32;
    case VTK_UNSIGNED_LONG:
        return sizeof(unsigned long) == sizeof(std::uint64_t)
            ? ImageValueType::UInt64 : ImageValueType::UInt32;
    case VTK_LONG_LONG:
        return ImageValueType::Int64;
    case VTK_UNSIGNED_LONG_LONG:
        return ImageValueType::UInt64;
    default:
        return ImageValueType::Unknown;
    }
}

bool GetGeometrySame(
    vtkImageData* first,
    vtkImageData* second) noexcept
{
    if (!first || !second) return false;
    int firstExtent[6] = {};
    int secondExtent[6] = {};
    double firstSpacing[3] = {};
    double secondSpacing[3] = {};
    double firstOrigin[3] = {};
    double secondOrigin[3] = {};
    first->GetExtent(firstExtent);
    second->GetExtent(secondExtent);
    first->GetSpacing(firstSpacing);
    second->GetSpacing(secondSpacing);
    first->GetOrigin(firstOrigin);
    second->GetOrigin(secondOrigin);
    for (std::size_t index = 0; index < 6; ++index) {
        if (firstExtent[index] != secondExtent[index]) return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (firstSpacing[axis] != secondSpacing[axis]
            || firstOrigin[axis] != secondOrigin[axis]) {
            return false;
        }
    }
    const auto* firstDirection = first->GetDirectionMatrix();
    const auto* secondDirection = second->GetDirectionMatrix();
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const double firstValue = firstDirection
                ? firstDirection->GetElement(
                    static_cast<int>(row),
                    static_cast<int>(column))
                : (row == column ? 1.0 : 0.0);
            const double secondValue = secondDirection
                ? secondDirection->GetElement(
                    static_cast<int>(row),
                    static_cast<int>(column))
                : (row == column ? 1.0 : 0.0);
            if (firstValue != secondValue) return false;
        }
    }
    return true;
}

std::optional<LabelMapDescriptor> BuildDescriptor(
    const std::string& ownerFeatureId,
    const TrustedImageSnapshot& source,
    const TrustedLabelMapCandidate& candidate,
    vtkImageData& image)
{
    if (ownerFeatureId.empty()
        || ownerFeatureId.size() > labelMapIdByteLimit
        || candidate.id.empty()
        || candidate.id.size() > labelMapIdByteLimit
        || candidate.displayName.empty()
        || candidate.displayName.size() > labelMapNameByteLimit
        || !source
        || !source->image
        || source->version == 0
        || source->metadata.identity.datasetId.empty()
        || candidate.datasetId
            != source->metadata.identity.datasetId
        || candidate.sourceVersion != source->version
        || !GetGeometrySame(source->image, &image)
        || image.GetNumberOfScalarComponents() != 1
        || !image.GetScalarPointer()) {
        return std::nullopt;
    }

    vtkPointData* const pointData = image.GetPointData();
    vtkDataArray* const values = pointData
        ? pointData->GetScalars() : nullptr;
    const ImageValueType valueType = values
        ? GetValueType(values->GetDataType())
        : ImageValueType::Unknown;
    const vtkIdType pointCount = image.GetNumberOfPoints();
    if (!values
        || valueType == ImageValueType::Unknown
        || pointCount <= 0
        || values->GetNumberOfTuples() != pointCount
        || static_cast<std::uint64_t>(pointCount)
            > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }

    LabelMapDescriptor descriptor;
    descriptor.id = candidate.id;
    descriptor.displayName = candidate.displayName;
    descriptor.producerFeatureId = ownerFeatureId;
    descriptor.datasetId = candidate.datasetId;
    descriptor.sourceVersion = candidate.sourceVersion;
    image.GetExtent(descriptor.extent.data());
    image.GetDimensions(descriptor.dims.data());
    image.GetSpacing(descriptor.spacing.data());
    image.GetOrigin(descriptor.origin.data());
    const auto* direction = image.GetDirectionMatrix();
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            descriptor.direction[row * 3 + column] = direction
                ? direction->GetElement(
                    static_cast<int>(row),
                    static_cast<int>(column))
                : (row == column ? 1.0 : 0.0);
        }
    }
    image.GetScalarRange(descriptor.scalarRange.data());
    descriptor.valueType = valueType;
    descriptor.componentBytes = static_cast<std::size_t>(
        values->GetDataTypeSize());
    descriptor.componentCount = 1;
    descriptor.voxelCount = static_cast<std::size_t>(pointCount);
    return descriptor;
}

struct ReadPlan final {
    TrustedLabelMapSnapshot snapshot;
    vtkDataArray* values = nullptr;
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
    TrustedLabelMapSnapshot snapshot,
    const LabelMapReadRequest& request)
{
    ReadPlanResult result;
    if (!snapshot || !snapshot->image) {
        result.error = LabelMapError::NotFound;
        return result;
    }
    if (request.expectedVersion
        && *request.expectedVersion
            != snapshot->descriptor.version) {
        result.error = LabelMapError::VersionMismatch;
        return result;
    }

    vtkPointData* const pointData = snapshot->image->GetPointData();
    vtkDataArray* const values = pointData
        ? pointData->GetScalars() : nullptr;
    if (!values
        || values->GetNumberOfComponents() != 1
        || values->GetNumberOfTuples()
            != static_cast<vtkIdType>(
                snapshot->descriptor.voxelCount)
        || !values->GetVoidPointer(0)) {
        result.error = LabelMapError::InvalidData;
        return result;
    }
    if (GetValueType(values->GetDataType())
        != snapshot->descriptor.valueType) {
        result.error = LabelMapError::UnsupportedType;
        return result;
    }

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
            plan.values->GetVoidPointer(0));
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

class LabelMapStore::Impl final {
public:
    struct PublishedEntry final {
        std::string ownerFeatureId;
        TrustedImageSnapshot source;
        TrustedLabelMapSnapshot snapshot;
    };

    struct StagedEntry final {
        std::string ownerFeatureId;
        TrustedImageSnapshot source;
        TrustedLabelMapSnapshot snapshot;
        std::optional<LabelMapVersion> expectedVersion;
        LabelMapStageToken token = 0;
    };

    Impl(
        std::weak_ptr<AbstractDataManager> source,
        const std::thread::id ownerThread)
        : source(std::move(source))
        , ownerThread(ownerThread)
    {
    }

    TrustedImageSnapshot GetSource() const
    {
        const auto manager = source.lock();
        const auto snapshot = manager
            ? manager->GetImageSnapshot() : TrustedImageSnapshot{};
        return snapshot
            && snapshot->version != 0
            && !snapshot->metadata.identity.datasetId.empty()
            ? snapshot : TrustedImageSnapshot{};
    }

    bool GetIsOwnerThread() const noexcept
    {
        return ownerThread != std::thread::id{}
            && ownerThread == std::this_thread::get_id();
    }

    std::optional<PublishedEntry> GetLiveEntry(
        const std::string_view id,
        LabelMapError& error) const
    {
        error = LabelMapError::Unavailable;
        const auto currentSource = GetSource();
        if (!currentSource) return std::nullopt;
        const std::lock_guard<std::mutex> lock(mutex);
        const auto entry = published.find(id);
        if (entry == published.end()
            || entry->second.source != currentSource
            || !entry->second.snapshot
            || entry->second.snapshot->descriptor.datasetId
                != currentSource->metadata.identity.datasetId
            || entry->second.snapshot->descriptor.sourceVersion
                != currentSource->version) {
            error = LabelMapError::NotFound;
            return std::nullopt;
        }
        error = LabelMapError::None;
        return entry->second;
    }

    std::weak_ptr<AbstractDataManager> source;
    std::thread::id ownerThread;
    mutable std::mutex mutex;
    std::map<std::string, PublishedEntry, std::less<>> published;
    std::map<std::string, StagedEntry, std::less<>> staged;
    LabelMapVersion nextVersion = 1;
    LabelMapStageToken nextToken = 1;
};

LabelMapStore::LabelMapStore(
    std::weak_ptr<AbstractDataManager> source,
    const std::thread::id ownerThread)
    : m_impl(std::make_unique<Impl>(
        std::move(source), ownerThread))
{
}

LabelMapStore::~LabelMapStore() = default;

std::vector<LabelMapDescriptor>
LabelMapStore::GetDescriptors() const
{
    std::vector<LabelMapDescriptor> result;
    const auto currentSource = m_impl->GetSource();
    if (!currentSource) return result;
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    try {
        result.reserve(m_impl->published.size());
        for (const auto& [id, entry] : m_impl->published) {
            (void)id;
            if (entry.source == currentSource
                && entry.snapshot
                && entry.snapshot->descriptor.datasetId
                    == currentSource->metadata.identity.datasetId
                && entry.snapshot->descriptor.sourceVersion
                    == currentSource->version) {
                result.push_back(entry.snapshot->descriptor);
            }
        }
    }
    catch (...) {
        result.clear();
    }
    return result;
}

std::optional<LabelMapDescriptor>
LabelMapStore::GetDescriptor(const std::string_view id) const
{
    LabelMapError error = LabelMapError::Unavailable;
    const auto entry = m_impl->GetLiveEntry(id, error);
    return entry && entry->snapshot
        ? std::optional<LabelMapDescriptor>{
            entry->snapshot->descriptor }
        : std::optional<LabelMapDescriptor>{};
}

TrustedLabelMapSnapshot LabelMapStore::GetSnapshot(
    const std::string_view id) const
{
    LabelMapError error = LabelMapError::Unavailable;
    const auto entry = m_impl->GetLiveEntry(id, error);
    return entry ? entry->snapshot : TrustedLabelMapSnapshot{};
}

LabelMapReadResult LabelMapStore::GetReadResult(
    const LabelMapReadRequest& request) const
{
    LabelMapReadResult result;
    if (request.id.empty()) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    LabelMapError lookupError = LabelMapError::Unavailable;
    const auto entry = m_impl->GetLiveEntry(
        request.id, lookupError);
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

LabelMapReadChunkResult LabelMapStore::GetReadChunk(
    const LabelMapReadRequest& request,
    const std::size_t voxelOffset) const
{
    LabelMapReadChunkResult result;
    if (request.id.empty()) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    LabelMapError lookupError = LabelMapError::Unavailable;
    const auto entry = m_impl->GetLiveEntry(
        request.id, lookupError);
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

TrustedLabelMapStageResult LabelMapStore::Stage(
    const std::string_view ownerFeatureId,
    TrustedLabelMapCandidate candidate)
{
    TrustedLabelMapStageResult result;
    if (!m_impl->GetIsOwnerThread()) {
        result.error = LabelMapError::WrongThread;
        return result;
    }
    if (ownerFeatureId.empty()
        || ownerFeatureId.size() > labelMapIdByteLimit
        || candidate.id.empty()
        || candidate.id.size() > labelMapIdByteLimit
        || candidate.displayName.empty()
        || candidate.displayName.size() > labelMapNameByteLimit) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    const auto source = m_impl->GetSource();
    if (!source) {
        result.error = LabelMapError::Unavailable;
        return result;
    }
    if (!candidate.image) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    if (!GetGeometrySame(source->image, candidate.image)) {
        result.error = LabelMapError::GeometryMismatch;
        return result;
    }
    if (candidate.datasetId
            != source->metadata.identity.datasetId
        || candidate.sourceVersion != source->version) {
        result.error = LabelMapError::SourceMismatch;
        return result;
    }

    vtkPointData* const pointData = candidate.image->GetPointData();
    vtkDataArray* const values = pointData
        ? pointData->GetScalars() : nullptr;
    const vtkIdType pointCount = candidate.image->GetNumberOfPoints();
    if (!values || !candidate.image->GetScalarPointer()
        || pointCount <= 0
        || values->GetNumberOfTuples() != pointCount) {
        result.error = LabelMapError::InvalidData;
        return result;
    }
    if (values->GetNumberOfComponents() != 1
        || GetValueType(values->GetDataType())
            == ImageValueType::Unknown) {
        result.error = LabelMapError::UnsupportedType;
        return result;
    }

    vtkSmartPointer<vtkImageData> imageCopy;
    std::shared_ptr<TrustedLabelMapState> state;
    try {
        imageCopy = vtkSmartPointer<vtkImageData>::New();
        imageCopy->DeepCopy(candidate.image);
        auto descriptor = BuildDescriptor(
            std::string{ ownerFeatureId },
            source,
            candidate,
            *imageCopy);
        if (!descriptor) {
            result.error = LabelMapError::InvalidData;
            return result;
        }
        state = std::make_shared<TrustedLabelMapState>();
        state->descriptor = std::move(*descriptor);
        state->image = std::move(imageCopy);
    }
    catch (...) {
        result.error = LabelMapError::CopyFailed;
        return result;
    }
    try {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->staged.find(candidate.id)
            != m_impl->staged.end()) {
            result.error = LabelMapError::Busy;
            return result;
        }
        const auto current = m_impl->published.find(candidate.id);
        if (current != m_impl->published.end()) {
            if (current->second.ownerFeatureId != ownerFeatureId) {
                result.error = LabelMapError::OwnerMismatch;
                return result;
            }
            if (!candidate.expectedVersion
                || !current->second.snapshot
                || *candidate.expectedVersion
                    != current->second.snapshot->descriptor.version) {
                result.error = LabelMapError::VersionMismatch;
                return result;
            }
        }
        else if (candidate.expectedVersion) {
            result.error = LabelMapError::VersionMismatch;
            return result;
        }
        if (m_impl->nextVersion
                == std::numeric_limits<LabelMapVersion>::max()
            || m_impl->nextToken
                == std::numeric_limits<LabelMapStageToken>::max()) {
            result.error = LabelMapError::Unavailable;
            return result;
        }
        state->descriptor.version = m_impl->nextVersion++;
        const LabelMapStageToken token = m_impl->nextToken++;
        Impl::StagedEntry entry;
        entry.ownerFeatureId = ownerFeatureId;
        entry.source = source;
        entry.snapshot = state;
        entry.expectedVersion = candidate.expectedVersion;
        entry.token = token;
        m_impl->staged.emplace(candidate.id, std::move(entry));
        result.error = LabelMapError::None;
        result.token = token;
        result.candidate = std::move(state);
        return result;
    }
    catch (...) {
        result.error = LabelMapError::CopyFailed;
        return result;
    }
}

TrustedLabelMapCommitResult LabelMapStore::Commit(
    const std::string_view ownerFeatureId,
    const LabelMapStageToken token)
{
    TrustedLabelMapCommitResult result;
    if (!m_impl->GetIsOwnerThread()) {
        result.error = LabelMapError::WrongThread;
        return result;
    }
    if (token == 0) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    const auto currentSource = m_impl->GetSource();
    if (!currentSource) {
        result.error = LabelMapError::Unavailable;
        return result;
    }
    try {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto staged = std::find_if(
            m_impl->staged.begin(),
            m_impl->staged.end(),
            [token](const auto& value) {
                return value.second.token == token;
            });
        if (staged == m_impl->staged.end()) {
            result.error = LabelMapError::NotFound;
            return result;
        }
        auto& entry = staged->second;
        if (entry.ownerFeatureId != ownerFeatureId) {
            result.error = LabelMapError::OwnerMismatch;
            return result;
        }
        if (entry.source != currentSource
            || !entry.snapshot
            || entry.snapshot->descriptor.datasetId
                != currentSource->metadata.identity.datasetId
            || entry.snapshot->descriptor.sourceVersion
                != currentSource->version
            || !GetGeometrySame(
                currentSource->image, entry.snapshot->image)) {
            result.error = LabelMapError::SourceMismatch;
            return result;
        }
        const auto current = m_impl->published.find(staged->first);
        if (entry.expectedVersion) {
            if (current == m_impl->published.end()
                || !current->second.snapshot
                || current->second.snapshot->descriptor.version
                    != *entry.expectedVersion) {
                result.error = LabelMapError::VersionMismatch;
                return result;
            }
        }
        else if (current != m_impl->published.end()) {
            result.error = LabelMapError::VersionMismatch;
            return result;
        }
        Impl::PublishedEntry published;
        published.ownerFeatureId = entry.ownerFeatureId;
        published.source = entry.source;
        published.snapshot = entry.snapshot;
        m_impl->published.insert_or_assign(
            staged->first, std::move(published));
        result.error = LabelMapError::None;
        result.published = entry.snapshot;
        m_impl->staged.erase(staged);
        return result;
    }
    catch (...) {
        result.error = LabelMapError::CopyFailed;
        return result;
    }
}

bool LabelMapStore::Discard(
    const std::string_view ownerFeatureId,
    const LabelMapStageToken token) noexcept
{
    if (!m_impl->GetIsOwnerThread() || token == 0) return false;
    try {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto staged = std::find_if(
            m_impl->staged.begin(),
            m_impl->staged.end(),
            [ownerFeatureId, token](const auto& value) {
                return value.second.token == token
                    && value.second.ownerFeatureId == ownerFeatureId;
            });
        if (staged == m_impl->staged.end()) return false;
        m_impl->staged.erase(staged);
        return true;
    }
    catch (...) {
        return false;
    }
}

TrustedLabelMapRemoveResult LabelMapStore::Remove(
    const std::string_view ownerFeatureId,
    const std::string_view id,
    const std::optional<LabelMapVersion> expectedVersion)
{
    TrustedLabelMapRemoveResult result;
    if (!m_impl->GetIsOwnerThread()) {
        result.error = LabelMapError::WrongThread;
        return result;
    }
    if (id.empty()) {
        result.error = LabelMapError::InvalidRequest;
        return result;
    }
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto entry = m_impl->published.find(id);
    if (entry == m_impl->published.end()) {
        result.error = LabelMapError::NotFound;
        return result;
    }
    if (entry->second.ownerFeatureId != ownerFeatureId) {
        result.error = LabelMapError::OwnerMismatch;
        return result;
    }
    if (!entry->second.snapshot
        || (expectedVersion
            && entry->second.snapshot->descriptor.version
                != *expectedVersion)) {
        result.error = LabelMapError::VersionMismatch;
        return result;
    }
    result.removedVersion =
        entry->second.snapshot->descriptor.version;
    m_impl->published.erase(entry);
    result.error = LabelMapError::None;
    result.isRemoved = true;
    return result;
}

void LabelMapStore::RemoveOwner(
    const std::string_view ownerFeatureId) noexcept
{
    try {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        for (auto entry = m_impl->staged.begin();
            entry != m_impl->staged.end();) {
            entry = entry->second.ownerFeatureId == ownerFeatureId
                ? m_impl->staged.erase(entry) : std::next(entry);
        }
        for (auto entry = m_impl->published.begin();
            entry != m_impl->published.end();) {
            entry = entry->second.ownerFeatureId == ownerFeatureId
                ? m_impl->published.erase(entry) : std::next(entry);
        }
    }
    catch (...) {
    }
}

void LabelMapStore::Clear() noexcept
{
    try {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->staged.clear();
        m_impl->published.clear();
    }
    catch (...) {
    }
}
