#pragma once

#include "Data/DataGraphTypes.h"
#include "Data/ImageReadTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace DataTypes {
inline const DataTypeId imageGrid3D{
    "org.mvvcvtk.image-grid-3d", 1 };
inline const DataTypeId labelMap3D{
    "org.mvvcvtk.label-map-3d", 1 };
inline const DataTypeId surfaceMesh{
    "org.mvvcvtk.surface-mesh", 1 };
inline const DataTypeId recordTable{
    "org.mvvcvtk.record-table", 1 };
inline const DataTypeId roiGeometry{
    "org.mvvcvtk.roi-geometry", 1 };
inline const DataTypeId transform3D{
    "org.mvvcvtk.transform-3d", 1 };
inline const DataTypeId dataCollection{
    "org.mvvcvtk.data-collection", 1 };
inline const DataTypeId binaryMask3D{
    "org.mvvcvtk.binary-mask-3d", 1 };
}

namespace DataFacets {
inline const DataFacetId scalarGrid3D{ "scalar-grid-3d" };
inline const DataFacetId labelMap3D{ "label-map-3d" };
inline const DataFacetId surfaceMesh{ "surface-mesh" };
inline const DataFacetId tabularRecords{ "tabular-records" };
inline const DataFacetId roiGeometry{ "roi-geometry" };
inline const DataFacetId transform3D{ "transform-3d" };
inline const DataFacetId dataCollection{ "data-collection" };
inline const DataFacetId binaryMask3D{ "binary-mask-3d" };
}

using DataBytes = std::shared_ptr<const std::vector<std::uint8_t>>;

inline DataBytes GetDataBytesSnapshot(const DataBytes& values)
{
    return values
        ? std::make_shared<const std::vector<std::uint8_t>>(*values)
        : DataBytes{};
}

struct GridGeometry3D final {
    std::array<int, 6> extent = { 0, -1, 0, -1, 0, -1 };
    std::array<int, 3> dimensions = { 0, 0, 0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    std::array<double, 9> direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::string coordinateFrame = "RAS";
};

inline std::optional<std::size_t> GetGridVoxelCount(
    const GridGeometry3D& geometry) noexcept
{
    std::size_t count = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto minIndex = static_cast<std::int64_t>(
            geometry.extent[axis * 2]);
        const auto maxIndex = static_cast<std::int64_t>(
            geometry.extent[axis * 2 + 1]);
        const auto extentSize = maxIndex - minIndex + 1;
        if (geometry.dimensions[axis] <= 0
            || extentSize != geometry.dimensions[axis]
            || count > std::numeric_limits<std::size_t>::max()
                / static_cast<std::size_t>(geometry.dimensions[axis])) {
            return std::nullopt;
        }
        count *= static_cast<std::size_t>(geometry.dimensions[axis]);
    }
    return count;
}

inline bool GetGridGeometryValid(
    const GridGeometry3D& geometry) noexcept
{
    if (!GetGridVoxelCount(geometry)
        || geometry.coordinateFrame.empty()) {
        return false;
    }
    const auto isFinite = [](const double value) {
        return std::isfinite(value);
    };
    return std::all_of(
            geometry.spacing.begin(), geometry.spacing.end(),
            [isFinite](const double value) {
                return isFinite(value) && value > 0.0;
            })
        && std::all_of(
            geometry.origin.begin(), geometry.origin.end(), isFinite)
        && std::all_of(
            geometry.direction.begin(), geometry.direction.end(), isFinite);
}

class ImageGrid3DPayload final : public IDataPayload {
public:
    ImageGrid3DPayload(
        GridGeometry3D geometry,
        ImageValueType valueType,
        std::size_t componentCount,
        DataBytes values,
        DataBytes validityMask = {},
        std::array<double, 2> scalarRange = { 0.0, 0.0 },
        ImageMetadata metadata = {})
        : m_geometry(std::move(geometry))
        , m_valueType(valueType)
        , m_componentCount(componentCount)
        , m_values(GetDataBytesSnapshot(values))
        , m_validityMask(GetDataBytesSnapshot(validityMask))
        , m_scalarRange(scalarRange)
        , m_metadata(std::move(metadata))
    {
    }

