#pragma once

#include "Data/DataGraphTypes.h"
#include "Data/ImageReadTypes.h"
#include "Data/RoiTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <functional>
#include <memory>
#include <new>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <functional>
#include <iterator>
#include <stdexcept>

// 数组的存储 owner 不要求元素始终驻留 RAM。实现可以由仓内生产者提供文件后备，
// 读取者继续使用只读连续范围；allocator owner 保活后备存储而不锁定物理页。
class DataArrayStorage {
public:
    virtual ~DataArrayStorage() noexcept = default;
    virtual void* Allocate(std::size_t bytes, std::size_t alignment) = 0;
    virtual void Deallocate(void* data, std::size_t bytes, std::size_t alignment) noexcept = 0;
    static std::shared_ptr<DataArrayStorage>& GetCurrent() {
        thread_local std::shared_ptr<DataArrayStorage> storage;
        return storage;
    }
};

template<class T> class DataArrayAllocator {
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    std::shared_ptr<DataArrayStorage> storage;
    DataArrayAllocator() noexcept : storage(DataArrayStorage::GetCurrent()) {}
    DataArrayAllocator(const DataArrayAllocator&) noexcept = default;
    DataArrayAllocator& operator=(const DataArrayAllocator&) noexcept = default;
    // 移动 allocator 仍保留源 owner；MSVC 的空容器调试 proxy 也需要原分配器释放。
    DataArrayAllocator(DataArrayAllocator&& source) noexcept : storage(source.storage) {}
    DataArrayAllocator& operator=(DataArrayAllocator&& source) noexcept { storage=source.storage;return *this; }
    template<class U> DataArrayAllocator(const DataArrayAllocator<U>& source) noexcept : storage(source.storage) {}
    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return storage ? static_cast<T*>(storage->Allocate(count * sizeof(T), alignof(T)))
            : std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* data, std::size_t count) noexcept {
        if (storage) storage->Deallocate(data, count * sizeof(T), alignof(T));
        else std::allocator<T>{}.deallocate(data, count);
    }
    template<class U> bool operator==(const DataArrayAllocator<U>& other) const noexcept { return storage == other.storage; }
    template<class U> bool operator!=(const DataArrayAllocator<U>& other) const noexcept { return !(*this == other); }
};

template<class T> class DataArray : public std::vector<T, DataArrayAllocator<T>> {
    using Base = std::vector<T, DataArrayAllocator<T>>;
public:
    using Base::Base;
    using Base::operator=;
    DataArray() = default;
    DataArray(const std::vector<T>& source) : Base(source.begin(), source.end()) {}
    DataArray(std::vector<T>&& source)
        : Base(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end())) {}
    DataArray& operator=(const std::vector<T>& source) { this->assign(source.begin(), source.end()); return *this; }
    DataArray& operator=(std::vector<T>&& source) {
        this->assign(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end())); return *this;
    }
};

namespace DataTypes {
inline const DataTypeId labelMap3D{
    "org.mvvcvtk.label-map-3d", 1 };
inline const DataTypeId surfaceMesh{
    "org.mvvcvtk.surface-mesh", 1 };
inline const DataTypeId recordTable{
    "org.mvvcvtk.record-table", 1 };
inline const DataTypeId roiGeometry{
    "org.mvvcvtk.roi-geometry", roiSchemaVersion };
inline const DataTypeId roiCatalog{ "org.mvvcvtk.roi-catalog", 1 };
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

// SDK 统一空间长度为 mm，direction 无量纲；origin 对应连续索引 0，非 extent 起点。
// 无已确认长度单位的数据不能作为物理几何发布；输入方须先换算 spacing/origin。
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

// 已冻结且保持对齐的标量存储；共享外部字节输入仍复制隔离。
// 标准容器访问语法只暴露只读字节，底层 owner 一直存活到最后一个读者释放。
class ImageScalarStorage final {
public:
    const std::uint8_t* data() const noexcept { return static_cast<const std::uint8_t*>(m_data); }
    std::size_t size() const noexcept { return m_size; }
    const std::uint8_t* begin() const noexcept { return data(); }
    const std::uint8_t* end() const noexcept { return data() + m_size; }
    const std::uint8_t& operator[](std::size_t i) const noexcept { return data()[i]; }
    friend bool operator==(const ImageScalarStorage& a, const ImageScalarStorage& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    }
    friend bool operator!=(const ImageScalarStorage& a, const ImageScalarStorage& b) { return !(a == b); }
private:
    friend class ImageGrid3DPayload;
    friend class VtkDataBridge;
    ImageScalarStorage(std::shared_ptr<const void> owner, void* data, std::size_t size)
        : m_owner(std::move(owner)), m_data(data), m_size(size) {}
    std::shared_ptr<const void> m_owner;
    void* m_data = nullptr;
    std::size_t m_size = 0;
    // 仅由 bridge 从实际标量计算并在发布前写入；外部声明范围不能成为缓存凭据。
    // 同一冻结字节可被重新解释，复用时必须同时匹配类型与组件布局。
    struct ScalarRange final {
        ImageValueType valueType;
        std::vector<std::array<double, 2>> components;
    };
    std::optional<ScalarRange> m_verifiedRange;
};
using ImageScalarBytes = std::shared_ptr<const ImageScalarStorage>;

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
        , m_values(CreateValuesSnapshot(values))
        , m_validityMask(GetDataBytesSnapshot(validityMask))
        , m_scalarRange(scalarRange)
        , m_metadata(std::move(metadata))
    {
    }

