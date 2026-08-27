#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

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
    std::uint64_t version = 0;
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