    DataTypeId GetDataType() const override { return DataTypes::imageGrid3D; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::shared_ptr<const ImageGrid3DPayload>(
                new ImageGrid3DPayload(*this));
        }
        catch (...) {
            return {};
        }
    }

    const GridGeometry3D& GetGeometry() const noexcept { return m_geometry; }
    ImageValueType GetValueType() const noexcept { return m_valueType; }
    std::size_t GetComponentCount() const noexcept { return m_componentCount; }
    const DataBytes& GetValues() const noexcept { return m_values; }
    const DataBytes& GetValidityMask() const noexcept { return m_validityMask; }
    const ImageMetadata& GetMetadata() const noexcept { return m_metadata; }
    const std::array<double, 2>& GetScalarRange() const noexcept
    {
        return m_scalarRange;
    }
    std::shared_ptr<const ImageGrid3DPayload> CreateGeometrySnapshot(
        GridGeometry3D geometry) const
    {
        try {
            return std::shared_ptr<const ImageGrid3DPayload>(
                new ImageGrid3DPayload(
                    std::move(geometry),
                    m_valueType,
                    m_componentCount,
                    m_values,
                    m_validityMask,
                    m_scalarRange,
                    m_metadata,
                    SnapshotUse{}));
        }
        catch (...) {
            return {};
        }
    }
    std::shared_ptr<const ImageGrid3DPayload> CreateMaskSnapshot(
        DataBytes validityMask) const
    {
        try {
            return std::shared_ptr<const ImageGrid3DPayload>(
                new ImageGrid3DPayload(
                    m_geometry,
                    m_valueType,
                    m_componentCount,
                    m_values,
                    GetDataBytesSnapshot(validityMask),
                    m_scalarRange,
                    m_metadata,
                    SnapshotUse{}));
        }
        catch (...) {
            return {};
        }
    }
    bool GetValid() const noexcept
    {
        const auto voxelCount = GetGridVoxelCount(m_geometry);
        const auto componentBytes = GetImageValueBytes(m_valueType);
        if (!voxelCount || componentBytes == 0 || m_componentCount == 0
            || !m_values
            || *voxelCount > std::numeric_limits<std::size_t>::max()
                / componentBytes
            || *voxelCount * componentBytes
                > std::numeric_limits<std::size_t>::max()
                    / m_componentCount
            || m_values->size()
                != *voxelCount * componentBytes * m_componentCount
            || (m_validityMask
                && m_validityMask->size() != *voxelCount)
            || !std::isfinite(m_scalarRange[0])
            || !std::isfinite(m_scalarRange[1])
            || m_scalarRange[0] > m_scalarRange[1]) {
            return false;
        }
        return GetGridGeometryValid(m_geometry);
    }

private:
    struct SnapshotUse final {};

    ImageGrid3DPayload(
        GridGeometry3D geometry,
        const ImageValueType valueType,
        const std::size_t componentCount,
        DataBytes values,
        DataBytes validityMask,
        const std::array<double, 2> scalarRange,
        ImageMetadata metadata,
        SnapshotUse)
        : m_geometry(std::move(geometry))
        , m_valueType(valueType)
        , m_componentCount(componentCount)
        , m_values(std::move(values))
        , m_validityMask(std::move(validityMask))
        , m_scalarRange(scalarRange)
        , m_metadata(std::move(metadata))
    {
    }

    GridGeometry3D m_geometry;
    ImageValueType m_valueType = ImageValueType::Unknown;
    std::size_t m_componentCount = 0;
    DataBytes m_values;
    DataBytes m_validityMask;
    std::array<double, 2> m_scalarRange = { 0.0, 0.0 };
    ImageMetadata m_metadata;
};

struct LabelDefinition final {
    std::uint32_t value = 0;
    std::string name;
    std::array<double, 4> color = { 1.0, 1.0, 1.0, 1.0 };
};

using LabelMapValues = std::variant<
    std::shared_ptr<const std::vector<std::int8_t>>,
    std::shared_ptr<const std::vector<std::uint8_t>>,
    std::shared_ptr<const std::vector<std::int16_t>>,
    std::shared_ptr<const std::vector<std::uint16_t>>,
    std::shared_ptr<const std::vector<std::int32_t>>,
    std::shared_ptr<const std::vector<std::uint32_t>>,
    std::shared_ptr<const std::vector<std::int64_t>>,
    std::shared_ptr<const std::vector<std::uint64_t>>>;