    // 已冻结存储可以安全共享；外部 mask 仍须形成独立快照。
    ImageGrid3DPayload(GridGeometry3D geometry, ImageValueType type, std::size_t components,
        ImageScalarBytes values, DataBytes mask = {}, std::array<double, 2> range = {0, 0},
        ImageMetadata metadata = {})
        : ImageGrid3DPayload(std::move(geometry), type, components, std::move(values),
            GetDataBytesSnapshot(mask), range, std::move(metadata), SnapshotUse{}) {}

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
    const ImageScalarBytes& GetValues() const noexcept { return m_values; }
    const DataBytes& GetValidityMask() const noexcept { return m_validityMask; }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override
    {
        return { m_values, m_validityMask };
    }
    // 只接受独占字节的移交；外部 shared 输入仍使用复制构造路径。
    std::shared_ptr<const ImageGrid3DPayload> CreateMaskSnapshot(
        std::vector<std::uint8_t>&& validityMask) const
    {
        auto mask = std::make_shared<const std::vector<std::uint8_t>>(
            std::move(validityMask));
        return std::shared_ptr<const ImageGrid3DPayload>(new ImageGrid3DPayload(
            m_geometry, m_valueType, m_componentCount, m_values,
            std::move(mask), m_scalarRange, m_metadata, SnapshotUse{}));
    }
    // 接收生产者独占的 float 输出；移交后不得保留任何可写别名。
    std::shared_ptr<const ImageGrid3DPayload> CreateFloatSnapshot(
        std::unique_ptr<std::vector<float>> values,
        std::array<double, 2> scalarRange, ImageMetadata metadata) const
    {
        if (!values || values->size() > std::numeric_limits<std::size_t>::max() / sizeof(float)) return {};
        auto owner = std::shared_ptr<std::vector<float>>(std::move(values));
        auto storage = ImageScalarBytes(new ImageScalarStorage(
            owner, owner->data(), owner->size() * sizeof(float)));
        auto result = std::shared_ptr<const ImageGrid3DPayload>(new ImageGrid3DPayload(
            m_geometry, ImageValueType::Float32, 1, std::move(storage),
            m_validityMask, scalarRange, std::move(metadata), SnapshotUse{}));
        return result->GetValid() ? result : nullptr;
    }
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
    friend class VtkDataBridge;
    static ImageScalarBytes CreateValuesSnapshot(const DataBytes& values) {
        if (!values) return {};
        auto owned = std::make_shared<std::vector<std::uint8_t>>(*values);
        return ImageScalarBytes(new ImageScalarStorage(owned, owned->data(), owned->size()));
    }
    struct SnapshotUse final {};

