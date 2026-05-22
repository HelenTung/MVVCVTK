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

inline std::array<double, 16> GetIdentityMatrixArray()
{
    return {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
}

enum class CropRemovalMode {
    KeepInside,
    RemoveInside
};

enum class CropExecutionMode {
    VirtualCrop,
    PhysicalCrop
};

enum class OrthogonalCropDataSource {
    Auto,
    ImageData,
    PolyData
};

enum class OrthogonalCropResolvedBackend {
    None,
    ImageVirtualMask,
    ImageExtractVOI,
    ImageMapperCropping,
    PolyDataClipDataSet
};

enum class CropHandleId {
    None,
    MinR,
    MaxR,
    MinA,
    MaxA,
    MinS,
    MaxS,
    Center
};

enum class CropInteractionPhase {
    Idle,
    Hover,
    Dragging,
    Released
};

enum class CropBoundsMode {
    InputVolumeBounds,
    MinMaxCoordinates,
    CenterAndDimensions,
    LocalCenterAndDimensions
};

enum class OrthogonalCropFailureReason {
    None,
    InputImageMissing,
    InputPolyDataMissing,
    InvalidBounds,
    BoundsOutOfRange,
    PhysicalRemoveInsideUnsupported,
    InsufficientRam,
    VirtualMaskCreationFailed,
    DerivedImageCreationFailed,
    DerivedPolyDataCreationFailed
};

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
    std::array<double, 6> m_rasBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 16> m_globalOffsetMatrix = GetIdentityMatrixArray();
    std::array<double, 16> m_localAlignmentMatrix = GetIdentityMatrixArray();
    bool m_localAlignmentEnabled = false;
    std::array<double, 3> m_localCenter = { 0.0, 0.0, 0.0 };
    std::array<double, 3> m_localDimensions = { 0.0, 0.0, 0.0 };
};

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
    bool m_cropEnabled = true;
    double m_insideOpacity = 0.35;
    bool m_outsideVisibility = false;
    std::array<double, 3> m_lutColor = { 1.0, 0.4, 0.2 };
    CropHandleId m_activeHandle = CropHandleId::None;
    CropInteractionPhase m_interactionPhase = CropInteractionPhase::Idle;
};

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