class LabelMap3DPayload final : public IDataPayload {
public:
    LabelMap3DPayload(
        GridGeometry3D geometry,
        LabelMapValues labels,
        std::vector<LabelDefinition> definitions = {},
        std::string id = {},
        std::string displayName = {})
        : m_geometry(std::move(geometry))
        , m_labels(std::visit([](const auto& values) -> LabelMapValues {
            using Owner = std::decay_t<decltype(values)>;
            using Values = typename Owner::element_type;
            return values ? Owner{ std::make_shared<Values>(*values) } : Owner{};
        }, labels))
        , m_definitions(std::move(definitions))
        , m_id(std::move(id))
        , m_displayName(std::move(displayName))
        , m_scalarRange(std::visit([](const auto& values) {
            if (!values || values->empty()) return std::array<double, 2>{0, 0};
            const auto bounds = std::minmax_element(values->begin(), values->end());
            return std::array<double, 2>{static_cast<double>(*bounds.first), static_cast<double>(*bounds.second)};
        }, m_labels))
    {
    }

    DataTypeId GetDataType() const override { return DataTypes::labelMap3D; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const LabelMap3DPayload>(*this);
        }
        catch (...) {
            return {};
        }
    }

    const GridGeometry3D& GetGeometry() const noexcept { return m_geometry; }
    std::shared_ptr<const std::vector<std::uint32_t>> GetLabels() const noexcept
    {
        const auto* values = std::get_if<std::shared_ptr<const std::vector<std::uint32_t>>>(&m_labels);
        return values ? *values : nullptr;
    }
    const LabelMapValues& GetValues() const noexcept { return m_labels; }
    const std::string& GetId() const noexcept { return m_id; }
    const std::string& GetDisplayName() const noexcept { return m_displayName; }
    const std::array<double, 2>& GetScalarRange() const noexcept { return m_scalarRange; }
    ImageValueType GetValueType() const noexcept
    {
        constexpr std::array<ImageValueType, 8> types{
            ImageValueType::Int8, ImageValueType::UInt8,
            ImageValueType::Int16, ImageValueType::UInt16,
            ImageValueType::Int32, ImageValueType::UInt32,
            ImageValueType::Int64, ImageValueType::UInt64 };
        return types[m_labels.index()];
    }
    std::size_t GetValueCount() const noexcept
    {
        return std::visit([](const auto& values) { return values ? values->size() : 0U; }, m_labels);
    }
    const void* GetValueData() const noexcept
    {
        return std::visit([](const auto& values) -> const void* {
            return values ? values->data() : nullptr;
        }, m_labels);
    }
    const std::vector<LabelDefinition>& GetDefinitions() const noexcept
    {
        return m_definitions;
    }
    bool GetValid() const noexcept
    {
        const auto voxelCount = GetGridVoxelCount(m_geometry);
        if (!voxelCount || !GetValueData() || GetValueCount() != *voxelCount
            || !GetGridGeometryValid(m_geometry)
            || m_id.size() > labelMapIdByteLimit
            || m_displayName.size() > labelMapNameByteLimit
            || m_id.empty() != m_displayName.empty()) {
            return false;
        }
        for (const auto& definition : m_definitions) {
            if (definition.name.empty()
                || !std::all_of(
                    definition.color.begin(), definition.color.end(),
                    [](const double value) {
                        return std::isfinite(value)
                            && value >= 0.0 && value <= 1.0;
                    })) {
                return false;
            }
        }
        return true;
    }

private:
    GridGeometry3D m_geometry;
    LabelMapValues m_labels;
    std::vector<LabelDefinition> m_definitions;
    std::string m_id;
    std::string m_displayName;
    std::array<double, 2> m_scalarRange;
};

struct MeshAttribute final {
    std::string name;
    std::size_t componentCount = 0;
    std::vector<double> values;
};

class SurfaceMeshPayload final : public IDataPayload {
public:
    SurfaceMeshPayload(
        std::vector<double> vertices,
        std::vector<std::uint64_t> triangles,
        std::vector<MeshAttribute> pointAttributes = {},
        std::string coordinateFrame = "RAS")
        : m_vertices(std::move(vertices))
        , m_triangles(std::move(triangles))
        , m_pointAttributes(std::move(pointAttributes))
        , m_coordinateFrame(std::move(coordinateFrame))
    {
    }