    ImageGrid3DPayload(
        GridGeometry3D geometry,
        const ImageValueType valueType,
        const std::size_t componentCount,
        ImageScalarBytes values,
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
    ImageScalarBytes m_values;
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
    std::vector<std::shared_ptr<const void>> GetDataResources() const override
    {
        return { std::visit([](const auto& values) -> std::shared_ptr<const void> {
            return values;
        }, m_labels) };
    }
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
    // 仅仓内候选构建器可消耗独占存储；公开输入仍执行防御复制。
    friend class LabelMapBuilder;

    GridGeometry3D m_geometry;
    LabelMapValues m_labels;
    std::vector<LabelDefinition> m_definitions;
    std::string m_id;
    std::string m_displayName;
    std::array<double, 2> m_scalarRange;
};

namespace MeshAttributeRoles {
inline constexpr std::uint32_t scalars=1, vectors=2, normals=4, textureCoordinates=8, tensors=16;
inline constexpr std::uint32_t all=scalars|vectors|normals|textureCoordinates|tensors;
}

struct MeshValueRun final {
    std::uint64_t first = 0, count = 0;
    std::int32_t value = 0;
};
// 浮点属性沿用连续数组；连续同值的整数归属保留区间，不生成等长 double 副本。
class MeshAttributeValues final {
public:
    using value_type = double;
    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = double;
        using difference_type = std::ptrdiff_t;
        using pointer = const double*;
        using reference = double;
        const_iterator() = default;
        const_iterator(const MeshAttributeValues* values,std::size_t position) : m_values(values),m_position(position) {}
        double operator*()const {return (*m_values)[m_position];}
        double operator[](difference_type distance)const {return *(*this+distance);}
        const_iterator& operator++(){++m_position;return *this;}
        const_iterator operator++(int){auto previous=*this;++*this;return previous;}
        const_iterator& operator--(){--m_position;return *this;}
        const_iterator operator--(int){auto previous=*this;--*this;return previous;}
        const_iterator& operator+=(difference_type distance){m_position=static_cast<std::size_t>(static_cast<difference_type>(m_position)+distance);return *this;}
        const_iterator& operator-=(difference_type distance){return *this+=-distance;}
        const_iterator operator+(difference_type distance)const {auto next=*this;return next+=distance;}
        friend const_iterator operator+(difference_type distance,const_iterator position){return position+=distance;}
        const_iterator operator-(difference_type distance)const {auto next=*this;return next-=distance;}
        difference_type operator-(const const_iterator& other)const {return static_cast<difference_type>(m_position)-static_cast<difference_type>(other.m_position);}
        bool operator==(const const_iterator& other)const {return m_values==other.m_values&&m_position==other.m_position;}
        bool operator!=(const const_iterator& other)const {return !(*this==other);}
        bool operator<(const const_iterator& other)const {return m_position<other.m_position;}
        bool operator>(const const_iterator& other)const {return other<*this;}
        bool operator<=(const const_iterator& other)const {return !(other<*this);}
        bool operator>=(const const_iterator& other)const {return !(*this<other);}
    private:
        const MeshAttributeValues* m_values=nullptr;
        std::size_t m_position=0;
    };
    MeshAttributeValues()=default;
    MeshAttributeValues(std::initializer_list<double> values):m_values(values) {}
    MeshAttributeValues(DataArray<double> values):m_values(std::move(values)) {}
    MeshAttributeValues(std::vector<double> values):m_values(std::move(values)) {}
    static MeshAttributeValues FromRuns(DataArray<MeshValueRun> runs) {
        MeshAttributeValues values;std::uint64_t next=0;
        for(const auto& run:runs) {
            if(!run.count || run.first!=next || run.count>static_cast<std::uint64_t>(PTRDIFF_MAX)-next)
                throw std::invalid_argument("Invalid integer attribute runs.");
            next+=run.count;
        }
        values.m_count=static_cast<std::size_t>(next);
        values.m_runs=std::make_shared<const DataArray<MeshValueRun>>(std::move(runs));
        return values;
    }
    static MeshAttributeValues FromIntegers(DataArray<std::int32_t> integers) {
        MeshAttributeValues values;
        values.m_integers=std::make_shared<const DataArray<std::int32_t>>(std::move(integers));
        return values;
    }
    std::size_t size()const noexcept {return m_runs?m_count:m_integers?m_integers->size():m_values.size();}
    std::size_t GetResidentBytes()const noexcept {
        if(m_runs)return m_runs->get_allocator().storage?0:m_runs->capacity()*sizeof(MeshValueRun);
        if(m_integers)return m_integers->get_allocator().storage?0:m_integers->capacity()*sizeof(std::int32_t);
        return m_values.get_allocator().storage?0:m_values.capacity()*sizeof(double);
    }
    bool empty()const noexcept {return !size();}
    friend bool operator==(const MeshAttributeValues& left,const MeshAttributeValues& right) {
        return left.size()==right.size()&&std::equal(left.begin(),left.end(),right.begin());
    }
    friend bool operator!=(const MeshAttributeValues& left,const MeshAttributeValues& right){return !(left==right);}
    ImageValueType GetValueType()const noexcept {return m_runs||m_integers?ImageValueType::Int32:ImageValueType::Float64;}
    const std::shared_ptr<const DataArray<MeshValueRun>>& GetRuns()const noexcept {return m_runs;}
    const std::shared_ptr<const DataArray<std::int32_t>>& GetIntegers()const noexcept {return m_integers;}
    bool GetFinite()const noexcept {
        return m_runs || m_integers || std::all_of(m_values.begin(),m_values.end(),[](double value){return std::isfinite(value);});
    }
    double operator[](std::size_t index)const {
        if(m_integers)return (*m_integers)[index];
        if(!m_runs)return m_values[index];
        const auto next=std::upper_bound(m_runs->begin(),m_runs->end(),index,
            [](auto value,const auto& run){return value<run.first;});
        return static_cast<double>((next-1)->value);
    }
    double& operator[](std::size_t index){SetDense();return m_values[index];}
    double at(std::size_t index)const {if(index>=size())throw std::out_of_range("Mesh attribute index");return (*this)[index];}
    double& at(std::size_t index){SetDense();return m_values.at(index);}
    double front()const {return (*this)[0];}
    double back()const {return (*this)[size()-1];}
    const_iterator begin()const noexcept {return {this,0};}
    const_iterator end()const noexcept {return {this,size()};}
    const_iterator cbegin()const noexcept {return begin();}
    const_iterator cend()const noexcept {return end();}
    void reserve(std::size_t count){SetDense();m_values.reserve(count);}
    void resize(std::size_t count){SetDense();m_values.resize(count);}
    void resize(std::size_t count,double value){SetDense();m_values.resize(count,value);}
    void push_back(double value){SetDense();m_values.push_back(value);}
    void clear()noexcept {m_runs.reset();m_integers.reset();m_count=0;m_values.clear();}
    template<class Iterator> void assign(Iterator first,Iterator last){clear();m_values.assign(first,last);}
    void assign(std::size_t count,double value){clear();m_values.assign(count,value);}
    template<class Iterator> const_iterator insert(const_iterator position,Iterator first,Iterator last) {
        const auto offset=position-begin();SetDense();m_values.insert(m_values.begin()+offset,first,last);return {this,static_cast<std::size_t>(offset)};
    }
    std::vector<double> GetChunk(std::size_t offset,std::size_t count)const {
        if(offset>size())throw std::out_of_range("Mesh attribute chunk offset");
        count=std::min(count,size()-offset);return {begin()+static_cast<std::ptrdiff_t>(offset),begin()+static_cast<std::ptrdiff_t>(offset+count)};
    }
private:
    void SetDense() {
        if(!m_runs&&!m_integers)return;
        DataArray<double> values(m_values.get_allocator());values.resize(size());
        if(m_runs)for(const auto& run:*m_runs)std::fill_n(values.begin()+static_cast<std::size_t>(run.first),static_cast<std::size_t>(run.count),static_cast<double>(run.value));
        else std::copy(m_integers->begin(),m_integers->end(),values.begin());
        m_values=std::move(values);m_runs.reset();m_integers.reset();m_count=0;
    }
    DataArray<double> m_values;
    std::shared_ptr<const DataArray<MeshValueRun>> m_runs;
    std::shared_ptr<const DataArray<std::int32_t>> m_integers;
    std::size_t m_count=0;
};

