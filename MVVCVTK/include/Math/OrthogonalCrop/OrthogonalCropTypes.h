#pragma once
// =====================================================================
// Path: MVVCVTK/include/Math/OrthogonalCrop/OrthogonalCropTypes.h
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
// - OrthogonalCropResult：一次执行结果，虚拟裁切返回 mask，物理裁切返回 derived image。

#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

using CropBoundsDouble6Array = std::array<double, 6>;
using CropVectorDouble3Array = std::array<double, 3>;
using CropMatrixDouble16Array = std::array<double, 16>;
using CropIjkBoundsInt6Array = std::array<int, 6>;

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

// 裁切盒内外的保留/移除语义。
enum class CropRemovalMode {
    // 保留裁切盒内部内容，移除外部内容。
    KeepInside,
    // 移除裁切盒内部内容，保留外部内容。
    RemoveInside
};

// 本次请求到底只做预览，还是要产出真正可复用的裁切结果。
enum class CropExecutionMode {
    // 虚拟裁切：偏向交互预览，不要求立刻生成独立数据副本。
    VirtualCrop,
    // 物理裁切：要求得到可脱离原输入单独使用的输出数据。
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
    // image 路径走虚拟 mask 方案，只改变可视语义，不复制体数据。
    ImageVirtualMask,
    // image 路径通过提取 VOI 直接生成裁切后的子体数据。
    ImageExtractVOI,
    // image mapper 原生 cropping 方案，通常只用于渲染层裁切语义。
    ImageMapperCropping,
    // polydata 路径使用 box clip + geometry filter 得到输出网格。
    PolyDataClipDataSet
};

// 交互层当前选中的裁切手柄语义。
enum class CropHandleId {
    // 当前没有任何有效手柄被激活。
    None,
    // R 轴负方向平面手柄。
    MinR,
    // R 轴正方向平面手柄。
    MaxR,
    // A 轴负方向平面手柄。
    MinA,
    // A 轴正方向平面手柄。
    MaxA,
    // S 轴负方向平面手柄。
    MinS,
    // S 轴正方向平面手柄。
    MaxS,
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

// 请求中 bounds 的解释方式。
enum class CropBoundsMode {
    // 直接以输入数据整体 bounds 作为裁切范围。
    InputVolumeBounds,
    // 以 min/max 六个坐标直接表达裁切盒。
    MinMaxCoordinates,
    // 以世界坐标系下的中心点和尺寸表达裁切盒。
    CenterAndDimensions,
    // 以局部对齐坐标系下的中心点和尺寸表达裁切盒。
    LocalCenterAndDimensions
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
    // 虚拟裁切阶段构建 mask 失败。
    VirtualMaskCreationFailed,
    // 需要输出裁切 image 时，生成 derived image 失败。
    DerivedImageCreationFailed,
    // 需要输出裁切 polydata 时，生成 derived polydata 失败。
    DerivedPolyDataCreationFailed
};

// 纯几何数据快照：描述裁切盒客观空间范围，以及和局部对齐相关的矩阵信息。
class CropDataModel {
public:
    // 返回当前裁切盒在后端输入坐标系下的 RAS/physical min/max bounds。
    // image 路径这里对应 vtkImageData 的 physical 坐标；polydata 路径对应模型坐标。
    const CropBoundsDouble6Array& GetRasBounds() const { return m_rasBounds; }

    // 直接写入后端输入坐标系下的 RAS bounds；适合 MinMaxCoordinates 模式或结果回填。
    void SetRasBounds(const CropBoundsDouble6Array& rasBounds) { m_rasBounds = rasBounds; }

    // 用六个显式 min/max 坐标直接定义裁切盒。
    void SetMinMaxCoordinates(
        double minR,
        double maxR,
        double minA,
        double maxA,
        double minS,
        double maxS)
    {
        m_rasBounds = { minR, maxR, minA, maxA, minS, maxS };
    }

    // 用后端输入坐标系中的中心点和尺寸换算出对应的 RAS bounds。
    void SetCenterAndDimensions(
        const std::array<double, 3>& center,
        const std::array<double, 3>& dimensions)
    {
        m_rasBounds = {
            center[0] - dimensions[0] * 0.5,
            center[0] + dimensions[0] * 0.5,
            center[1] - dimensions[1] * 0.5,
            center[1] + dimensions[1] * 0.5,
            center[2] - dimensions[2] * 0.5,
            center[2] + dimensions[2] * 0.5
        };
    }

    // 根据当前 bounds 反推后端输入坐标系中的中心点。
    std::array<double, 3> GetCenter() const
    {
        return {
            (m_rasBounds[0] + m_rasBounds[1]) * 0.5,
            (m_rasBounds[2] + m_rasBounds[3]) * 0.5,
            (m_rasBounds[4] + m_rasBounds[5]) * 0.5
        };
    }

