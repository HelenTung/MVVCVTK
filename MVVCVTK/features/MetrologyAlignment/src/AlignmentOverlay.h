#pragma once
#include "AlignmentData.h"
#include "Render/Support/FeatureOverlayBase.h"

class vtkActor;
class vtkPolyData;
class vtkPolyDataMapper;
class AlignmentOverlay final : public FeatureOverlayBase {
  public:
    AlignmentOverlay();
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(const FeatureOverlayState &state) override;
    static vtkSmartPointer<vtkPolyData> BuildData(const AlignmentMatrix &sourceToTarget,
                                                  const std::vector<AlignmentGeometry> &geometries,
                                                  const AlignmentRecipe &recipe, double axisLength);

  private:
    vtkSmartPointer<vtkActor> m_actor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
};