struct MeshAttribute final {
    std::string name;
    std::size_t componentCount = 0;
    MeshAttributeValues values;
    std::uint32_t activeRoles = 0;
};

enum class MeshReadError : std::uint8_t { None, InvalidRange, Cancelled, AllocationFailed, ReadFailed };
template<class Value> struct MeshReadChunk final {
    MeshReadError error = MeshReadError::None;
    std::vector<Value> values;
    std::size_t nextOffset = 0;
    bool isComplete = false;
};

// 可信 SPI 的只读值源。实现必须保证深层不可变、并发读取和 owner 有效期；不得返回借用指针。
template<class Value> class MeshValueSource {
public:
    virtual ~MeshValueSource() = default;
    virtual std::size_t GetCount() const noexcept = 0;
    virtual std::size_t GetResidentBytes()const noexcept {return GetCount()>SIZE_MAX/sizeof(Value)?SIZE_MAX:GetCount()*sizeof(Value);}
    virtual void ReadValues(std::size_t offset,std::size_t count,Value* destination) const = 0;
};

template<class Value> class MeshReadArray final {
    template<class Container> class Owned final : public MeshValueSource<Value> {
    public:
        explicit Owned(Container input):values(std::move(input)){}
        std::size_t GetCount()const noexcept override{return values.size();}
        std::size_t GetResidentBytes()const noexcept override {
            if constexpr(std::is_same_v<Container,MeshAttributeValues>)return values.GetResidentBytes();
            else return values.get_allocator().storage?0:values.capacity()*sizeof(Value);
        }
        void ReadValues(std::size_t offset,std::size_t count,Value* target)const override {
            if(offset>values.size() || count>values.size()-offset)throw std::out_of_range("Mesh read range is invalid.");
            std::copy_n(values.begin()+offset,count,target);
        }
    private:const Container values;
    };
    class Memory final : public MeshValueSource<Value> {
    public:
        explicit Memory(std::vector<Value> input):values(std::move(input)){}
        std::size_t GetCount()const noexcept override{return values.size();}
        std::size_t GetResidentBytes()const noexcept override{return values.capacity()*sizeof(Value);}
        void ReadValues(std::size_t offset,std::size_t count,Value* out)const override {
            if(offset>values.size() || count>values.size()-offset)throw std::out_of_range("Mesh read range is invalid.");
            std::copy_n(values.begin()+offset,count,out);
        }
    private:const std::vector<Value> values;
    };
public:
    using value_type=Value;
    MeshReadArray() = default;
    explicit MeshReadArray(std::vector<Value> values):m_source(std::make_shared<const Memory>(std::move(values))){}
    explicit MeshReadArray(DataArray<Value> values):m_source(std::make_shared<const Owned<DataArray<Value>>>(std::move(values))){}
    explicit MeshReadArray(MeshAttributeValues values):m_source(std::make_shared<const Owned<MeshAttributeValues>>(std::move(values))){}
    explicit MeshReadArray(std::shared_ptr<const MeshValueSource<Value>> source):m_source(std::move(source)){}
    std::size_t GetCount()const noexcept{return m_source?m_source->GetCount():0;}
    std::size_t GetResidentBytes()const noexcept{return m_source?m_source->GetResidentBytes():0;}
    // STL 范围协议只逐值读取；没有 data()、完整 vector 或可写别名。
    std::size_t size()const noexcept{return GetCount();}
    bool empty()const noexcept{return !GetCount();}
    Value at(std::size_t index)const {
        if(index>=GetCount())throw std::out_of_range("Mesh element is outside the array.");
        Value value{};m_source->ReadValues(index,1,&value);return value;
    }
    Value operator[](std::size_t index)const{return at(index);}
    Value front()const{return at(0);}
    Value back()const{return at(GetCount()-1);}
    MeshReadChunk<Value> GetChunk(std::size_t offset,std::size_t count,
        const std::function<bool()>& getCancelled={})const {
        MeshReadChunk<Value> result;result.nextOffset=offset;
        if(offset>GetCount() || !count){result.error=MeshReadError::InvalidRange;return result;}
        const auto stopped=[&]{try{return getCancelled && getCancelled();}catch(...){return true;}};
        if(stopped()){result.error=MeshReadError::Cancelled;return result;}
        try {
            count=std::min({count,GetCount()-offset,imageChunkLimit/sizeof(Value)});
            if(!count && offset!=GetCount()){result.error=MeshReadError::InvalidRange;return result;}
            result.values.resize(count);
            if(count)m_source->ReadValues(offset,count,result.values.data());
            if(stopped()){result.values.clear();result.error=MeshReadError::Cancelled;return result;}
            result.nextOffset=offset+count;result.isComplete=result.nextOffset==GetCount();
        }catch(const std::bad_alloc&){result.values.clear();result.error=MeshReadError::AllocationFailed;}
        catch(...){result.values.clear();result.error=MeshReadError::ReadFailed;}
        return result;
    }
    class Iterator final {
    public:
        using iterator_category=std::input_iterator_tag;
        using value_type=Value;using difference_type=std::ptrdiff_t;using reference=Value;using pointer=void;
        Iterator()=default;Iterator(const MeshReadArray* owner,std::size_t index):m_owner(owner),m_index(index){}
        Value operator*()const{return (*m_owner)[m_index];}
        Value operator[](difference_type n)const{return *(*this+n);}
        Iterator& operator++(){++m_index;return *this;}Iterator operator++(int){auto old=*this;++*this;return old;}
        Iterator& operator--(){--m_index;return *this;}Iterator operator--(int){auto old=*this;--*this;return old;}
        Iterator& operator+=(difference_type n){m_index=std::size_t(difference_type(m_index)+n);return *this;}
        Iterator& operator-=(difference_type n){return *this+=-n;}
        Iterator operator+(difference_type n)const{auto value=*this;return value+=n;}
        friend Iterator operator+(difference_type n,Iterator value){return value+=n;}
        Iterator operator-(difference_type n)const{auto value=*this;return value-=n;}
        difference_type operator-(const Iterator& other)const{return difference_type(m_index)-difference_type(other.m_index);}
        bool operator==(const Iterator& other)const{return m_owner==other.m_owner && m_index==other.m_index;}
        bool operator!=(const Iterator& other)const{return !(*this==other);}
        bool operator<(const Iterator& other)const{return m_index<other.m_index;}
        bool operator>(const Iterator& other)const{return other<*this;}
        bool operator<=(const Iterator& other)const{return !(other<*this);}
        bool operator>=(const Iterator& other)const{return !(*this<other);}
    private:const MeshReadArray* m_owner=nullptr;std::size_t m_index=0;
    };
    Iterator begin()const{return {this,0};}Iterator end()const{return {this,GetCount()};}
    template<class Predicate> bool All(Predicate predicate,const std::function<bool()>& cancelled={})const {
        for(std::size_t offset=0;offset<size();) {
            auto chunk=GetChunk(offset,65536/sizeof(Value)+1,cancelled);
            if(chunk.error!=MeshReadError::None)throw std::runtime_error("Mesh validation read failed.");
            if(!std::all_of(chunk.values.begin(),chunk.values.end(),predicate))return false;
            offset=chunk.nextOffset;
        }
        return true;
    }
    bool operator==(const MeshReadArray& other)const{return size()==other.size() && std::equal(begin(),end(),other.begin());}
    bool operator!=(const MeshReadArray& other)const{return !(*this==other);}
private:
    friend class MeshPayloadBuilder;
    std::shared_ptr<const MeshValueSource<Value>> m_source;
};

