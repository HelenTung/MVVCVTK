#pragma once

#include "Data/DataVersion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

inline constexpr std::size_t imageMetadataAttributeLimit = 64;
inline constexpr std::size_t imageMetadataKeyByteLimit = 128;
inline constexpr std::size_t imageMetadataTextByteLimit = 4096;
inline constexpr std::size_t imageDatasetIdByteLimit = 256;

enum class ImageSourceKind : std::uint8_t {
    RawFile,
    TiffSeries,
    Memory
};

struct DatasetIdentity final {
    std::string datasetId;
    std::optional<std::string> inspectionId;
    std::optional<std::string> objectId;
    std::optional<std::string> batchId;
};

struct ImageSourceMetadata final {
    ImageSourceKind kind = ImageSourceKind::Memory;
    std::string uri;
    // 请求传 0 表示由加载边界解析；发布后的 descriptor 始终携带实际字节数。
    std::uint64_t byteSize = 0;
    std::optional<std::string> digest;
};

struct ScalarSemantics final {
    std::string quantity = "gray";
    std::string unit = "1";
    double slope = 1.0;
    double intercept = 0.0;
    // noData 只解释 scalar；逐体素有效性仍由 validityMask 表达。
    std::optional<double> noData;
};

using MetadataAttributeValue = std::variant<
    bool,
    std::int64_t,
    std::uint64_t,
    double,
    std::string>;

struct MetadataAttribute final {
    std::string key;
    MetadataAttributeValue value;
};

struct ImageMetadata final {
    DatasetIdentity identity;
    ImageSourceMetadata source;
    ScalarSemantics scalar;
    std::vector<MetadataAttribute> attributes;
};

enum class ImageValueType {
    Unknown,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64
};

enum class ImageByteOrder {
    Native
};

enum class ImageTupleOrder {
    XFastestInterleaved
};

// 当前已发布图像的 metadata-only 值快照；几何已经位于内部规范 RAS 空间。
struct ImageDescriptor final {
    ImageMetadata metadata;
    std::array<int, 6> extent = { 0, -1, 0, -1, 0, -1 };
    std::array<int, 3> dims = { 0, 0, 0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    std::array<double, 9> direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    ImageValueType valueType = ImageValueType::Unknown;
    ImageByteOrder byteOrder = ImageByteOrder::Native;
    ImageTupleOrder tupleOrder = ImageTupleOrder::XFastestInterleaved;
    std::size_t componentBytes = 0;
    std::size_t componentCount = 0;
    DataVersion version = 0;
};

enum class ImageReadError {
    None,
    NoImage,
    InvalidData,
    UnsupportedType,
    InvalidRegion,
    TooLarge,
    CopyFailed,
    Cancelled
};

enum class ImageReadAdmission : std::uint8_t {
    Accepted,
    InvalidRequest,
    Busy,
    QueueFull,
    Stopping,
    Unavailable
};

inline constexpr std::size_t imageReadLimit =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t imageChunkLimit =
    8ULL * 1024ULL * 1024ULL;

// 相对 source image 的半开区间 [offset, offset + size)，三个分量均按 X/Y/Z。
struct ImageReadRegion final {
    std::array<std::size_t, 3> offset = { 0, 0, 0 };
    std::array<std::size_t, 3> size = { 0, 0, 0 };
};

struct ImageReadRequest final {
    // 空 region 表示整卷；显式 region 的 size 不允许为 0。
    std::optional<ImageReadRegion> region;
    std::size_t maxBytes = imageReadLimit;
};

using ImageReadBytes =
    std::shared_ptr<const std::vector<std::uint8_t>>;

// 普通读取结果只含值和几何，不包含 VTK identity 或可写 scalar 指针。
struct ImageReadState final {
    std::array<int, 6> extent = { 0, -1, 0, -1, 0, -1 };
    std::array<int, 3> dims = { 0, 0, 0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    // 行主序 3x3；region 输出使用局部 extent，origin 已平移到 region 首体素。
    std::array<double, 9> direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    ImageValueType valueType = ImageValueType::Unknown;
    ImageByteOrder byteOrder = ImageByteOrder::Native;
    ImageTupleOrder tupleOrder = ImageTupleOrder::XFastestInterleaved;
    std::size_t componentBytes = 0;
    std::size_t componentCount = 0;
    ImageReadRegion region;
    // chunk 在 region 的 x-fast 线性序列内从 voxelOffset 开始。
    std::size_t voxelOffset = 0;
    std::size_t voxelCount = 0;
    ImageReadBytes values;
    ImageReadBytes validityMask;
    DataVersion version = 0;
};

struct ImageReadResult final {
    ImageReadError error = ImageReadError::NoImage;
    std::size_t requiredBytes = 0;
    std::optional<ImageReadState> state;
};

struct ImageReadChunkResult final {
    ImageReadError error = ImageReadError::NoImage;
    // 整个 region 的总字节数，而不是当前 chunk 大小。
    std::size_t requiredBytes = 0;
    std::size_t nextVoxelOffset = 0;
    bool isDone = false;
    std::optional<ImageReadState> state;
};

using ImageReadCallback =
    std::function<void(ImageReadResult)>;

inline constexpr std::size_t labelMapIdByteLimit = 128;
inline constexpr std::size_t labelMapNameByteLimit = 256;

using LabelMapVersion = std::uint64_t;
using LabelMapStageToken = std::uint64_t;

enum class LabelMapError : std::uint8_t {
    None,
    Unavailable,
    WrongThread,
    InvalidRequest,
    InvalidData,
    UnsupportedType,
    InvalidRegion,
    NotFound,
    OwnerMismatch,
    SourceMismatch,
    GeometryMismatch,
    VersionMismatch,
    Busy,
    TooLarge,
    CopyFailed,
    Cancelled
};

struct LabelMapDescriptor final {
    std::string id;
    std::string displayName;
    std::string producerFeatureId;
    std::string datasetId;
    DataVersion sourceVersion = 0;
    LabelMapVersion version = 0;
    std::array<int, 6> extent = { 0, -1, 0, -1, 0, -1 };
    std::array<int, 3> dims = { 0, 0, 0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    std::array<double, 9> direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    ImageValueType valueType = ImageValueType::Unknown;
    ImageByteOrder byteOrder = ImageByteOrder::Native;
    ImageTupleOrder tupleOrder = ImageTupleOrder::XFastestInterleaved;
    std::size_t componentBytes = 0;
    std::size_t componentCount = 0;
    std::size_t voxelCount = 0;
};

struct LabelMapReadRequest final {
    std::string id;
    std::optional<ImageReadRegion> region;
    std::size_t maxBytes = imageReadLimit;
    std::optional<LabelMapVersion> expectedVersion;
};

struct LabelMapReadState final {
    LabelMapDescriptor descriptor;
    std::array<int, 6> extent = { 0, -1, 0, -1, 0, -1 };
    std::array<int, 3> dims = { 0, 0, 0 };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    ImageReadRegion region;
    std::size_t voxelOffset = 0;
    std::size_t voxelCount = 0;
    ImageReadBytes values;
};

struct LabelMapReadResult final {
    LabelMapError error = LabelMapError::NotFound;
    std::size_t requiredBytes = 0;
    std::optional<LabelMapReadState> state;
};

struct LabelMapReadChunkResult final {
    LabelMapError error = LabelMapError::NotFound;
    std::size_t requiredBytes = 0;
    std::size_t nextVoxelOffset = 0;
    bool isDone = false;
    std::optional<LabelMapReadState> state;
};
