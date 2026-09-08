#include "Data/VtkDataBridge.h"
#include "Data/Internal/VtkDataResourceLease.h"

#include <vtkCellArray.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkIdList.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <utility>

namespace {

std::shared_ptr<const DataResourceLease> StartDataUse(const DataSnapshot& data)
{
    const auto lifetime = data->lifetime.lock();
    return lifetime ? lifetime->StartResourceUse(data->self, "VtkDataBridge", DataResourceKind::RenderObject) : nullptr;
}

ImageValueType GetImageValueType(const int vtkType) noexcept
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
    case VTK_FLOAT:
        return ImageValueType::Float32;
    case VTK_DOUBLE:
        return ImageValueType::Float64;
    default:
        return ImageValueType::Unknown;
    }
}

int GetVtkValueType(const ImageValueType valueType) noexcept
{
    switch (valueType) {
    case ImageValueType::Int8: return VTK_SIGNED_CHAR;
    case ImageValueType::UInt8: return VTK_UNSIGNED_CHAR;
    case ImageValueType::Int16: return VTK_SHORT;
    case ImageValueType::UInt16: return VTK_UNSIGNED_SHORT;
    case ImageValueType::Int32: return VTK_INT;
    case ImageValueType::UInt32: return VTK_UNSIGNED_INT;
    case ImageValueType::Int64: return VTK_LONG_LONG;
    case ImageValueType::UInt64: return VTK_UNSIGNED_LONG_LONG;
    case ImageValueType::Float32: return VTK_FLOAT;
    case ImageValueType::Float64: return VTK_DOUBLE;
    case ImageValueType::Unknown:
    default:
        return VTK_VOID;
    }
}

bool GetByteCount(
    vtkDataArray* values,
    std::size_t& byteCount) noexcept
{
    byteCount = 0;
    if (!values || values->GetDataSize() < 0
        || values->GetDataTypeSize() <= 0) {
        return false;
    }
    const auto count = static_cast<std::uint64_t>(values->GetDataSize());
    const auto bytes = static_cast<std::size_t>(values->GetDataTypeSize());
    if (count > std::numeric_limits<std::size_t>::max() / bytes) {
        return false;
    }
    byteCount = static_cast<std::size_t>(count) * bytes;
    return true;
}

std::optional<GridGeometry3D> GetGridGeometry(vtkImageData* image)
{
    if (!image) return std::nullopt;
    GridGeometry3D geometry;
    image->GetExtent(geometry.extent.data());
    image->GetDimensions(geometry.dimensions.data());
    image->GetSpacing(geometry.spacing.data());
    image->GetOrigin(geometry.origin.data());
    const auto* direction = image->GetDirectionMatrix();
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            geometry.direction[row * 3 + column] = direction
                ? direction->GetElement(
                    static_cast<int>(row), static_cast<int>(column))
                : (row == column ? 1.0 : 0.0);
        }
    }
    return GetGridGeometryValid(geometry)
        ? std::optional<GridGeometry3D>{ geometry }
        : std::optional<GridGeometry3D>{};
}

bool GetGeometrySame(
    const GridGeometry3D& left,
    const GridGeometry3D& right) noexcept
{
    return left.extent == right.extent
        && left.dimensions == right.dimensions
        && left.spacing == right.spacing
        && left.origin == right.origin
        && left.direction == right.direction
        && left.coordinateFrame == right.coordinateFrame;
}