struct MeshField final {
    std::string name;
    std::size_t componentCount=0;
    MeshReadArray<double> values;
    std::uint32_t activeRoles=0;
};

class SurfaceMeshPayload final : public IDataPayload {
    friend class RoiService;
    friend class MeshPayloadBuilder;
    class RoiMeshIndex;
    struct MeshData final {
        MeshData(MeshReadArray<double> points,MeshReadArray<std::uint64_t> faces,
            std::vector<MeshField> pointData,std::vector<MeshField> cellData)
            : vertices(std::move(points)),triangles(std::move(faces)),
              pointAttributes(std::move(pointData)),cellAttributes(std::move(cellData)) {}
        MeshReadArray<double> vertices;
        MeshReadArray<std::uint64_t> triangles;
        std::vector<MeshField> pointAttributes;
        std::vector<MeshField> cellAttributes;
        // 索引只由不可变网格载荷拥有；任务借用拓扑时另外持有该载荷快照。
        mutable std::mutex roiIndexMutex;
        mutable std::array<double,16> roiIndexTransform{};
        mutable std::shared_ptr<const RoiMeshIndex> roiIndex;
    };
public:
    SurfaceMeshPayload(MeshReadArray<double> vertices,MeshReadArray<std::uint64_t> triangles,
        std::vector<MeshField> pointFields,std::string coordinateFrame,std::vector<MeshField> cellFields={})
        :SurfaceMeshPayload(std::move(vertices),std::move(triangles),std::move(pointFields),
            std::move(coordinateFrame),std::move(cellFields),ReadUse{}){}
    SurfaceMeshPayload(
        DataArray<double> vertices,
        DataArray<std::uint64_t> triangles,
        std::vector<MeshAttribute> pointAttributes = {},
        std::string coordinateFrame = "RAS",
        std::vector<MeshAttribute> cellAttributes = {})
        : m_mesh(std::make_shared<const MeshData>(
            MeshReadArray<double>(std::move(vertices)), MeshReadArray<std::uint64_t>(std::move(triangles)),
            GetFields(std::move(pointAttributes)),GetFields(std::move(cellAttributes))))
        , m_coordinateFrame(std::move(coordinateFrame))
        , m_isValid(GetMeshValid())
    {
    }

