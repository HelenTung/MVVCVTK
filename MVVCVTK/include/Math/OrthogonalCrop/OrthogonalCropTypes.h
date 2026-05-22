#pragma once
// =====================================================================
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
    const std::array<double, 6>& GetRasBounds() const
    {
        return m_rasBounds;
    }

    void SetRasBounds(const std::array<double, 6>& rasBounds)
    {
        m_rasBounds = rasBounds;
    }

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

    std::array<double, 3> GetCenter() const
    {
        return {
            (m_rasBounds[0] + m_rasBounds[1]) * 0.5,
            (m_rasBounds[2] + m_rasBounds[3]) * 0.5,
            (m_rasBounds[4] + m_rasBounds[5]) * 0.5
        };
    }

    std::array<double, 3> GetDimensions() const
    {
        return {
            m_rasBounds[1] - m_rasBounds[0],
            m_rasBounds[3] - m_rasBounds[2],
            m_rasBounds[5] - m_rasBounds[4]
        };
    }

    double GetPhysicalVolume() const
    {
        const auto dimensions = GetDimensions();
        return dimensions[0] * dimensions[1] * dimensions[2];
    }

    const std::array<double, 16>& GetGlobalOffsetMatrix() const
    {
        return m_globalOffsetMatrix;
    }

    void SetGlobalOffsetMatrix(const std::array<double, 16>& matrix)
    {
        m_globalOffsetMatrix = matrix;
    }

    const std::array<double, 16>& GetLocalAlignmentMatrix() const
    {
        return m_localAlignmentMatrix;
    }

    void SetLocalAlignmentMatrix(const std::array<double, 16>& matrix)
    {
        m_localAlignmentMatrix = matrix;
    }

    bool GetLocalAlignmentEnabled() const
    {
        return m_localAlignmentEnabled;
    }

    void SetLocalAlignmentEnabled(bool enabled)
    {
        m_localAlignmentEnabled = enabled;
    }

    const std::array<double, 3>& GetLocalCenter() const
    {
        return m_localCenter;
    }

    void SetLocalCenter(const std::array<double, 3>& center)
    {
        m_localCenter = center;
    }

    const std::array<double, 3>& GetLocalDimensions() const
    {
        return m_localDimensions;
    }

    void SetLocalDimensions(const std::array<double, 3>& dimensions)
    {
        m_localDimensions = dimensions;
    }

private:
    // 世界坐标系下的 RAS min/max bounds，是最基础的裁切盒表达。
    std::array<double, 6> m_rasBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 全局偏移矩阵，用于把算法输入和上层世界坐标系对齐。
    std::array<double, 16> m_globalOffsetMatrix = GetIdentityMatrixArray();
    // 局部对齐矩阵，用于表达非世界轴对齐情况下的局部裁切参考系。
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
    bool GetCropEnabled() const
    {
        return m_cropEnabled;
    }

    void SetCropEnabled(bool enabled)
    {
        m_cropEnabled = enabled;
    }

    double GetInsideOpacity() const
    {
        return m_insideOpacity;
    }

    void SetInsideOpacity(double opacity)
    {
        m_insideOpacity = opacity;
    }

    bool GetOutsideVisibility() const
    {
        return m_outsideVisibility;
    }

    void SetOutsideVisibility(bool visible)
    {
        m_outsideVisibility = visible;
    }

    const std::array<double, 3>& GetLutColor() const
    {
        return m_lutColor;
    }

    void SetLutColor(const std::array<double, 3>& color)
    {
        m_lutColor = color;
    }

    CropHandleId GetActiveHandle() const
    {
        return m_activeHandle;
    }

    void SetActiveHandle(CropHandleId handleId)
    {
        m_activeHandle = handleId;
    }

    CropInteractionPhase GetInteractionPhase() const
    {
        return m_interactionPhase;
    }

    void SetInteractionPhase(CropInteractionPhase phase)
    {
        m_interactionPhase = phase;
    }

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
    CropBoundsMode GetBoundsMode() const
    {
        return m_boundsMode;
    }

    void SetBoundsMode(CropBoundsMode boundsMode)
    {
        m_boundsMode = boundsMode;
    }

    CropExecutionMode GetExecutionMode() const
    {
        return m_executionMode;
    }

    void SetExecutionMode(CropExecutionMode executionMode)
    {
        m_executionMode = executionMode;
    }

    CropRemovalMode GetRemovalMode() const
    {
        return m_removalMode;
    }

    void SetRemovalMode(CropRemovalMode removalMode)
    {
        m_removalMode = removalMode;
    }

    const std::array<double, 6>& GetRasBounds() const
    {
        return m_rasBounds;
    }

    void SetRasBounds(const std::array<double, 6>& rasBounds)
    {
        m_rasBounds = rasBounds;
    }

    const std::array<double, 3>& GetCenter() const
    {
        return m_center;
    }

    void SetCenter(const std::array<double, 3>& center)
    {
        m_center = center;
    }

    const std::array<double, 3>& GetDimensions() const
    {
        return m_dimensions;
    }

    void SetDimensions(const std::array<double, 3>& dimensions)
    {
        m_dimensions = dimensions;
    }

    const std::array<double, 3>& GetLocalCenter() const
    {
        return m_localCenter;
    }

    void SetLocalCenter(const std::array<double, 3>& center)
    {
        m_localCenter = center;
    }

    const std::array<double, 3>& GetLocalDimensions() const
    {
        return m_localDimensions;
    }

    void SetLocalDimensions(const std::array<double, 3>& dimensions)
    {
        m_localDimensions = dimensions;
    }

    const std::array<double, 16>& GetLocalAlignmentMatrix() const
    {
        return m_localAlignmentMatrix;
    }

    void SetLocalAlignmentMatrix(const std::array<double, 16>& matrix)
    {
        m_localAlignmentMatrix = matrix;
    }

    const std::array<double, 16>& GetGlobalOffsetMatrix() const
    {
        return m_globalOffsetMatrix;
    }

    void SetGlobalOffsetMatrix(const std::array<double, 16>& matrix)
    {
        m_globalOffsetMatrix = matrix;
    }

    const CropStateModel& GetCropStateModel() const
    {
        return m_cropStateModel;
    }

    void SetCropStateModel(const CropStateModel& cropStateModel)
    {
        m_cropStateModel = cropStateModel;
    }

    std::size_t GetAvailableRamBytes() const
    {
        return m_availableRamBytes;
    }

    void SetAvailableRamBytes(std::size_t availableRamBytes)
    {
        m_availableRamBytes = availableRamBytes;
    }