    // 根据当前 bounds 反推后端输入坐标系中的三轴尺寸。
    std::array<double, 3> GetDimensions() const
    {
        return {
            m_rasBounds[1] - m_rasBounds[0],
            m_rasBounds[3] - m_rasBounds[2],
            m_rasBounds[5] - m_rasBounds[4]
        };
    }

    // 返回当前裁切盒在物理空间中的体积估计值；用于日志或粗粒度规模判断。
    double GetPhysicalVolume() const
    {
        const auto dimensions = GetDimensions();
        return dimensions[0] * dimensions[1] * dimensions[2];
    }

    // 返回当前裁切结果相对原输入的全局偏移补偿矩阵。
    const CropMatrixDouble16Array& GetGlobalOffsetMatrix() const { return m_globalOffsetMatrix; }

    // 写入全局偏移补偿矩阵；physical crop 结果会基于 origin 位移更新它。
    void SetGlobalOffsetMatrix(const CropMatrixDouble16Array& matrix) { m_globalOffsetMatrix = matrix; }

    // 返回局部裁切盒坐标系到后端输入坐标系的对齐矩阵。
    const CropMatrixDouble16Array& GetLocalAlignmentMatrix() const { return m_localAlignmentMatrix; }

    // 写入局部坐标系到后端输入坐标系的变换矩阵。
    void SetLocalAlignmentMatrix(const CropMatrixDouble16Array& matrix) { m_localAlignmentMatrix = matrix; }

    // 当前是否启用了局部对齐裁切语义。
    bool GetLocalAlignmentEnabled() const { return m_localAlignmentEnabled; }

    // 显式打开或关闭局部对齐语义；关闭时通常按普通世界轴对齐盒处理。
    void SetLocalAlignmentEnabled(bool enabled) { m_localAlignmentEnabled = enabled; }

    // 返回局部对齐坐标系中的中心点。
    const CropVectorDouble3Array& GetLocalCenter() const { return m_localCenter; }

    // 写入局部坐标系中的中心点。
    void SetLocalCenter(const CropVectorDouble3Array& center) { m_localCenter = center; }

    // 返回局部对齐坐标系中的尺寸。
    const CropVectorDouble3Array& GetLocalDimensions() const { return m_localDimensions; }

    // 写入局部对齐坐标系中的尺寸。
    void SetLocalDimensions(const CropVectorDouble3Array& dimensions) { m_localDimensions = dimensions; }

private:
    // 后端输入坐标系下的 RAS/physical min/max bounds，是最基础的裁切盒表达。
    std::array<double, 6> m_rasBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 全局偏移矩阵，用于把算法输入和上层世界坐标系对齐。
    std::array<double, 16> m_globalOffsetMatrix = GetIdentityMatrixArray();
    // 局部对齐矩阵，用于表达局部裁切参考系到后端输入坐标系的映射。
    std::array<double, 16> m_localAlignmentMatrix = GetIdentityMatrixArray();
    // 当前是否启用了局部对齐语义。
    bool m_localAlignmentEnabled = false;
    // 局部对齐坐标系下的中心点。
    std::array<double, 3> m_localCenter = { 0.0, 0.0, 0.0 };
    // 局部对齐坐标系下的尺寸。
    std::array<double, 3> m_localDimensions = { 0.0, 0.0, 0.0 };
};

// UI/交互层状态快照：描述当前裁切是否启用、手柄状态和显示语义。
class CropStateModel {
public:
    // 当前结果或请求是否仍然处于“启用裁切语义”的状态。
    bool GetCropEnabled() const { return m_cropEnabled; }

    // 设置裁切语义是否生效；physical crop 产出结果时常会把它关掉。
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
    // inside 区域的可视透明度，常用于虚拟裁切或 overlay 预览。
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

// 一次裁切执行请求：既包含 bounds 语义，也包含执行模式和显示态快照。
class OrthogonalCropRequest {
public:
    // 返回本次请求采用的 bounds 解释方式。
    CropBoundsMode GetBoundsMode() const { return m_boundsMode; }

    // 设置本次请求采用哪一种 bounds 解释方式。
    void SetBoundsMode(CropBoundsMode boundsMode) { m_boundsMode = boundsMode; }

    // 明确声明本次请求使用完整输入范围，不消费其它 bounds payload 字段。
    void SetInputVolumeBounds()
    {
        m_boundsMode = CropBoundsMode::InputVolumeBounds;
    }

    // 返回本次请求是 VirtualCrop 还是 PhysicalCrop。
    CropExecutionMode GetExecutionMode() const { return m_executionMode; }