    DataTypeId GetDataType() const override { return DataTypes::surfaceMesh; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const SurfaceMeshPayload>(
                m_vertices, m_triangles, m_pointAttributes, m_coordinateFrame);
        }
        catch (...) {
            return {};
        }
    }

    const std::vector<double>& GetVertices() const noexcept { return m_vertices; }
    const std::vector<std::uint64_t>& GetTriangles() const noexcept
    {
        return m_triangles;
    }
    const std::vector<MeshAttribute>& GetPointAttributes() const noexcept
    {
        return m_pointAttributes;
    }
    const std::string& GetCoordinateFrame() const noexcept
    {
        return m_coordinateFrame;
    }
    bool GetValid() const noexcept
    {
        if (m_vertices.size() % 3 != 0
            || m_triangles.size() % 3 != 0
            || (m_vertices.empty() && !m_triangles.empty())
            || m_coordinateFrame.empty()
            || !std::all_of(
                m_vertices.begin(), m_vertices.end(),
                [](const double value) { return std::isfinite(value); })) {
            return false;
        }
        const auto pointCount = m_vertices.size() / 3;
        if (!std::all_of(
                m_triangles.begin(), m_triangles.end(),
                [pointCount](const std::uint64_t value) {
                    return value < pointCount;
                })) {
            return false;
        }
        for (const auto& attribute : m_pointAttributes) {
            if (attribute.name.empty() || attribute.componentCount == 0
                || pointCount > std::numeric_limits<std::size_t>::max()
                    / attribute.componentCount
                || attribute.values.size()
                    != pointCount * attribute.componentCount
                || !std::all_of(
                    attribute.values.begin(), attribute.values.end(),
                    [](const double value) { return std::isfinite(value); })) {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<double> m_vertices;
    std::vector<std::uint64_t> m_triangles;
    std::vector<MeshAttribute> m_pointAttributes;
    std::string m_coordinateFrame;
};

using RecordColumnValues = std::variant<
    std::vector<std::int64_t>,
    std::vector<std::uint64_t>,
    std::vector<double>,
    std::vector<std::string>,
    std::vector<std::array<std::int64_t, 3>>,
    std::vector<std::array<std::int64_t, 6>>,
    std::vector<std::array<double, 3>>,
    std::vector<std::array<double, 6>>,
    std::vector<std::uint8_t>>;

struct RecordColumn final {
    std::string name;
    RecordColumnValues values;
};

inline std::size_t GetRecordColumnSize(
    const RecordColumn& column) noexcept
{
    return std::visit(
        [](const auto& values) { return values.size(); }, column.values);
}

class RecordTablePayload final : public IDataPayload {
public:
    RecordTablePayload(
        DataTypeId type,
        std::string schemaName,
        std::vector<RecordColumn> columns)
        : m_type(std::move(type))
        , m_schemaName(std::move(schemaName))
        , m_columns(std::move(columns))
    {
    }

    DataTypeId GetDataType() const override { return m_type; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const RecordTablePayload>(
                m_type, m_schemaName, m_columns);
        }
        catch (...) {
            return {};
        }
    }
    const std::string& GetSchemaName() const noexcept { return m_schemaName; }
    const std::vector<RecordColumn>& GetColumns() const noexcept
    {
        return m_columns;
    }
    std::size_t GetRowCount() const noexcept
    {
        return m_columns.empty() ? 0 : GetRecordColumnSize(m_columns.front());
    }
    bool GetValid() const noexcept
    {
        if (!GetDataTypeIdValid(m_type)
            || m_schemaName.empty() || m_columns.empty()) {
            return false;
        }
        const auto rowCount = GetRowCount();
        std::vector<std::string> names;
        names.reserve(m_columns.size());
        for (const auto& column : m_columns) {
            if (column.name.empty()
                || GetRecordColumnSize(column) != rowCount
                || std::find(names.begin(), names.end(), column.name)
                    != names.end()) {
                return false;
            }
            names.push_back(column.name);
        }
        return true;
    }

private:
    DataTypeId m_type;
    std::string m_schemaName;
    std::vector<RecordColumn> m_columns;
};

enum class RoiShape : std::uint8_t {
    Box,
    Plane,
    Polyline,
    Contour,
    MaskReference
};

struct RoiPrimitive final {
    RoiShape shape = RoiShape::Box;
    std::array<double, 16> localToSource = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    std::array<double, 3> normal = { 0.0, 0.0, 1.0 };
    std::vector<std::array<double, 3>> points;
    std::optional<DataRevisionRef> mask;
    std::string operation;
};

class RoiGeometryPayload final : public IDataPayload {
public:
    explicit RoiGeometryPayload(std::vector<RoiPrimitive> primitives)
        : m_primitives(std::move(primitives))
    {
    }
    DataTypeId GetDataType() const override { return DataTypes::roiGeometry; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const RoiGeometryPayload>(m_primitives);
        }
        catch (...) {
            return {};
        }
    }
    const std::vector<RoiPrimitive>& GetPrimitives() const noexcept
    {
        return m_primitives;
    }
    bool GetValid() const noexcept
    {
        if (m_primitives.empty()) return false;
        for (const auto& primitive : m_primitives) {
            if (primitive.operation.empty()
                || !std::all_of(
                    primitive.localToSource.begin(),
                    primitive.localToSource.end(),
                    [](const double value) { return std::isfinite(value); })
                || !std::all_of(
                    primitive.origin.begin(), primitive.origin.end(),
                    [](const double value) { return std::isfinite(value); })
                || !std::all_of(
                    primitive.normal.begin(), primitive.normal.end(),
                    [](const double value) { return std::isfinite(value); })) {
                return false;
            }
            if (primitive.shape == RoiShape::MaskReference
                && (!primitive.mask
                    || !GetDataRevisionRefValid(*primitive.mask))) {
                return false;
            }
        }
        return true;
    }

private:
    std::vector<RoiPrimitive> m_primitives;
};

