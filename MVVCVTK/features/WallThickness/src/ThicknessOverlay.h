#pragma once
#include "ThicknessData.h"
#include "Render/Support/FeatureOverlayBase.h"
#include <vtkSmartPointer.h>

class vtkActor;
class vtkCutter;
class vtkPlane;
class vtkPolyDataMapper;
class vtkLookupTable;
class vtkScalarBarActor;
class vtkLegendBoxActor;
class vtkPolyData;

struct ThicknessDisplayData final
{
    vtkSmartPointer<vtkPolyData> mesh;
    vtkSmartPointer<vtkLookupTable> lookup;
};
class ThicknessOverlay final : public FeatureOverlayBase
{
  public:
    static ThicknessDisplayData BuildData(const ThicknessData::Record &record,
                                          const SurfaceMeshPayload &mesh,
                                          const ThicknessDisplay &display);
    ThicknessOverlay(ThicknessDisplayData data, const ThicknessDisplay &display, ThicknessUnit unit,
                     HostRenderViewRole role);
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(const FeatureOverlayState &state) override;
    void SetSelection(const ThicknessSample *sample);
    std::optional<std::size_t> GetPickedSample(int x, int y, vtkRenderer *renderer);

  private:
    vtkSmartPointer<vtkActor> m_actor, m_lineActor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper, m_lineMapper;
    vtkSmartPointer<vtkScalarBarActor> m_legend;
    vtkSmartPointer<vtkLegendBoxActor> m_invalidLegend;
    vtkSmartPointer<vtkCutter> m_cutter;
    vtkSmartPointer<vtkPlane> m_plane;
    std::array<double, 3> m_normal{};
    bool m_isSlice = false;
};
