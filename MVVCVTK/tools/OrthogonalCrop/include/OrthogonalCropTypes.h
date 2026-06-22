#pragma once
// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/OrthogonalCropTypes.h
// 分类: Math / Data Types
// OrthogonalCropTypes.h — 正交裁切独立插件纯数据结构
// =====================================================================
// 类型层只保存请求、结果、诊断、失败原因和数据/状态快照；
// 它不依赖 MedicalVizService、Renderer、Interactor 或具体窗口对象，
// 让前端或 ViewModel 只需要组装 OrthogonalCropRequest 并消费 OrthogonalCropResult。
//
// 语义边界：
// - CropDataModel：客观几何快照；
// - CropStateModel：瞬态显示与交互状态快照；
// - OrthogonalCropRequest：一次执行请求，携带目标数据源、目标后端、几何、保留语义和交互状态快照；
// - OrthogonalCropResult：一次执行结果，预览链返回 image 2D mask / box 3D outline / polydata 3D clip，
//   image submit 链返回可重新载入主数据的 image。

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
inline std::array<double, 16> GetBoxToInputModelMatrixFromBounds(const std::array<double, 6>& inputModelBounds)
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
    // 自动选择：按当前可用输入在 image/polydata 之间回退。
    Auto,
    // 强制优先走 vtkImageData 路径。
    ImageData,
    // 强制优先走 vtkPolyData 路径。
    PolyData
};

// 一次裁切请求或结果采用的具体后端；便于 bridge 明确目标、result 回填来源。
enum class OrthogonalCropBackend {
    // request 尚未指定目标后端，或 result 尚未成功解析出实际后端。
    None,
    // image 路径生成 2D slice mask preview，同时提供 3D outline 所需 cropData。
    MaskPreview,
    // image 路径通过提取 VOI 生成 submit 后的新主子体数据。
    SubmitExtractVOI,
    // polydata 路径使用 3D box clip + geometry filter 得到 preview 网格。
    ClipPreview
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
enum class OrthogonalCropFailureReason {
    // 没有失败，当前请求可以视作正常完成。
    None,
    // image 路径需要 vtkImageData，但当前没有绑定输入图像。
    InputImageMissing,
    // polydata 路径需要 vtkPolyData，但当前没有绑定输入网格。
    InputPolyDataMissing,
    // 请求里的 bounds 自身就不合法，例如 min >= max。
    InvalidBounds,
    // bounds 虽合法，但超出了输入数据允许的范围。
    BoundsOutOfRange,
    // 请求后端与当前算法入口不匹配，例如 image 算法收到 polydata clip 后端。
    UnsupportedBackend,
    // image submit 不支持“移除内部、保留外部”的执行方式。
    SubmitRemoveInsideUnsupported,
    // 预估或执行时发现内存不足，无法安全完成裁切。
    InsufficientRam,
    // image 2D mask preview 产物构建失败。
    MaskPreviewCreationFailed,
    // image submit 需要输出主数据 image 时，生成输出 image 失败。
    SubmitImageCreationFailed,
    // polydata 3D clip preview 需要输出裁切 polydata 时，生成输出 polydata 失败。
    ClipPreviewPolyDataCreationFailed
};

// 纯几何数据快照：保存裁切盒的稳定后端表达。
// boxToInputModelMatrix 是标准盒 [-1,1]^3 到 active input model 的唯一几何真源，
// inputModelBounds 只是从矩阵派生出的外接 AABB，便于校验、吸附和粗粒度执行。
class CropDataModel {
public:
    // 返回标准盒到 active input model 的变换矩阵；后端用它还原真实有向盒姿态。
    const CropMatrixDouble16Array& GetBoxToInputModelMatrix() const { return m_boxToInputModelMatrix; }

    // 写入标准盒到 active input model 的变换矩阵；调用方必须把当前裁切姿态折叠到这一份矩阵里。
    void SetBoxToInputModelMatrix(const CropMatrixDouble16Array& boxToInputModelMatrix) { m_boxToInputModelMatrix = boxToInputModelMatrix; }

