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
        const OrthogonalCropResult* volumePreviewResult,
        const OrthogonalCropResult* polyDataPreviewResult,
        CropRemovalMode removalMode);

    void RestorePreview(
        const std::shared_ptr<AbstractInteractiveService>& targetService,
        const std::shared_ptr<OrthogonalCropPreviewOverlayStrategy>& overlayStrategy);

    void Clear();

private:
    struct TargetState {
        // 非拥有引用：只记录上次被 preview 接管的 mapper 身份。
        // restore 时必须重新从当前 actor 取 mapper，reload/rebuild 可能已释放旧 mapper。
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