class Transform3DPayload final : public IDataPayload {
public:
    Transform3DPayload(
        std::array<double, 16> sourceToTarget,
        std::string sourceFrame,
        std::string targetFrame)
        : m_sourceToTarget(sourceToTarget)
        , m_sourceFrame(std::move(sourceFrame))
        , m_targetFrame(std::move(targetFrame))
    {
    }
    DataTypeId GetDataType() const override { return DataTypes::transform3D; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const Transform3DPayload>(
                m_sourceToTarget, m_sourceFrame, m_targetFrame);
        }
        catch (...) {
            return {};
        }
    }
    const std::array<double, 16>& GetSourceToTarget() const noexcept
    {
        return m_sourceToTarget;
    }
    const std::string& GetSourceFrame() const noexcept { return m_sourceFrame; }
    const std::string& GetTargetFrame() const noexcept { return m_targetFrame; }
    bool GetValid() const noexcept
    {
        return !m_sourceFrame.empty() && !m_targetFrame.empty()
            && std::all_of(
                m_sourceToTarget.begin(), m_sourceToTarget.end(),
                [](const double value) { return std::isfinite(value); });
    }

private:
    std::array<double, 16> m_sourceToTarget{};
    std::string m_sourceFrame;
    std::string m_targetFrame;
};

struct DataCollectionEntry final {
    std::string role;
    DataRevisionRef data;
};

class DataCollectionPayload final : public IDataPayload {
public:
    DataCollectionPayload(
        DataTypeId type,
        std::vector<DataCollectionEntry> items)
        : m_type(std::move(type)), m_items(std::move(items))
    {
    }
    DataTypeId GetDataType() const override { return m_type; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const DataCollectionPayload>(m_type, m_items);
        }
        catch (...) {
            return {};
        }
    }
    const std::vector<DataCollectionEntry>& GetItems() const noexcept
    {
        return m_items;
    }
    bool GetValid() const noexcept
    {
        if (!GetDataTypeIdValid(m_type) || m_items.empty()) return false;
        std::vector<std::string> roles;
        roles.reserve(m_items.size());
        for (const auto& item : m_items) {
            if (item.role.empty() || !GetDataRevisionRefValid(item.data)
                || std::find(roles.begin(), roles.end(), item.role)
                    != roles.end()) {
                return false;
            }
            roles.push_back(item.role);
        }
        return true;
    }

private:
    DataTypeId m_type;
    std::vector<DataCollectionEntry> m_items;
};