vtkSmartPointer<vtkImageData> BuildImageShell(
    const GridGeometry3D& geometry,
    const int vtkType,
    const std::size_t componentCount,
    const std::uint8_t* values,
    const std::size_t valueBytes)
{
    if (!values || vtkType == VTK_VOID || componentCount == 0
        || componentCount > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return {};
    }
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(
        geometry.extent[0], geometry.extent[1],
        geometry.extent[2], geometry.extent[3],
        geometry.extent[4], geometry.extent[5]);
    image->SetSpacing(geometry.spacing.data());
    image->SetOrigin(geometry.origin.data());
    auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
    direction->DeepCopy(geometry.direction.data());
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(vtkType, static_cast<int>(componentCount));
    void* const target = image->GetScalarPointer();
    vtkDataArray* const scalars = image->GetPointData()
        ? image->GetPointData()->GetScalars() : nullptr;
    std::size_t targetBytes = 0;
    if (!target || !GetByteCount(scalars, targetBytes)
        || targetBytes != valueBytes) {
        return {};
    }
    std::memcpy(target, values, valueBytes);
    return image;
}

bool GetPayloadSame(const IDataPayload* left, const IDataPayload* right)
{
    if (left == right) return true;
    if (const auto* image = dynamic_cast<const ImageGrid3DPayload*>(left)) {
        const auto* other = dynamic_cast<const ImageGrid3DPayload*>(right);
        return other && image->GetValues() == other->GetValues()
            && image->GetValidityMask() == other->GetValidityMask()
            && image->GetValueType() == other->GetValueType()
            && image->GetComponentCount() == other->GetComponentCount()
            && GetGeometrySame(image->GetGeometry(), other->GetGeometry());
    }
    if (const auto* mesh = dynamic_cast<const SurfaceMeshPayload*>(left)) {
        const auto* other = dynamic_cast<const SurfaceMeshPayload*>(right);
        return other && &mesh->GetVertices() == &other->GetVertices()
            && &mesh->GetTriangles() == &other->GetTriangles()
            && &mesh->GetPointAttributes() == &other->GetPointAttributes();
    }
    return false;
}

template<class View>
std::shared_ptr<const View> GetCanonicalView(std::shared_ptr<const View> cached, DataSnapshot data)
{
    if (!cached || !cached->data || !GetPayloadSame(cached->data->payload.get(), data->payload.get())) return {};
    if (cached->data == data) return cached;
    auto view = std::make_shared<View>(*cached);
    auto retained = std::make_shared<std::pair<DataSnapshot, std::shared_ptr<const View>>>(std::move(data), std::move(cached));
    view->data = DataSnapshot(retained, retained->first.get());
    return view;
}
} // namespace

class VtkDataBridge::Impl final {
public:
    mutable std::mutex mutex;
    mutable std::map<
        DataRevisionRef,
        std::weak_ptr<const VtkImageGridView>> images;
    mutable std::map<
        DataRevisionRef,
        std::weak_ptr<const VtkLabelMapView>> labels;
    mutable std::map<
        DataRevisionRef,
        std::weak_ptr<const VtkSurfaceMeshView>> meshes;
};

VtkDataBridge::VtkDataBridge()
    : m_impl(std::make_unique<Impl>())
{
}

VtkDataBridge::~VtkDataBridge() = default;

