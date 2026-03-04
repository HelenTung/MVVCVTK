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

#include <vector>
#include <array>
#include <string>
#include <functional>

// --- 可视化模式枚举 ---
enum class VizMode {
    Volume,
    IsoSurface,
    SliceAxial,
    SliceCoronal,
    SliceSagittal,
    CompositeVolume,        // 3D 体渲染 + 切片平面
    CompositeIsoSurface     // 3D 等值面 + 切片平面
};

// --- 交互工具枚举 ---
enum class ToolMode {
    Navigation,         // 默认漫游/切片浏览
    DistanceMeasure,    // 距离测量
    AngleMeasure,       // 角度测量
    ModelTransform      // 模型变换（旋转/缩放/平移）
};

// --- 数据加载状态枚举（替代裸 bool，区分"未加载/加载中/成功/失败"）---
enum class LoadState {
    Idle,       // 未发起加载
    Loading,    // 加载中（后台线程运行）
    Succeeded,  // 加载成功
    Failed      // 加载失败
};

// --- 传输函数节点 ---
struct TFNode {
    double position; // 0.0 - 1.0（归一化位置）
    double opacity;  // 0.0 - 1.0
    double r, g, b;  // 颜色
};

// --- 材质参数 ---
struct MaterialParams {
    double ambient = 0.1;
    double diffuse = 0.7;
    double specular = 0.2;
    double specularPower = 10.0;
    double opacity = 1.0;
    bool   shadeOn = false;
};

// --- 背景色（RGB，0~1）---
struct BackgroundColor {
    double r = 0.1, g = 0.1, b = 0.1;
};

// --- 更新类型位掩码（可组合）---
enum class UpdateFlags : int {
    None = 0,
    Cursor = 1 << 0,  // 位置改变        (0x01)
    TF = 1 << 1,  // 颜色/透明度改变 (0x02)
    IsoValue = 1 << 2,  // 阈值改变        (0x04)
    Material = 1 << 3,  // 材质参数改变    (0x08)
    Interaction = 1 << 4,  // 交互状态改变    (0x10)
    Transform = 1 << 5,  // 变换矩阵改变    (0x20)
    DataReady = 1 << 6,  // 数据加载成功    (0x40)
    LoadFailed = 1 << 7,  // 数据加载失败    (0x80)
    Background = 1 << 8,  // 背景色改变      (0x100)
	Visibility = 1 << 9,  // 可见性改变      (0x200)
    All = Cursor | TF | IsoValue | Material | Interaction | Transform
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
inline bool HasFlag(UpdateFlags flags, UpdateFlags bit) {
    return (static_cast<int>(flags) & static_cast<int>(bit)) != 0;
}

// 可视元素位定义
namespace VisFlags {
    constexpr uint32_t ClipPlanes = 1 << 0;
    constexpr uint32_t Crosshair = 1 << 1;
    constexpr uint32_t RulerAxes = 1 << 2;
}

// --- 渲染参数结构体（Strategy 的唯一输入，不含 VTK 指针）---
struct RenderParams {
    std::array<int, 3>     cursor = { 0, 0, 0 };
    std::vector<TFNode>    tfNodes;
    double                 scalarRange[2] = { 0.0, 255.0 };
    MaterialParams         material;
    double                 isoValue = 0.0;
    std::array<double, 16> modelMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    BackgroundColor        background;
    uint32_t visibilityMask = 
        VisFlags::ClipPlanes
        | VisFlags::Crosshair
        | VisFlags::RulerAxes; // 默认全部显示
};

// --- 切片朝向枚举 ---
// AXIAL(0,0,1)  CORONAL(0,1,0)  SAGITTAL(1,0,0)
enum class Orientation { AXIAL = 2, CORONAL = 1, SAGITTAL = 0 };

// --- 前处理配置快照（批量提交，减少锁争用和广播次数）---
struct PreInitConfig {
    VizMode             vizMode = VizMode::IsoSurface;
    MaterialParams      material;
    std::vector<TFNode> tfNodes;
    double              isoThreshold = 0.0;
    BackgroundColor     bgColor;         
    bool                hasTF = false;
    bool                hasIso = false;
    bool                hasBgColor = false; 
};

// --- 窗口配置（用于批量建窗）---
struct WindowConfig {
    std::string     title;
    int             width = 600;
    int             height = 600;
    int             posX = 0;
    int             posY = 0;
    VizMode         vizMode = VizMode::SliceAxial;
    bool            showAxes = false;
    PreInitConfig   preInitCfg;
};