    // 用 active input model 轴对齐 bounds 重建 boxToInputModelMatrix；
    // 适合默认全量盒或 image physical 结果回填，因为这些场景本身没有额外旋转姿态。
    void SetBoxToInputModelMatrixFromBounds(const CropBoundsDouble6Array& inputModelBounds)
    {
        m_boxToInputModelMatrix = GetBoxToInputModelMatrixFromBounds(inputModelBounds);
    }

    // 返回由 boxToInputModelMatrix 派生出的 active input model AABB。
    // image model 底层由 VTK physical-point API 表达；polyData input model 对应网格自身坐标。
    const CropBoundsDouble6Array& GetInputModelBounds() const { return m_inputModelBounds; }

    // 写入派生 active input model AABB；算法用它做 bounds 校验、index 吸附和粗范围裁剪。
    // 它不保留旋转姿态，因此不能替代 boxToInputModelMatrix。
    void SetInputModelBounds(const CropBoundsDouble6Array& inputModelBounds) { m_inputModelBounds = inputModelBounds; }

    // 根据当前 input model bounds 反推 active input model 中的中心点。
    std::array<double, 3> GetInputModelCenter() const
    {
        return {
            (m_inputModelBounds[0] + m_inputModelBounds[1]) * 0.5,
            (m_inputModelBounds[2] + m_inputModelBounds[3]) * 0.5,
            (m_inputModelBounds[4] + m_inputModelBounds[5]) * 0.5
        };
    }

    // 根据当前 input model bounds 反推 active input model 中的三轴尺寸。
    std::array<double, 3> GetInputModelDimensions() const
    {
        return {
            m_inputModelBounds[1] - m_inputModelBounds[0],
            m_inputModelBounds[3] - m_inputModelBounds[2],
            m_inputModelBounds[5] - m_inputModelBounds[4]
        };
    }

    // 返回当前裁切盒在 active input model 中的体积估计值；用于日志或粗粒度规模判断。
    double GetInputModelVolume() const
    {
        const auto inputModelDimensions = GetInputModelDimensions();
        return inputModelDimensions[0] * inputModelDimensions[1] * inputModelDimensions[2];
    }

private:
    // 保存标准盒 [-1,1]^3 到 active input model 的完整 affine；
    // 旋转、缩放、平移都在这里，后端所有精确几何判断以它为准。
    std::array<double, 16> m_boxToInputModelMatrix = GetIdentityMatrixArray();
    // 保存由 boxToInputModelMatrix 派生出的 active input model AABB；
    // 它用于快速排除、index 吸附和缓存键比较，不作为有向盒真源。
    std::array<double, 6> m_inputModelBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};

// UI/交互层状态快照：描述当前裁切是否启用、手柄状态和显示语义。
class CropStateModel {
public:
    // 当前结果或请求是否仍然处于“启用裁切语义”的状态。
    bool GetCropEnabled() const { return m_cropEnabled; }

    // 设置裁切语义是否生效；submit 产出结果时常会把它关掉。
    void SetCropEnabled(bool enabled) { m_cropEnabled = enabled; }

    // 返回 inside 区域当前期望的显示透明度。
    double GetInsideOpacity() const { return m_insideOpacity; }

    // 设置 inside 区域的透明度；主要服务 preview/overlay 表现。
    void SetInsideOpacity(double opacity) { m_insideOpacity = opacity; }

    // 返回 outside 区域当前是否应继续显示。
    bool GetOutsideVisibility() const { return m_outsideVisibility; }

    // 设置 outside 区域是否可见；上层可据此决定是否遮罩或淡出外围区域。
    void SetOutsideVisibility(bool visible) { m_outsideVisibility = visible; }

    // 返回裁切相关的高亮/LUT 颜色。
    const CropVectorDouble3Array& GetLutColor() const { return m_lutColor; }

    // 设置裁切相关的高亮/LUT 颜色。
    void SetLutColor(const CropVectorDouble3Array& color) { m_lutColor = color; }

