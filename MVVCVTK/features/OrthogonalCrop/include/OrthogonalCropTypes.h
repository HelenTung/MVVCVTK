#pragma once
// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/OrthogonalCropTypes.h
// 分类: Math / Data Types
// OrthogonalCropTypes.h — 正交裁切独立插件纯数据结构
// =====================================================================
// 类型层只保存请求、结果、诊断、失败原因和数据快照；
// 它不依赖 VizService、Renderer、Interactor 或具体窗口对象，
// 让前端或 ViewModel 只需要组装 OrthogonalCropRequest 并消费 OrthogonalCropResult。
//
// 语义边界：
// - CropDataModel：客观几何快照；
// - OrthogonalCropRequest：一次执行请求，携带目标数据源、业务动作、裁切几何类型、几何矩阵和保留语义；
// - OrthogonalCropResult：一次执行结果，预览链返回盒体三维轮廓，图像提交链返回主数据图像和提交 mask。

#include <array>
#include <cstddef>
#include <string>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// active input model AABB，布局固定为 [minX, maxX, minY, maxY, minZ, maxZ]。
using CropBoundsDouble6Array = std::array<double, 6>;
// 三维点或向量，布局固定为 [x, y, z]；具体坐标系由字段名或接口注释限定。
using CropVectorDouble3Array = std::array<double, 3>;
using CropMatrixDouble16Array = std::array<double, 16>; // 4x4 仿射变换矩阵，按 VTK DeepCopy 约定展开。
// 闭区间 voxel index bounds，布局固定为 [minI, maxI, minJ, maxJ, minK, maxK]。
using CropIndexBoundsInt6Array = std::array<int, 6>;

// 裁切几何的 Inside 由几何轴决定：Box 是标准盒 [-1,1]^3 内部，
// Plane 是 active input model 中法线指向的正半空间。
enum class CropRemovalMode {
    // mapper preview 保留 Box 的 6 个朝内半空间交集，或 Plane 的法线正半空间。
    KeepInside,
    // shader/submit 移除上述 Inside，保留其余区域。
    RemoveInside
};

// Router 输入侧的数据来源选择。
enum class OrthogonalCropDataSource {
    // 强制优先走 vtkImageData 路径。
    ImageData,
    // 体渲染主预览路径；输入仍复用 vtkImageData，只用路由身份区分主预览目标。
    VolumeData,
    // 强制优先走 vtkPolyData 路径。
    PolyData
};

// 裁切几何类型；router 用它和数据源、动作一起决定可执行路径。
enum class CropShape {
    // 已接入的有向盒裁切。
    Box,
    // 已接入的无限半空间平面裁切。
    Plane,
    // 预留圆柱裁切入口。
    Cylinder
};

// 一次裁切请求的业务动作；数据处理方法由 dataSource 决定。
enum class OrthogonalCropOperation {
    // request 尚未指定可执行动作。
    None,
    // 生成 render-only 预览结果；当前只允许 VolumeData / PolyData。
    Preview,
    // 提交裁切结果；当前只允许 ImageData，KeepInside / RemoveInside 均可执行。
    Submit
};

// widget 生产、bridge 消费的瞬时交互轴；它与 preview 开关、submit reload 状态相互独立。
enum class CropInteractionPhase {
    // 当前没有任何交互发生。
    Idle,
    // 已进入可交互区域，但尚未开始真正拖拽。
    Hover,
    // 正在拖拽；bridge 记录最新 world 几何，但阻止重型 preview 和 submit。
    Dragging,
    // 一次拖拽刚结束；preview 意图开启时由 bridge 消费当前几何，submit 也重新允许。
    Released
};

// 统一归档请求/执行失败原因，便于 bridge 日志和上层 UI 提示复用同一套语义。
enum class CropFailure {
    // 没有失败，当前请求可以视作正常完成。
    None,
    // image 路径需要 vtkImageData，但当前没有绑定输入图像。
    NoImage,
    // polydata 路径需要 vtkPolyData，但当前没有绑定输入网格。
    NoPolyData,
    // 请求里的 bounds 自身就不合法，例如 min >= max。
    BadBounds,
    // bounds 虽合法，但超出了输入数据允许的范围。
    OutOfBounds,
    // 请求三元组没有可执行路径，或与当前算法输入不匹配。
    NoBackend,
    // image submit 的保留语义无法由当前后端执行。
    BadSubmitMode,
    // 预估或执行时发现内存不足，无法安全完成裁切。
    LowRam,
    // image submit 需要输出 2D mask 时，生成 mask 失败。
    MaskFailed,
    // image submit 需要输出主数据 image 时，生成输出 image 失败。
    ImageFailed,
    // polydata 预览需要可选裁切网格 artifact 时，生成输出 polydata 失败。
    ClipFailed
};

