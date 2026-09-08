#pragma once

#include "App/ViewTypes.h"
#include "Data/DataPayloads.h"

#include <array>
#include <string>

enum class RulerUnit { Auto, Millimeter, Micrometer };
enum class RulerPosition { BottomLeft, BottomRight, TopLeft, TopRight };
enum class RulerStatus {
    Pending, Visible, Hidden, NoData, InvalidGeometry,
    InvalidTransform, UnsupportedProjection, ViewportTooSmall, InvalidScale
};

struct RulerParams final {
    RulerUnit unit = RulerUnit::Auto;
    RulerPosition position = RulerPosition::BottomRight;
    double targetPixels = 150.0;
    int fontSize = 16;
    std::array<double, 3> color{ 1.0, 1.0, 1.0 };
};

// 全部长度属于 SDK 的 mm 数据空间；不保存 image/payload 或其资源 owner。
struct RulerInput final {
    GridGeometry3D geometry;
    std::array<double, 16> modelToWorld{
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    VizMode mode = VizMode::Volume;
    bool hasData = false;
    bool isVisible = false;
    DataRevisionRef dataRevision;
    DataBindingRevision bindingRevision = 0;
};

struct RulerState final {
    RulerStatus status = RulerStatus::NoData;
    double lengthMm = 0.0;
    double lengthPixels = 0.0;
    std::string label;
    DataRevisionRef dataRevision;
    DataBindingRevision bindingRevision = 0;
};

// 常数规模计算；输入是物理 world 差向量，不再乘 spacing。
class RulerMetrics final {
public:
    static bool GetParamsValid(const RulerParams& params) noexcept;
    static bool GetGeometryValid(const GridGeometry3D& geometry) noexcept;
    static bool GetInverse(const std::array<double, 16>& matrix,
        std::array<double, 9>& inverse) noexcept;
    static double GetLength(const std::array<double, 9>& inverse,
        const std::array<double, 3>& worldDelta) noexcept;
    static RulerState BuildState(double mmPerPixel, double availablePixels,
        const RulerParams& params, double previousLengthMm);
};