std::shared_ptr<const ImageGrid3DPayload>
VtkDataBridge::CreateImagePayload(
    vtkImageData* image,
    vtkImageData* validityMask,
    ImageMetadata metadata) const
{
    try {
        const auto geometry = GetGridGeometry(image);
        vtkDataArray* const scalars = image && image->GetPointData()
            ? image->GetPointData()->GetScalars() : nullptr;
        std::size_t byteCount = 0;
        const auto valueType = scalars
            ? GetImageValueType(scalars->GetDataType())
            : ImageValueType::Unknown;
        if (!geometry || !scalars
            || valueType == ImageValueType::Unknown
            || scalars->GetNumberOfComponents() <= 0
            || !GetByteCount(scalars, byteCount)
            || (byteCount != 0 && !scalars->GetVoidPointer(0))) {
            return {};
        }
        auto values = std::make_shared<std::vector<std::uint8_t>>(byteCount);
        if (byteCount != 0) {
            std::memcpy(values->data(), scalars->GetVoidPointer(0), byteCount);
        }

        DataBytes maskBytes;
        if (validityMask) {
            const auto maskGeometry = GetGridGeometry(validityMask);
            vtkDataArray* const mask = validityMask->GetPointData()
                ? validityMask->GetPointData()->GetScalars() : nullptr;
            std::size_t maskByteCount = 0;
            if (!maskGeometry || !GetGeometrySame(*geometry, *maskGeometry)
                || !mask || mask->GetDataType() != VTK_UNSIGNED_CHAR
                || mask->GetNumberOfComponents() != 1
                || !GetByteCount(mask, maskByteCount)
                || maskByteCount != *GetGridVoxelCount(*geometry)
                || !mask->GetVoidPointer(0)) {
                return {};
            }
            auto bytes = std::make_shared<std::vector<std::uint8_t>>(
                maskByteCount);
            std::memcpy(bytes->data(), mask->GetVoidPointer(0), maskByteCount);
            maskBytes = std::move(bytes);
        }
        double range[2] = {};
        image->GetScalarRange(range);
        auto payload = std::make_shared<const ImageGrid3DPayload>(
            *geometry,
            valueType,
            static_cast<std::size_t>(scalars->GetNumberOfComponents()),
            std::move(values),
            std::move(maskBytes),
            std::array<double, 2>{ range[0], range[1] },
            std::move(metadata));
        return payload->GetValid() ? payload : nullptr;
    }
    catch (...) {
        return {};
    }
}

std::shared_ptr<const LabelMap3DPayload>
VtkDataBridge::CreateLabelPayload(vtkImageData* labels) const
{
    try {
        const auto geometry = GetGridGeometry(labels);
        vtkDataArray* const scalars = labels && labels->GetPointData()
            ? labels->GetPointData()->GetScalars() : nullptr;
        const auto voxelCount = geometry
            ? GetGridVoxelCount(*geometry) : std::optional<std::size_t>{};
        if (!geometry || !voxelCount || !scalars
            || scalars->GetNumberOfComponents() != 1
            || scalars->GetNumberOfTuples()
                != static_cast<vtkIdType>(*voxelCount)) {
            return {};
        }
        const auto* source = scalars->GetVoidPointer(0);
        if (!source) return {};
        // 按原始整数类型复制，保留负标签和超出 double 精确范围的 64 位值。
        const auto create = [&](auto value) -> std::shared_ptr<const LabelMap3DPayload> {
            using Integer = decltype(value);
            if (*voxelCount > std::numeric_limits<std::size_t>::max() / sizeof(Integer)) return {};
            auto values = std::make_shared<std::vector<Integer>>(*voxelCount);
            std::memcpy(values->data(), source, values->size() * sizeof(Integer));
            auto payload = std::make_shared<const LabelMap3DPayload>(
                *geometry, LabelMapValues{ std::shared_ptr<const std::vector<Integer>>(std::move(values)) });
            return payload->GetValid() ? payload : nullptr;
        };
        switch (GetImageValueType(scalars->GetDataType())) {
        case ImageValueType::Int8: return create(std::int8_t{});
        case ImageValueType::UInt8: return create(std::uint8_t{});
        case ImageValueType::Int16: return create(std::int16_t{});
        case ImageValueType::UInt16: return create(std::uint16_t{});
        case ImageValueType::Int32: return create(std::int32_t{});
        case ImageValueType::UInt32: return create(std::uint32_t{});
        case ImageValueType::Int64: return create(std::int64_t{});
        case ImageValueType::UInt64: return create(std::uint64_t{});
        default: return {};
        }
    }
    catch (...) {
        return {};
    }
}

