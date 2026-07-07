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
#include <utility>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

using CropBoundsDouble6Array = std::array<double, 6>;
using CropVectorDouble3Array = std::array<double, 3>;
using CropMatrixDouble16Array = std::array<double, 16>; // 4x4 仿射变换矩阵，按 VTK DeepCopy 约定展开。
using CropIndexBoundsInt6Array = std::array<int, 6>;

// 返回 4x4 单位矩阵，作为 request/result 中仿射矩阵字段的默认值。
inline std::array<double, 16> GetIdentityMatrixArray()
{
    return {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
}

// 标准裁切盒固定为 [-1, 1]^3；所有请求只携带 boxToInputModelMatrix 作为几何真源。
inline std::array<double, 6> GetCanonicalCropBoxBounds()
{
    return { -1.0, 1.0, -1.0, 1.0, -1.0, 1.0 };
}

// 从 active input model 轴对齐 bounds 构造标准盒到 active input model 的矩阵。
inline std::array<double, 16> GetBoxMatrixFromBounds(const std::array<double, 6>& inputModelBounds)
{
    const double centerX = (inputModelBounds[0] + inputModelBounds[1]) * 0.5;
    const double centerY = (inputModelBounds[2] + inputModelBounds[3]) * 0.5;
    const double centerZ = (inputModelBounds[4] + inputModelBounds[5]) * 0.5;
    const double halfX = (inputModelBounds[1] - inputModelBounds[0]) * 0.5;
    const double halfY = (inputModelBounds[3] - inputModelBounds[2]) * 0.5;
    const double halfZ = (inputModelBounds[5] - inputModelBounds[4]) * 0.5;

    return {
        halfX, 0.0,   0.0,   centerX,
        0.0,   halfY, 0.0,   centerY,
        0.0,   0.0,   halfZ, centerZ,
        0.0,   0.0,   0.0,   1.0
    };
}

// 裁切盒内外的保留/移除语义。
enum class CropRemovalMode {
    // 保留裁切盒内部内容，移除外部内容。
    KeepInside,
    // 移除裁切盒内部内容，保留外部内容。
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
    // 当前唯一可执行的有向盒裁切。
    Box,
    // 预留平面裁切入口。
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
    // 提交裁切结果；当前只允许 ImageData + KeepInside。
    Submit
};

// 裁切交互过程的瞬时状态；主要用于区分“拖拽中”和“已释放”。
enum class CropInteractionPhase {
    // 当前没有任何交互发生。
    Idle,
    // 已进入可交互区域，但尚未开始真正拖拽。
    Hover,
    // 正在拖拽，适合只更新轻量 UI，不做重型 preview。
    Dragging,
    // 一次拖拽刚结束，适合触发正式 preview 或结果提交。
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
    // image submit 不支持“移除内部、保留外部”的执行方式。
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
class CropDataModel {
public:
    // 返回标准盒到 active input model 的变换矩阵；后端用它还原真实有向盒姿态。
    const CropMatrixDouble16Array& GetBoxMatrix() const { return m_boxToInputModelMatrix; }

    // 写入标准盒到 active input model 的变换矩阵；调用方必须把当前裁切姿态折叠到这一份矩阵里。
    void SetBoxMatrix(const CropMatrixDouble16Array& boxToInputModelMatrix) { m_boxToInputModelMatrix = boxToInputModelMatrix; }

    // 用 active input model 轴对齐 bounds 重建 boxToInputModelMatrix；
    // 适合默认全量盒或 image physical 结果回填，因为这些场景本身没有额外旋转姿态。
    void SetBoxBounds(const CropBoundsDouble6Array& inputModelBounds)
    {
        m_boxToInputModelMatrix = GetBoxMatrixFromBounds(inputModelBounds);
    }

    // 返回 active input model 坐标系下的平面法线；法线正侧定义为 Inside。
    const CropVectorDouble3Array& GetPlaneNormal() const { return m_planeNormalInInputModel; }

    // 写入 active input model 坐标系下的平面法线。
    void SetPlaneNormal(const CropVectorDouble3Array& planeNormalInInputModel)
    {
        m_planeNormalInInputModel = planeNormalInInputModel;
    }

    // 返回 active input model 坐标系下的平面中心。
    const CropVectorDouble3Array& GetPlaneCenter() const { return m_planeCenterInInputModel; }

    // 写入 active input model 坐标系下的平面中心。
    void SetPlaneCenter(const CropVectorDouble3Array& planeCenterInInputModel)
    {
        m_planeCenterInInputModel = planeCenterInInputModel;
    }

    // 返回平面 widget 可视区域半尺寸，布局为 [halfWidth, halfHeight]。
    // 该值用于保存交互尺度，不参与无限半空间裁切方程。
    const std::array<double, 2>& GetPlaneHalf() const { return m_planeHalf; }

    // 写入平面 widget 可视区域半尺寸，布局为 [halfWidth, halfHeight]。
    void SetPlaneHalf(const std::array<double, 2>& planeHalf)
    {
        m_planeHalf = planeHalf;
    }

    // 返回由 boxToInputModelMatrix 派生出的 active input model AABB。
    // image model 底层由 VTK physical-point API 表达；polyData input model 对应网格自身坐标。
    const CropBoundsDouble6Array& GetInputBounds() const { return m_inputModelBounds; }

    // 写入派生 active input model AABB；算法用它做 bounds 校验、index 吸附和粗范围裁剪。
    // 它不保留旋转姿态，因此不能替代 boxToInputModelMatrix。
    void SetInputBounds(const CropBoundsDouble6Array& inputModelBounds) { m_inputModelBounds = inputModelBounds; }

    // 根据当前 input model bounds 反推 active input model 中的中心点。
    std::array<double, 3> GetInputCenter() const
    {
        return {
            (m_inputModelBounds[0] + m_inputModelBounds[1]) * 0.5,
            (m_inputModelBounds[2] + m_inputModelBounds[3]) * 0.5,
            (m_inputModelBounds[4] + m_inputModelBounds[5]) * 0.5
        };
    }

    // 根据当前 input model bounds 反推 active input model 中的三轴尺寸。
    std::array<double, 3> GetInputSize() const
    {
        return {
            m_inputModelBounds[1] - m_inputModelBounds[0],
            m_inputModelBounds[3] - m_inputModelBounds[2],
            m_inputModelBounds[5] - m_inputModelBounds[4]
        };
    }

    // 返回当前裁切盒在 active input model 中的体积估计值；用于日志或粗粒度规模判断。
    double GetInputVolume() const
    {
        const auto inputModelDimensions = GetInputSize();
        return inputModelDimensions[0] * inputModelDimensions[1] * inputModelDimensions[2];
    }

private:
    // Box 几何真源：保存标准盒 [-1,1]^3 到 active input model 的完整 affine；
    // 旋转、缩放、平移都在这里，后端所有精确几何判断以它为准。
    std::array<double, 16> m_boxToInputModelMatrix = GetIdentityMatrixArray();
    // Plane 几何真源：active input model 坐标系下的平面法线、中心点和 widget 尺度快照。
    // 裁切方程只依赖 center + normal；halfExtents 不再表示可见裁切框，避免把无限半空间误读成有限矩形。
    CropVectorDouble3Array m_planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    CropVectorDouble3Array m_planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    std::array<double, 2> m_planeHalf = { 1.0, 1.0 };
    // 保存由 boxToInputModelMatrix 派生出的 active input model AABB；
    // 它用于快速排除、index 吸附和缓存键比较，不作为有向盒真源。
    std::array<double, 6> m_inputModelBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};

// 一次裁切执行请求：boxToInputModelMatrix 是标准盒 [-1,1]^3 到 active input model 的唯一几何真源。
class OrthogonalCropRequest {
public:
    // 返回标准裁切盒到 active input model 的矩阵。
    const CropMatrixDouble16Array& GetBoxMatrix() const { return m_boxToInputModelMatrix; }

    // 写入标准裁切盒到 active input model 的矩阵。
    void SetBoxMatrix(const CropMatrixDouble16Array& boxToInputModelMatrix) { m_boxToInputModelMatrix = boxToInputModelMatrix; }

    // 用 active input model 轴对齐 bounds 构造标准盒请求。
    void SetBoxBounds(const CropBoundsDouble6Array& inputModelBounds)
    {
        m_boxToInputModelMatrix = GetBoxMatrixFromBounds(inputModelBounds);
    }

    // 返回 active input model 坐标系下的平面法线；geometryType 为 Plane 时有效。
    const CropVectorDouble3Array& GetPlaneNormal() const { return m_planeNormalInInputModel; }

    // 写入 active input model 坐标系下的平面法线。
    void SetPlaneNormal(const CropVectorDouble3Array& planeNormalInInputModel)
    {
        m_planeNormalInInputModel = planeNormalInInputModel;
    }

    // 返回 active input model 坐标系下的平面中心；geometryType 为 Plane 时有效。
    const CropVectorDouble3Array& GetPlaneCenter() const { return m_planeCenterInInputModel; }

    // 写入 active input model 坐标系下的平面中心。
    void SetPlaneCenter(const CropVectorDouble3Array& planeCenterInInputModel)
    {
        m_planeCenterInInputModel = planeCenterInInputModel;
    }

    // 返回平面 widget 可视区域半尺寸，布局为 [halfWidth, halfHeight]。
    // Plane preview 不再把该值转换为 overlay 方框。
    const std::array<double, 2>& GetPlaneHalf() const { return m_planeHalf; }

    // 写入平面 widget 可视区域半尺寸，布局为 [halfWidth, halfHeight]。
    void SetPlaneHalf(const std::array<double, 2>& planeHalf)
    {
        m_planeHalf = planeHalf;
    }

    // 返回 bridge 决定好的目标数据源；router 只按它执行，不再自行推断。
    OrthogonalCropDataSource GetDataSource() const { return m_dataSource; }

    // 写入本次请求目标数据源。
    void SetDataSource(OrthogonalCropDataSource dataSource) { m_dataSource = dataSource; }

    // 返回 bridge 决定好的业务动作；router 结合 dataSource 选择具体数据处理方法。
    OrthogonalCropOperation GetOperation() const { return m_operation; }

    // 写入本次请求业务动作；Preview/Submit 不再伪装成数据后端。
    void SetOperation(OrthogonalCropOperation operation) { m_operation = operation; }

    // 返回本次请求的裁切几何类型。
    CropShape GetGeometryType() const { return m_geometryType; }

    // 写入本次请求的裁切几何类型。
    void SetGeometryType(CropShape geometryType) { m_geometryType = geometryType; }

    // 返回 inside / outside 的保留语义。
    CropRemovalMode GetRemovalMode() const { return m_removalMode; }

    // 设置 inside / outside 的保留语义。
    void SetRemovalMode(CropRemovalMode removalMode) { m_removalMode = removalMode; }

    // 返回调用方显式指定的可用内存上限。
    std::size_t GetRamBytes() const { return m_availableRamBytes; }

    // 写入调用方显式指定的可用内存上限；为 0 表示交给后端兜底判断。
    void SetRamBytes(std::size_t availableRamBytes) { m_availableRamBytes = availableRamBytes; }

private:
    // Box 几何真源：标准裁切盒 [-1,1]^3 到 active input model 的矩阵。
    std::array<double, 16> m_boxToInputModelMatrix = GetIdentityMatrixArray();
    // Plane 几何真源：active input model 坐标系下的平面法线、中心点和矩形半尺寸。
    // 法线指向正半空间；正半空间在平面裁切语义中视为 Inside。
    CropVectorDouble3Array m_planeNormalInInputModel = { 0.0, 0.0, 1.0 };
    CropVectorDouble3Array m_planeCenterInInputModel = { 0.0, 0.0, 0.0 };
    std::array<double, 2> m_planeHalf = { 1.0, 1.0 };
    // 本次请求的目标数据源。
    OrthogonalCropDataSource m_dataSource = OrthogonalCropDataSource::ImageData;
    // 本次请求的业务动作；None 表示还没有可执行目标，避免缺输入时伪装成 preview。
    OrthogonalCropOperation m_operation = OrthogonalCropOperation::None;
    // 本次请求的裁切几何类型；当前只有 Box 可执行。
    CropShape m_geometryType = CropShape::Box;
    // inside / outside 的保留语义；影响 image submit 合法性判断和提交 mask 取值。
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;
    // 可选的可用内存上限；为 0 表示交给后端使用系统 RAM 查询或默认兜底值。
    std::size_t m_availableRamBytes = 0;
};

class OrthogonalCropStatistics {
public:
    OrthogonalCropDataSource GetResolvedDataSource() const { return m_resolvedDataSource; }
    void SetResolvedDataSource(OrthogonalCropDataSource dataSource) { m_resolvedDataSource = dataSource; }

    OrthogonalCropOperation GetResolvedOperation() const { return m_resolvedOperation; }
    void SetResolvedOperation(OrthogonalCropOperation operation) { m_resolvedOperation = operation; }

    CropShape GetResolvedGeometryType() const { return m_resolvedGeometryType; }
    void SetResolvedGeometryType(CropShape geometryType) { m_resolvedGeometryType = geometryType; }

    CropRemovalMode GetResolvedRemovalMode() const { return m_resolvedRemovalMode; }
    void SetResolvedRemovalMode(CropRemovalMode removalMode) { m_resolvedRemovalMode = removalMode; }

    CropFailure GetFailureReason() const { return m_failureReason; }
    void SetFailureReason(CropFailure failureReason) { m_failureReason = failureReason; }

    const std::string& GetValidationMessage() const { return m_validationMessage; }

    void SetValidationMessage(const std::string& validationMessage) { m_validationMessage = validationMessage; }

private:
    // 这次诊断最终落到的数据源；用于日志和上层 UI 提示。
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::ImageData;
    // 这次诊断真正采用的业务动作，如 preview 或 submit。
    OrthogonalCropOperation m_resolvedOperation = OrthogonalCropOperation::None;
    // 这次诊断真正采用的裁切几何类型。
    CropShape m_resolvedGeometryType = CropShape::Box;
    // 这次诊断真正采用的保留语义。
    CropRemovalMode m_resolvedRemovalMode = CropRemovalMode::KeepInside;
    // 诊断阶段发现的失败原因；None 表示校验通过。
    CropFailure m_failureReason = CropFailure::None;
    // 给调用方看的统一校验/告警文本；失败时通常直接透传到 result 或 UI 提示。
    std::string m_validationMessage;
};

class OrthogonalCropResult {
public:
    OrthogonalCropDataSource GetResolvedDataSource() const { return m_resolvedDataSource; }
    void SetResolvedDataSource(OrthogonalCropDataSource dataSource) { m_resolvedDataSource = dataSource; }

    OrthogonalCropOperation GetResolvedOperation() const { return m_resolvedOperation; }
    void SetResolvedOperation(OrthogonalCropOperation operation) { m_resolvedOperation = operation; }

    CropShape GetResolvedGeometryType() const { return m_resolvedGeometryType; }
    void SetResolvedGeometryType(CropShape geometryType) { m_resolvedGeometryType = geometryType; }

    CropRemovalMode GetResolvedRemovalMode() const { return m_resolvedRemovalMode; }
    void SetResolvedRemovalMode(CropRemovalMode removalMode) { m_resolvedRemovalMode = removalMode; }

    bool GetSucceeded() const { return m_isSucceeded; }
    void SetSucceeded(bool isSucceeded) { m_isSucceeded = isSucceeded; }

    CropFailure GetFailureReason() const { return m_failureReason; }
    void SetFailureReason(CropFailure failureReason) { m_failureReason = failureReason; }

    const std::string& GetMessage() const { return m_message; }
    void SetMessage(const std::string& message) { m_message = message; }

    vtkSmartPointer<vtkImageData> GetSubmitImage() const { return m_submitImage; }
    void SetSubmitImage(vtkSmartPointer<vtkImageData> submitImage) { m_submitImage = std::move(submitImage); }

    vtkSmartPointer<vtkPolyData> GetClipPolyData() const { return m_clipPolyData; }
    void SetClipPolyData(vtkSmartPointer<vtkPolyData> clipPolyData) { m_clipPolyData = std::move(clipPolyData); }

    vtkSmartPointer<vtkImageData> GetMaskImage() const { return m_maskImage; }
    void SetMaskImage(vtkSmartPointer<vtkImageData> maskImage) { m_maskImage = std::move(maskImage); }

    vtkSmartPointer<vtkPolyData> GetOutlinePolyData() const { return m_outlinePolyData; }
    void SetOutlinePolyData(vtkSmartPointer<vtkPolyData> outlinePolyData) { m_outlinePolyData = std::move(outlinePolyData); }

    const CropDataModel& GetCropDataModel() const { return m_cropDataModel; }
    void SetCropDataModel(const CropDataModel& cropDataModel) { m_cropDataModel = cropDataModel; }

    const OrthogonalCropStatistics& GetStatistics() const { return m_statistics; }
    void SetStatistics(const OrthogonalCropStatistics& statistics) { m_statistics = statistics; }

private:
    // 最终执行实际落到的数据源；通常和 statistics 保持一致，但以结果本身为准。
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::ImageData;
    // 结果实际采用的业务动作；数据来源由 resolvedDataSource 表达。
    OrthogonalCropOperation m_resolvedOperation = OrthogonalCropOperation::None;
    // 结果实际采用的裁切几何类型。
    CropShape m_resolvedGeometryType = CropShape::Box;
    // 结果实际采用的保留语义。
    CropRemovalMode m_resolvedRemovalMode = CropRemovalMode::KeepInside;
    // 本次结果是否构造成功；false 不一定是崩溃，也可能是被策略性阻断。
    bool m_isSucceeded = false;
    // 最终失败原因；当 succeeded 为 false 时，上层应优先读取它和 message。
    CropFailure m_failureReason = CropFailure::None;
    // 对当前成功/失败状态的文字解释；可直接给日志或 UI 弹框使用。
    std::string m_message;
    // image submit 链路返回的主数据 image。
    vtkSmartPointer<vtkImageData> m_submitImage;
    // polydata preview 可选返回的裁切网格；render-only 主预览和 image 路径通常为空。
    vtkSmartPointer<vtkPolyData> m_clipPolyData;
    // image submit 链路生成的遮罩图；真正控制 inside/outside 语义。
    vtkSmartPointer<vtkImageData> m_maskImage;
    // box 3D outline preview 链路生成的裁切盒可视几何；常用于 overlay 或调试显示。
    vtkSmartPointer<vtkPolyData> m_outlinePolyData;
    // 这次结果对应的客观几何快照；image submit 后可能已经更新为输出 image 自身的 bounds。
    CropDataModel m_cropDataModel;
    // 与结果配套的诊断信息；避免调用方再次重复跑一遍诊断。
    OrthogonalCropStatistics m_statistics;
};
