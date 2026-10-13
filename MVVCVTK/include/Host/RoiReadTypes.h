#pragma once

#include "Data/RoiTypes.h"

#include <functional>
#include <memory>
#include <vector>

struct RoiPlane final {
    std::array<double, 3> origin{};
    std::array<double, 3> normal{ 0, 0, 1 };
};

struct RoiPlanesResult final {
    RoiError error = RoiError::UnsupportedRoi;
    // 每个法线正侧的交集；source 域已包含在内。
    std::vector<RoiPlane> planes;
};

struct RoiPointsResult final {
    RoiError error = RoiError::InvalidRequest;
    std::size_t requiredBytes = 0;
    std::vector<std::uint8_t> values;
};

struct RoiPolygonResult final {
    RoiError error = RoiError::UnsupportedRoi;
    std::size_t requiredBytes = 0;
    std::vector<std::array<double,3>> points;
};

struct RoiTriangleSelection final {
    RoiError error = RoiError::UnsupportedRoi;
    std::vector<std::array<std::array<double,3>,3>> triangles;
    // 通用路径按源体素单元选片；解析定义离散化时报告最大半单元对角线。
    double boundaryTolerance = 0;
    bool isDiscrete = false;
};

struct RoiMaskRequest final {
    ImageReadRegion region;
    std::size_t voxelOffset = 0;
    std::size_t maxBytes = roiCopyLimit;
    // 可能由多个 worker 调用；只读且线程安全。抛异常按取消处理。
    std::function<bool()> getCancelled;
};

struct RoiMaskResult final {
    RoiError error = RoiError::InvalidRequest;
    std::size_t requiredBytes = 0;
    std::size_t nextOffset = 0;
    bool isComplete = false;
    std::vector<std::uint8_t> values;
};

enum class RoiBlockClassification : std::uint8_t { Unavailable, Outside, Inside, Boundary };
struct RoiBlockResult final {RoiError error=RoiError::Unavailable;RoiBlockClassification classification=RoiBlockClassification::Unavailable;};

// 不可变、可跨 worker 只读持有，不包含 Session 或可写 VTK identity。
class RoiReadView {
public:
    virtual ~RoiReadView() noexcept = default;
    // 对完整索引块判定；资源或读取错误与已证实的空区域分开返回。
    RoiBlockResult GetBlockClassification(RoiMaskRequest request) const
    {
        request.voxelOffset=0;bool inside=false,outside=false;
        for(;;){const auto chunk=GetMaskChunk(request);if(chunk.error!=RoiError::None)return {chunk.error};
            for(auto value:chunk.values){inside=inside || value!=0;outside=outside || value==0;}
            if(inside && outside)return {RoiError::None,RoiBlockClassification::Boundary};
            if(chunk.isComplete)return {RoiError::None,inside?RoiBlockClassification::Inside:RoiBlockClassification::Outside};
            if(chunk.nextOffset<=request.voxelOffset)return {RoiError::Unavailable};request.voxelOffset=chunk.nextOffset;
        }
    }
    virtual DataRevisionRef GetRevision() const noexcept = 0;
    virtual DataRevisionRef GetSource() const noexcept = 0;
    virtual const RoiDefinition& GetDefinition() const noexcept = 0;
    virtual const std::vector<DataRevisionRef>& GetDependencies() const noexcept = 0;
    virtual std::array<double, 6> GetBounds() const noexcept = 0;
    virtual bool GetContains(const std::array<double, 3>& sourcePoint) const noexcept = 0;
    virtual RoiPointsResult GetPoints(
        const std::vector<std::array<double, 3>>& sourcePoints,
        std::size_t maxBytes = roiCopyLimit) const = 0;
    virtual RoiPlanesResult GetClipPlanes() const = 0;
    virtual RoiTriangleSelection GetTriangleSelection(const std::array<std::array<double,3>,3>&,
        std::size_t = roiCopyLimit,double = 0,std::function<bool()> = {}) const = 0;
    // 单个三角形与凸 ROI 的精确平面交，最多 planeCount+3 个顶点；不生成封口面。
    virtual RoiPolygonResult GetClippedTriangle(
        const std::array<std::array<double,3>,3>& triangle,
        std::size_t maxBytes = roiCopyLimit) const = 0;
    // 仅凸平面区域内点支持到边界的距离；区域外或不支持的表达式返回 NaN。
    virtual double GetBoundaryDistance(
        const std::array<double, 3>& sourcePoint) const noexcept = 0;
    virtual RoiMaskResult GetMaskChunk(const RoiMaskRequest& request) const = 0;
};
using RoiReadSnapshot = std::shared_ptr<const RoiReadView>;

struct RoiReadResult final {
    RoiError error = RoiError::Unavailable;
    RoiReadSnapshot roi;
    std::string message;
};