    // 返回当前交互阶段，如 Idle、Dragging、Released。
    CropInteractionPhase GetInteractionPhase() const { return m_interactionPhase; }

    // 更新当前交互阶段；bridge 会把它打进 request/result，供上层理解结果产生时机。
    void SetInteractionPhase(CropInteractionPhase phase) { m_interactionPhase = phase; }

private:
    // 当前是否启用了裁切语义；上层可据此决定是否显示/应用裁切效果。
    bool m_cropEnabled = true;
    // inside 区域的可视透明度，常用于 2D/3D overlay 预览。
    double m_insideOpacity = 0.35;
    // outside 区域当前是否可见。
    bool m_outsideVisibility = false;
    // 裁切相关 LUT/高亮颜色。
    std::array<double, 3> m_lutColor = { 1.0, 0.4, 0.2 };
    // 当前交互阶段，供上层区分 dragging/released。
    CropInteractionPhase m_interactionPhase = CropInteractionPhase::Idle;
};

// 一次裁切执行请求：boxToInputModelMatrix 是标准盒 [-1,1]^3 到 active input model 的唯一几何真源。
class OrthogonalCropRequest {
public:
    // 返回标准裁切盒到 active input model 的矩阵。
    const CropMatrixDouble16Array& GetBoxToInputModelMatrix() const { return m_boxToInputModelMatrix; }

    // 写入标准裁切盒到 active input model 的矩阵。
    void SetBoxToInputModelMatrix(const CropMatrixDouble16Array& boxToInputModelMatrix) { m_boxToInputModelMatrix = boxToInputModelMatrix; }

    // 用 active input model 轴对齐 bounds 构造标准盒请求。
    void SetBoxToInputModelMatrixFromBounds(const CropBoundsDouble6Array& inputModelBounds)
    {
        m_boxToInputModelMatrix = GetBoxToInputModelMatrixFromBounds(inputModelBounds);
    }

    // 返回 bridge 决定好的目标数据源；router 只按它执行，不再自行推断。
    OrthogonalCropDataSource GetDataSource() const { return m_dataSource; }

    // 写入本次请求目标数据源；Auto 只允许出现在默认请求或输入缺失场景。
    void SetDataSource(OrthogonalCropDataSource dataSource) { m_dataSource = dataSource; }

    // 返回 bridge 决定好的目标后端；它直接说明本次请求要生成哪类产物。
    OrthogonalCropBackend GetBackend() const { return m_backend; }

    // 写入本次请求目标后端；router 使用它选择 image mask、polydata clip 或 image submit。
    void SetBackend(OrthogonalCropBackend backend) { m_backend = backend; }

    // 返回 inside / outside 的保留语义。
    CropRemovalMode GetRemovalMode() const { return m_removalMode; }

    // 设置 inside / outside 的保留语义。
    void SetRemovalMode(CropRemovalMode removalMode) { m_removalMode = removalMode; }

    // 返回随请求一起下发的 UI/交互状态快照。
    const CropStateModel& GetCropStateModel() const { return m_cropStateModel; }

    // 写入随请求一起下发的 UI/交互状态快照。
    void SetCropStateModel(const CropStateModel& cropStateModel) { m_cropStateModel = cropStateModel; }

    // 返回调用方显式指定的可用内存上限。
    std::size_t GetAvailableRamBytes() const { return m_availableRamBytes; }

    // 写入调用方显式指定的可用内存上限；为 0 表示交给后端兜底判断。
    void SetAvailableRamBytes(std::size_t availableRamBytes) { m_availableRamBytes = availableRamBytes; }

private:
    // 标准裁切盒 [-1,1]^3 到 active input model 的矩阵。
    std::array<double, 16> m_boxToInputModelMatrix = GetIdentityMatrixArray();
    // 本次请求的目标数据源；bridge 在调用 router 前把 Auto 解析成具体输入。
    OrthogonalCropDataSource m_dataSource = OrthogonalCropDataSource::Auto;
    // 本次请求的目标后端；None 表示还没有可执行目标，避免缺输入时伪装成 preview。
    OrthogonalCropBackend m_backend = OrthogonalCropBackend::None;
    // inside / outside 的保留语义；影响 image 2D mask preview 取值和 image submit 合法性判断。
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;
    // UI/交互层随本次请求一并带下去的状态快照，如是否启用、当前 phase、透明度等。
    CropStateModel m_cropStateModel;
    // 可选的可用内存上限；为 0 表示交给后端使用系统 RAM 查询或默认兜底值。
    std::size_t m_availableRamBytes = 0;
};

class OrthogonalCropStatistics {
public:
    // 返回诊断信息最终落到的数据源。
    OrthogonalCropDataSource GetResolvedDataSource() const { return m_resolvedDataSource; }

