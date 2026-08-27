#pragma once
// =====================================================================
// AppTypes.h   纯数据结构定义，无 VTK / 线程 依赖
//
// 所有其他头文件均可安全包含此文件，不会引入循环依赖。
//
//   • 新增 LoadState 枚举（替代 bool m_isLoading）
//   • WindowConfig 增加 backgroundColor
//   • PreInitConfig 增加 bgColor / hasBgColor 字段
//   • 新增 operator|= / operator&= 辅助
// =====================================================================

#include "App/ViewTypes.h"
#include "Data/DataVersion.h"

#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>

// 通用交互来源键；主体只比较完整键，不解释 Feature 的 owner/channel 文本。
struct InteractionSource final {
    std::string ownerId;
    std::string channelId;

    bool operator==(const InteractionSource& other) const noexcept
    {
        return ownerId == other.ownerId && channelId == other.channelId;
    }
};

// 通用 Feature 来源键；App 只比较 id，不解释具体模块名称。
struct FeatureSource final {
    std::string id;

    bool operator==(const FeatureSource& other) const noexcept
    {
        return id == other.id;
    }
};

// --- 通用状态枚举（用于数据可信、文件流加载、重载加载等状态）---
enum class LoadState {
    Idle,       // 未发起加载
    Loading,    // 加载中（后台线程运行）
    Succeeded,  // 加载成功
    Failed      // 加载失败
};

// --- 加载事件来源枚举（用于区分文件流加载和重载加载的失败事件）--- 
enum class LoadEventKind {
    None,
    File,
    Reload
};

// --- 传输函数节点 ---
struct TFNode {
    double position; // 标量范围内的归一化位置，[0.0, 1.0] 映射到 [rangeMin, rangeMax]
    double opacity;  // 节点不透明度，[0.0, 1.0]；体渲染时再乘材质全局 opacity
    double r, g, b;  // 归一化 RGB 分量，顺序为 [r, g, b]，各分量范围 [0.0, 1.0]
};

// 数据导出接纳时冻结的完整参数；worker 只消费本值对象，不再读取后续视觉状态。
struct DataExportParams final {
    std::string extension;
    double isoValue = 0.0;
    std::array<double, 16> modelToWorld = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    std::vector<TFNode> tfNodes;
};

// --- 材质参数 ---
struct MaterialParams {
    // 无量纲光照系数，直接传给 VTK 材质；常规有效范围为 [0.0, 1.0]。
    double ambient = 0.1;
    double diffuse = 0.7;
    double specular = 0.2;
    double specularPower = 10.0; // Phong 高光指数，非负；默认 10.0
    double opacity = 1.0;        // 全局不透明度，[0.0, 1.0]；默认完全不透明
    bool   isShadeOn = false;    // true 启用体渲染阴影或等值面 Phong 插值；默认关闭
};

enum class VolumeQuality {
    Quality,
    Custom
};

struct VolumeQualityParams final {
    VolumeQuality quality = VolumeQuality::Quality;
    int maxDimension = 766;
    double sampleDistance = 1.0;
    bool isJitterOn = true;
};

// Feature 只派生临时 Quality，不改写调用方保存的 Quality/Custom 配置。
inline VolumeQuality GetVolumeQuality(
    const VolumeQualityParams& configured,
    bool isFeatureActive) noexcept
{
    return isFeatureActive
        ? VolumeQuality::Quality : configured.quality;
}

// 刷新调度只反映交互生命周期，不再选择质量档或 producer。
inline double GetRenderRate(bool isInteracting) noexcept
{
    constexpr double staticRate = 0.001;
    constexpr double fastRate = 15.0;
    return isInteracting ? fastRate : staticRate;
}

struct GradientOpacityNode final {
    double gradient = 0.0; // VTK gradient-opacity 原生域中的梯度幅值，必须非负。
    double opacity = 0.0;  // 归一化不透明度 [0,1]。
};

// Scalar TF 是 session-wide 真源；Manual 表示外部节点，Percentile 表示随数据版本重算。
enum class TransferPreset {
    Manual,
    Percentile
};

// --- 背景色（RGB，0~1）---
struct BackgroundColor {
    double r = 0.1, g = 0.1, b = 0.1; // 归一化 [r, g, b]，各分量范围 [0.0, 1.0]，默认深灰
};

// -- 切片窗宽窗位参数 ---
struct WindowLevelParams {
    double windowWidth = 400.0;  // 标量值域中的窗宽，期望大于 0；交互路径下限为 0.01
    double windowCenter = 40.0;  // 标量值域中的窗位中心；默认 40.0
};

// Auto 随已提交数据范围重算；任何显式配置或交互写入都会切换为 Manual。
enum class WindowLevelMode {
    Auto,
    Manual
};

