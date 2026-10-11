#pragma once
#include "Host/MetrologyAlignmentHostTypes.h"
#include "Render/Support/AnalysisColorStyle.h"
namespace AlignmentDeviationColors {
using Color = std::array<std::uint8_t, 3>;
inline constexpr Color invalidColor{128, 128, 128}, selectedColor{255, 255, 0};
inline constexpr const char *version = "deviation-hue-loop-2";
class Ramp final {
public:
  Ramp(const AlignmentDeviationDisplay &display,
       const std::optional<std::array<double, 2>> &tolerance);
  Color ColorAt(double value) const;
  vtkSmartPointer<vtkLookupTable> GetLookup() const { return m_lookup; }
private:
  AnalysisColorStyle::RampParams m_parameters;
  vtkSmartPointer<vtkLookupTable> m_lookup;
  std::optional<std::array<double, 2>> m_tolerance;
};
bool GetDisplayValid(const AlignmentDeviationDisplay &display,
                     const AlignmentDeviationSnapshot &result);
std::optional<std::array<double,2>> GetEffectiveRange(const AlignmentDeviationField &field);
std::optional<AlignmentDeviationDisplay> GetResolvedDisplay(
    const AlignmentDeviationDisplay &display,const AlignmentDeviationSnapshot &result);
std::array<Color, 257>
GetLegend(const AlignmentDeviationDisplay &display,
          const std::optional<std::array<double, 2>> &tolerance);
} // namespace AlignmentDeviationColors
