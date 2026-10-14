#include "AlignmentReportDisplay.h"
#include "AlignmentDeviationColors.h"
#include "AlignmentMath.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace AlignmentReportDisplay {
MetrologyReportDeviationDisplay
BuildDeviation(const AlignmentDeviationDisplay &display,
               const AlignmentDeviationSnapshot &result,
               const std::optional<AlignmentPoint> &sourcePoint) {
  const auto resolved=AlignmentDeviationColors::GetResolvedDisplay(display,result);
  if (!resolved)
    throw std::invalid_argument("Invalid report colorbar configuration.");
  MetrologyReportDeviationDisplay output;
  output.parameters=display;AnalysisColorStyle::SetTailColors(output.parameters);
  output.mappingRange=AlignmentDeviationColors::GetEffectiveRange(*result.forward) ? std::optional<std::array<double,2>>(resolved->range):std::nullopt;
  output.legendRgb = AlignmentDeviationColors::GetLegend(
      *resolved, result.parameters->tolerance);
  if (display.selectedVertex) {
    if (!sourcePoint ||
        !AlignmentMath::Finite(AlignmentMath::Vector(*sourcePoint)))
      throw std::invalid_argument("Selected report point is unavailable.");
    AlignmentDeviationSelection selected;
    selected.result = result.result;
    selected.vertex = *display.selectedVertex;
    selected.sourcePoint = *sourcePoint;
    selected.alignedPoint = AlignmentMath::Point(AlignmentMath::Transform(
        result.sourceToTarget, AlignmentMath::Vector(*sourcePoint)));
    if (!AlignmentMath::Finite(AlignmentMath::Vector(selected.alignedPoint)))
      throw std::invalid_argument("Selected report point overflows.");
    selected.sample = result.forward->samples[*display.selectedVertex];
    selected.isCurrent = result.isCurrent;
    output.selection = selected;
  }
  return output;
}
bool GetValid(const MetrologyReport &r) {
  const auto &p = r.presentation;
  if (static_cast<unsigned>(p.source) > 2 ||
      (p.source == MetrologyReportDisplaySource::None &&
       (!p.deviations.empty() || p.scene)))
    return false;
  if (p.source == MetrologyReportDisplaySource::ExplicitConfiguration &&
      p.deviations.empty() && !p.scene)
    return false;
  std::set<DataRevisionRef> displays;
  for (const auto &d : p.deviations) {
    const auto result = std::find_if(
        r.deviations.begin(), r.deviations.end(),
        [&](const auto &v) { return v.result == d.parameters.result; });
    const auto resolved=result!=r.deviations.end() ? AlignmentDeviationColors::GetResolvedDisplay(d.parameters,*result):std::nullopt;
    if (!resolved ||
        !displays.insert(d.parameters.result).second ||
        result == r.deviations.end() ||
        !AlignmentDeviationColors::GetDisplayValid(d.parameters, *result) ||
        d.mappingRange != (AlignmentDeviationColors::GetEffectiveRange(*result->forward)
            ? std::optional<std::array<double,2>>(resolved->range):std::nullopt) ||
        d.colorMapVersion != AlignmentDeviationColors::version ||
        d.invalidRgb != AlignmentDeviationColors::invalidColor ||
        d.selectedRgb != AlignmentDeviationColors::selectedColor ||
        d.legendRgb != AlignmentDeviationColors::GetLegend(
                           *resolved, result->parameters->tolerance))
      return false;
    if (d.selection.has_value() != d.parameters.selectedVertex.has_value())
      return false;
    if (d.selection) {
      const auto &s = *d.selection;
      const auto &sample =
          result->forward->samples[*d.parameters.selectedVertex];
      const auto equal = [](double a, double b) {
        return a == b || (std::isnan(a) && std::isnan(b));
      };
      if (s.result != result->result ||
          s.vertex != *d.parameters.selectedVertex ||
          s.isCurrent != result->isCurrent ||
          !AlignmentMath::Finite(AlignmentMath::Vector(s.sourcePoint)) ||
          !AlignmentMath::Finite(AlignmentMath::Vector(s.alignedPoint)) ||
          s.alignedPoint != AlignmentMath::Point(AlignmentMath::Transform(
                                result->sourceToTarget,
                                AlignmentMath::Vector(s.sourcePoint))) ||
          s.sample.state != sample.state ||
          s.sample.closestTriangle != sample.closestTriangle ||
          !equal(s.sample.area, sample.area) ||
          !equal(s.sample.value, sample.value) ||
          !equal(s.sample.distance, sample.distance))
        return false;
    }
  }
  if (!p.scene)
    return true;
  const auto &s = *p.scene;
  if (!s.parameters || !s.parameters->isVisible ||
      s.parameters->parts.empty() || !std::isfinite(s.glyphLength) ||
      s.glyphLength <= 0 || s.glyphVersion != "inspection-glyphs-1")
    return false;
  const auto &request = *s.parameters;
  const auto anchor = std::find_if(
      r.alignments.begin(), r.alignments.end(),
      [&](const auto &v) { return v.result == request.anchorAlignment; });
  if (anchor == r.alignments.end() || !anchor->recipe || !anchor->diagnostics ||
      anchor->diagnostics->status != AlignmentStatus::FullyDetermined ||
      !anchor->diagnostics->isQualityPassed ||
      s.targetToModel != AlignmentMath::Inverse(anchor->sourceToTarget))
    return false;
  for (const auto &color : {request.measuredColor, request.referenceColor})
    for (double value : color)
      if (!std::isfinite(value) || value < 0 || value > 1)
        return false;
  if (!std::isfinite(request.referenceOpacity) ||
      request.referenceOpacity <= 0 || request.referenceOpacity > 1)
    return false;
  bool current = anchor->wasCurrentAtCapture;
  std::set<std::string> parts;
  std::set<DataRevisionRef> owners;
  for (const auto &part : request.parts) {
    if (part.id.empty() || part.id.size() > 128 ||
        !parts.insert(part.id).second ||
        !owners.insert(part.alignmentResult).second ||
        (!part.showMeasured && !part.showReference && !part.inspection) ||
        (part.selectedItem && !part.inspection))
      return false;
    const auto a = std::find_if(
        r.alignments.begin(), r.alignments.end(),
        [&](const auto &v) { return v.result == part.alignmentResult; });
    if (a == r.alignments.end() || !a->recipe || !a->diagnostics ||
        a->diagnostics->status != AlignmentStatus::FullyDetermined ||
        !a->diagnostics->isQualityPassed ||
        a->input.unit != anchor->input.unit ||
        a->input.coordinateFrame != anchor->input.coordinateFrame ||
        a->recipe->targetFrameId != anchor->recipe->targetFrameId)
      return false;
    current = current && a->wasCurrentAtCapture;
    if (part.showMeasured) {
      const auto mesh =
          std::find_if(r.inputs.begin(), r.inputs.end(), [&](const auto &v) {
            return v.revision == a->input.mesh;
          });
      if (mesh == r.inputs.end() || !mesh->mesh || !mesh->mesh->triangles)
        return false;
    }
    if (part.showReference) {
      const auto mesh =
          std::find_if(r.inputs.begin(), r.inputs.end(), [&](const auto &v) {
            return v.revision == a->recipe->nominalData;
          });
      if (mesh == r.inputs.end() || !mesh->mesh || !mesh->mesh->triangles)
        return false;
    }
    if (part.inspection) {
      const auto inspection = std::find_if(
          r.inspections.begin(), r.inspections.end(),
          [&](const auto &v) { return v.result == *part.inspection; });
      if (inspection == r.inspections.end() ||
          inspection->alignmentResult != part.alignmentResult ||
          !inspection->plan ||
          (part.selectedItem &&
           std::none_of(
               inspection->plan->items.begin(), inspection->plan->items.end(),
               [&](const auto &v) { return v.id == *part.selectedItem; })))
        return false;
      current = current && inspection->isCurrent;
    }
  }
  return current == s.wasCurrentAtCapture &&
         (!request.selectedPart || parts.count(*request.selectedPart));
}
} // namespace AlignmentReportDisplay