    // 写入诊断信息最终落到的数据源。
    void SetResolvedDataSource(OrthogonalCropDataSource dataSource) { m_resolvedDataSource = dataSource; }

    // 返回诊断信息采用的具体后端实现。
    OrthogonalCropBackend GetResolvedBackend() const { return m_resolvedBackend; }

    // 写入诊断信息采用的具体后端实现。
    void SetResolvedBackend(OrthogonalCropBackend backend) { m_resolvedBackend = backend; }

    // 返回诊断阶段发现的失败原因。
    OrthogonalCropFailureReason GetFailureReason() const { return m_failureReason; }

    // 写入诊断阶段发现的失败原因。
    void SetFailureReason(OrthogonalCropFailureReason failureReason) { m_failureReason = failureReason; }

    // 返回统一的校验/告警文本。
    const std::string& GetValidationMessage() const { return m_validationMessage; }

    // GetValidationMessage 的别名，便于调用方统一按 message 读取。
    const std::string& GetMessage() const
    {
        return m_validationMessage;
    }

    // 写入统一的校验/告警文本。
    void SetValidationMessage(const std::string& validationMessage) { m_validationMessage = validationMessage; }

    // SetValidationMessage 的别名，便于与 Result 的 message 语义保持一致。
    void SetMessage(const std::string& message)
    {
        m_validationMessage = message;
    }

private:
    // 这次诊断最终落到的数据源；用于日志和上层 UI 提示。
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::Auto;
    // 这次诊断真正采用的底层实现路径，如 2D mask preview、extract VOI 或 polydata clip。
    OrthogonalCropBackend m_resolvedBackend = OrthogonalCropBackend::None;
    // 诊断阶段发现的失败原因；None 表示校验通过。
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    // 给调用方看的统一校验/告警文本；失败时通常直接透传到 result 或 UI 提示。
    std::string m_validationMessage;
};

class OrthogonalCropResult {
public:
    // 返回结果最终落到的数据源。
    OrthogonalCropDataSource GetResolvedDataSource() const { return m_resolvedDataSource; }

    // 写入结果最终落到的数据源。
    void SetResolvedDataSource(OrthogonalCropDataSource dataSource) { m_resolvedDataSource = dataSource; }

    // 返回结果采用的具体后端实现。
    OrthogonalCropBackend GetResolvedBackend() const { return m_resolvedBackend; }

    // 写入结果采用的具体后端实现。
    void SetResolvedBackend(OrthogonalCropBackend backend) { m_resolvedBackend = backend; }

    // 返回本次执行是否构造出了有效结果。
    bool GetSucceeded() const { return m_succeeded; }

    // 写入本次执行是否成功。
    void SetSucceeded(bool succeeded) { m_succeeded = succeeded; }

    // 返回本次执行的失败原因。
    OrthogonalCropFailureReason GetFailureReason() const { return m_failureReason; }

    // 写入本次执行的失败原因。
    void SetFailureReason(OrthogonalCropFailureReason failureReason) { m_failureReason = failureReason; }

    // 返回本次执行的说明文本。
    const std::string& GetMessage() const { return m_message; }

    // 写入本次执行的说明文本。
    void SetMessage(const std::string& message) { m_message = message; }

    // 返回 image submit 链路产出的主数据 image。
    vtkSmartPointer<vtkImageData> GetSubmitImage() const { return m_submitImage; }