    // 设置本次请求只做预览还是要求真正输出派生结果。
    void SetExecutionMode(CropExecutionMode executionMode) { m_executionMode = executionMode; }

    // 返回 inside / outside 的保留语义。
    CropRemovalMode GetRemovalMode() const { return m_removalMode; }

    // 设置 inside / outside 的保留语义。
    void SetRemovalMode(CropRemovalMode removalMode) { m_removalMode = removalMode; }

    // 返回显式给定的后端输入坐标系 RAS bounds。
    const CropBoundsDouble6Array& GetRasBounds() const { return m_rasBounds; }

    // 写入显式给定的后端输入坐标系 RAS bounds。
    void SetRasBounds(const CropBoundsDouble6Array& rasBounds) { m_rasBounds = rasBounds; }

    // 用 min/max bounds 完整定义请求，同时切换到对应 bounds mode。
    void SetMinMaxCoordinates(const std::array<double, 6>& rasBounds)
    {
        m_boundsMode = CropBoundsMode::MinMaxCoordinates;
        m_rasBounds = rasBounds;
    }

    // 返回后端输入坐标系中的中心点定义。
    const CropVectorDouble3Array& GetCenter() const { return m_center; }

    // 写入后端输入坐标系中的中心点定义。
    void SetCenter(const CropVectorDouble3Array& center) { m_center = center; }

    // 返回后端输入坐标系中的尺寸定义。
    const CropVectorDouble3Array& GetDimensions() const { return m_dimensions; }

    // 写入后端输入坐标系中的尺寸定义。
    void SetDimensions(const CropVectorDouble3Array& dimensions) { m_dimensions = dimensions; }

    // 用后端输入坐标系中的中心点和尺寸完整定义请求，同时切换到对应 bounds mode。
    void SetCenterAndDimensions(
        const std::array<double, 3>& center,
        const std::array<double, 3>& dimensions)
    {
        m_boundsMode = CropBoundsMode::CenterAndDimensions;
        m_center = center;
        m_dimensions = dimensions;
    }

    // 返回局部对齐坐标系中的中心点定义。
    const CropVectorDouble3Array& GetLocalCenter() const { return m_localCenter; }

    // 写入局部对齐坐标系中的中心点定义。
    void SetLocalCenter(const CropVectorDouble3Array& center) { m_localCenter = center; }

    // 返回局部对齐坐标系中的尺寸定义。
    const CropVectorDouble3Array& GetLocalDimensions() const { return m_localDimensions; }

    // 写入局部对齐坐标系中的尺寸定义。
    void SetLocalDimensions(const CropVectorDouble3Array& dimensions) { m_localDimensions = dimensions; }

    // 用局部中心点、局部尺寸和对齐矩阵完整定义请求，同时切换到对应 bounds mode。
    // 交互预览路径里，这个局部坐标系通常直接取 widget 所在的世界坐标系，
    // localAlignmentMatrix 则提供 world -> model/physical 的折叠关系。
    void SetLocalCenterAndDimensions(
        const std::array<double, 3>& center,
        const std::array<double, 3>& dimensions,
        const std::array<double, 16>& localAlignmentMatrix)
    {
        m_boundsMode = CropBoundsMode::LocalCenterAndDimensions;
        m_localCenter = center;
        m_localDimensions = dimensions;
        m_localAlignmentMatrix = localAlignmentMatrix;
    }

    // 返回局部坐标系到后端输入坐标系的对齐矩阵。
    const CropMatrixDouble16Array& GetLocalAlignmentMatrix() const { return m_localAlignmentMatrix; }

    // 写入局部坐标系到后端输入坐标系的对齐矩阵。
    void SetLocalAlignmentMatrix(const CropMatrixDouble16Array& matrix) { m_localAlignmentMatrix = matrix; }

    // 返回请求当前携带的全局偏移补偿矩阵。
    const CropMatrixDouble16Array& GetGlobalOffsetMatrix() const { return m_globalOffsetMatrix; }

    // 写入请求当前携带的全局偏移补偿矩阵。
    void SetGlobalOffsetMatrix(const CropMatrixDouble16Array& matrix) { m_globalOffsetMatrix = matrix; }

    // 返回随请求一起下发的 UI/交互状态快照。
    const CropStateModel& GetCropStateModel() const { return m_cropStateModel; }

    // 写入随请求一起下发的 UI/交互状态快照。
    void SetCropStateModel(const CropStateModel& cropStateModel) { m_cropStateModel = cropStateModel; }

    // 返回调用方显式指定的可用内存上限。
    std::size_t GetAvailableRamBytes() const { return m_availableRamBytes; }

