#pragma once

#include "AppInterfaces.h"
#include "OrthogonalCropPreviewOverlayStrategy.h"
#include "OrthogonalCropTypes.h"

#include <vtkSmartPointer.h>

#include <memory>
#include <unordered_map>

class vtkActor;
class vtkAlgorithmOutput;
class vtkGPUVolumeRayCastMapper;
class vtkPlaneCollection;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkVolume;
class vtkVolumeMapper;

// OrthogonalCropPreviewPlugService owns preview-time VTK display state.
// It does not build requests or process crop data; router/algo produce OrthogonalCropResult,
// and this plug maps that result onto overlay, mapper, shader, and volume preview state.
class OrthogonalCropPreviewPlugService {
public:
    bool ApplyPreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy,
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    void RestorePreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy);

    void Clear();

private:
    struct TargetState {
        vtkPolyDataMapper* mainPreviewMapper = nullptr;
        vtkAlgorithmOutput* mainPreviewInputConnection = nullptr;
        vtkSmartPointer<vtkPolyData> mainPreviewInputData;
    };

    vtkSmartPointer<vtkPlaneCollection> BuildWorldClippingPlanes(
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    bool ApplyVolumePreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    void ApplyVolumeKeepInsidePreview(
        vtkVolumeMapper* volumeMapper,
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    bool ApplyVolumeRemoveInsidePreview(
        vtkVolume* volume,
        vtkGPUVolumeRayCastMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    void RestorePolyDataPreview(const std::shared_ptr<AbstractInteractiveService>& targetService);

    bool ApplyPolyDataPreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    void ApplyPolyDataKeepInsidePreview(
        vtkPolyDataMapper* mapper,
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    bool ApplyPolyDataRemoveInsidePreview(
        vtkActor* actor,
        vtkPolyDataMapper* mapper,
        const OrthogonalCropResult& previewResult) const;

    std::unordered_map<AbstractInteractiveService*, TargetState> m_targetStates;
};
