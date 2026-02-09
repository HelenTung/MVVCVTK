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

class ColoredPlanesStrategy : public AbstractVisualStrategy {
public:
    ColoredPlanesStrategy();

    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void Attach(vtkSmartPointer<vtkRenderer> renderer) override;
    void Detach(vtkSmartPointer<vtkRenderer> renderer) override;
    void UpdateVisuals(const RenderParams& params, UpdateFlags flags) override;
    int GetPlaneAxis(vtkActor* actor) override;

private:
    void UpdateAllPositions(int x, int y, int z);
    vtkSmartPointer<vtkActor> m_planeActors[3];
    vtkSmartPointer<vtkPlaneSource> m_planeSources[3];
    vtkSmartPointer<vtkImageData> m_imageData;
};