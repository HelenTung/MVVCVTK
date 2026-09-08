#pragma once

#include "Data/ImageReadTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

inline constexpr std::size_t roiNodeLimit = 256;
inline constexpr std::size_t roiDepthLimit = 64;
inline constexpr std::size_t roiCatalogLimit = 4096;
inline constexpr std::uint32_t roiSchemaVersion = 2;
inline constexpr std::size_t roiCopyLimit = 8ULL * 1024 * 1024;
inline constexpr std::array<double, 16> roiIdentityMatrix = {
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
inline constexpr std::string_view roiCatalogBinding = "session.roi.catalog";

enum class RoiShape : std::uint8_t { Box, HalfSpace, MaskReference };
enum class RoiNodeKind : std::uint8_t {
    Empty, SourceDomain, Primitive, Union, Intersection, Difference
};

struct RoiPrimitive final {
    RoiShape shape = RoiShape::Box;
    // sourcePoint = localToSource * localPoint；Box 局部域为 [-1,1]^3。
    std::array<double, 16> localToSource = roiIdentityMatrix;
    // HalfSpace 在源物理坐标中定义，法线正侧（含平面）属于区域。
    std::array<double, 3> origin = { 0, 0, 0 };
    std::array<double, 3> normal = { 0, 0, 1 };
    // 同源同网格的精确掩码修订，非零为前景；不接受额外空间变换。
    std::optional<DataRevisionRef> mask;
};

struct RoiNode final {
    RoiNodeKind kind = RoiNodeKind::Primitive;
    RoiPrimitive primitive;
    // 二元节点只引用更早节点；叶节点两项必须为 0，最后节点是根。
    std::uint32_t left = 0;
    std::uint32_t right = 0;
};

struct RoiDefinition final {
    DataRevisionRef source;
    std::vector<RoiNode> nodes;
};

struct RoiMetadata final {
    std::string name;
    std::string group;
    std::string description;
    bool isArchived = false;
};

struct RoiDescriptor final {
    DataRevisionRef revision;
    DataRevisionRef currentRevision;
    RoiDefinition definition;
    // 元信息为当前目录投影，几何可为指定的历史修订。
    RoiMetadata metadata;
    DataBindingRevision catalogRevision = 0;
};

enum class RoiError : std::uint8_t {
    None, Unavailable, WrongThread, InvalidRequest, InvalidGeometry,
    UnsupportedRoi, MissingInput, SourceMismatch, SourceUnresolved,
    RevisionConflict, TooLarge, Cancelled, CommitFailed
};

enum class RoiAction : std::uint8_t { Create, SetGeometry, SetMetadata, Copy };

struct RoiRequest final {
    RoiAction action = RoiAction::Create;
    RoiDefinition definition;
    RoiMetadata metadata;
    std::optional<DataRevisionRef> expectedRoi;
    DataBindingRevision expectedCatalogRevision = 0;
    std::optional<DataRevisionRef> copyFrom;
    // 当前绑定驱动的操作可冻结来源绑定；显式历史来源操作可省略。
    std::optional<DataBinding> expectedSourceBinding;
};

struct RoiResult final {
    RoiError error = RoiError::Unavailable;
    std::optional<RoiDescriptor> roi;
    std::size_t requiredBytes = 0;
    std::string message;
    DataCommitId commitId = 0;
};

// 来源的外部稳定身份由调用应用解析；image 内的运行期 revision 必须清除。
struct RoiSourceDescriptor final {
    DataTypeId type;
    std::string coordinateFrame;
    std::optional<ImageDescriptor> image;
    std::array<double, 6> sourceBounds{};
    std::size_t pointCount = 0;
    std::size_t triangleCount = 0;
};

struct RoiArchiveMask final {
    ImageDescriptor grid;
    std::vector<std::uint32_t> nodeIndices;
    std::vector<std::uint8_t> values;
};

struct RoiArchive final {
    std::uint32_t schemaVersion = roiSchemaVersion;
    std::string sourceKey;
    RoiSourceDescriptor source;
    RoiMetadata metadata;
    // mask 引用清空，由 masks.nodeIndices 关联归档内数据，不持久化运行期 ID。
    std::vector<RoiNode> nodes;
    std::vector<RoiArchiveMask> masks;
};

struct RoiArchiveResult final {
    RoiError error = RoiError::Unavailable;
    std::size_t requiredBytes = 0;
    std::optional<RoiArchive> archive;
    std::string message;
};
