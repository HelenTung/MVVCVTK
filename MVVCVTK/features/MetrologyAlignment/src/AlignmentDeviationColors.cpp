#include "AlignmentDeviationColors.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace AlignmentDeviationColors {
AnalysisColorStyle::RampParams GetRampParameters(const AlignmentDeviationDisplay& display,
    const std::optional<std::array<double,2>>& tolerance) {
  AnalysisColorStyle::RampParams params;
  params.mode=static_cast<AnalysisColorStyle::RampMode>(display.palette);
  params.isReversed=display.isReversed;params.constantColor=display.constantColor;
  params.lowColor=display.lowColor;params.highColor=display.highColor;
  params.belowColor=display.belowColor;params.aboveColor=display.aboveColor;
  for(const auto& segment:display.segments)params.segments.push_back({segment.lower,segment.upper,
      static_cast<AnalysisColorStyle::RampMode>(segment.mode),segment.lowColor,segment.highColor});
  if(display.useToleranceColors && tolerance){params.tolerance=tolerance;params.toleranceColor={60/255.0,180/255.0,95/255.0};}
  AnalysisColorStyle::SetTailColors(params);return params;
}
Ramp::Ramp(const AlignmentDeviationDisplay& display,const std::optional<std::array<double,2>>& tolerance)
    :m_parameters(GetRampParameters(display,tolerance)),m_range(display.range) {
  if(!AnalysisColorStyle::GetRampValid(m_parameters))throw std::invalid_argument("Invalid deviation color mapping.");
  m_lookup=AnalysisColorStyle::BuildRamp(m_range,m_parameters);
}
Color Ramp::ColorAt(double value) const {
  const auto mapped=AnalysisColorStyle::GetMappedColor(m_range,m_parameters,value);
  Color color{};for(int k=0;k<3;++k)color[k]=static_cast<std::uint8_t>(std::lround(255*mapped[k]));
  return color;
}
std::optional<std::array<double,2>> GetEffectiveRange(const AlignmentDeviationField &field) {
  std::optional<std::array<double,2>> range;
  for(const auto &sample:field.samples) {
    if(sample.state!=AlignmentDeviationState::Valid)continue;
    if(!std::isfinite(sample.value))continue;
    if(!range)range=std::array<double,2>{sample.value,sample.value};
    else {(*range)[0]=std::min((*range)[0],sample.value);(*range)[1]=std::max((*range)[1],sample.value);}
  }
  return range;
}
std::optional<AlignmentDeviationDisplay> GetResolvedDisplay(
    const AlignmentDeviationDisplay &input,const AlignmentDeviationSnapshot &r,const std::optional<std::array<double,2>>* cachedRange) {
  auto d=input;AnalysisColorStyle::SetTailColors(d);
  if (!r.forward || !r.parameters || !GetDataRevisionRefValid(d.result) ||
      d.result != r.result ||
      (d.selectedVertex && *d.selectedVertex >= r.forward->samples.size()))return {};
  const auto actualRange=cachedRange ? *cachedRange:GetEffectiveRange(*r.forward);
  if(d.rangeMode==AlignmentRangeMode::Result) {
    d.range=actualRange.value_or(std::array<double,2>{0,0});
    if(d.useToleranceColors && r.parameters->tolerance && actualRange) {
      d.range[0]=std::min(d.range[0],(*r.parameters->tolerance)[0]);
      d.range[1]=std::max(d.range[1],(*r.parameters->tolerance)[1]);
    }
  } else if(d.rangeMode!=AlignmentRangeMode::Manual)return {};
  if(!AnalysisColorStyle::GetLegendLayoutValid(d))return {};
  if(!std::isfinite(d.range[0]) || !std::isfinite(d.range[1]) || d.range[0]>d.range[1]
      || (input.rangeMode==AlignmentRangeMode::Manual && d.range[0]==d.range[1])
      || !std::isfinite(d.range[1]-d.range[0]) || !std::isfinite(d.opacity) || d.opacity<0 || d.opacity>1
      || (r.parameters->metric==AlignmentDeviationMetric::UnsignedDistance && d.range[0]<0))return {};
  if(d.useToleranceColors && r.parameters->tolerance && actualRange &&
      (d.range[0]>(*r.parameters->tolerance)[0] || d.range[1]<(*r.parameters->tolerance)[1]))return {};
  try { if(!AnalysisColorStyle::GetRampValid(GetRampParameters(d,r.parameters->tolerance)))return {}; } catch(...) { return {}; }
  return d;
}
bool GetDisplayValid(const AlignmentDeviationDisplay &d,const AlignmentDeviationSnapshot &r) {
  return GetResolvedDisplay(d,r).has_value();
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