// 纯几何数据快照：保存裁切盒的稳定后端表达。
// boxToInputModelMatrix 是标准盒 [-1,1]^3 到 active input model 的唯一几何真源，
// inputModelBounds 只是从矩阵派生出的外接 AABB，便于校验、吸附和粗粒度执行。
struct CropDataModel {
    // Box 几何真源：保存标准盒 [-1,1]^3 到 active input model 的完整 affine；
    // 旋转、缩放、平移都在这里，后端所有精确几何判断以它为准。
    CropMatrixDouble16Array boxToInputModelMatrix = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    // Plane 几何真源：active input model 坐标系下的法线、中心点和 widget 尺度快照；
    // PlanarCropAlgorithm 会在构造结果数据前归一化 request 中的法线。
    // 裁切方程只依赖 center + normal；halfExtents 布局为 [halfWidth, halfHeight]，不限制无限半空间。
    CropVectorDouble3Array planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    CropVectorDouble3Array planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    std::array<double, 2> planeHalf = { 1.0, 1.0 };
    // shape 相关校验范围：Box 保存矩阵派生 AABB，Plane 保存 active input 完整 bounds；
    // 它只服务校验、index 吸附和粗范围判断，不作为精确几何真源。
    CropBoundsDouble6Array inputModelBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};

// 一次裁切执行请求：boxToInputModelMatrix 是标准盒 [-1,1]^3 到 active input model 的唯一几何真源。
struct OrthogonalCropRequest {
    // Box 几何真源：标准裁切盒 [-1,1]^3 到 active input model 的矩阵。
    CropMatrixDouble16Array boxToInputModelMatrix = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    // Plane 几何真源：active input model 坐标系下的法线、中心点和 widget 可视半尺寸。
    // half 布局为 [halfWidth, halfHeight]；法线正半空间为 Inside，half 不参与无限平面方程。
    CropVectorDouble3Array planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    CropVectorDouble3Array planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    std::array<double, 2> planeHalf = { 1.0, 1.0 };
    // 本次请求的目标数据源。
    OrthogonalCropDataSource dataSource = OrthogonalCropDataSource::ImageData;
    // 本次请求的业务动作；None 表示还没有可执行目标，避免缺输入时伪装成 preview。
    OrthogonalCropOperation operation = OrthogonalCropOperation::None;
    // 本次请求的裁切几何类型；Box / Plane 已接入，Cylinder 仍为预留。
    CropShape geometryType = CropShape::Box;
    // inside / outside 的保留语义；影响 image submit 合法性判断和提交 mask 取值。
    CropRemovalMode removalMode = CropRemovalMode::KeepInside;
    // 可选的可用内存上限；为 0 表示交给后端使用系统 RAM 查询或默认兜底值。
    std::size_t availableRamBytes = 0;
};
struct OrthogonalCropResult {
    // 算法实际解析的四个请求轴；这些身份字段不能用于推断某类 payload 必然非空。
    OrthogonalCropDataSource resolvedDataSource = OrthogonalCropDataSource::ImageData;
    OrthogonalCropOperation resolvedOperation = OrthogonalCropOperation::None;
    CropShape resolvedGeometryType = CropShape::Box;
    CropRemovalMode resolvedRemovalMode = CropRemovalMode::KeepInside;
    // 本次结果 payload 是否可消费；false 也覆盖路由拒绝、输入校验或资源不足等正常失败边界。
    bool isSucceeded = false;
    // 结构化失败边界；成功结果通常保持 None。
    CropFailure failureReason = CropFailure::None;
    // 面向日志/UI 的补充诊断文本，不参与路由或成功判定。
    std::string message;
    // image submit 的引用计数 payload；保留 voxel 复制源值，移除 voxel 写入输入标量最小值。
    vtkSmartPointer<vtkImageData> submitImage;
    // polydata preview 可选的引用计数 artifact；render-only mapper 预览和 image 路径可为空。
    vtkSmartPointer<vtkPolyData> clipPolyData;
    // 与 submit image 对齐的单分量 unsigned-char mask：255 表示保留，0 表示移除。
    vtkSmartPointer<vtkImageData> maskImage;
    // active input model 坐标中的 Box 线框 artifact；Plane 的无限半空间结果不生成该有限轮廓。
    vtkSmartPointer<vtkPolyData> outlinePolyData;
    // 结果对应的客观几何快照；Box image submit 可回填输出 bounds，Plane 保留输入 bounds 与平面真源。
    CropDataModel cropDataModel;
};
