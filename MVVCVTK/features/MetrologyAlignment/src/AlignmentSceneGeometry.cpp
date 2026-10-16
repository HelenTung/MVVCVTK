#include "AlignmentSceneGeometry.h"
#include "AlignmentOverlay.h"
#include "AlignmentMath.h"
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <algorithm>
#include <stdexcept>
namespace AlignmentSceneGeometry {
namespace {
template <class F> void Slots(std::size_t size, std::size_t count, F visit) {
  if (!count)
    return;
  const auto step = size / count, remainder = size % count;
  std::size_t slot = 0, error = 0;
  for (std::size_t j = 0; j < count; ++j) {
    visit(slot);
    slot += step;
    if (error >= count - remainder) {
      error -= count - remainder;
      ++slot;
    } else
      error += remainder;
  }
}
} // namespace
std::vector<std::size_t> GetItemGeometries(const InspectionPlan &plan,
                                           std::size_t item) {
  if (item >= plan.items.size())
    throw std::invalid_argument("Inspection item is missing.");
  std::vector<std::size_t> result;
  result.reserve(8);
  const auto add = [&](std::size_t index) {
    if (index >= plan.geometries.size())
      throw std::invalid_argument("Inspection geometry index is invalid.");
    if (std::find(result.begin(), result.end(), index) == result.end())
      result.push_back(index);
  };
  const auto operand = [&](const InspectionOperand &v) {
    add(v.geometry);
    if (v.sectionPlane)
      add(*v.sectionPlane);
  };
  const auto measure = [&](const InspectionMeasure &v) {
    operand(v.first);
    if (v.second)
      operand(*v.second);
  };
  measure(plan.items[item].measured);
  if (plan.items[item].nominalMeasure)
    measure(*plan.items[item].nominalMeasure);
  return result;
}
void Build(AlignmentSceneProduct &product, AlignmentScenePartData &part,
           const AlignmentScenePart &request,
           const SurfaceMeshPayload &measured,
           const SurfaceMeshPayload *reference, double glyphLength,
           std::size_t markerLimit, std::size_t viewCount,
           const std::function<void(std::size_t, std::size_t)> &charge) {
  using namespace AlignmentMath;
  if (!part.inspection)
    return;
  const auto &snapshot = *part.inspection;
  const auto &plan = *snapshot.plan;
  const auto &report = *snapshot.report;
  std::vector<std::size_t> selected;
  if (request.selectedItem) {
    const auto it =
        std::find_if(plan.items.begin(), plan.items.end(), [&](const auto &v) {
          return v.id == *request.selectedItem;
        });
    if (it == plan.items.end())
      throw std::invalid_argument("Selected inspection item is missing.");
    selected = GetItemGeometries(
        plan, static_cast<std::size_t>(it - plan.items.begin()));
  }
  // 线框仅示意已拟合理想元素；glyphLength 不代表实体端面或圆柱高度。
  charge(report.geometries.size(), 65536);
  for (std::size_t i = 0; i < viewCount; ++i)
    charge(report.geometries.size(), 65536);
  std::array<std::vector<AlignmentGeometry>, 2> fitted;
  for (std::size_t i = 0; i < report.geometries.size(); ++i) {
    const auto &g = report.geometries[i];
    if (g.status != AlignmentStatus::FullyDetermined)
      continue;
    AlignmentGeometry value;
    value.kind = g.fitted.kind;
    value.radius = g.fitted.radius;
    value.sourceCenter = g.alignedCenter;
    value.sourceDirection = g.alignedDirection;
    fitted[std::find(selected.begin(), selected.end(), i) != selected.end()]
        .push_back(std::move(value));
  }
  for (std::size_t group = 0; group < 2; ++group)
    if (!fitted[group].empty()) {
        auto poly =
          AlignmentOverlay::BuildData(alignmentIdentity, fitted[group], {},
                                      glyphLength, nullptr, false);
      if (!poly)
        throw std::runtime_error("Inspection glyph construction failed.");
      product.glyphs.push_back({poly, product.targetToModel,
                                group ? std::array<double, 3>{1, .8, .1}
                                      : std::array<double, 3>{.3, 1, .8},
                                5, 2.5});
    }
  if (selected.empty())
    return;
  charge(viewCount, 16384);
  charge(8192, 1);
  auto points = vtkSmartPointer<vtkPoints>::New();
  points->SetDataTypeToDouble();
  auto vertices = vtkSmartPointer<vtkCellArray>::New();
  auto lines = vtkSmartPointer<vtkCellArray>::New();
  const auto marker = [&](const Vec &point) {
    if (!Finite(point))
      throw std::invalid_argument("Inspection display coordinate overflow.");
    const auto id = points->InsertNextPoint(point.val);
    vertices->InsertNextCell(1, &id);
  };
  for (auto index : selected) {
    const auto &spec = plan.geometries[index];
    const auto &g = report.geometries[index];
    if (spec.source == InspectionGeometrySource::NominalDefinition)
      continue;
    const auto *mesh = spec.source == InspectionGeometrySource::MeasuredSurface
                           ? &measured
                           : reference;
    if (!mesh)
      throw std::invalid_argument("Inspection region mesh is unavailable.");
    const auto pose = spec.source == InspectionGeometrySource::MeasuredSurface
                          ? snapshot.sourceToTarget
                          : alignmentIdentity;
    const auto &xyz = mesh->GetPointValues();
    const auto point = [&](std::uint64_t id) {
      if (id >= xyz.size() / 3)
        throw std::invalid_argument("Inspection region vertex is unavailable.");
      return Vec(xyz[3 * id], xyz[3 * id + 1], xyz[3 * id + 2]);
    };
    AlignmentSceneRegionDisplay info;
    info.geometry = index;
    if (spec.section) {
      info.kind = AlignmentSceneRegionKind::SectionSamples;
      if (g.section) {
        info.sourceCount = g.section->samples.size();
        const auto count = std::min(markerLimit, g.section->samples.size());
        charge(count, 128);
        for (std::size_t v = 0; v < viewCount; ++v)
          charge(count, 128);
        const auto &tri = mesh->GetTriangleIndices();
        const auto n = Vector(g.section->sourcePlaneNormal);
        Slots(g.section->samples.size(), count, [&](std::size_t slot) {
                const auto &sample = g.section->samples[slot];
          if (sample.triangle >= tri.size() / 3)
            throw std::invalid_argument(
                "Inspection section triangle is unavailable.");
          Vec p{};
          for (std::size_t a = 0; a < 3; ++a)
            p += sample.barycentric[a] * point(tri[3 * sample.triangle + a]);
          marker(Transform(pose, p - sample.planeCorrection * n));
        });
        info.drawnMarkers = count;
      }
    } else if (spec.fit.region.targetBounds) {
      info.kind = AlignmentSceneRegionKind::TargetBounds;
      const auto &bounds = *spec.fit.region.targetBounds;
      vtkIdType ids[8];
      for (int i = 0; i < 8; ++i) {
        const double p[]{bounds[(i & 1) ? 1 : 0], bounds[(i & 2) ? 3 : 2],
                         bounds[(i & 4) ? 5 : 4]};
        ids[i] = points->InsertNextPoint(p);
      }
      for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
          if (!(i & bit)) {
            const vtkIdType edge[]{ids[i], ids[i | bit]};
            lines->InsertNextCell(2, edge);
          }
    } else {
      info.kind = AlignmentSceneRegionKind::VertexCandidates;
      const auto &ids = spec.fit.region.vertexIds;
      info.sourceCount = ids.size();
      const auto count = std::min(markerLimit, ids.size());
      charge(count, 128);
      for (std::size_t v = 0; v < viewCount; ++v)
        charge(count, 128);
      Slots(ids.size(), count, [&](std::size_t slot) {
            marker(Transform(pose, point(ids[slot])));
      });
      info.drawnMarkers = count;
    }
    part.regions.push_back(info);
  }
  if (points->GetNumberOfPoints()) {
    auto poly = vtkSmartPointer<vtkPolyData>::New();
    poly->SetPoints(points);
    poly->SetVerts(vertices);
    poly->SetLines(lines);
    product.glyphs.push_back(
        {poly, product.targetToModel, {1, .2, .6}, 4, 1.5});
  }
}
} // namespace AlignmentSceneGeometry
