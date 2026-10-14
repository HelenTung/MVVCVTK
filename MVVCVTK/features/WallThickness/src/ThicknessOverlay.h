#pragma once
#include "ThicknessData.h"
#include "Render/Support/AnalysisLegend.h"
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
    AnalysisColorStyle::RampParams ramp;
    std::array<double,2> range{};
    bool hasValues=false;
    std::string resultLabel;
};
class ThicknessOverlay final : public FeatureOverlayBase
{
  public:
    static bool GetColorValid(const ThicknessDisplay& display) noexcept;
    static ThicknessDisplayData BuildData(const ThicknessData::Record &record,
                                          const SurfaceMeshPayload &mesh,
                                          const ThicknessDisplay &display, const ThicknessDisplayData* previous=nullptr);
    ThicknessOverlay(ThicknessDisplayData data, const ThicknessDisplay &display, ThicknessUnit unit,
                     HostRenderViewRole role);
    void SetInputData(vtkSmartPointer<vtkDataObject> data) override;
    void SetOverlayState(const FeatureOverlayState &state) override;
    void SetDisplayData(const ThicknessDisplayData& data,const ThicknessDisplay& display);
    void SetSelection(const ThicknessSample *sample);
    std::optional<std::size_t> GetPickedSample(int x, int y, vtkRenderer *renderer);

  private:
    vtkSmartPointer<vtkActor> m_actor, m_selectionActor, m_contourActor;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper, m_selectionMapper;
    vtkSmartPointer<AnalysisLegend> m_legend;
    vtkSmartPointer<vtkCutter> m_cutter;
    vtkSmartPointer<vtkPlane> m_plane;
    std::array<double, 3> m_normal{};
    bool m_isSlice = false;
    std::string m_resultLabel;
    ThicknessUnit m_unit=ThicknessUnit::Unknown;
};
