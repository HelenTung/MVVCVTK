#pragma once

#include "App/ViewTypes.h"
#include "Render/Support/FeatureOverlayBase.h"

#include <vtkSmartPointer.h>

#include <array>

class vtkActor;
class vtkImageData;
class vtkImageResliceMapper;
class vtkImageSlice;
class vtkLookupTable;
class vtkPlane;
class vtkPolyDataMapper;

class PartSurfaceOverlayStrategy final : public FeatureOverlayBase {
public:
    PartSurfaceOverlayStrategy();

    void SetInputData(
        vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(
        const FeatureOverlayState& state) override;

private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
    vtkSmartPointer<vtkLookupTable> m_lut;
};

class PartSliceOverlayStrategy final : public FeatureOverlayBase {
public:
    explicit PartSliceOverlayStrategy(Orientation orientation);

    void SetInputData(
        vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(
        const FeatureOverlayState& state) override;

private:
    void SetLookupTable(vtkImageData& image);

    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    vtkSmartPointer<vtkLookupTable> m_lut;
    vtkSmartPointer<vtkPlane> m_plane;
    std::array<double, 3> m_normal{ 0.0, 0.0, 1.0 };
    Orientation m_orientation;
};
