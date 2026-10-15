#include "AlignmentDeviationDisplay.h"
#include "AlignmentDeviationData.h"
#include "AlignmentDeviationColors.h"
#include "AlignmentMath.h"
#include "App/Services/FeatureViewService.h"
#include <stdexcept>

void AlignmentDeviationDisplayController::Clear() noexcept {
    for (auto i=m_bindings.rbegin();i!=m_bindings.rend();++i) i->port->RemoveOverlay(i->overlay);
    m_bindings.clear(); m_product.reset();m_marker=nullptr;m_request.reset();m_resolved.reset();
}
bool AlignmentDeviationDisplayController::GetReady(const DataGraphSnapshot &graph) const {
    if (!m_product || !m_request || !m_request->isVisible || m_bindings.empty()) return false;
    const auto result=AlignmentDeviationDataStore::GetResult(graph,m_request->result);
    return result && result->isCurrent;
}
void AlignmentDeviationDisplayController::Refresh(const DataGraphSnapshot &graph) {
    if (m_request) { const auto result=AlignmentDeviationDataStore::GetResult(graph,m_request->result);if(!result || !result->isCurrent)Clear(); }
}
AlignmentResult AlignmentDeviationDisplayController::Set(TrustedDataPort &data, FeatureViewDirectory *views,
    const AlignmentConfig &config, const AlignmentDeviationDisplay &request) {
    AlignmentResult result;
    const auto graph=data.GetDataGraph();
    const auto deviation=AlignmentDeviationDataStore::GetResult(graph,request.result);
    const bool sameResult=m_product && m_request && m_request->result==request.result && deviation && m_product->field==deviation->forward;
    const auto resolved=deviation ? AlignmentDeviationColors::GetResolvedDisplay(request,*deviation,sameResult ? &m_product->actualRange:nullptr):std::nullopt;
    if (!resolved)
        throw std::invalid_argument("Invalid deviation display parameters.");
    if (!deviation->isCurrent) { result.status=AlignmentStatus::Stale; return result; }
    if (!views) { result.status=AlignmentStatus::Unavailable; return result; }
    auto applied=request;AnalysisColorStyle::SetTailColors(applied);
    std::map<DataRevisionRef,AlignmentDeviationDisplay> saved;saved.emplace(request.result,applied);
    auto resolvedCopy=*resolved;
    auto commit=[&]{m_request=std::move(applied);m_resolved=std::move(resolvedCopy);m_saved.erase(request.result);m_saved.insert(saved.extract(saved.begin()));};
    std::vector<Binding> next;
    try {
        const auto targets=views->GetViews(config.targetViews);
        if (targets.empty()) { result.status=AlignmentStatus::Unavailable; return result; }
        for (const auto &view:targets) {
            if (view.role!=HostRenderViewRole::Primary3D && view.role!=HostRenderViewRole::Composite3D)
                throw std::invalid_argument("Deviation surface overlay requires a 3D view.");
            const auto port=views->GetOverlayPort(view.id);
            if (!port) throw std::runtime_error("Deviation overlay port unavailable.");
            next.push_back({port,{}});
        }
        auto appearance=[&](std::size_t index) {
            auto value=request;value.legendPosition=AnalysisColorStyle::GetLegendPosition(request,targets[index].id);value.hasLegend=request.hasLegend && std::find(request.hiddenLegendViews.begin(),request.hiddenLegendViews.end(),targets[index].id)==request.hiddenLegendViews.end();return value;
        };
        std::vector<AlignmentDeviationDisplay> appearances;
        for(std::size_t i=0;i<next.size();++i)appearances.push_back(appearance(i));
        const auto notify=[&] {
            for(const auto& view:targets)if(auto port=views->GetFeaturePort(view.id))port->SetRenderNeeded();
        };
        const bool sameViews=m_request && m_product && m_request->result==request.result && next.size()==m_bindings.size()
            && std::equal(next.begin(),next.end(),m_bindings.begin(),[](const auto& a,const auto& b){return a.port==b.port;});
        const auto parameters=AlignmentDeviationColors::GetRampParameters(*resolved,deviation->parameters->tolerance);
        if(sameViews && m_resolved->range==resolved->range && AnalysisColorStyle::GetRampEqual(m_product->ramp,parameters)) {
            // 选点／整体透明度／图例显隐不改颜色数组，也不重建 mapper 或几何。
            auto marker=m_request->selectedVertex==request.selectedVertex ? m_marker:AlignmentDeviationOverlay::BuildMarker(m_product->source,*deviation,request.selectedVertex);
            notify();
            for(std::size_t i=0;i<m_bindings.size();++i) { m_bindings[i].overlay->SetSelection(marker);m_bindings[i].overlay->SetPresentation(appearances[i]); }
            m_marker=marker;commit();
            result.status=AlignmentStatus::FullyDetermined;result.deviationResult=request.result;result.isDisplayReady=request.isVisible;return result;
        }
        const auto mesh=data.GetSurfaceMesh(graph,deviation->sourceMesh);
        if(!mesh)throw std::runtime_error("Measured surface render view unavailable.");
        const auto product=AlignmentDeviationOverlay::Build(mesh,*deviation,*resolved,sameViews ? m_product.get():nullptr,
            sameViews && m_request->selectedVertex==request.selectedVertex ? std::optional<vtkSmartPointer<vtkPolyData>>(m_marker):std::nullopt);
        if(sameViews) {
            notify();
            for(std::size_t i=0;i<m_bindings.size();++i) { m_bindings[i].overlay->SetProduct(product);m_bindings[i].overlay->SetPresentation(appearances[i]); }
            m_product=product;m_marker=product->marker;commit();
            result.status=AlignmentStatus::FullyDetermined;result.deviationResult=request.result;result.isDisplayReady=request.isVisible;return result;
        }
        for (auto &binding:next) {
            binding.overlay=std::make_shared<AlignmentDeviationOverlay>(product);
            binding.overlay->SetPresentation(appearances[static_cast<std::size_t>(&binding-next.data())]);
            if (!binding.port->AttachOverlay(binding.overlay)) throw std::runtime_error("Deviation overlay attach failed.");
        }
        // 挂载完成后再核对真源；端口回调不能把过期表面变为当前显示。
        const auto current=AlignmentDeviationDataStore::GetResult(data.GetDataGraph(),request.result);
        if (!current || !current->isCurrent) {
            result.status=AlignmentStatus::Stale;
            for (auto i=next.rbegin();i!=next.rend();++i) if (i->overlay) i->port->RemoveOverlay(i->overlay);
            return result;
        }
        Clear(); m_bindings=std::move(next); m_product=product;m_marker=product->marker;commit();
        result.status=AlignmentStatus::FullyDetermined; result.deviationResult=request.result;result.isDisplayReady=request.isVisible;
        return result;
    } catch (const std::length_error &e) { result.status=AlignmentStatus::ResourceExhausted; result.message=e.what(); }
      catch (const std::bad_alloc &) { result.status=AlignmentStatus::ResourceExhausted; }
      catch (const std::invalid_argument &) {
        for (auto i=next.rbegin();i!=next.rend();++i) if (i->overlay) i->port->RemoveOverlay(i->overlay);
        throw;
    } catch (const std::exception &e) {
        result.status=AlignmentStatus::InternalError; result.message=e.what();
    } catch (...) { result.status=AlignmentStatus::InternalError; }
    for (auto i=next.rbegin();i!=next.rend();++i) if (i->overlay) i->port->RemoveOverlay(i->overlay);
    return result;
}
std::optional<AlignmentDeviationSelection> AlignmentDeviationDisplayController::GetSelection(
    const DataGraphSnapshot &graph,const DataRevisionRef &ref,std::uint64_t vertex) {
    const auto result=AlignmentDeviationDataStore::GetResult(graph,ref);
    if (!result || vertex>=result->forward->samples.size()) return {};
    const auto node=graph.view->GetData(result->sourceMesh);
    const auto mesh=node ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(node->payload) : nullptr;
    if (!mesh || (vertex>=mesh->GetPointValues().size()/3 && !result->forward->samples[vertex].sourcePosition)) return {};
    const auto &v=mesh->GetPointValues();
    AlignmentDeviationSelection value;
    value.result=ref; value.vertex=vertex;
    if(result->forward->samples[vertex].sourcePosition)value.sourcePoint=*result->forward->samples[vertex].sourcePosition;else value.sourcePoint={v[3*vertex],v[3*vertex+1],v[3*vertex+2]};
    value.alignedPoint=AlignmentMath::Point(AlignmentMath::Transform(result->sourceToTarget,AlignmentMath::Vector(value.sourcePoint)));
    value.sample=result->forward->samples[vertex]; value.isCurrent=result->isCurrent;
    return value;
}