// --- 更新类型位掩码（可组合）---
enum class UpdateFlags : int {
    None = 0,
    Cursor = 1 << 0,  // 位置改变        (0x01)
    TF = 1 << 1,  // 颜色/透明度改变 (0x02)
    IsoValue = 1 << 2,  // 阈值改变        (0x04)
    Material = 1 << 3,  // 材质参数改变    (0x08)
    RenderRate = 1 << 4,  // 交互来源边界改变，仅用于同步窗口刷新率 (0x10)
    Transform = 1 << 5,  // 变换矩阵改变    (0x20)
    DataReady = 1 << 6,  // 数据加载成功    (0x40)
    LoadFailed = 1 << 7,  // 数据加载失败    (0x80)
    Background = 1 << 8,  // 背景色改变      (0x100)
	Visibility = 1 << 9,  // 可见性改变      (0x200)
    WindowLevel = 1 << 10,  // 切片窗宽窗位改变   (0x400)
    Spacing = 1 << 11,  // 体数据 spacing 改变 (0x800)
    FileLoad = 1 << 12,  // load 终态来源：文件加载
    ReloadLoad = 1 << 13,  // load 终态来源：buffer/reload
    Quality = 1 << 14, // 目标 view 的 Volume producer/mapper 质量配置
    GradientOpacity = 1 << 15, // 目标 view 的梯度不透明度函数
    Denoise = 1 << 16, // 目标 view 的仅显示降噪 producer
    All = Cursor | TF | IsoValue | Material | RenderRate | Transform
        | WindowLevel | Visibility | Background | Spacing | DataReady
        | Quality | GradientOpacity | Denoise
};

// 位运算辅助
inline UpdateFlags operator|(UpdateFlags a, UpdateFlags b) {
    return static_cast<UpdateFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline UpdateFlags operator&(UpdateFlags a, UpdateFlags b) {
    return static_cast<UpdateFlags>(static_cast<int>(a) & static_cast<int>(b));
}
inline UpdateFlags& operator|=(UpdateFlags& a, UpdateFlags b) {
    a = a | b; return a;
}
// 可视元素位定义
namespace VisFlags {
    constexpr uint32_t Planes3D = 1 << 0;   // 3D 彩色切平面
    constexpr uint32_t Crosshair = 1 << 1;  // 2D 十字线
    constexpr uint32_t Ruler = 1 << 2;      // 3D 标尺
}

// --- 渲染参数结构体（Strategy 的唯一输入，不含 VTK 指针）---
struct RenderParams {
    std::array<double, 3>  cursor = { 0, 0, 0 }; // 轴约束后的联动点，VTK world 坐标 [x, y, z]
    std::array<double, 3>  cursorRaw = { 0, 0, 0 }; // 拾取原始点，VTK world 坐标 [x, y, z]
    int                    cursorAxis = -1; // 光标来源轴：0/1/2 为 X/Y/Z，-1 为自由点或无固定轴
    bool                   isFeatureActive = false; // true 时锁定 Quality producer 与 mapper；交互只影响刷新调度
    std::vector<TFNode>    tfNodes; // 传输函数节点；position 按 scalarRange 归一化映射
    double                 scalarRange[2] = { 0.0, 255.0 }; // 当前数据标量范围 [min, max]
    MaterialParams         material; // 当前材质快照；默认值来自 MaterialParams
    VolumeQualityParams    volumeQuality; // 当前 view 的 Volume producer/mapper 质量配置
    std::vector<GradientOpacityNode> gradientOpacity; // 空数组表示使用 VTK 默认梯度不透明度
    bool                   isDenoiseOn = false; // true 仅在 Volume 显示 producer 前启用降噪
    double                 isoValue = 0.0; // 等值面阈值，单位与 scalarRange 相同
    WindowLevelParams      windowLevel; // 切片灰度映射快照，单位与 scalarRange 相同
    // model-to-world 仿射矩阵，world = M * model；按 vtkMatrix4x4::DeepCopy 的
    // [m00, m01, ..., m03, m10, ..., m33] 顺序展开，默认单位矩阵。
    std::array<double, 16> modelMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    // VisFlags 位掩码，可按位组合；默认 Planes3D/Crosshair/Ruler 全部可见。
    uint32_t visibilityMask =
        VisFlags::Planes3D
        | VisFlags::Crosshair
        | VisFlags::Ruler; // 默认全部显示
};

// --- 切片朝向枚举 ---
// Top_down(0,0,1)  Front_back(0,1,0)  Left_right(1,0,0)
enum class Orientation { Top_down = 2, Front_back = 1, Left_right = 0 };

// --- 前处理配置快照（批量提交，减少锁争用和广播次数）---
struct PreInitConfig {
    VizMode             vizMode = VizMode::IsoSurface; // 无选择 flag；批量配置始终写入当前视图模式
    MaterialParams      material; // 无选择 flag；批量配置始终提交完整材质快照
    std::vector<TFNode> tfNodes; // hasTF=true 时提交；显式空数组非法
    double              isoThreshold = 0.0; // hasIso=true 时提交，单位与数据标量相同
    BackgroundColor     bgColor; // hasBgColor=true 时提交，RGB 分量范围 [0.0, 1.0]
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 }; // hasSpacing=true 时提交 RAS [sx, sy, sz]
    WindowLevelParams   windowLevel; // hasWindowLevel=true 时提交，单位与数据标量相同
    // false 表示忽略对应 payload 并保留现状；true 表示显式提交，包括 payload 的默认值。
    bool                hasTF = false;
    bool                hasIso = false;
    bool                hasBgColor = false;
    bool                hasSpacing = false;
    bool                hasWindowLevel = false;
};

// --- 窗口配置（用于批量建窗）---
struct WindowConfig {
    std::string     title; // 原样传给 VTK render window；空字符串设置空标题
    int             width = 600; // 窗口宽度，单位为屏幕像素；默认 600
    int             height = 600; // 窗口高度，单位为屏幕像素；默认 600
    int             posX = 0; // VTK SetPosition 使用的桌面 X 像素坐标；默认 0
    int             posY = 0; // VTK SetPosition 使用的桌面 Y 像素坐标；默认 0
    bool            isAxesVisible = false; // true 显示方向轴 widget；默认隐藏
    PreInitConfig   preInitCfg; // 建窗后一次性提交的视图与渲染配置
};
