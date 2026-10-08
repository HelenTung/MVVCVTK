#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Host/TrustedDataPort.h"
#include "Render/Support/FeatureOverlayBase.h"
#include <chrono>

class vtkPolyData;
class vtkLookupTable;
struct AlignmentDeviationRenderData final {
    VtkSurfaceMeshSnapshot source;
    std::shared_ptr<const AlignmentDeviationField> field;
    vtkSmartPointer<vtkPolyData> colored, marker;
    vtkSmartPointer<vtkLookupTable> lookup;
    std::string title;
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
        const AlignmentDeviationDisplay &display, std::size_t availableBytes,
        std::chrono::steady_clock::time_point deadline);
  private:
    // 保活原桥接 view 及正式场；浅共享顶点/单元，不在源 polydata 上挂接颜色。
    std::shared_ptr<const AlignmentDeviationRenderData> m_data;
};