private:
    CropBoundsMode m_boundsMode = CropBoundsMode::InputVolumeBounds;
    CropExecutionMode m_executionMode = CropExecutionMode::VirtualCrop;
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;
    std::array<double, 6> m_rasBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 3> m_center = { 0.0, 0.0, 0.0 };
    std::array<double, 3> m_dimensions = { 0.0, 0.0, 0.0 };
    std::array<double, 3> m_localCenter = { 0.0, 0.0, 0.0 };
    std::array<double, 3> m_localDimensions = { 0.0, 0.0, 0.0 };
    std::array<double, 16> m_localAlignmentMatrix = GetIdentityMatrixArray();
    std::array<double, 16> m_globalOffsetMatrix = GetIdentityMatrixArray();
    CropStateModel m_cropStateModel;
    std::size_t m_availableRamBytes = 0;
};

class OrthogonalCropStatistics {
public:
    OrthogonalCropDataSource GetResolvedDataSource() const
    {
        return m_resolvedDataSource;
    }

    void SetResolvedDataSource(OrthogonalCropDataSource dataSource)
    {
        m_resolvedDataSource = dataSource;
    }

    OrthogonalCropResolvedBackend GetResolvedBackend() const
    {
        return m_resolvedBackend;
    }

    void SetResolvedBackend(OrthogonalCropResolvedBackend backend)
    {
        m_resolvedBackend = backend;
    }

    OrthogonalCropFailureReason GetFailureReason() const
    {
        return m_failureReason;
    }

    void SetFailureReason(OrthogonalCropFailureReason failureReason)
    {
        m_failureReason = failureReason;
    }

    std::size_t GetTotalVoxelCount() const
    {
        return m_totalVoxelCount;
    }

    void SetTotalVoxelCount(std::size_t voxelCount)
    {
        m_totalVoxelCount = voxelCount;
    }

    std::size_t GetInsideVoxelCount() const
    {
        return m_insideVoxelCount;
    }

    void SetInsideVoxelCount(std::size_t voxelCount)
    {
        m_insideVoxelCount = voxelCount;
    }

    std::size_t GetOutputVoxelCount() const
    {
        return m_outputVoxelCount;
    }

    void SetOutputVoxelCount(std::size_t voxelCount)
    {
        m_outputVoxelCount = voxelCount;
    }

    std::size_t GetEstimatedRamUsageBytes() const
    {
        return m_estimatedRamUsageBytes;
    }

    void SetEstimatedRamUsageBytes(std::size_t ramUsageBytes)
    {
        m_estimatedRamUsageBytes = ramUsageBytes;
    }

    const std::array<int, 6>& GetSnappedIjkBounds() const
    {
        return m_snappedIjkBounds;
    }

    void SetSnappedIjkBounds(const std::array<int, 6>& ijkBounds)
    {
        m_snappedIjkBounds = ijkBounds;
    }

    bool GetCanExecutePhysicalCrop() const
    {
        return m_canExecutePhysicalCrop;
    }

