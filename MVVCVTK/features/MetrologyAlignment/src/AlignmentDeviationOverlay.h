#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Host/TrustedDataPort.h"
#include "Render/Support/FeatureOverlayBase.h"
#include <chrono>
#include "Render/Support/AnalysisLegend.h"
class vtkActor;
class vtkPolyDataMapper;

class vtkPolyData;
class vtkLookupTable;
struct AlignmentDeviationRenderData final {
    VtkSurfaceMeshSnapshot source;
    std::shared_ptr<const AlignmentDeviationField> field;
    vtkSmartPointer<vtkPolyData> colored,surface,samplePoints,marker;
    bool usesCellColors=false;
    vtkSmartPointer<vtkLookupTable> lookup;
    std::string title;
    std::shared_ptr<const std::vector<std::size_t>> sampleIndices;
    AlignmentDeviationDisplay display;
    AnalysisColorStyle::RampParams ramp;
    bool hasValues=false;
    std::optional<std::array<double,2>> actualRange;
    std::size_t workingBytes = 0;
};
class AlignmentDeviationOverlay final : public FeatureOverlayBase {
  public:
    explicit AlignmentDeviationOverlay(std::shared_ptr<const AlignmentDeviationRenderData> data);
    void SetInputData(vtkSmartPointer<vtkDataObject>) override;
    void SetOverlayState(const FeatureOverlayState &state) override;
    static bool GetDisplayValid(const AlignmentDeviationDisplay &display,
                                const AlignmentDeviationSnapshot &result);
    static std::size_t GetRequiredBytes(std::size_t pointCount);
    static std::shared_ptr<const AlignmentDeviationRenderData> Build(
        VtkSurfaceMeshSnapshot mesh, const AlignmentDeviationSnapshot &result,
        const AlignmentDeviationDisplay &display, const AlignmentDeviationRenderData* previous=nullptr,
        std::optional<vtkSmartPointer<vtkPolyData>> previousMarker=std::nullopt);
    static vtkSmartPointer<vtkPolyData> BuildMarker(VtkSurfaceMeshSnapshot mesh,
        const AlignmentDeviationSnapshot& result,std::optional<std::uint64_t> vertex);
    void SetProduct(std::shared_ptr<const AlignmentDeviationRenderData> product);
    void SetSelection(vtkSmartPointer<vtkPolyData> marker);
    void SetPresentation(const AlignmentDeviationDisplay& display);
  private:
    vtkSmartPointer<vtkActor> m_actor,m_markerActor,m_sampleActor;
    bool m_hasMarker=false;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper,m_markerMapper,m_sampleMapper;
    vtkSmartPointer<AnalysisLegend> m_legend;
    // 保活原桥接 view 及正式场；浅共享顶点/单元，不在源 polydata 上挂接颜色。
    std::shared_ptr<const AlignmentDeviationRenderData> m_data;
};