    DataTypeId GetDataType() const override { return DataTypes::surfaceMesh; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const SurfaceMeshPayload>(*this);
        }
        catch (...) {
            return {};
        }
    }

    const MeshReadArray<double>& GetPointValues() const noexcept { return m_mesh->vertices; }
    const MeshReadArray<std::uint64_t>& GetTriangleIndices() const noexcept
    {
        return m_mesh->triangles;
    }
    const std::vector<MeshField>& GetPointFields() const noexcept
    {
        return m_mesh->pointAttributes;
    }
    const std::vector<MeshField>& GetCellFields() const noexcept { return m_mesh->cellAttributes; }
    const std::string& GetCoordinateFrame() const noexcept
    {
        return m_coordinateFrame;
    }
    bool GetValid() const noexcept { return m_isValid; }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override
    {
        return { m_mesh };
    }

private:
    struct ReadUse final {std::function<bool()> cancelled;};
    SurfaceMeshPayload(MeshReadArray<double> points,MeshReadArray<std::uint64_t> faces,
        std::vector<MeshField> pointData,std::string frame,std::vector<MeshField> cellData,ReadUse use)
        :m_mesh(std::make_shared<const MeshData>(std::move(points),std::move(faces),std::move(pointData),std::move(cellData))),
         m_coordinateFrame(std::move(frame)),m_isValid(GetMeshValid(use.cancelled)){}
    static std::vector<MeshField> GetFields(std::vector<MeshAttribute> input) {
        std::vector<MeshField> fields;fields.reserve(input.size());
        for(auto& field:input)fields.push_back({std::move(field.name),field.componentCount,MeshReadArray<double>(std::move(field.values)),field.activeRoles});
        return fields;
    }
    bool GetMeshValid(const std::function<bool()>& cancelled={}) const noexcept
    {
        try {
        if (m_mesh->vertices.size() % 3 != 0
            || m_mesh->triangles.size() % 3 != 0
            || (m_mesh->vertices.empty() && !m_mesh->triangles.empty())
            || m_coordinateFrame.empty()
            || !m_mesh->vertices.All(
                [](const double value) { return std::isfinite(value); },cancelled)) {
            return false;
        }
        const auto pointCount = m_mesh->vertices.size() / 3;
        if (!m_mesh->triangles.All(
                [pointCount](const std::uint64_t value) {
                    return value < pointCount;
                },cancelled)) {
            return false;
        }
        const auto validAttributes=[&cancelled](const std::vector<MeshField>& attributes,std::size_t tuples) {
            std::uint32_t roles=0;
            for(std::size_t index=0;index<attributes.size();++index) {
                const auto& attribute=attributes[index];
                if(attribute.name.empty()||attribute.componentCount==0
                    ||attribute.componentCount>static_cast<std::size_t>(std::numeric_limits<int>::max())
                    ||tuples>std::numeric_limits<std::size_t>::max()/attribute.componentCount
                    ||attribute.values.size()!=tuples*attribute.componentCount
                    ||(attribute.activeRoles&~MeshAttributeRoles::all)||(roles&attribute.activeRoles)
                    ||!attribute.values.All([](double v){return std::isfinite(v);},cancelled))return false;
                for(std::size_t prior=0;prior<index;++prior)if(attributes[prior].name==attribute.name)return false;
                if((attribute.activeRoles&(MeshAttributeRoles::normals|MeshAttributeRoles::vectors))&&attribute.componentCount!=3)return false;
                if((attribute.activeRoles&MeshAttributeRoles::tensors)&&attribute.componentCount!=9)return false;
                if((attribute.activeRoles&MeshAttributeRoles::textureCoordinates)&&attribute.componentCount>3)return false;
                roles|=attribute.activeRoles;
            }
            return true;
        };
        if(!validAttributes(m_mesh->pointAttributes,pointCount)
            ||!validAttributes(m_mesh->cellAttributes,m_mesh->triangles.size()/3))return false;
        return true;
        }catch(...){return false;}
    }