    void SetCanExecutePhysicalCrop(bool canExecute)
    {
        m_canExecutePhysicalCrop = canExecute;
    }

    const std::string& GetValidationMessage() const
    {
        return m_validationMessage;
    }

    const std::string& GetMessage() const
    {
        return m_validationMessage;
    }

    void SetValidationMessage(const std::string& validationMessage)
    {
        m_validationMessage = validationMessage;
    }

    void SetMessage(const std::string& message)
    {
        m_validationMessage = message;
    }

private:
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::Auto;
    OrthogonalCropResolvedBackend m_resolvedBackend = OrthogonalCropResolvedBackend::None;
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    std::size_t m_totalVoxelCount = 0;
    std::size_t m_insideVoxelCount = 0;
    std::size_t m_outputVoxelCount = 0;
    std::size_t m_estimatedRamUsageBytes = 0;
    std::array<int, 6> m_snappedIjkBounds = { 0, 0, 0, 0, 0, 0 };
    bool m_canExecutePhysicalCrop = false;
    std::string m_validationMessage;
};

class OrthogonalCropResult {
public:
    OrthogonalCropDataSource GetResolvedDataSource() const
    {
        return m_resolvedDataSource;
    }

    void SetResolvedDataSource(OrthogonalCropDataSource dataSource)
    {
        m_resolvedDataSource = dataSource;
    }

    OrthogonalCropResolvedBackend GetResolvedBackend() const
    {
        return m_resolvedBackend;
    }

    void SetResolvedBackend(OrthogonalCropResolvedBackend backend)
    {
        m_resolvedBackend = backend;
    }

    bool GetSucceeded() const
    {
        return m_succeeded;
    }

    void SetSucceeded(bool succeeded)
    {
        m_succeeded = succeeded;
    }

    OrthogonalCropFailureReason GetFailureReason() const
    {
        return m_failureReason;
    }

    void SetFailureReason(OrthogonalCropFailureReason failureReason)
    {
        m_failureReason = failureReason;
    }

    const std::string& GetMessage() const
    {
        return m_message;
    }

    void SetMessage(const std::string& message)
    {
        m_message = message;
    }

    vtkSmartPointer<vtkImageData> GetDerivedImage() const
    {
        return m_derivedImage;
    }

    void SetDerivedImage(vtkSmartPointer<vtkImageData> derivedImage)
    {
        m_derivedImage = std::move(derivedImage);
    }

    vtkSmartPointer<vtkPolyData> GetDerivedPolyData() const
    {
        return m_derivedPolyData;
    }

    void SetDerivedPolyData(vtkSmartPointer<vtkPolyData> derivedPolyData)
    {
        m_derivedPolyData = std::move(derivedPolyData);
    }

    vtkSmartPointer<vtkImageData> GetVirtualMaskImage() const
    {
        return m_virtualMaskImage;
    }

    void SetVirtualMaskImage(vtkSmartPointer<vtkImageData> virtualMaskImage)
    {
        m_virtualMaskImage = std::move(virtualMaskImage);
    }

    vtkSmartPointer<vtkPolyData> GetOutlinePolyData() const
    {
        return m_outlinePolyData;
    }

    void SetOutlinePolyData(vtkSmartPointer<vtkPolyData> outlinePolyData)
    {
        m_outlinePolyData = std::move(outlinePolyData);
    }

    const CropDataModel& GetCropDataModel() const
    {
        return m_cropDataModel;
    }

    void SetCropDataModel(const CropDataModel& cropDataModel)
    {
        m_cropDataModel = cropDataModel;
    }

    const CropStateModel& GetCropStateModel() const
    {
        return m_cropStateModel;
    }

    void SetCropStateModel(const CropStateModel& cropStateModel)
    {
        m_cropStateModel = cropStateModel;
    }

    const OrthogonalCropStatistics& GetStatistics() const
    {
        return m_statistics;
    }

    void SetStatistics(const OrthogonalCropStatistics& statistics)
    {
        m_statistics = statistics;
    }

private:
    OrthogonalCropDataSource m_resolvedDataSource = OrthogonalCropDataSource::Auto;
    OrthogonalCropResolvedBackend m_resolvedBackend = OrthogonalCropResolvedBackend::None;
    bool m_succeeded = false;
    OrthogonalCropFailureReason m_failureReason = OrthogonalCropFailureReason::None;
    std::string m_message;
    vtkSmartPointer<vtkImageData> m_derivedImage;
    vtkSmartPointer<vtkPolyData> m_derivedPolyData;
    vtkSmartPointer<vtkImageData> m_virtualMaskImage;
    vtkSmartPointer<vtkPolyData> m_outlinePolyData;
    CropDataModel m_cropDataModel;
    CropStateModel m_cropStateModel;
    OrthogonalCropStatistics m_statistics;
};
