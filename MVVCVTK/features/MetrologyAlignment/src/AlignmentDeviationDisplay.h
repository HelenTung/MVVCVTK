#pragma once
#include "AlignmentDeviationOverlay.h"
#include "Host/HostFeature.h"
#include "Render/Contracts/OverlayService.h"

class AlignmentDeviationDisplayController final {
  public:
    ~AlignmentDeviationDisplayController() { Clear(); }
    AlignmentResult Set(TrustedDataPort &data, FeatureViewDirectory *views, const AlignmentConfig &config,
                        const AlignmentDeviationDisplay &request);
    void Clear() noexcept;
    void Refresh(const DataGraphSnapshot &graph);
    std::optional<AlignmentDeviationDisplay> GetRequest() const { return m_request; }
    bool GetReady(const DataGraphSnapshot &graph) const;
    static std::optional<AlignmentDeviationSelection> GetSelection(const DataGraphSnapshot &graph,
        const DataRevisionRef &result, std::uint64_t vertex);
  private:
    struct Binding final {
        std::shared_ptr<OverlayService> port;
        std::shared_ptr<AlignmentDeviationOverlay> overlay;
    };
    std::vector<Binding> m_bindings;
    std::shared_ptr<const AlignmentDeviationRenderData> m_product;
    std::optional<AlignmentDeviationDisplay> m_request;
    std::size_t m_viewBytes = 0;
};
