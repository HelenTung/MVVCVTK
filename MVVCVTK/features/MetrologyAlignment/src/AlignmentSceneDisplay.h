#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Host/HostFeature.h"
#include "Render/Contracts/OverlayService.h"
#include "Render/Support/FeatureOverlayBase.h"
class vtkActor;
struct AlignmentSceneSurface final {
  VtkSurfaceMeshSnapshot mesh;
  AlignmentMatrix localToModel = alignmentIdentity;
  std::array<double, 3> color{};
  double opacity = 1;
  bool wireframe = false;
};
struct AlignmentScenePartData final {
  DataRevisionRef alignment, measured, reference;
  AlignmentMatrix sourceToTarget = alignmentIdentity;
};
struct AlignmentSceneProduct final {
  std::shared_ptr<const AlignmentSceneDisplay> request;
  AlignmentMatrix targetToModel = alignmentIdentity;
  AlignmentUnit unit = AlignmentUnit::ModelUnit;
  std::vector<AlignmentSceneSurface> surfaces;
  std::vector<AlignmentScenePartData> parts;
  std::vector<DataExpectation> expectations;
  std::size_t workingBytes = 0;
};
class AlignmentSceneOverlay final : public FeatureOverlayBase {
public:
  explicit AlignmentSceneOverlay(
      std::shared_ptr<const AlignmentSceneProduct> product);
  void SetInputData(vtkSmartPointer<vtkDataObject>) override {}
  void SetOverlayState(const FeatureOverlayState &state) override;

private:
  std::shared_ptr<const AlignmentSceneProduct> m_product;
  std::vector<vtkSmartPointer<vtkActor>> m_actors;
};
class AlignmentSceneDisplayController final {
public:
  ~AlignmentSceneDisplayController() { Clear(); }
  AlignmentResult Set(TrustedDataPort &data, FeatureViewDirectory *views,
                      const AlignmentConfig &config,
                      const AlignmentSceneDisplay &request);
  void Clear() noexcept;
  void Refresh(const DataGraphSnapshot &graph);
  bool GetReady(const DataGraphSnapshot &graph) const;
  std::shared_ptr<const AlignmentSceneDisplay> GetRequest() const {
    return m_product ? m_product->request : nullptr;
  }
  std::size_t GetWorkingBytes() const {
    return m_product ? m_product->workingBytes : 0;
  }
  std::optional<AlignmentSceneVertex> GetVertex(const DataGraphSnapshot &graph,
                                                const std::string &part,
                                                bool reference,
                                                std::uint64_t vertex) const;

private:
  struct Binding {
    std::shared_ptr<OverlayService> port;
    std::shared_ptr<AlignmentSceneOverlay> overlay;
  };
  std::shared_ptr<const AlignmentSceneProduct> m_product;
  std::vector<Binding> m_bindings;
};
