#pragma once

#include "OrthogonalCrop/OrthogonalCropPluginService.h"

#include <vtkBox.h>
#include <vtkExtractVOI.h>
#include <vtkGeometryFilter.h>
#include <vtkPolyData.h>
#include <vtkTableBasedClipDataSet.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

class IOrthogonalCropBackendService {
public:
    virtual ~IOrthogonalCropBackendService() = default;

    virtual void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image) = 0;
    virtual void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData) = 0;
    virtual void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource) = 0;
    virtual OrthogonalCropDataSource GetActiveDataSource() const = 0;
    virtual std::array<double, 6> GetActiveInputBounds() const = 0;
    virtual OrthogonalCropRequest GetDefaultRequest() const = 0;
    virtual OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const = 0;
    virtual OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const = 0;
};

class OrthogonalCropBackendRouterService : public IOrthogonalCropBackendService {
public:
    void CropPreInit_SetInputImage(vtkSmartPointer<vtkImageData> image) override
    {
        m_imageService.SetInputImage(std::move(image));
    }

    void SetInputImage(vtkSmartPointer<vtkImageData> image)
    {
        CropPreInit_SetInputImage(std::move(image));
    }

    vtkSmartPointer<vtkImageData> GetInputImage() const
    {
        return m_imageService.GetInputImage();
    }

    void CropPreInit_SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData) override
    {
        m_inputPolyData = std::move(polyData);
    }

    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData)
    {
        CropPreInit_SetInputPolyData(std::move(polyData));
    }

    vtkSmartPointer<vtkPolyData> GetInputPolyData() const
    {
        return m_inputPolyData;
    }

    void CropPreInit_SetPreferredDataSource(OrthogonalCropDataSource dataSource) override
    {
        m_preferredDataSource = dataSource;
    }

    void SetPreferredDataSource(OrthogonalCropDataSource dataSource)
    {
        CropPreInit_SetPreferredDataSource(dataSource);
    }

    OrthogonalCropDataSource GetActiveDataSource() const override
    {
        const bool hasImage = GetInputImage() != nullptr;
        const bool hasPolyData = m_inputPolyData != nullptr;

        if (m_preferredDataSource == OrthogonalCropDataSource::PolyData && hasPolyData) {
            return OrthogonalCropDataSource::PolyData;
        }

        if (m_preferredDataSource == OrthogonalCropDataSource::ImageData && hasImage) {
            return OrthogonalCropDataSource::ImageData;
        }

        if (hasImage) {
            return OrthogonalCropDataSource::ImageData;
        }

        if (hasPolyData) {
            return OrthogonalCropDataSource::PolyData;
        }

        return OrthogonalCropDataSource::Auto;
    }

    std::array<double, 6> GetActiveInputBounds() const override
    {
        std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        switch (GetActiveDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            return GetImageBounds();
        case OrthogonalCropDataSource::PolyData:
            return GetPolyDataBounds();
        default:
            return bounds;
        }
    }

    OrthogonalCropRequest GetDefaultRequest() const override
    {
        if (GetActiveDataSource() == OrthogonalCropDataSource::ImageData) {
            return m_imageService.GetDefaultRequest();
        }

        OrthogonalCropRequest request;
        request.SetBoundsMode(CropBoundsMode::InputVolumeBounds);
        request.SetExecutionMode(CropExecutionMode::VirtualCrop);
        request.SetRemovalMode(CropRemovalMode::KeepInside);
        request.SetGlobalOffsetMatrix(GetIdentityMatrixArray());
        request.SetLocalAlignmentMatrix(GetIdentityMatrixArray());
        request.SetLocalCenter({ 0.0, 0.0, 0.0 });

        const auto bounds = GetActiveInputBounds();
        request.SetRasBounds(bounds);
        request.SetCenter({
            (bounds[0] + bounds[1]) * 0.5,
            (bounds[2] + bounds[3]) * 0.5,
            (bounds[4] + bounds[5]) * 0.5
        });
        request.SetDimensions({
            bounds[1] - bounds[0],
            bounds[3] - bounds[2],
            bounds[5] - bounds[4]
        });
        request.SetLocalDimensions(request.GetDimensions());
        return request;
    }

    OrthogonalCropStatistics GetStatistics(const OrthogonalCropRequest& request) const override
    {
        switch (GetActiveDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            return GetImageStatistics(request);
        case OrthogonalCropDataSource::PolyData:
            return GetPolyDataStatistics(request);
        default:
            return GetMissingInputStatistics();
        }
    }

    OrthogonalCropResult GetResult(const OrthogonalCropRequest& request) const override
    {
        switch (GetActiveDataSource()) {
        case OrthogonalCropDataSource::ImageData:
            return GetImageResult(request);
        case OrthogonalCropDataSource::PolyData:
            return GetPolyDataResult(request);
        default:
            return GetMissingInputResult();
        }
    }

