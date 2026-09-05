#pragma once

#include "Host/Types/HostValueTypes.h"
#include "Host/Types/HostViewTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class HostErrorCode {
    None,
    SessionNotReady,
    WrongThread,
    RequestRejected,
    OperationFailed
};

struct HostResult final {
    bool isSucceeded = false;
    HostErrorCode errorCode = HostErrorCode::OperationFailed;
    std::string message;
};

enum class HostInputKind : std::uint8_t {
    None,
    WheelForward,
    WheelBackward,
    PrimaryPress,
    PrimaryRelease,
    SecondaryPress,
    SecondaryRelease,
    PointerMove,
    KeyPress,
    KeyRelease,
    TextInput,
    Cancel
};

// 上位机注入的值语义输入；坐标使用 VTK 左下角原点的 display 像素坐标。
struct HostInputEvent final {
    std::string viewId;
    HostInputKind kind = HostInputKind::None;
    int x = 0;
    int y = 0;
    bool isShiftDown = false;
    bool isCtrlDown = false;
    bool isAltDown = false;
    char keyCode = 0;
    std::string keySym;
};

struct HostInputResult final {
    bool isSucceeded = false;
    bool isHandled = false;
    bool isDefaultSuppressed = false;
    HostErrorCode errorCode = HostErrorCode::OperationFailed;
    std::string message;
};

class HostInputEndpoint {
public:
    virtual ~HostInputEndpoint() noexcept = default;
    virtual HostInputResult SendInput(const HostInputEvent& event) = 0;
};

using HostCompleteCallback =
    std::function<void(bool isSuccess)>;
using HostResultCallback =
    std::function<void(HostResult result)>;

struct HostRequest {
    virtual ~HostRequest() = default;
};

struct HostLoadRequest final : HostRequest {
    std::string filePath; // UTF-8 文件路径。
    HostVolumeGeometry geometry;
};

struct HostReloadRequest final : HostRequest {
    std::vector<float> voxels; // X 最快、随后 Y/Z 的连续 float32 标量；请求对象拥有其存储。
    HostVolumeGeometry geometry; // dimensions 的乘积必须与 voxels.size() 一致。
};

struct HostDataExportRequest final : HostRequest {
    std::string outputPath; // UTF-8 输出目录；文件名由 Data 层基于冻结数据生成。
    // 缺省时由 sourceView 模式收敛：体渲染导出 RAW，等值面导出 PLY。
    std::optional<HostDataExportFormat> format;
    // 未指定 selector 时使用 Primary3D。
    HostViewTarget sourceView;
};

struct HostSliceExportRequest final : HostRequest {
    std::string outputDir; // UTF-8 输出目录；每层写入 PNG。
    // 热键缺省时由触发窗口补齐；显式请求仅在需要指定切片方向时设置。
    HostViewTarget sourceView;
    std::optional<double> angleDeg; // 可选的平面内旋转角；未提供时保持目标视图当前方向。
};

struct HostViewSetRequest final : HostRequest {
    HostViewTarget targetView; // 单目标解析遵循 id 优先且失败不回退 role。
    // 全部字段均为 targetView 私有展示状态；缺省字段保留该 View 当前值。
    std::optional<HostRenderMode> mode;
    std::optional<HostMaterialParams> material;
    std::optional<HostMaterialPreset> materialPreset;
    std::optional<double> opacity;
    std::optional<HostVolumeTransferFunction>
        volumeTransferFunction;
    std::optional<double> iso;
    std::optional<HostBackgroundColor> background;
    std::optional<HostWindowLevelParams> windowLevel;
    std::optional<HostVolumeQuality> volumeQuality;
    std::optional<HostVisibilityParams> visibility;
    std::optional<bool> isAxesVisible; // 目标 context 的世界方向轴 marker。
};

// 数据物理元信息与 world cursor 是 Session 真源；命令不接收 targetView，避免伪装成单 View 写入。
struct HostSessionSetRequest final : HostRequest {
    std::optional<std::array<double, 3>> spacing;
    std::optional<HostCursorParams> cursor;
};

struct HostViewResetRequest final : HostRequest {
    HostViewTarget targetView;
};

struct HostToolSetRequest final : HostRequest {
    HostViewTarget targetView;
    HostToolMode toolMode = HostToolMode::Navigation;
};

struct HostToolSwitchRequest final : HostRequest {
    HostViewTarget targetView;
};