std::shared_ptr<const SurfaceMeshPayload>
VtkDataBridge::CreateMeshPayload(vtkPolyData* mesh) const
{
    if (!mesh) return {};
    try {
        auto isolated = vtkSmartPointer<vtkPolyData>::New();
        isolated->DeepCopy(mesh);
        auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
        triangles->SetInputData(isolated);
        triangles->PassLinesOff();
        triangles->PassVertsOff();
        triangles->Update();
        vtkPolyData* const output = triangles->GetOutput();
        vtkPoints* const points = output ? output->GetPoints() : nullptr;
        vtkCellArray* const polys = output ? output->GetPolys() : nullptr;
        if (!output) return {};
        if (!points || !polys) {
            if (output->GetNumberOfPoints() != 0
                || output->GetNumberOfCells() != 0) {
                return {};
            }
            auto payload = std::make_shared<const SurfaceMeshPayload>(
                std::vector<double>{}, std::vector<std::uint64_t>{});
            return payload->GetValid() ? payload : nullptr;
        }

        std::vector<double> vertices;
        vertices.resize(static_cast<std::size_t>(points->GetNumberOfPoints()) * 3);
        for (vtkIdType index = 0; index < points->GetNumberOfPoints(); ++index) {
            points->GetPoint(index, vertices.data() + static_cast<std::size_t>(index) * 3);
        }
        std::vector<std::uint64_t> cells;
        auto ids = vtkSmartPointer<vtkIdList>::New();
        polys->InitTraversal();
        while (polys->GetNextCell(ids)) {
            if (ids->GetNumberOfIds() != 3) return {};
            for (vtkIdType index = 0; index < 3; ++index) {
                const auto value = ids->GetId(index);
                if (value < 0) return {};
                cells.push_back(static_cast<std::uint64_t>(value));
            }
        }
        auto payload = std::make_shared<const SurfaceMeshPayload>(
            std::move(vertices), std::move(cells));
        return payload->GetValid() ? payload : nullptr;
    }
    catch (...) {
        return {};
    }
}

std::shared_ptr<const VtkPreparedDataView> VtkPreparedDataView::BuildDataView(
    std::shared_ptr<const IDataPayload> payload, VtkImageGridSnapshot source)
{
    return VtkDataBridge::BuildDataView(std::move(payload), std::move(source));
}

std::shared_ptr<const VtkPreparedDataView> VtkPreparedDataView::BuildDataView(vtkPolyData* mesh)
{
    VtkDataBridge bridge;
    return VtkDataBridge::BuildDataView(bridge.CreateMeshPayload(mesh));
}

std::shared_ptr<const VtkPreparedDataView> VtkDataBridge::BuildDataView(
    std::shared_ptr<const IDataPayload> payload, VtkImageGridSnapshot source)
{
    if (!payload) return {};
    auto result = std::make_shared<VtkPreparedDataView>();
    result->payload = std::move(payload);
    result->resourceUse = {std::make_shared<const DataResourceLease>(), "prepared-vtk-view", DataResourceKind::RenderObject};
    auto draft = std::make_shared<const DataRevision>(DataRevision{{}, result->payload->GetDataType(), {}, result->payload});
    VtkDataBridge worker;
    if (const auto* image = dynamic_cast<const ImageGrid3DPayload*>(result->payload.get())) {
        if (!image->GetValid()) return {};
        const auto* root = source && source->data
            ? dynamic_cast<const ImageGrid3DPayload*>(source->data->payload.get()) : nullptr;
        if (root && source->image && source->image->GetPointData() && source->image->GetPointData()->GetScalars()
            && root->GetValues() == image->GetValues()
            && GetGeometrySame(root->GetGeometry(), image->GetGeometry())) {
            auto view = std::make_shared<VtkImageGridView>();
            view->data = draft;
            view->image = vtkSmartPointer<vtkImageData>::New();
            view->image->CopyStructure(source->image);
            view->image->GetPointData()->SetScalars(source->image->GetPointData()->GetScalars());
            // The scalar allocation belongs to Root. Only this result's shell and mask carry its lease.
            VtkDataResourceLease::Attach(view->image, result->resourceUse.lease);
            VtkDataResourceLease::Attach(view->image->GetPointData(), result->resourceUse.lease);
            if (image->GetValidityMask()) {
                view->validityMask = BuildImageShell(image->GetGeometry(), VTK_UNSIGNED_CHAR, 1,
                    image->GetValidityMask()->data(), image->GetValidityMask()->size());
                if (!view->validityMask) return {};
                VtkDataResourceLease::AttachImage(view->validityMask, result->resourceUse.lease);
            }
            result->image = std::move(view);
        } else {
            result->image = worker.GetImageGrid(draft);
            if (!result->image) return {};
            VtkDataResourceLease::AttachImage(result->image->image, result->resourceUse.lease);
            VtkDataResourceLease::AttachImage(result->image->validityMask, result->resourceUse.lease);
        }
    } else if (dynamic_cast<const SurfaceMeshPayload*>(result->payload.get())) {
        result->mesh = worker.GetSurfaceMesh(draft);
        if (!result->mesh) return {};
        VtkDataResourceLease::AttachMesh(result->mesh->mesh, result->resourceUse.lease);
    } else return {};
    return result;
}

