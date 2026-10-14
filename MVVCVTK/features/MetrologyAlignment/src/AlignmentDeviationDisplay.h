#pragma once
#include "AlignmentDeviationOverlay.h"
#include "Host/HostFeature.h"
#include "Render/Contracts/OverlayService.h"
#include <map>

class AlignmentDeviationDisplayController final {
  public:
    ~AlignmentDeviationDisplayController() { Clear(); }
    AlignmentResult Set(TrustedDataPort &data, FeatureViewDirectory *views, const AlignmentConfig &config,
                        const AlignmentDeviationDisplay &request);
    void Clear() noexcept;
    void Reset() noexcept { Clear();m_saved.clear(); }
    std::optional<AlignmentDeviationDisplay> GetSaved(const DataRevisionRef& ref) const {
        const auto i=m_saved.find(ref);return i==m_saved.end() ? std::nullopt:std::optional<AlignmentDeviationDisplay>(i->second);
    }
    void Refresh(const DataGraphSnapshot &graph);
    std::optional<AlignmentDeviationDisplay> GetRequest() const { return m_request; }
    std::optional<std::array<double,2>> GetRange() const {
        return m_product && m_product->hasValues && m_resolved ? std::optional<std::array<double,2>>(m_resolved->range):std::nullopt;
    }
    bool GetReady(const DataGraphSnapshot &graph) const;
    static std::optional<AlignmentDeviationSelection> GetSelection(const DataGraphSnapshot &graph,
        const DataRevisionRef &result, std::uint64_t vertex);
  private:
    struct Binding final {
        std::shared_ptr<OverlayService> port;
        std::shared_ptr<AlignmentDeviationOverlay> overlay;
    };
    std::vector<Binding> m_bindings;
    std::map<DataRevisionRef,AlignmentDeviationDisplay> m_saved;
    std::shared_ptr<const AlignmentDeviationRenderData> m_product;
    vtkSmartPointer<vtkPolyData> m_marker;
    std::optional<AlignmentDeviationDisplay> m_request,m_resolved;
};
