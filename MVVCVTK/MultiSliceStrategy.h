#pragma once
#include "AppInterfaces.h"
#include <vtkActor.h>
#include <vtkVolume.h>
#include <vtkImageSlice.h>
#include <vtkImageResliceMapper.h>
#include <vtkLineSource.h>
#include <vtkPlane.h>
#include <vtkPlaneSource.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCubeAxesActor.h>
#include <vtkFlyingEdges3D.h>
#include <vtkPolyDataMapper.h>


class MultiSliceStrategy : public AbstractVisualStrategy {
public:
    MultiSliceStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params, UpdateFlags flags) override;

private:
    void UpdateAllPositions(int x, int y, int z);

    vtkSmartPointer<vtkImageSlice> m_slices[3];
    vtkSmartPointer<vtkImageResliceMapper> m_mappers[3];
    int m_indices[3] = { 0, 0, 0 };
};