private:
    std::shared_ptr<const MeshData> m_mesh;
    std::string m_coordinateFrame;
    bool m_isValid = false;
};

using RecordColumnValues = std::variant<
    DataArray<std::int64_t>,
    DataArray<std::uint64_t>,
    DataArray<double>,
    DataArray<std::string>,
    DataArray<std::array<std::int64_t, 3>>,
    DataArray<std::array<std::int64_t, 6>>,
    DataArray<std::array<double, 3>>,
    DataArray<std::array<double, 6>>,
    DataArray<std::uint8_t>>;

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
        , m_columns(std::make_shared<const std::vector<RecordColumn>>(std::move(columns)))
    {
    }

    DataTypeId GetDataType() const override { return m_type; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try {
            return std::make_shared<const RecordTablePayload>(*this);
        }
        catch (...) {
            return {};
        }
    }
    const std::string& GetSchemaName() const noexcept { return m_schemaName; }
    const std::vector<RecordColumn>& GetColumns() const noexcept
    {
        return *m_columns;
    }
    std::size_t GetRowCount() const noexcept
    {
        return m_columns->empty() ? 0 : GetRecordColumnSize(m_columns->front());
    }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override { return {m_columns}; }
    bool GetValid() const noexcept
    {
        if (!GetDataTypeIdValid(m_type)
            || m_schemaName.empty() || m_columns->empty()) {
            return false;
        }
        const auto rowCount = GetRowCount();
        std::vector<std::string> names;
        names.reserve(m_columns->size());
        for (const auto& column : *m_columns) {
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
    // 列在接纳时冻结；图快照和分页读者只共享 owner，不重复复制全表。
    std::shared_ptr<const std::vector<RecordColumn>> m_columns;
};

class RoiGeometryPayload final : public IDataPayload {
public:
    explicit RoiGeometryPayload(RoiDefinition definition)
        : m_definition(std::move(definition)) {}
    DataTypeId GetDataType() const override { return DataTypes::roiGeometry; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try { return std::make_shared<const RoiGeometryPayload>(m_definition); }
        catch (...) { return {}; }
    }
    const RoiDefinition& GetDefinition() const noexcept { return m_definition; }
    // 这里只检查封装；完整局部/关系校验由 Store 的 Host 私有求值器执行。
    bool GetValid() const noexcept
    {
        return GetDataRevisionRefValid(m_definition.source)
            && !m_definition.nodes.empty() && m_definition.nodes.size() <= roiNodeLimit;
    }
private:
    RoiDefinition m_definition;
};

struct RoiCatalogEntry final {
    DataRevisionRef geometry;
    RoiMetadata metadata;
};

class RoiCatalogPayload final : public IDataPayload {
public:
    explicit RoiCatalogPayload(std::vector<RoiCatalogEntry> entries)
        : m_entries(std::move(entries)) {}
    DataTypeId GetDataType() const override { return DataTypes::roiCatalog; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override
    {
        try { return std::make_shared<const RoiCatalogPayload>(m_entries); }
        catch (...) { return {}; }
    }
    const std::vector<RoiCatalogEntry>& GetEntries() const noexcept { return m_entries; }
    bool GetValid() const noexcept
    {
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            const auto& entry = m_entries[i];
            if (!GetDataRevisionRefValid(entry.geometry)
                || entry.metadata.name.empty() || entry.metadata.name.size() > 256
                || entry.metadata.group.size() > 256 || entry.metadata.description.size() > 4096
                || (i && !(m_entries[i - 1].geometry.entityId < entry.geometry.entityId))) return false;
        }
        return true;
    }
private:
    std::vector<RoiCatalogEntry> m_entries;
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
    std::vector<std::shared_ptr<const void>> GetDataResources() const override
    {
        return { m_values };
    }
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
            DataTypes::roiCatalog, {},
            [](const IDataPayload& payload, std::string& message) {
                const auto* catalog = dynamic_cast<const RoiCatalogPayload*>(&payload);
                if (!catalog || !catalog->GetValid()) {
                    message = "RoiCatalog payload is invalid.";
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