    // 写入调用方显式指定的可用内存上限；为 0 表示交给后端兜底判断。
    void SetAvailableRamBytes(std::size_t availableRamBytes) { m_availableRamBytes = availableRamBytes; }

private:
    // 本次请求采用哪一种 bounds 解释方式；算法层会据此决定如何还原 cropData。
    CropBoundsMode m_boundsMode = CropBoundsMode::InputVolumeBounds;
    // 当前请求只做预览，还是要求输出真正可复用的派生结果。
    CropExecutionMode m_executionMode = CropExecutionMode::VirtualCrop;
    // inside / outside 的保留语义；影响虚拟 mask 取值和物理裁切合法性判断。
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;
    // 直接给出的世界坐标系 RAS min/max bounds；MinMaxCoordinates 模式主要使用它。
    std::array<double, 6> m_rasBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 世界坐标系下的中心点；CenterAndDimensions 模式使用它重建裁切盒。
    std::array<double, 3> m_center = { 0.0, 0.0, 0.0 };
    // 世界坐标系下的尺寸；与中心点一起定义轴对齐裁切盒。
    std::array<double, 3> m_dimensions = { 0.0, 0.0, 0.0 };
    // 局部对齐坐标系中的中心点；只有 LocalCenterAndDimensions 模式会消费它。
    std::array<double, 3> m_localCenter = { 0.0, 0.0, 0.0 };
    // 局部对齐坐标系中的尺寸；会先和局部中心一起生成局部盒子，再映射回世界坐标。
    std::array<double, 3> m_localDimensions = { 0.0, 0.0, 0.0 };
    // 局部坐标系到世界坐标系的对齐矩阵；用于表达旋转后的局部裁切框。
    std::array<double, 16> m_localAlignmentMatrix = GetIdentityMatrixArray();
    // 输入数据当前附带的全局偏移补偿矩阵；物理裁切后会继续累加新的 origin 偏移。
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

    // 返回 image 路径吸附后的 IJK 边界。
    const CropIjkBoundsInt6Array& GetSnappedIjkBounds() const { return m_snappedIjkBounds; }

    // 写入 image 路径吸附后的 IJK 边界。
    void SetSnappedIjkBounds(const CropIjkBoundsInt6Array& ijkBounds) { m_snappedIjkBounds = ijkBounds; }

    // 返回当前是否允许继续执行 physical crop。
    bool GetCanExecutePhysicalCrop() const { return m_canExecutePhysicalCrop; }

    // 写入当前是否允许继续执行 physical crop。
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
    // 这次统计真正采用的底层实现路径，如 virtual mask、extract VOI 或 polydata clip。
    OrthogonalCropResolvedBackend m_resolvedBackend = OrthogonalCropResolvedBackend::None;
    // 统计阶段发现的失败原因；None 表示校验通过。
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    // 原始输入总体规模；image 路径用 voxel 数，polydata 路径用 cell 数近似表达。
    std::size_t m_totalVoxelCount = 0;
    // 裁切盒内部规模；用于估算 preview 影响范围和 physical crop 输出大小。
    std::size_t m_insideVoxelCount = 0;
    // 真正输出结果的规模；virtual crop 常等于 total，physical crop 常等于 inside。
    std::size_t m_outputVoxelCount = 0;
    // 当前请求若继续执行，预估至少需要的内存量；主要服务 physical crop 风险判断。
    std::size_t m_estimatedRamUsageBytes = 0;
    // image 路径里世界坐标 bounds 吸附后的 IJK 整数边界；供 mask/VOI 提取直接复用。
    std::array<int, 6> m_snappedIjkBounds = { 0, 0, 0, 0, 0, 0 };
    // 当前统计是否允许继续做 physical crop；会综合 removal mode 与内存约束。
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
    // image 路径 physical crop 或 extracted preview 返回的派生体数据。
    vtkSmartPointer<vtkImageData> m_derivedImage;
    // polydata 路径裁切后的输出网格；image 路径通常为空。
    vtkSmartPointer<vtkPolyData> m_derivedPolyData;
    // virtual crop 在 image 路径下生成的同尺寸遮罩图；真正控制 inside/outside 语义。
    vtkSmartPointer<vtkImageData> m_virtualMaskImage;
    // 当前裁切盒轮廓的可视几何；常用于 overlay 或调试显示。
    vtkSmartPointer<vtkPolyData> m_outlinePolyData;
    // 这次结果对应的客观几何快照；physical crop 后可能已经更新为派生体自身的 bounds。
    CropDataModel m_cropDataModel;
    // 这次结果对应的交互/显示状态快照；便于上层知道结果产生时是否仍处于交互态。
    CropStateModel m_cropStateModel;
    // 与结果配套的统计信息；避免调用方再次重复跑一遍统计。
    OrthogonalCropStatistics m_statistics;
};
