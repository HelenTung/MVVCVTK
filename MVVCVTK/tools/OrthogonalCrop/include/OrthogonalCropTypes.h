#pragma once
// =====================================================================
// Path: MVVCVTK/tools/OrthogonalCrop/include/OrthogonalCropTypes.h
// 分类: Math / Data Types
// OrthogonalCropTypes.h — 正交裁切独立插件纯数据结构
// =====================================================================
// 设计约束：
// - 这里只保留请求、结果、统计、失败原因以及数据/状态快照；
// - 不依赖 MedicalVizService、Renderer、Interactor 或具体窗口对象；
// - 前端或 ViewModel 只需组装 OrthogonalCropRequest，再消费 OrthogonalCropResult。
//
// 语义边界：
// - CropDataModel：客观几何与绝对坐标补偿信息；
// - CropStateModel：瞬态显示与交互状态快照；
// - OrthogonalCropRequest：一次执行请求；
// - OrthogonalCropResult：一次执行结果，预览产物链返回 2D mask / 3D outline，主数据物理提交链返回 derived image。

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

// 返回 4x4 单位矩阵，作为 request/result 中各种坐标补偿矩阵的默认值。
inline std::array<double, 16> GetIdentityMatrixArray()
{
    return {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
}

// 标准裁切盒固定为 [-1, 1]^3；所有请求只携带 boxToModelMatrix 作为几何真源。
inline std::array<double, 6> GetCanonicalCropBoxBounds()
{
    return { -1.0, 1.0, -1.0, 1.0, -1.0, 1.0 };
}

// 从 model 轴对齐 bounds 构造标准盒到 model 的矩阵。
inline std::array<double, 16> GetBoxToModelMatrixFromBounds(const std::array<double, 6>& modelBounds)
{
    const double centerX = (modelBounds[0] + modelBounds[1]) * 0.5;
    const double centerY = (modelBounds[2] + modelBounds[3]) * 0.5;
    const double centerZ = (modelBounds[4] + modelBounds[5]) * 0.5;
    const double halfX = (modelBounds[1] - modelBounds[0]) * 0.5;
    const double halfY = (modelBounds[3] - modelBounds[2]) * 0.5;
    const double halfZ = (modelBounds[5] - modelBounds[4]) * 0.5;

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

// 本次请求到底只做显示预览产物，还是要产出真正可复用的主数据裁切结果。
enum class CropExecutionMode {
    // 预览产物裁切：生成 2D mask / 3D outline / 3D clip 等显示产物，不替换主数据。
    VirtualCrop,
    // 主数据 physical submit：要求得到可脱离原输入单独使用的输出数据。
    PhysicalCrop
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

// 本次执行最终落到的具体后端实现；便于日志、调试和 UI 展示。
enum class OrthogonalCropResolvedBackend {
    // 还没有成功解析出实际 backend，通常表示输入缺失或执行失败。
    None,
    // image 路径生成 2D slice mask preview，同时提供 3D outline 所需 cropData。
    ImageVirtualMask,
    // image 路径通过提取 VOI 生成 physical submit 后的新主子体数据。
    ImageExtractVOI,
    // image volume mapper 原生 3D cropping preview 方案。
    ImageMapperCropping,
    // polydata 路径使用 3D box clip + geometry filter 得到 preview 网格。
    PolyDataClipDataSet
};

// 交互层当前选中的裁切手柄语义。
enum class CropHandleId {
    // 当前没有任何有效手柄被激活。
    None,
    // model X 轴负方向平面手柄。
    MinModelX,
    // model X 轴正方向平面手柄。
    MaxModelX,
    // model Y 轴负方向平面手柄。
    MinModelY,
    // model Y 轴正方向平面手柄。
    MaxModelY,
    // model Z 轴负方向平面手柄。
    MinModelZ,
    // model Z 轴正方向平面手柄。
    MaxModelZ,
    // 中心平移手柄，代表整体拖拽裁切盒。
    Center
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
    // 当前 backend 不支持“物理移除内部、保留外部”的执行方式。
    PhysicalRemoveInsideUnsupported,
    // 预估或执行时发现内存不足，无法安全完成裁切。
    InsufficientRam,
    // 2D mask preview 产物构建失败。
    VirtualMaskCreationFailed,
    // 需要输出裁切 image 时，生成 derived image 失败。
    DerivedImageCreationFailed,
    // 需要输出裁切 polydata 时，生成 derived polydata 失败。
    DerivedPolyDataCreationFailed
};

// 纯几何数据快照：boxToModelMatrix 是标准盒 [-1,1]^3 到 model 的唯一几何真源。
class CropDataModel {
public:
    // 返回标准盒到 model 的变换矩阵。
    const CropMatrixDouble16Array& GetBoxToModelMatrix() const { return m_boxToModelMatrix; }

    // 写入标准盒到 model 的变换矩阵。
    void SetBoxToModelMatrix(const CropMatrixDouble16Array& boxToModelMatrix) { m_boxToModelMatrix = boxToModelMatrix; }

    // 用 model 轴对齐 bounds 重建 boxToModelMatrix；适合默认全量盒或 physical 结果回填。
    void SetBoxToModelMatrixFromBounds(const CropBoundsDouble6Array& modelBounds)
    {
        m_boxToModelMatrix = GetBoxToModelMatrixFromBounds(modelBounds);
    }

    // 返回由 boxToModelMatrix 派生出的 model AABB。
    // image model 底层由 VTK physical-point API 表达；polydata model 对应网格自身坐标。
    const CropBoundsDouble6Array& GetModelBounds() const { return m_modelBounds; }

    // 写入派生 model AABB；只作为执行/校验范围缓存，不再是裁切盒真源。
    void SetModelBounds(const CropBoundsDouble6Array& modelBounds) { m_modelBounds = modelBounds; }

    // 根据当前 bounds 反推 model 中的中心点。
    std::array<double, 3> GetCenter() const
    {
        return {
            (m_modelBounds[0] + m_modelBounds[1]) * 0.5,
            (m_modelBounds[2] + m_modelBounds[3]) * 0.5,
            (m_modelBounds[4] + m_modelBounds[5]) * 0.5
        };
    }

    // 根据当前 bounds 反推 model 中的三轴尺寸。
    std::array<double, 3> GetDimensions() const
    {
        return {
            m_modelBounds[1] - m_modelBounds[0],
            m_modelBounds[3] - m_modelBounds[2],
            m_modelBounds[5] - m_modelBounds[4]
        };
    }

    // 返回当前裁切盒在 model 中的体积估计值；用于日志或粗粒度规模判断。
    double GetPhysicalVolume() const
    {
        const auto dimensions = GetDimensions();
        return dimensions[0] * dimensions[1] * dimensions[2];
    }

    // 返回当前裁切结果相对原输入的全局偏移补偿矩阵。
    const CropMatrixDouble16Array& GetGlobalOffsetMatrix() const { return m_globalOffsetMatrix; }

    // 写入全局偏移补偿矩阵；physical submit 结果会基于 origin 位移更新它。
    void SetGlobalOffsetMatrix(const CropMatrixDouble16Array& globalOffsetMatrix) { m_globalOffsetMatrix = globalOffsetMatrix; }

private:
    // 标准盒 [-1,1]^3 到 model 的唯一几何真源。
    std::array<double, 16> m_boxToModelMatrix = GetIdentityMatrixArray();
    // 由 boxToModelMatrix 派生出的 model AABB，用于 bounds 校验、index 吸附和粗粒度执行范围。
    std::array<double, 6> m_modelBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 全局偏移矩阵，用于把后端结果重新对齐回上层共享坐标语义。
    std::array<double, 16> m_globalOffsetMatrix = GetIdentityMatrixArray();
};

// UI/交互层状态快照：描述当前裁切是否启用、手柄状态和显示语义。
class CropStateModel {
public:
    // 当前结果或请求是否仍然处于“启用裁切语义”的状态。
    bool GetCropEnabled() const { return m_cropEnabled; }

    // 设置裁切语义是否生效；physical submit 产出结果时常会把它关掉。
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

    // 返回当前交互层认为被激活的裁切手柄。
    CropHandleId GetActiveHandle() const { return m_activeHandle; }

    // 写入当前激活的裁切手柄语义。
    void SetActiveHandle(CropHandleId handleId) { m_activeHandle = handleId; }

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
    // 当前被激活的交互手柄。
    CropHandleId m_activeHandle = CropHandleId::None;
    // 当前交互阶段，供上层区分 dragging/released。
    CropInteractionPhase m_interactionPhase = CropInteractionPhase::Idle;
};

// 一次裁切执行请求：boxToModelMatrix 是标准盒 [-1,1]^3 到 model 的唯一几何真源。
class OrthogonalCropRequest {
public:
    // 返回标准裁切盒到 model 的矩阵。
    const CropMatrixDouble16Array& GetBoxToModelMatrix() const { return m_boxToModelMatrix; }

    // 写入标准裁切盒到 model 的矩阵。
    void SetBoxToModelMatrix(const CropMatrixDouble16Array& boxToModelMatrix) { m_boxToModelMatrix = boxToModelMatrix; }

    // 用 model 轴对齐 bounds 构造标准盒请求。
    void SetBoxToModelMatrixFromBounds(const CropBoundsDouble6Array& modelBounds)
    {
        m_boxToModelMatrix = GetBoxToModelMatrixFromBounds(modelBounds);
    }

    // 返回本次请求是 VirtualCrop 还是 PhysicalCrop。
    CropExecutionMode GetExecutionMode() const { return m_executionMode; }

    // 设置本次请求只做预览还是要求真正输出派生结果。
    void SetExecutionMode(CropExecutionMode executionMode) { m_executionMode = executionMode; }

    // 返回 inside / outside 的保留语义。
    CropRemovalMode GetRemovalMode() const { return m_removalMode; }

    // 设置 inside / outside 的保留语义。
    void SetRemovalMode(CropRemovalMode removalMode) { m_removalMode = removalMode; }

    // 返回请求当前携带的全局偏移补偿矩阵。
    const CropMatrixDouble16Array& GetGlobalOffsetMatrix() const { return m_globalOffsetMatrix; }

    // 写入请求当前携带的全局偏移补偿矩阵。
    void SetGlobalOffsetMatrix(const CropMatrixDouble16Array& globalOffsetMatrix) { m_globalOffsetMatrix = globalOffsetMatrix; }

    // 返回随请求一起下发的 UI/交互状态快照。
    const CropStateModel& GetCropStateModel() const { return m_cropStateModel; }

    // 写入随请求一起下发的 UI/交互状态快照。
    void SetCropStateModel(const CropStateModel& cropStateModel) { m_cropStateModel = cropStateModel; }

    // 返回调用方显式指定的可用内存上限。
    std::size_t GetAvailableRamBytes() const { return m_availableRamBytes; }

    // 写入调用方显式指定的可用内存上限；为 0 表示交给后端兜底判断。
    void SetAvailableRamBytes(std::size_t availableRamBytes) { m_availableRamBytes = availableRamBytes; }

private:
    // 标准裁切盒 [-1,1]^3 到 model 的矩阵。
    std::array<double, 16> m_boxToModelMatrix = GetIdentityMatrixArray();
    // 当前请求只做预览，还是要求输出真正可复用的派生结果。
    CropExecutionMode m_executionMode = CropExecutionMode::VirtualCrop;
    // inside / outside 的保留语义；影响 2D mask preview 取值和 physical submit 合法性判断。
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;
    // 输入数据当前附带的全局偏移补偿矩阵；physical submit 后会继续累加新的 origin 偏移。
    std::array<double, 16> m_globalOffsetMatrix = GetIdentityMatrixArray();
    // UI/交互层随本次请求一并带下去的状态快照，如是否启用、当前 phase、透明度等。
    CropStateModel m_cropStateModel;
    // 可选的可用内存上限；为 0 表示交给后端使用系统 RAM 查询或默认兜底值。
    std::size_t m_availableRamBytes = 0;
};

class OrthogonalCropStatistics {
public:
    // 返回统计最终落到的数据源。
    OrthogonalCropDataSource GetResolvedDataSource() const { return m_resolvedDataSource; }

    // 写入统计最终落到的数据源。
    void SetResolvedDataSource(OrthogonalCropDataSource dataSource) { m_resolvedDataSource = dataSource; }

    // 返回统计采用的具体后端实现。
    OrthogonalCropResolvedBackend GetResolvedBackend() const { return m_resolvedBackend; }

    // 写入统计采用的具体后端实现。
    void SetResolvedBackend(OrthogonalCropResolvedBackend backend) { m_resolvedBackend = backend; }

    // 返回统计阶段发现的失败原因。
    OrthogonalCropFailureReason GetFailureReason() const { return m_failureReason; }

    // 写入统计阶段发现的失败原因。
    void SetFailureReason(OrthogonalCropFailureReason failureReason) { m_failureReason = failureReason; }

    // 返回原始输入总体规模。
    std::size_t GetTotalVoxelCount() const { return m_totalVoxelCount; }

    // 写入原始输入总体规模。
    void SetTotalVoxelCount(std::size_t voxelCount) { m_totalVoxelCount = voxelCount; }

    // 返回裁切盒内部规模。
    std::size_t GetInsideVoxelCount() const { return m_insideVoxelCount; }

    // 写入裁切盒内部规模。
    void SetInsideVoxelCount(std::size_t voxelCount) { m_insideVoxelCount = voxelCount; }

    // 返回真正输出结果的规模。
    std::size_t GetOutputVoxelCount() const { return m_outputVoxelCount; }

    // 写入真正输出结果的规模。
    void SetOutputVoxelCount(std::size_t voxelCount) { m_outputVoxelCount = voxelCount; }

    // 返回当前请求的内存预估值。
    std::size_t GetEstimatedRamUsageBytes() const { return m_estimatedRamUsageBytes; }

    // 写入当前请求的内存预估值。
    void SetEstimatedRamUsageBytes(std::size_t ramUsageBytes) { m_estimatedRamUsageBytes = ramUsageBytes; }

    // 返回 image 路径吸附后的 index 边界。
    const CropIndexBoundsInt6Array& GetSnappedIndexBounds() const { return m_snappedIndexBounds; }

    // 写入 image 路径吸附后的 index 边界。
    void SetSnappedIndexBounds(const CropIndexBoundsInt6Array& indexBounds) { m_snappedIndexBounds = indexBounds; }

    // 返回当前是否允许继续执行 physical submit。
    bool GetCanExecutePhysicalCrop() const { return m_canExecutePhysicalCrop; }

    // 写入当前是否允许继续执行 physical submit。
    void SetCanExecutePhysicalCrop(bool canExecute) { m_canExecutePhysicalCrop = canExecute; }

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
    // 这次统计最终落到的数据源；用于日志和上层 UI 提示。
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::Auto;
    // 这次统计真正采用的底层实现路径，如 2D mask preview、extract VOI 或 polydata clip。
    OrthogonalCropResolvedBackend m_resolvedBackend = OrthogonalCropResolvedBackend::None;
    // 统计阶段发现的失败原因；None 表示校验通过。
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    // 原始输入总体规模；image 路径用 voxel 数，polydata 路径用 cell 数近似表达。
    std::size_t m_totalVoxelCount = 0;
    // 裁切盒内部规模；用于估算 preview 影响范围和 physical submit 输出大小。
    std::size_t m_insideVoxelCount = 0;
    // 真正输出结果的规模；preview artifact 常等于 total，physical submit 常等于 inside。
    std::size_t m_outputVoxelCount = 0;
    // 当前请求若继续执行，预估至少需要的内存量；主要服务 physical submit 风险判断。
    std::size_t m_estimatedRamUsageBytes = 0;
    // image 路径里 model bounds 吸附后的 index 整数边界；供 mask/VOI 提取直接复用。
    std::array<int, 6> m_snappedIndexBounds = { 0, 0, 0, 0, 0, 0 };
    // 当前统计是否允许继续做 physical submit；会综合 removal mode 与内存约束。
    bool m_canExecutePhysicalCrop = false;
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
    OrthogonalCropResolvedBackend GetResolvedBackend() const { return m_resolvedBackend; }

    // 写入结果采用的具体后端实现。
    void SetResolvedBackend(OrthogonalCropResolvedBackend backend) { m_resolvedBackend = backend; }

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

    // 返回 image 路径产出的派生图像结果。
    vtkSmartPointer<vtkImageData> GetDerivedImage() const { return m_derivedImage; }

    // 写入 image 路径产出的派生图像结果。
    void SetDerivedImage(vtkSmartPointer<vtkImageData> derivedImage) { m_derivedImage = std::move(derivedImage); }

    // 返回 polydata 路径产出的派生网格结果。
    vtkSmartPointer<vtkPolyData> GetDerivedPolyData() const { return m_derivedPolyData; }

    // 写入 polydata 路径产出的派生网格结果。
    void SetDerivedPolyData(vtkSmartPointer<vtkPolyData> derivedPolyData) { m_derivedPolyData = std::move(derivedPolyData); }

    // 返回 virtual crop 生成的遮罩图像。
    vtkSmartPointer<vtkImageData> GetVirtualMaskImage() const { return m_virtualMaskImage; }

    // 写入 virtual crop 生成的遮罩图像。
    void SetVirtualMaskImage(vtkSmartPointer<vtkImageData> virtualMaskImage) { m_virtualMaskImage = std::move(virtualMaskImage); }

    // 返回裁切盒轮廓几何结果。
    vtkSmartPointer<vtkPolyData> GetOutlinePolyData() const { return m_outlinePolyData; }

    // 写入裁切盒轮廓几何结果。
    void SetOutlinePolyData(vtkSmartPointer<vtkPolyData> outlinePolyData) { m_outlinePolyData = std::move(outlinePolyData); }

    // 返回这次执行对应的客观几何快照。
    const CropDataModel& GetCropDataModel() const { return m_cropDataModel; }

    // 写入这次执行对应的客观几何快照。
    void SetCropDataModel(const CropDataModel& cropDataModel) { m_cropDataModel = cropDataModel; }

    // 返回这次执行对应的交互/显示状态快照。
    const CropStateModel& GetCropStateModel() const { return m_cropStateModel; }

    // 写入这次执行对应的交互/显示状态快照。
    void SetCropStateModel(const CropStateModel& cropStateModel) { m_cropStateModel = cropStateModel; }

    // 返回与本次结果配套的统计信息。
    const OrthogonalCropStatistics& GetStatistics() const { return m_statistics; }

    // 写入与本次结果配套的统计信息。
    void SetStatistics(const OrthogonalCropStatistics& statistics) { m_statistics = statistics; }

private:
    // 最终执行实际落到的数据源；通常和 statistics 保持一致，但以结果本身为准。
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::Auto;
    // 结果实际采用的后端实现；决定调用方该如何理解 derived data 的来源。
    OrthogonalCropResolvedBackend m_resolvedBackend = OrthogonalCropResolvedBackend::None;
    // 本次结果是否构造成功；false 不一定是崩溃，也可能是被策略性阻断。
    bool m_succeeded = false;
    // 最终失败原因；当 succeeded 为 false 时，上层应优先读取它和 message。
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    // 对当前成功/失败状态的文字解释；可直接给日志或 UI 弹框使用。
    std::string m_message;
    // image 路径 physical submit 或 extracted preview 返回的派生体数据。
    vtkSmartPointer<vtkImageData> m_derivedImage;
    // polydata 路径裁切后的输出网格；image 路径通常为空。
    vtkSmartPointer<vtkPolyData> m_derivedPolyData;
    // virtual crop 在 image 路径下生成的同尺寸遮罩图；真正控制 inside/outside 语义。
    vtkSmartPointer<vtkImageData> m_virtualMaskImage;
    // 当前裁切盒轮廓的可视几何；常用于 overlay 或调试显示。
    vtkSmartPointer<vtkPolyData> m_outlinePolyData;
    // 这次结果对应的客观几何快照；physical submit 后可能已经更新为派生体自身的 bounds。
    CropDataModel m_cropDataModel;
    // 这次结果对应的交互/显示状态快照；便于上层知道结果产生时是否仍处于交互态。
    CropStateModel m_cropStateModel;
    // 与结果配套的统计信息；避免调用方再次重复跑一遍统计。
    OrthogonalCropStatistics m_statistics;
};
