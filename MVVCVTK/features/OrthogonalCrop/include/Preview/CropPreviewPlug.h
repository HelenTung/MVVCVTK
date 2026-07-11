#pragma once

#include "AppInterfaces.h"
#include "Render/Strategies/CropOverlay.h"
#include "OrthogonalCropTypes.h"

#include <vtkSmartPointer.h>

#include <memory>

class vtkActor;
class vtkAbstractMapper;
class vtkGPUVolumeRayCastMapper;
class vtkPlaneCollection;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkVolume;
class vtkVolumeMapper;

// CropPreviewPlug 只拥有 preview 期间的 VTK 显示状态。
// request 构造和裁切数据处理交给 router / algorithm；本类只应用已经分发好的预览结果。
class CropPreviewPlug {
public:
    // targetService 与 overlayStrategy 表示同一 preview 目标；referenceService 只提供
    // active input model -> world 的几何参照。两个 result 指针均为本次调用的非拥有可选输入，
    // 本类不跨调用保存它们；返回 true 仅表示 volume 或 mesh 主显示已成功接管。
    bool SetPreview(
        const std::shared_ptr<InteractiveService>& targetService,
        const std::shared_ptr<CropOverlay>& overlayStrategy,
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult* volumePreviewResult,
        const OrthogonalCropResult* polyDataPreviewResult,
        CropRemovalMode removalMode);

    // 清空本目标的 overlay 内容、mapper clipping planes 与 shader 临时状态，恢复未裁切主显示。
    void ResetPreview(
        const std::shared_ptr<InteractiveService>& targetService,
        const std::shared_ptr<CropOverlay>& overlayStrategy);

    vtkSmartPointer<vtkPolyData> GetPreviewData(
        const std::shared_ptr<InteractiveService>& targetService) const;

private:
    void ResetVolumeView(vtkVolume* volume, vtkVolumeMapper* volumeMapper) const;

    void ClearVolumeCut(vtkVolume* volume, vtkVolumeMapper* volumeMapper) const;

    vtkSmartPointer<vtkPlaneCollection> BuildBoxClip(
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    vtkSmartPointer<vtkPlaneCollection> BuildPlaneClip(
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    // mapper 是本次调用的非拥有显示目标；referenceService 把 result 中的 active input model
    // 几何提升到 world。成功后 mapper 持有生成的 plane collection，Reset*View 统一移除。
    bool SetKeepClip(
        vtkAbstractMapper* mapper,
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    bool SetVolumeView(
        const std::shared_ptr<InteractiveService>& targetService,
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    bool SetVolumeRemove(
        vtkVolume* volume,
        vtkGPUVolumeRayCastMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    bool SetVolumePlane(
        vtkVolume* volume,
        vtkGPUVolumeRayCastMapper* volumeMapper,
        const OrthogonalCropResult& previewResult) const;

    void ResetMeshView(const std::shared_ptr<InteractiveService>& targetService);

    bool SetMeshView(
        const std::shared_ptr<InteractiveService>& targetService,
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult,
        CropRemovalMode removalMode);

    bool SetMeshRemove(
        vtkActor* actor,
        vtkPolyDataMapper* mapper,
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

    bool SetMeshPlaneCut(
        vtkActor* actor,
        vtkPolyDataMapper* mapper,
        const std::shared_ptr<InteractiveService>& referenceService,
        const OrthogonalCropResult& previewResult) const;

};