class BinaryMask3DPayload final : public IDataPayload {
public:
    BinaryMask3DPayload(GridGeometry3D geometry, DataBytes values)
        : m_geometry(std::move(geometry)), m_values(GetDataBytesSnapshot(values))
    {
    }
    DataTypeId GetDataType() const override { return DataTypes::binaryMask3D; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const BinaryMask3DPayload>(*this);
        }
        catch (...) {
            return {};
        }
    }
    const GridGeometry3D& GetGeometry() const noexcept { return m_geometry; }
    const DataBytes& GetValues() const noexcept { return m_values; }
    bool GetValid() const noexcept
    {
        const auto count = GetGridVoxelCount(m_geometry);
        return count && GetGridGeometryValid(m_geometry)
            && m_values && m_values->size() == *count;
    }

private:
    GridGeometry3D m_geometry;
    DataBytes m_values;
};

inline DataTypeDescriptor GetRecordTableDescriptor(
    DataTypeId type,
    std::vector<DataFacetId> facets = { DataFacets::tabularRecords })
{
    return DataTypeDescriptor{
        std::move(type),
        std::move(facets),
        [](const IDataPayload& payload, std::string& message) {
            const auto* table = dynamic_cast<const RecordTablePayload*>(&payload);
            if (!table || !table->GetValid()) {
                message = "RecordTable payload is invalid.";
                return false;
            }
            return true;
        }
    };
}

inline DataTypeDescriptor GetDataCollectionDescriptor(
    DataTypeId type,
    std::vector<DataFacetId> facets = { DataFacets::dataCollection })
{
    return DataTypeDescriptor{
        std::move(type),
        std::move(facets),
        [](const IDataPayload& payload, std::string& message) {
            const auto* collection =
                dynamic_cast<const DataCollectionPayload*>(&payload);
            if (!collection || !collection->GetValid()) {
                message = "DataCollection payload is invalid.";
                return false;
            }
            return true;
        }
    };
}

inline std::vector<DataTypeDescriptor> GetBuiltInDataTypes()
{
    return {
        DataTypeDescriptor{
            DataTypes::imageGrid3D,
            { DataFacets::scalarGrid3D },
            [](const IDataPayload& payload, std::string& message) {
                const auto* image =
                    dynamic_cast<const ImageGrid3DPayload*>(&payload);
                if (!image || !image->GetValid()) {
                    message = "ImageGrid3D payload is invalid.";
                    return false;
                }
                return true;
            } },
        DataTypeDescriptor{
            DataTypes::labelMap3D,
            { DataFacets::labelMap3D },
            [](const IDataPayload& payload, std::string& message) {
                const auto* labels =
                    dynamic_cast<const LabelMap3DPayload*>(&payload);
                if (!labels || !labels->GetValid()) {
                    message = "LabelMap3D payload is invalid.";
                    return false;
                }
                return true;
            } },
        DataTypeDescriptor{
            DataTypes::surfaceMesh,
            { DataFacets::surfaceMesh },
            [](const IDataPayload& payload, std::string& message) {
                const auto* mesh =
                    dynamic_cast<const SurfaceMeshPayload*>(&payload);
                if (!mesh || !mesh->GetValid()) {
                    message = "SurfaceMesh payload is invalid.";
                    return false;
                }
                return true;
            } },
        GetRecordTableDescriptor(DataTypes::recordTable),
        DataTypeDescriptor{
            DataTypes::roiGeometry,
            { DataFacets::roiGeometry },
            [](const IDataPayload& payload, std::string& message) {
                const auto* roi =
                    dynamic_cast<const RoiGeometryPayload*>(&payload);
                if (!roi || !roi->GetValid()) {
                    message = "RoiGeometry payload is invalid.";
                    return false;
                }
                return true;
            } },
        DataTypeDescriptor{
            DataTypes::transform3D,
            { DataFacets::transform3D },
            [](const IDataPayload& payload, std::string& message) {
                const auto* transform =
                    dynamic_cast<const Transform3DPayload*>(&payload);
                if (!transform || !transform->GetValid()) {
                    message = "Transform3D payload is invalid.";
                    return false;
                }
                return true;
            } },
        GetDataCollectionDescriptor(DataTypes::dataCollection),
        DataTypeDescriptor{
            DataTypes::binaryMask3D,
            { DataFacets::binaryMask3D },
            [](const IDataPayload& payload, std::string& message) {
                const auto* mask =
                    dynamic_cast<const BinaryMask3DPayload*>(&payload);
                if (!mask || !mask->GetValid()) {
                    message = "BinaryMask3D payload is invalid.";
                    return false;
                }
                return true;
            } }
    };
}
