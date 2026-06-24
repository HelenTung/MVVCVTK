#pragma once

#include "AppInterfaces.h"
#include "OrthogonalCropPreviewOverlayStrategy.h"
#include "OrthogonalCropTypes.h"

#include <vtkSmartPointer.h>

#include <memory>
#include <unordered_map>

class vtkActor;
class vtkGPUVolumeRayCastMapper;
class vtkPlaneCollection;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkVolume;
class vtkVolumeMapper;

// OrthogonalCropPreviewPlugService 只拥有 preview 期间的 VTK 显示状态。
// request 构造和裁切数据处理交给 router / algorithm；本类只应用已经分发好的预览结果。
class OrthogonalCropPreviewPlugService {
public:
    bool ApplyPreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy,
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult* imagePreviewResult,
        const OrthogonalCropResult* polyDataPreviewResult,
        CropRemovalMode removalMode);

    void RestorePreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy);

    void Clear();

private:
    struct TargetState {
        // 非拥有引用：记录当前带有 preview-only clipping 状态的 mapper。
        // RemoveInside shader 状态挂在 actor 上，恢复时单独清理。
        vtkPolyDataMapper* mainPreviewMapper = nullptr;
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
        const std::shared_ptr<AbstractInteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    std::unordered_map<AbstractInteractiveService*, TargetState> m_targetStates;
};
