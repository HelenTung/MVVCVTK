#include "AlignmentSceneDisplay.h"
#include "AlignmentResultSource.h"
#include "AlignmentMath.h"
#include "FeatureSupport/WorkLimit.h"
#include <vtkPolyData.h>
#include <algorithm>
#include <set>
#include <stdexcept>
namespace {
void Require(bool value, const char *message) {
  if (!value)
    throw std::invalid_argument(message);
}
bool Same(const AlignmentSceneDisplay &a, const AlignmentSceneDisplay &b) {
  if (a.anchorAlignment != b.anchorAlignment ||
      a.parts.size() != b.parts.size() || a.measuredColor != b.measuredColor ||
      a.referenceColor != b.referenceColor ||
      a.referenceOpacity != b.referenceOpacity ||
      a.referenceWireframe != b.referenceWireframe ||
      a.selectedPart != b.selectedPart)
    return false;
  for (std::size_t i = 0; i < a.parts.size(); ++i)
    if (a.parts[i].id != b.parts[i].id ||
        a.parts[i].alignmentResult != b.parts[i].alignmentResult ||
        a.parts[i].showMeasured != b.parts[i].showMeasured ||
        a.parts[i].showReference != b.parts[i].showReference)
      return false;
  return true;
}
struct Budget {
  std::size_t limit, used = 0;
  void Add(std::size_t n, std::size_t width = 1) {
    if (n > (limit - used) / width)
      throw std::length_error("Scene replacement working budget exceeded.");
    used += n * width;
  }
};
} // namespace
void AlignmentSceneDisplayController::Clear() noexcept {
  for (auto it = m_bindings.rbegin(); it != m_bindings.rend(); ++it)
    it->port->RemoveOverlay(it->overlay);
  m_bindings.clear();
  m_product.reset();
}
bool AlignmentSceneDisplayController::GetReady(
    const DataGraphSnapshot &graph) const {
  return m_product && !m_bindings.empty() &&
         AlignmentData::GetExpectationsCurrent(graph, m_product->expectations);
}
void AlignmentSceneDisplayController::Refresh(const DataGraphSnapshot &graph) {
  if (m_product && !GetReady(graph))
    Clear();
}
AlignmentResult AlignmentSceneDisplayController::Set(
    TrustedDataPort &data, FeatureViewDirectory *views,
    const AlignmentConfig &config, const AlignmentSceneDisplay &request) {
  AlignmentResult result;
  if (!request.isVisible) {
    Clear();
    result.status = AlignmentStatus::FullyDetermined;
    return result;
  }
  std::vector<Binding> next;
  const auto deadline = WorkLimit(config.deadlineMs).GetDeadline();
  const auto checkTime = [&] {
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Scene deadline exceeded.");
  };
  try {
    Require(GetDataRevisionRefValid(request.anchorAlignment) &&
                !request.parts.empty(),
            "Scene needs anchor and parts.");
    for (const auto &color : {request.measuredColor, request.referenceColor})
      for (double v : color)
        Require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid scene color.");
    Require(std::isfinite(request.referenceOpacity) &&
                request.referenceOpacity > 0 && request.referenceOpacity <= 1,
            "Invalid reference opacity.");
    if (!views) {
      result.status = AlignmentStatus::Unavailable;
      return result;
    }
    const auto targets = views->GetViews(config.targetViews);
    if (targets.empty()) {
      result.status = AlignmentStatus::Unavailable;
      return result;
    }
    Budget budget{
        config.workingBytes.value_or(std::numeric_limits<std::size_t>::max())};
    const auto retained = GetWorkingBytes();
    budget.Add(8192);
    budget.Add(request.parts.size(), 4096);
    budget.Add(targets.size(), 4096);
    const auto graph = data.GetDataGraph();
    std::set<std::string> ids;
    std::set<DataRevisionRef> alignments;
    for (const auto &part : request.parts) {
      Require(!part.id.empty() && part.id.size() <= 128 &&
                  ids.insert(part.id).second &&
                  GetDataRevisionRefValid(part.alignmentResult) &&
                  alignments.insert(part.alignmentResult).second &&
                  (part.showMeasured || part.showReference),
              "Invalid or duplicate scene part.");
      budget.Add(part.id.size(), 4);
      checkTime();
    }
    Require(!request.selectedPart || ids.count(*request.selectedPart),
            "Selected scene part is missing.");
    for (const auto &target : targets) {
      Require(target.role == HostRenderViewRole::Primary3D ||
                  target.role == HostRenderViewRole::Composite3D,
              "Scene surfaces require 3D views.");
      const auto port = views->GetOverlayPort(target.id);
      if (!port)
        throw std::runtime_error("Scene overlay port unavailable.");
      next.push_back({port, {}});
    }
    if (m_product && GetReady(graph) && Same(*m_product->request, request) &&
        next.size() == m_bindings.size() &&
        std::equal(
            next.begin(), next.end(), m_bindings.begin(),
            [](const auto &a, const auto &b) { return a.port == b.port; })) {
      result.status = AlignmentStatus::FullyDetermined;
      result.isDisplayReady = true;
      return result;
    }
    budget.Add(retained);
    auto anchor =
        AlignmentResultSource::Read(graph, request.anchorAlignment, false);
    if (!anchor) {
      result.status = AlignmentStatus::Stale;
      return result;
    }
    const auto anchorTransform =
        std::dynamic_pointer_cast<const Transform3DPayload>(
            graph.view->GetData(anchor->transform)->payload);
    auto product = std::make_shared<AlignmentSceneProduct>();
    product->targetToModel = AlignmentMath::Inverse(anchor->sourceToTarget);
    product->unit = anchor->input.unit;
    product->expectations = anchor->expectations;
    std::vector<std::pair<DataRevisionRef, VtkSurfaceMeshSnapshot>> geometry;
    std::set<DataRevisionRef> drawnReferences;
    const auto meshView = [&](const DataRevisionRef &ref,
                              const SurfaceMeshPayload &mesh) {
      for (const auto &entry : geometry)
        if (entry.first == ref)
          return entry.second;
      Require((mesh.GetTriangles().size() / 3) > 0,
              "Scene display requires triangle surfaces.");
      // 保守缓存计账，含各视图并存的顶点/索引/GPU打包余量；不宣称驱动显存硬上限。
      for (std::size_t i = 0; i < targets.size(); ++i) {
        budget.Add(mesh.GetVertices().size() / 3, 128);
        budget.Add((mesh.GetTriangles().size() / 3), 64);
      }
      checkTime();
      const auto view = data.GetSurfaceMesh(graph, ref);
      if (!view || !view->mesh)
        throw std::runtime_error("Scene mesh view unavailable.");
      geometry.push_back({ref, view});
      return view;
    };
    for (const auto &part : request.parts) {
      checkTime();
      const auto source = AlignmentResultSource::Read(
          graph, part.alignmentResult, part.showReference);
      if (!source) {
        result.status = AlignmentStatus::Stale;
        return result;
      }
      const auto transform =
          std::dynamic_pointer_cast<const Transform3DPayload>(
              graph.view->GetData(source->transform)->payload);
      Require(
          source->input.unit == product->unit &&
              source->input.coordinateFrame == anchor->input.coordinateFrame &&
              transform->GetTargetFrame() == anchorTransform->GetTargetFrame(),
          "Scene parts must use the same target frame, axis convention and "
          "unit.");
      budget.Add(source->expectations.size(), 4 * sizeof(DataExpectation));
      product->expectations.insert(product->expectations.end(),
                                   source->expectations.begin(),
                                   source->expectations.end());
      product->parts.push_back({part.alignmentResult, source->input.mesh,
                                source->nominal, source->sourceToTarget});
      if (part.showMeasured) {
        budget.Add(targets.size(), 16384);
        product->surfaces.push_back(
            {meshView(source->input.mesh, *source->measuredMesh),
             AlignmentMath::Multiply(product->targetToModel,
                                     source->sourceToTarget),
             request.selectedPart && *request.selectedPart == part.id
                 ? std::array<double, 3>{1, .8, .1}
                 : request.measuredColor,
             1, false});
      }
      if (part.showReference &&
          drawnReferences.insert(source->nominal).second) {
        budget.Add(targets.size(), 16384);
        product->surfaces.push_back(
            {meshView(source->nominal, *source->referenceMesh),
             product->targetToModel, request.referenceColor,
             request.referenceOpacity, request.referenceWireframe});
      }
    }
    product->request = std::make_shared<const AlignmentSceneDisplay>(request);
    product->workingBytes = budget.used - retained;
    for (auto &binding : next) {
      checkTime();
      binding.overlay = std::make_shared<AlignmentSceneOverlay>(product);
      if (!binding.port->AttachOverlay(binding.overlay))
        throw std::runtime_error("Scene attach failed.");
    }
    checkTime();
    if (!AlignmentData::GetExpectationsCurrent(data.GetDataGraph(),
                                               product->expectations))
      result.status = AlignmentStatus::Stale;
    else {
      Clear();
      // RemoveOverlay 也可能触发宿主回调，撤旧后再次确认新场景依赖。
      if (AlignmentData::GetExpectationsCurrent(data.GetDataGraph(),
                                                product->expectations)) {
        m_product = std::move(product);
        m_bindings = std::move(next);
        result.status = AlignmentStatus::FullyDetermined;
        result.isDisplayReady = true;
        return result;
      }
      result.status = AlignmentStatus::Stale;
    }
  } catch (const std::length_error &e) {
    result.status = AlignmentStatus::BudgetExceeded;
    result.message = e.what();
  } catch (const std::bad_alloc &) {
    result.status = AlignmentStatus::BudgetExceeded;
  } catch (const std::invalid_argument &) {
    for (auto it = next.rbegin(); it != next.rend(); ++it)
      if (it->overlay)
        it->port->RemoveOverlay(it->overlay);
    throw;
  } catch (const std::exception &e) {
    result.status = std::chrono::steady_clock::now() >= deadline
                        ? AlignmentStatus::DeadlineExceeded
                        : AlignmentStatus::InternalError;
    result.message = e.what();
  } catch (...) {
    result.status = AlignmentStatus::InternalError;
  }
  for (auto it = next.rbegin(); it != next.rend(); ++it)
    if (it->overlay)
      it->port->RemoveOverlay(it->overlay);
  return result;
}
std::optional<AlignmentSceneVertex> AlignmentSceneDisplayController::GetVertex(
    const DataGraphSnapshot &graph, const std::string &partId, bool reference,
    std::uint64_t vertex) const {
  if (!m_product)
    return {};
  const auto &request = *m_product->request;
  for (std::size_t i = 0; i < request.parts.size(); ++i)
    if (request.parts[i].id == partId) {
      const auto &part = m_product->parts[i];
      const auto ref = reference ? part.reference : part.measured;
      const auto node = graph.view ? graph.view->GetData(ref) : nullptr;
      const auto mesh =
          node ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(
                     node->payload)
               : nullptr;
      if (!mesh || vertex >= mesh->GetVertices().size() / 3)
        return {};
      const auto &points = mesh->GetVertices();
      AlignmentSceneVertex value;
      value.partId = partId;
      value.alignmentResult = part.alignment;
      value.anchorAlignment = request.anchorAlignment;
      value.mesh = ref;
      value.vertex = vertex;
      value.isReference = reference;
      value.isCurrent = GetReady(graph);
      value.unit = m_product->unit;
      value.nativePoint = {points[3 * vertex], points[3 * vertex + 1],
                           points[3 * vertex + 2]};
      value.targetPoint = reference
                              ? value.nativePoint
                              : AlignmentMath::Point(AlignmentMath::Transform(
                                    part.sourceToTarget,
                                    AlignmentMath::Vector(value.nativePoint)));
      value.modelPoint = AlignmentMath::Point(AlignmentMath::Transform(
          m_product->targetToModel, AlignmentMath::Vector(value.targetPoint)));
      return value;
    }
  return {};
}
