#include "AlignmentDeviationColors.h"
#include <algorithm>
#include <cmath>
namespace AlignmentDeviationColors {
Ramp::Ramp(const AlignmentDeviationDisplay &display,
           const std::optional<std::array<double, 2>> &tolerance) {
  m_parameters.mode = AnalysisColorStyle::RampMode::HueLoop;
  if (display.useToleranceColors && tolerance) {
    m_tolerance = tolerance;
    // 公差内保留绿色；两侧与色标共用既有分段色环，窄公差不靠 LUT 分辨率判定。
    const std::array<double, 3> green{60 / 255.0, 180 / 255.0, 95 / 255.0};
    const auto low = (*tolerance)[0], high = (*tolerance)[1];
    if (display.range[0] < low)
      m_parameters.segments.push_back({display.range[0], low, AnalysisColorStyle::RampMode::HueLoop});
    m_parameters.segments.push_back({low, high, AnalysisColorStyle::RampMode::Constant, green, green});
    if (high < display.range[1])
      m_parameters.segments.push_back({high, display.range[1], AnalysisColorStyle::RampMode::HueLoop});
  }
  m_lookup = AnalysisColorStyle::BuildRamp(display.range, m_parameters);
  m_lookup->SetNanColor(128 / 255.0, 128 / 255.0, 128 / 255.0, 1);
}
Color Ramp::ColorAt(double value) const {
  if (std::isnan(value)) return invalidColor;
  if (m_tolerance && value >= (*m_tolerance)[0] && value <= (*m_tolerance)[1])
    return {60, 180, 95};
  const auto *range=m_lookup->GetRange();
  if(m_parameters.segments.empty() || value<range[0] || value>range[1]) {
    const auto *mapped=m_lookup->MapValue(value);
    return {mapped[0],mapped[1],mapped[2]};
  }
  // 分段已覆盖范围，直接复用其色环公式，避免先取 LUT/透明度后又覆盖 RGB。
  const auto &segment=value<(*m_tolerance)[0] ? m_parameters.segments.front() : m_parameters.segments.back();
  const auto mapped=AnalysisColorStyle::GetSegmentColor(segment,value);
  Color color{};
  for (std::size_t i = 0; i < color.size(); ++i)
    color[i] = static_cast<std::uint8_t>(std::lround(255 * mapped[i]));
  return color;
}
bool GetDisplayValid(const AlignmentDeviationDisplay &d,
                     const AlignmentDeviationSnapshot &r) {
  if (!r.forward || !r.parameters || !GetDataRevisionRefValid(d.result) ||
      d.result != r.result || !std::isfinite(d.range[0]) ||
      !std::isfinite(d.range[1]) || d.range[0] >= d.range[1] ||
      !std::isfinite(d.range[1] - d.range[0]) ||
      (r.parameters->metric == AlignmentDeviationMetric::UnsignedDistance &&
       d.range[0] < 0) ||
      (d.selectedVertex && *d.selectedVertex >= r.forward->samples.size()))
    return false;
  return !d.useToleranceColors || !r.parameters->tolerance ||
         (d.range[0] <= (*r.parameters->tolerance)[0] &&
          d.range[1] >= (*r.parameters->tolerance)[1]);
}
std::array<Color, 257>
GetLegend(const AlignmentDeviationDisplay &display,
          const std::optional<std::array<double, 2>> &tolerance) {
  const Ramp ramp(display, tolerance);
  std::array<Color, 257> result{};
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = ramp.ColorAt(display.range[0] +
                            (display.range[1] - display.range[0]) * (i / 256.0));
  return result;
}
} // namespace AlignmentDeviationColors