std::shared_ptr<const VtkPreparedDataView> VtkDataBridge::SetPreparedDataView(
    const DataRevisionRef& ref, std::shared_ptr<const VtkPreparedDataView> prepared)
{
    if (!GetDataRevisionRefValid(ref) || !prepared || !prepared->payload || !prepared->resourceUse.lease
        || static_cast<bool>(prepared->image) == static_cast<bool>(prepared->mesh)) return {};
    auto result = std::make_shared<VtkPreparedDataView>(*prepared);
    auto data = std::make_shared<const DataRevision>(DataRevision{ref, prepared->payload->GetDataType(), {}, prepared->payload});
    if (prepared->image) {
        auto view = std::make_shared<VtkImageGridView>(*prepared->image);
        view->data = data;
        result->image = view;
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->images[ref] = std::move(view);
    } else {
        auto view = std::make_shared<VtkSurfaceMeshView>(*prepared->mesh);
        view->data = data;
        result->mesh = view;
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->meshes[ref] = std::move(view);
    }
    return result;
}

VtkImageGridSnapshot VtkDataBridge::GetImageGrid(DataSnapshot data) const
{
    if (!data || data->type != DataTypes::imageGrid3D) return {};
    const auto lease = StartDataUse(data);
    if (GetDataEntityIdValid(data->lifetimeScope) && !lease) return {};
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto found = m_impl->images.find(data->self);
        if (found != m_impl->images.end()) {
            if (auto cached = found->second.lock()) {
                if (auto canonical = GetCanonicalView(std::move(cached), data)) return canonical;
            }
        }
    }
    const auto* payload = dynamic_cast<const ImageGrid3DPayload*>(
        data->payload.get());
    if (!payload || !payload->GetValid() || !payload->GetValues()) return {};
    const auto image = BuildImageShell(
        payload->GetGeometry(),
        GetVtkValueType(payload->GetValueType()),
        payload->GetComponentCount(),
        payload->GetValues()->data(),
        payload->GetValues()->size());
    if (!image) return {};
    vtkSmartPointer<vtkImageData> mask;
    if (payload->GetValidityMask()) {
        mask = BuildImageShell(
            payload->GetGeometry(),
            VTK_UNSIGNED_CHAR,
            1,
            payload->GetValidityMask()->data(),
            payload->GetValidityMask()->size());
        if (!mask) return {};
    }
    VtkDataResourceLease::AttachImage(image, lease);
    VtkDataResourceLease::AttachImage(mask, lease);
    auto view = std::make_shared<const VtkImageGridView>(VtkImageGridView{
        {}, {}, std::move(data), image, mask });
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto& cached = m_impl->images[view->data->self];
        if (auto existing = cached.lock()) {
            if (auto canonical = GetCanonicalView(std::move(existing), view->data)) return canonical;
        }
        cached = view;
    }
    return view;
}