protected:
    static vtkSmartPointer<vtkImageData> GetExtractedImage(
        vtkImageData* image,
        const std::array<int, 6>& ijkBounds)
    {
        if (!image) {
            return nullptr;
        }

        auto extract = vtkSmartPointer<vtkExtractVOI>::New();
        extract->SetInputData(image);
        extract->SetVOI(
            ijkBounds[0], ijkBounds[1],
            ijkBounds[2], ijkBounds[3],
            ijkBounds[4], ijkBounds[5]);
        extract->Update();

        auto output = vtkSmartPointer<vtkImageData>::New();
        output->ShallowCopy(extract->GetOutput());
        return output;
    }

    static vtkSmartPointer<vtkPolyData> GetClippedPolyData(
        vtkPolyData* polyData,
        const CropDataModel& cropData,
        CropRemovalMode removalMode)
    {
        if (!polyData) {
            return nullptr;
        }

        auto box = vtkSmartPointer<vtkBox>::New();
        const auto rasBounds = cropData.GetRasBounds();
        box->SetBounds(
            rasBounds[0], rasBounds[1],
            rasBounds[2], rasBounds[3],
            rasBounds[4], rasBounds[5]);

        auto clip = vtkSmartPointer<vtkTableBasedClipDataSet>::New();
        clip->SetInputData(polyData);
        clip->SetClipFunction(box);
        if (removalMode == CropRemovalMode::KeepInside) {
            clip->InsideOutOn();
        }
        else {
            clip->InsideOutOff();
        }
        clip->Update();

        auto geometry = vtkSmartPointer<vtkGeometryFilter>::New();
        geometry->SetInputConnection(clip->GetOutputPort());
        geometry->Update();

        auto output = vtkSmartPointer<vtkPolyData>::New();
        output->ShallowCopy(geometry->GetOutput());
        return output;
    }

