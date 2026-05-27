#pragma once

#include "OrthogonalCrop/OrthogonalCropPluginService.h"

#include <vtkImplicitFunction.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <string>

class OrthogonalCropBackendRouterService {
public:
    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image);
    void SetInputImage(vtkSmartPointer<vtkImageData> image);
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);
    vtkSmartPointer<vtkPolyData> GetInputPolyData() const;

    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource);
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    OrthogonalCropDataSource GetActiveDataSource() const;
    std::array<double, 6> GetActiveInputBounds() const;
    OrthogonalCropRequest GetDefaultRequest() const;
    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const;

    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        vtkImplicitFunction* clipFunction,
        CropRemovalMode removalMode);

    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        const CropDataModel& cropData,
        CropRemovalMode removalMode);

protected:
    static vtkSmartPointer<vtkImplicitFunction> GetClipFunction(const CropDataModel& cropData);

private:
    std::array<double, 6> GetImageBounds() const;
    std::array<double, 6> GetPolyDataBounds() const;
    OrthogonalCropStatistics GetMissingInputStatistics() const;
    OrthogonalCropResult GetMissingInputResult() const;

    bool GetPolyDataCropDataModel(
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message) const;

    OrthogonalCropStatistics GetPolyDataStatisticsFromClipped(vtkPolyData* clipped) const;
    OrthogonalCropStatistics GetImageStatistics(const OrthogonalCropRequest& request) const;
    OrthogonalCropStatistics GetPolyDataStatistics(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetImageResult(const OrthogonalCropRequest& request) const;
    OrthogonalCropResult GetPolyDataResult(const OrthogonalCropRequest& request) const;

    OrthogonalCropPluginService m_imageService;
    vtkSmartPointer<vtkPolyData> m_inputPolyData;
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::Auto;
};
