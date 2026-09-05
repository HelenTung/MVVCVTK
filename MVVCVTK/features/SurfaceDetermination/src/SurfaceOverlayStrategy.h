#pragma once

#include "Render/Support/FeatureOverlayBase.h"

#include <vtkSmartPointer.h>

#include <array>

class vtkActor;
class vtkCutter;
class vtkPlane;
class vtkPolyDataMapper;

class SurfaceOverlayStrategy final : public FeatureOverlayBase {
public:
    SurfaceOverlayStrategy();

    void SetInputData(
        vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(
        const FeatureOverlayState& state) override;

private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
};

class SurfaceSliceOverlayStrategy final : public FeatureOverlayBase {
public:
    explicit SurfaceSliceOverlayStrategy(
        std::array<double, 3> normalModel);

    void SetInputData(
        vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(
        const FeatureOverlayState& state) override;

private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkCutter> m_cutter;
    vtkSmartPointer<vtkPlane> m_plane;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
    std::array<double, 3> m_normalModel{ 0.0, 0.0, 1.0 };
};