VtkLabelMapSnapshot VtkDataBridge::GetLabelMap(DataSnapshot data) const
{
    if (!data || data->type != DataTypes::labelMap3D) return {};
    const auto lease = StartDataUse(data);
    if (GetDataEntityIdValid(data->lifetimeScope) && !lease) return {};
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto found = m_impl->labels.find(data->self);
        if (found != m_impl->labels.end()) {
            if (auto cached = found->second.lock()) {
                if (auto canonical = GetCanonicalView(std::move(cached), data)) return canonical;
            }
        }
    }
    const auto* payload = dynamic_cast<const LabelMap3DPayload*>(
        data->payload.get());
    if (!payload || !payload->GetValid()) return {};
    const auto valueBytes = GetImageValueBytes(payload->GetValueType());
    if (valueBytes == 0 || payload->GetValueCount() > std::numeric_limits<std::size_t>::max() / valueBytes) return {};
    const auto byteCount = payload->GetValueCount() * valueBytes;
    const auto labels = BuildImageShell(
        payload->GetGeometry(),
        GetVtkValueType(payload->GetValueType()),
        1,
        static_cast<const std::uint8_t*>(payload->GetValueData()),
        byteCount);
    if (!labels) return {};
    VtkDataResourceLease::AttachImage(labels, lease);
    auto view = std::make_shared<const VtkLabelMapView>(VtkLabelMapView{
        std::move(data), labels });
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto& cached = m_impl->labels[view->data->self];
        if (auto existing = cached.lock()) {
            if (auto canonical = GetCanonicalView(std::move(existing), view->data)) return canonical;
        }
        cached = view;
    }
    return view;
}

VtkSurfaceMeshSnapshot VtkDataBridge::GetSurfaceMesh(DataSnapshot data) const
{
    if (!data || data->type != DataTypes::surfaceMesh) return {};
    const auto lease = StartDataUse(data);
    if (GetDataEntityIdValid(data->lifetimeScope) && !lease) return {};
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto found = m_impl->meshes.find(data->self);
        if (found != m_impl->meshes.end()) {
            if (auto cached = found->second.lock()) {
                if (auto canonical = GetCanonicalView(std::move(cached), data)) return canonical;
            }
        }
    }
    const auto* payload = dynamic_cast<const SurfaceMeshPayload*>(
        data->payload.get());
    if (!payload || !payload->GetValid()) return {};

    auto points = vtkSmartPointer<vtkPoints>::New();
    const auto& vertices = payload->GetVertices();
    points->SetNumberOfPoints(static_cast<vtkIdType>(vertices.size() / 3));
    for (std::size_t index = 0; index < vertices.size() / 3; ++index) {
        points->SetPoint(
            static_cast<vtkIdType>(index), vertices.data() + index * 3);
    }
    auto cells = vtkSmartPointer<vtkCellArray>::New();
    const auto& triangles = payload->GetTriangles();
    for (std::size_t index = 0; index < triangles.size(); index += 3) {
        const vtkIdType ids[3] = {
            static_cast<vtkIdType>(triangles[index]),
            static_cast<vtkIdType>(triangles[index + 1]),
            static_cast<vtkIdType>(triangles[index + 2]) };
        cells->InsertNextCell(3, ids);
    }
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(cells);
    for (const auto& attribute : payload->GetPointAttributes()) {
        auto array = vtkSmartPointer<vtkDoubleArray>::New();
        array->SetName(attribute.name.c_str());
        array->SetNumberOfComponents(static_cast<int>(attribute.componentCount));
        array->SetNumberOfTuples(points->GetNumberOfPoints());
        std::memcpy(
            array->GetVoidPointer(0),
            attribute.values.data(),
            attribute.values.size() * sizeof(double));
        mesh->GetPointData()->AddArray(array);
    }
    VtkDataResourceLease::AttachMesh(mesh, lease);
    auto view = std::make_shared<const VtkSurfaceMeshView>(
        VtkSurfaceMeshView{ std::move(data), mesh });
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto& cached = m_impl->meshes[view->data->self];
        if (auto existing = cached.lock()) {
            if (auto canonical = GetCanonicalView(std::move(existing), view->data)) return canonical;
        }
        cached = view;
    }
    return view;
}