private:
    std::array<double, 6> GetImageBounds() const
    {
        std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        auto image = GetInputImage();
        if (!image) {
            return bounds;
        }

        double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        image->GetBounds(rawBounds);
        return {
            rawBounds[0], rawBounds[1],
            rawBounds[2], rawBounds[3],
            rawBounds[4], rawBounds[5]
        };
    }

    std::array<double, 6> GetPolyDataBounds() const
    {
        std::array<double, 6> bounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        if (!m_inputPolyData) {
            return bounds;
        }

        double rawBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
        m_inputPolyData->GetBounds(rawBounds);
        return {
            rawBounds[0], rawBounds[1],
            rawBounds[2], rawBounds[3],
            rawBounds[4], rawBounds[5]
        };
    }

    OrthogonalCropStatistics GetMissingInputStatistics() const
    {
        OrthogonalCropStatistics statistics;
        statistics.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        statistics.SetValidationMessage("No active crop input data is bound.");
        return statistics;
    }

    OrthogonalCropResult GetMissingInputResult() const
    {
        OrthogonalCropResult result;
        result.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
        result.SetMessage("No active crop input data is bound.");
        return result;
    }

    bool GetPolyDataCropDataModel(
        const OrthogonalCropRequest& request,
        CropDataModel& cropData,
        OrthogonalCropFailureReason& failureReason,
        std::string& message) const
    {
        if (!m_inputPolyData) {
            failureReason = OrthogonalCropFailureReason::InputPolyDataMissing;
            message = "Input polydata is null.";
            return false;
        }

        return OrthogonalCropAlgorithm::GetCropDataModel(
            GetPolyDataBounds(),
            request,
            cropData,
            failureReason,
            message);
    }

    OrthogonalCropStatistics GetImageStatistics(const OrthogonalCropRequest& request) const
    {
        auto statistics = m_imageService.GetStatistics(request);
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);
        if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop
            && request.GetRemovalMode() == CropRemovalMode::KeepInside) {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        }
        else if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
        }
        else {
            statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        }

        return statistics;
    }

    OrthogonalCropStatistics GetPolyDataStatistics(const OrthogonalCropRequest& request) const
    {
        OrthogonalCropStatistics statistics;
        statistics.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        statistics.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);

        if (!m_inputPolyData) {
            statistics.SetFailureReason(OrthogonalCropFailureReason::InputPolyDataMissing);
            statistics.SetValidationMessage("Input polydata is null.");
            return statistics;
        }

        CropDataModel cropData;
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string message;
        if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
            statistics.SetFailureReason(failureReason);
            statistics.SetValidationMessage(message);
            return statistics;
        }

        auto clipped = GetClippedPolyData(m_inputPolyData, cropData, request.GetRemovalMode());
        if (!clipped) {
            statistics.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
            statistics.SetValidationMessage("Failed to build clipped polydata.");
            return statistics;
        }

        statistics.SetFailureReason(OrthogonalCropFailureReason::None);
        statistics.SetTotalVoxelCount(static_cast<std::size_t>(m_inputPolyData->GetNumberOfCells()));
        statistics.SetInsideVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
        statistics.SetOutputVoxelCount(static_cast<std::size_t>(clipped->GetNumberOfCells()));
        statistics.SetCanExecutePhysicalCrop(true);
        return statistics;
    }

    OrthogonalCropResult GetImageResult(const OrthogonalCropRequest& request) const
    {
        auto result = m_imageService.GetResult(request);
        result.SetResolvedDataSource(OrthogonalCropDataSource::ImageData);

        if (!result.GetSucceeded()) {
            if (request.GetExecutionMode() == CropExecutionMode::VirtualCrop) {
                result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
            }
            return result;
        }

        const bool useExtractPreview = request.GetExecutionMode() == CropExecutionMode::PhysicalCrop
            || request.GetRemovalMode() == CropRemovalMode::KeepInside;
        if (!useExtractPreview) {
            result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageVirtualMask);
            return result;
        }

        auto image = GetInputImage();
        if (!image) {
            result.SetFailureReason(OrthogonalCropFailureReason::InputImageMissing);
            result.SetMessage("Input image is null.");
            result.SetSucceeded(false);
            return result;
        }

        const auto snappedIjkBounds = result.GetStatistics().GetSnappedIjkBounds();
        auto extractedImage = GetExtractedImage(image, snappedIjkBounds);
        if (!extractedImage) {
            result.SetFailureReason(OrthogonalCropFailureReason::DerivedImageCreationFailed);
            result.SetMessage("Failed to build extracted preview image.");
            result.SetSucceeded(false);
            return result;
        }

        result.SetDerivedImage(extractedImage);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::ImageExtractVOI);
        return result;
    }

    OrthogonalCropResult GetPolyDataResult(const OrthogonalCropRequest& request) const
    {
        OrthogonalCropResult result;
        result.SetResolvedDataSource(OrthogonalCropDataSource::PolyData);
        result.SetResolvedBackend(OrthogonalCropResolvedBackend::PolyDataClipDataSet);
        result.SetCropStateModel(request.GetCropStateModel());

        const auto statistics = GetPolyDataStatistics(request);
        result.SetStatistics(statistics);
        result.SetFailureReason(statistics.GetFailureReason());
        if (statistics.GetFailureReason() != OrthogonalCropFailureReason::None) {
            result.SetMessage(statistics.GetValidationMessage());
            return result;
        }

        CropDataModel cropData;
        OrthogonalCropFailureReason failureReason = OrthogonalCropFailureReason::None;
        std::string message;
        if (!GetPolyDataCropDataModel(request, cropData, failureReason, message)) {
            result.SetFailureReason(failureReason);
            result.SetMessage(message);
            return result;
        }

        auto clipped = GetClippedPolyData(m_inputPolyData, cropData, request.GetRemovalMode());
        if (!clipped) {
            result.SetFailureReason(OrthogonalCropFailureReason::DerivedPolyDataCreationFailed);
            result.SetMessage("Failed to build clipped polydata.");
            return result;
        }

        result.SetCropDataModel(cropData);
        result.SetOutlinePolyData(OrthogonalCropInternal::GetOutlinePolyData(cropData));
        result.SetDerivedPolyData(clipped);
        result.SetSucceeded(true);
        result.SetFailureReason(OrthogonalCropFailureReason::None);
        return result;
    }

    OrthogonalCropPluginService m_imageService;
    vtkSmartPointer<vtkPolyData> m_inputPolyData;
    OrthogonalCropDataSource m_preferredDataSource = OrthogonalCropDataSource::Auto;
};
