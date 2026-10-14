#pragma once

#include <array>
#include <cstdint>
#include <optional>

enum class InputPhase : std::uint8_t { Discrete, Begin, Update, End };
enum class WheelUnit : std::uint8_t { Steps, LogicalPixels };
enum class NavigationStatus : std::uint8_t {
    Applied, Unbound, Invalid, Unsupported, Busy, WrongThread, Failed
};
enum class ZoomAnchorMode : std::uint8_t {
    ViewportCenter, Pointer, DisplayPoint, NormalizedPoint, Crosshair, WorldPoint
};
struct ZoomAnchor final {
    ZoomAnchorMode mode = ZoomAnchorMode::ViewportCenter;
    std::array<double,3> point{};
    static ZoomAnchor ViewportCenter() { return {}; }
    static ZoomAnchor Pointer() { return {ZoomAnchorMode::Pointer,{}}; }
    static ZoomAnchor DisplayPoint(double x,double y) { return {ZoomAnchorMode::DisplayPoint,{x,y,0}}; }
    static ZoomAnchor NormalizedPoint(double x,double y) { return {ZoomAnchorMode::NormalizedPoint,{x,y,0}}; }
    static ZoomAnchor WorldPoint(std::array<double,3> p) { return {ZoomAnchorMode::WorldPoint,p}; }
    static ZoomAnchor Crosshair() { return {ZoomAnchorMode::Crosshair,{}}; }
};
struct NavigationOptions final {
    struct Zoom final {
        double wheelFactorPerStep = 1.1;
        double dragSensitivity = 0.009950330853168082; // ln(1.01)
        double pixelSensitivity = 0.004;
        double precisionMultiplier = 0.25;
        bool isInverted = false;
        ZoomAnchorMode wheelAnchor = ZoomAnchorMode::ViewportCenter;
        ZoomAnchorMode dragAnchor = ZoomAnchorMode::ViewportCenter;
    } zoom;
    struct Pan final { double sensitivity=1; bool isInverted=false; } pan;
    struct Rotate final {
        double degreesPerPixel=0.5;
        ZoomAnchorMode pivot=ZoomAnchorMode::ViewportCenter;
    } rotate;
    struct Slice final { int countPerStep=5; int acceleratedCount=10; } slice;
    struct WindowLevel final {
        double widthSensitivity=3; double centerSensitivity=3;
        bool isWidthInverted=false; bool isCenterInverted=false;
    } windowLevel;
};
struct NavigationState final {
    double zoomRatio=1;
    double visibleWorldHeight=0;
    double windowWidth=0;
    double windowCenter=0;
    bool isSliceView=false;
    bool isParallel=false;
};
struct NavigationInput final {
    double deltaX=0; double deltaY=0; // 逻辑像素；左下角原点
    std::array<double,2> displayPosition{};
    double wheelDelta=0;
    WheelUnit wheelUnit=WheelUnit::Steps;
    InputPhase phase=InputPhase::Discrete;
    bool isWheel=false;
    bool hasPointer=false;
    bool isPrecisionMode=false;
    bool isAccelerated=false;
    bool isRollMode=false;
    double displayScale=1;
};
struct WindowLevelRequest final { double width=0; double center=0; };
struct NavigationContext final {
    bool isSliceView=false;
    std::array<int,2> viewportSize{};
    std::array<int,2> viewportOrigin{};
    std::optional<std::array<double,3>> crosshairWorld;
    NavigationState state;
    std::optional<WindowLevelRequest> startWindowLevel;
    NavigationOptions options; // 手势开始时冻结，独立滚轮使用当前配置
};
struct ZoomRequest final { double factor=1; ZoomAnchor anchor; };
struct PanRequest final { double deltaX=0; double deltaY=0; };
struct RotateRequest final { double yawDegrees=0; double pitchDegrees=0; double rollDegrees=0; ZoomAnchor pivot; };
struct SliceScrollRequest final { double count=0; };
