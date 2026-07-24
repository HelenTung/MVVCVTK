#pragma once
// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/OrthogonalCropTypes.h
// 分类: Math / Data Types
// OrthogonalCropTypes.h — 正交裁切独立插件纯数据结构
// =====================================================================
// 类型层只保存公式节点、输入快照、shader transaction 与按需导出结果；
// 不依赖 VizService、Renderer、Interactor、mapper 或具体窗口对象。

#include "Render/RenderEffect.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

struct CropPredicateTable;

struct CropShaderPayload final {
    std::uint64_t revision = 0;
    RenderInputStamp sourceStamp;
    std::size_t nodeCount = 0;
    std::shared_ptr<const CropPredicateTable> predicateTable;
};

// active input model AABB，布局固定为 [minX, maxX, minY, maxY, minZ, maxZ]。
using CropBoundsDouble6Array = std::array<double, 6>;
// 三维点或向量，布局固定为 [x, y, z]；具体坐标系由字段名或接口注释限定。
using CropVectorDouble3Array = std::array<double, 3>;
using CropMatrixDouble16Array = std::array<double, 16>; // 4x4 仿射变换矩阵，按 VTK DeepCopy 约定展开。
using CropPointFloat3Array = std::array<float, 3>;
// 闭区间 voxel index bounds，布局固定为 [minI, maxI, minJ, maxJ, minK, maxK]。
using CropIndexBoundsInt6Array = std::array<int, 6>;

// 裁切几何的 Inside 由几何轴决定：Box 是标准盒 [-1,1]^3 内部，
// Plane 是 active input model 中法线指向的正半空间。
enum class CropRemovalMode {
    // 当前 widget 只允许定位，不生成或改写裁切历史。
    None,
    // mapper shader 保留 Box 的 6 个朝内半空间交集，或 Plane 的法线正半空间。
    KeepInside,
    // shader/export 移除上述 Inside，保留其余区域。
    RemoveInside
};

struct CropHistoryState final {
    std::size_t nodeCount = 0;
    std::size_t operationCount = 0;
    CropRemovalMode editMode = CropRemovalMode::None;
    bool hasEditableOp = false;
    bool isEditing = false;
    // allHistory 中已经物化进当前 image+mask 基线的绝对节点数。
    std::size_t baseNodeCount = 0;
    // 从原始根数据开始的完整参数历史；包含当前 active history。
    std::size_t allOperationCount = 0;
};

// Router 输入侧的数据来源选择。
enum class OrthogonalCropDataSource {
    // 强制优先走 vtkImageData 路径。
    ImageData,
    // 体渲染主路径；输入仍复用 vtkImageData，只用路由身份区分主目标。
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

// widget 生产、bridge 消费的瞬时交互轴；它与 shader revision、异步导出状态相互独立。
enum class CropInteractionPhase {
    // 当前没有任何交互发生。
    Idle,
    // 已进入可交互区域，但尚未开始真正拖拽。
    Hover,
    // 正在拖拽；bridge 记录最新 world 几何，但阻止 shader 提交和导出。
    Dragging,
    // 一次拖拽刚结束；bridge 消费当前几何，导出也重新允许。
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
    // image export 的保留语义无法由当前后端执行。
    BadExportMode,
    // 预估或执行时发现内存不足，无法安全完成裁切。
    LowRam,
    // image export 需要输出与输入体数据对齐的三维 mask 时，生成 mask 失败。
    MaskFailed,
    // image export 需要输出主数据 image 时，生成输出 image 失败。
    ImageFailed,
    // Box 几何需要生成 outlinePolyData 时，轮廓 artifact 生成失败。
    ClipFailed,
    // 公式节点、输入快照或导出参数不满足稳定契约。
    BadInput,
    // 有效前缀计算后没有任何点被保留。
    EmptyResult,
    // 同一 Bridge 已有一个导出任务执行中。
    Busy,
    // 导出请求与捕获输入的版本或数据源不一致。
    VersionMismatch,
    // packaged_task 已创建，但 joinable worker 未能启动。
    WorkerStartFailed,
    // worker 已启动，但任务以异常终止。
    WorkerFailed
};

// history 的唯一节点类型；只保存可序列化数学参数，不保存 VTK 对象或 GPU 资源。
struct CropOpItem final {
    std::uint64_t operationIndex = 0;
    CropShape geometryType = CropShape::Box;
    CropRemovalMode removalMode = CropRemovalMode::KeepInside;
    CropMatrixDouble16Array boxToInputModelMatrix = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
    CropVectorDouble3Array planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    CropVectorDouble3Array planeNormalInInputModel = { 0.0, 0.0, 1.0 };
};

// Host 在 owner thread 捕获的不可拆分快照；source/version/pointer/bounds 必须一起换代。
struct CropInputSnapshot final {
    OrthogonalCropDataSource dataSource = OrthogonalCropDataSource::ImageData;
    std::uint64_t inputVersion = 0;
    CropBoundsDouble6Array inputModelBounds = {};
    vtkSmartPointer<vtkImageData> imageData;
    vtkSmartPointer<vtkImageData> validityMask;
    vtkSmartPointer<vtkPolyData> polyData;
};

struct CropExportRequest final {
    OrthogonalCropDataSource dataSource = OrthogonalCropDataSource::ImageData;
    std::vector<CropOpItem> operations;
    std::size_t nodeCount = 0;
    std::uint64_t inputVersion = 0;
    std::size_t availableRamBytes = 0;
};

struct CropExportResult final {
    OrthogonalCropDataSource resolvedDataSource = OrthogonalCropDataSource::ImageData;
    bool isSucceeded = false;
    CropFailure failureReason = CropFailure::None;
    std::uint64_t failureOperationIndex = 0;
    std::vector<CropOpItem> operations;
    std::uint64_t inputVersion = 0;
    std::size_t nodeCount = 0;
    std::string message;
    vtkSmartPointer<vtkImageData> imageData;
    vtkSmartPointer<vtkImageData> maskImage;
    vtkSmartPointer<vtkPolyData> polyData;
};
