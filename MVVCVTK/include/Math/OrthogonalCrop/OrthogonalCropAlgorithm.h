#pragma once

// =====================================================================
// OrthogonalCropAlgorithm.h - 正交裁切独立插件纯算法层
// =====================================================================

#include "OrthogonalCropTypes.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cstddef>
#include <string>

class OrthogonalCropAlgorithm {
public:
    static bool GetBoundsAreValid(
        const std::array<double, 6>& inputBounds,
        const std::array<double, 6>& rasBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    static bool GetBoundsAreValid(
        vtkImageData* image,
        const std::array<double, 6>& rasBounds,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    static bool GetCropDataModel(
        const std::array<double, 6>& inputBounds,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    static bool GetCropDataModel(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message,
        bool allowPartialOverlap = false);

    static std::array<int, 6> GetSnappedVoxelBounds(vtkImageData* image, const CropDataModel& cropData);
    static vtkSmartPointer<vtkPolyData> GetOutlinePolyData(const CropDataModel& cropData);

    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const CropDataModel& cropData,
        CropRemovalMode removalMode,
        CropExecutionMode executionMode,
        std::size_t availableRamBytes = 0);

    static OrthogonalCropStatistics GetStatistics(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);

    static OrthogonalCropResult GetVirtualCropResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const CropStateModel& cropState,
        CropRemovalMode removalMode);

    static OrthogonalCropResult GetPhysicalCropResult(
        vtkImageData* image,
        const CropDataModel& cropData,
        const CropStateModel& cropState,
        CropRemovalMode removalMode,
        std::size_t availableRamBytes = 0);

    static OrthogonalCropResult GetResult(
        vtkImageData* image,
        const OrthogonalCropRequest& request,
        std::size_t fallbackAvailableRamBytes = 0);
};
