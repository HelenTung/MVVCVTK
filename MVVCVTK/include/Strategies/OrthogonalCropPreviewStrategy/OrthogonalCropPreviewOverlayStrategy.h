#pragma once

#include "BaseVisualStrategy.h"
#include "OrthogonalCrop/OrthogonalCropTypes.h"

#include <vtkActor.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkLookupTable.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp3D.h>

class OrthogonalCropPreviewOverlayStrategy : public BaseVisualStrategy {
public:
    OrthogonalCropPreviewOverlayStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetSliceAxis(int axis);
    void SetRemovalMode(CropRemovalMode removalMode);
    void SetCropResult(const OrthogonalCropResult& result);
    void ClearPreview();
    void SetVisualState(const RenderParams& params, UpdateFlags flags) override;

private:
    void UpdateVisiblePreviewProps();
    void ApplyRemovalVisualStyle();
    static void SetPropTransform(vtkProp3D* prop, const std::array<double, 16>& matrixData);

    vtkSmartPointer<vtkActor> m_previewRegionActor;
    vtkSmartPointer<vtkPolyDataMapper> m_previewRegionMapper;
    vtkSmartPointer<vtkActor> m_outlineActor;
    vtkSmartPointer<vtkPolyDataMapper> m_outlineMapper;
    vtkSmartPointer<vtkActor> m_polyDataActor;
    vtkSmartPointer<vtkPolyDataMapper> m_polyDataMapper;
    vtkSmartPointer<vtkImageSlice> m_maskSlice;
    vtkSmartPointer<vtkImageResliceMapper> m_maskMapper;
    vtkSmartPointer<vtkLookupTable> m_maskLut;
    vtkSmartPointer<vtkPlane> m_slicePlane;
    bool m_hasOutline = false;
    bool m_hasMaskImage = false;
    CropRemovalMode m_removalMode = CropRemovalMode::KeepInside;
    int m_sliceAxis = -1;
};