    // 写入 image submit 链路产出的主数据 image。
    void SetSubmitImage(vtkSmartPointer<vtkImageData> submitImage) { m_submitImage = std::move(submitImage); }

    // 返回 polydata 3D clip preview 链路产出的裁切网格。
    vtkSmartPointer<vtkPolyData> GetClipPolyData() const { return m_clipPolyData; }

    // 写入 polydata 3D clip preview 链路产出的裁切网格。
    void SetClipPolyData(vtkSmartPointer<vtkPolyData> clipPolyData) { m_clipPolyData = std::move(clipPolyData); }

    // 返回 image 2D mask preview 链路生成的遮罩图像。
    vtkSmartPointer<vtkImageData> GetMaskImage() const { return m_maskImage; }

    // 写入 image 2D mask preview 链路生成的遮罩图像。
    void SetMaskImage(vtkSmartPointer<vtkImageData> maskImage) { m_maskImage = std::move(maskImage); }

    // 返回 box 3D outline preview 链路生成的裁切盒轮廓几何。
    vtkSmartPointer<vtkPolyData> GetOutlinePolyData() const { return m_outlinePolyData; }

    // 写入 box 3D outline preview 链路生成的裁切盒轮廓几何。
    void SetOutlinePolyData(vtkSmartPointer<vtkPolyData> outlinePolyData) { m_outlinePolyData = std::move(outlinePolyData); }

    // 返回这次执行对应的客观几何快照。
    const CropDataModel& GetCropDataModel() const { return m_cropDataModel; }

    // 写入这次执行对应的客观几何快照。
    void SetCropDataModel(const CropDataModel& cropDataModel) { m_cropDataModel = cropDataModel; }

    // 返回这次执行对应的交互/显示状态快照。
    const CropStateModel& GetCropStateModel() const { return m_cropStateModel; }

    // 写入这次执行对应的交互/显示状态快照。
    void SetCropStateModel(const CropStateModel& cropStateModel) { m_cropStateModel = cropStateModel; }

    // 返回与本次结果配套的诊断信息。
    const OrthogonalCropStatistics& GetStatistics() const { return m_statistics; }

    // 写入与本次结果配套的诊断信息。
    void SetStatistics(const OrthogonalCropStatistics& statistics) { m_statistics = statistics; }

private:
    // 最终执行实际落到的数据源；通常和 statistics 保持一致，但以结果本身为准。
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::Auto;
    // 结果实际采用的后端实现；决定调用方该如何理解 image/polydata/2D/3D 产物来源。
    OrthogonalCropBackend m_resolvedBackend = OrthogonalCropBackend::None;
    // 本次结果是否构造成功；false 不一定是崩溃，也可能是被策略性阻断。
    bool m_succeeded = false;
    // 最终失败原因；当 succeeded 为 false 时，上层应优先读取它和 message。
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    // 对当前成功/失败状态的文字解释；可直接给日志或 UI 弹框使用。
    std::string m_message;
    // image submit 链路返回的主数据 image。
    vtkSmartPointer<vtkImageData> m_submitImage;
    // polydata 3D clip preview 链路返回的裁切网格；image 路径通常为空。
    vtkSmartPointer<vtkPolyData> m_clipPolyData;
    // image 2D mask preview 链路生成的遮罩图；真正控制 inside/outside 语义。
    vtkSmartPointer<vtkImageData> m_maskImage;
    // box 3D outline preview 链路生成的裁切盒可视几何；常用于 overlay 或调试显示。
    vtkSmartPointer<vtkPolyData> m_outlinePolyData;
    // 这次结果对应的客观几何快照；image submit 后可能已经更新为输出 image 自身的 bounds。
    CropDataModel m_cropDataModel;
    // 这次结果对应的交互/显示状态快照；便于上层知道结果产生时是否仍处于交互态。
    CropStateModel m_cropStateModel;
    // 与结果配套的诊断信息；避免调用方再次重复跑一遍诊断。
    OrthogonalCropStatistics m_statistics;
};
