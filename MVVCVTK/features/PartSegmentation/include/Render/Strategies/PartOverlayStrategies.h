#pragma once

#include "App/ViewTypes.h"
#include "Render/PartRenderStateTable.h"
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
class vtkPolyDataNormals;
class vtkThreshold;
class vtkGeometryFilter;
class vtkPolyDataSilhouette;
class vtkImageReslice;
class vtkImageThreshold;
class vtkFlyingEdges2D;
class vtkImageMathematics;

class PartSurfaceOverlayStrategy final
    : public FeatureOverlayBase
    , public PartOverlayControl {
public:
    explicit PartSurfaceOverlayStrategy(bool isSelectionOnly = false, bool isPreview = false);

    void SetInputData(
        vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(
        const FeatureOverlayState& state) override;
    bool SetPartStates(
        const PartRenderStateTable& states) noexcept override;
    std::optional<PartLabelId> GetPickedLabel(int x, int y, vtkRenderer* renderer) const override;
    void AttachRenderer(vtkSmartPointer<vtkRenderer> renderer) override;
    void DetachRenderer(vtkSmartPointer<vtkRenderer> renderer) override;

private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
    vtkSmartPointer<vtkPolyDataNormals> m_normals;
    vtkSmartPointer<vtkLookupTable> m_lut;
    PartRenderStateTable m_partStates;
    PartRenderStateTable m_pickStates;
    vtkSmartPointer<vtkLookupTable> m_pickLut;
    bool m_isSelectionOnly = false;
    bool m_isPreview = false;
    vtkSmartPointer<vtkThreshold> m_selection;
    vtkSmartPointer<vtkGeometryFilter> m_geometry;
    vtkSmartPointer<vtkPolyDataSilhouette> m_outline;
    vtkSmartPointer<vtkActor> m_outlineActor;
};

class PartSliceOverlayStrategy final
    : public FeatureOverlayBase
    , public PartOverlayControl {
public:
    explicit PartSliceOverlayStrategy(Orientation orientation, vtkImageData* previous = nullptr);

    void SetInputData(
        vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(
        const FeatureOverlayState& state) override;
    bool SetPartStates(
        const PartRenderStateTable& states) noexcept override;
    std::optional<PartLabelId> GetPickedLabel(int x, int y, vtkRenderer* renderer) const override;

private:
    vtkSmartPointer<vtkImageSlice> m_slice;
    vtkSmartPointer<vtkImageResliceMapper> m_mapper;
    vtkSmartPointer<vtkLookupTable> m_lut;
    PartRenderStateTable m_partStates;
    vtkSmartPointer<vtkPlane> m_plane;
    std::array<double, 3> m_normal{ 0.0, 0.0, 1.0 };
    Orientation m_orientation;
    vtkSmartPointer<vtkImageReslice> m_reslice;
    vtkSmartPointer<vtkImageThreshold> m_selection;
    vtkSmartPointer<vtkFlyingEdges2D> m_outline;
    vtkSmartPointer<vtkActor> m_outlineActor;
    vtkSmartPointer<vtkImageReslice> m_previousReslice;
    vtkSmartPointer<vtkImageMathematics> m_difference;
};
