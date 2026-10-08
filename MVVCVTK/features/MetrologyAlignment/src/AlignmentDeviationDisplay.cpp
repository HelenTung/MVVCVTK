#include "AlignmentDeviationDisplay.h"
#include "AlignmentDeviationData.h"
#include "AlignmentMath.h"
#include "FeatureSupport/WorkLimit.h"
#include <stdexcept>

void AlignmentDeviationDisplayController::Clear() noexcept {
    for (auto i=m_bindings.rbegin();i!=m_bindings.rend();++i) i->port->RemoveOverlay(i->overlay);
    m_bindings.clear(); m_product.reset(); m_request.reset(); m_viewBytes=0;
}
bool AlignmentDeviationDisplayController::GetReady(const DataGraphSnapshot &graph) const {
    if (!m_product || !m_request || m_bindings.empty()) return false;
    const auto result=AlignmentDeviationDataStore::GetResult(graph,m_request->result);
    return result && result->isCurrent;
}
void AlignmentDeviationDisplayController::Refresh(const DataGraphSnapshot &graph) {
    if (m_request && !GetReady(graph)) Clear();
}
AlignmentResult AlignmentDeviationDisplayController::Set(TrustedDataPort &data, FeatureViewDirectory *views,
    const AlignmentConfig &config, const AlignmentDeviationDisplay &request) {
    AlignmentResult result;
    if (!request.isVisible) { Clear(); result.status=AlignmentStatus::FullyDetermined; return result; }
    const auto graph=data.GetDataGraph();
    const auto deviation=AlignmentDeviationDataStore::GetResult(graph,request.result);
    if (!deviation || !AlignmentDeviationOverlay::GetDisplayValid(request,*deviation))
        throw std::invalid_argument("Invalid deviation display parameters.");
    if (!deviation->isCurrent) { result.status=AlignmentStatus::Stale; return result; }
    if (!views) { result.status=AlignmentStatus::Unavailable; return result; }
    std::vector<Binding> next;
    const auto deadline=WorkLimit(config.deadlineMs).GetDeadline();
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
        if (m_request && m_product && m_request->result==request.result && m_request->range==request.range &&
            m_request->useToleranceColors==request.useToleranceColors && m_request->selectedVertex==request.selectedVertex &&
            next.size()==m_bindings.size() && std::equal(next.begin(),next.end(),m_bindings.begin(),
                [](const auto &a,const auto &b) { return a.port==b.port; })) {
            // 相同请求不重建颜色、mapper 或 GPU 缓存。
            result.status=AlignmentStatus::FullyDetermined; result.deviationResult=request.result; result.isDisplayReady=true;
            return result;
        }
        const auto limit=config.workingBytes.value_or(std::numeric_limits<std::size_t>::max());
        const auto retained=m_product ? m_product->workingBytes+m_viewBytes : 0;
        if (retained>limit) throw std::length_error("Existing deviation display exceeds replacement budget.");
        // 每个视图保守预留 actor/mapper/legend 元数据；共享几何及 VTK/GPU 缓存独立保活。
        constexpr std::size_t perViewBytes=16384;
        if (targets.size()>(limit-retained)/perViewBytes) throw std::length_error("Deviation view count exceeds display budget.");
        const auto viewBytes=targets.size()*perViewBytes;
        if (AlignmentDeviationOverlay::GetRequiredBytes(deviation->forward->samples.size())>limit-retained-viewBytes)
            throw std::length_error("Replacement deviation colors exceed display working budget.");
        const auto mesh=data.GetSurfaceMesh(graph,deviation->sourceMesh);
        if (!mesh) throw std::runtime_error("Measured surface render view unavailable.");
        const auto product=AlignmentDeviationOverlay::Build(mesh,*deviation,request,limit-retained-viewBytes,deadline);
        for (auto &binding:next) {
            binding.overlay=std::make_shared<AlignmentDeviationOverlay>(product);
            if (!binding.port->AttachOverlay(binding.overlay)) throw std::runtime_error("Deviation overlay attach failed.");
        }
        // 挂载完成后再核对真源；端口回调不能把过期表面变为当前显示。
        const auto current=AlignmentDeviationDataStore::GetResult(data.GetDataGraph(),request.result);
        if (!current || !current->isCurrent) {
            result.status=AlignmentStatus::Stale;
            for (auto i=next.rbegin();i!=next.rend();++i) if (i->overlay) i->port->RemoveOverlay(i->overlay);
            return result;
        }
        Clear(); m_bindings=std::move(next); m_product=product; m_request=request; m_viewBytes=viewBytes;
        result.status=AlignmentStatus::FullyDetermined; result.deviationResult=request.result; result.isDisplayReady=true;
        return result;
    } catch (const std::length_error &e) { result.status=AlignmentStatus::BudgetExceeded; result.message=e.what(); }
      catch (const std::bad_alloc &) { result.status=AlignmentStatus::BudgetExceeded; }
      catch (const std::invalid_argument &) {
        for (auto i=next.rbegin();i!=next.rend();++i) if (i->overlay) i->port->RemoveOverlay(i->overlay);
        throw;
    } catch (const std::exception &e) {
        result.status=std::chrono::steady_clock::now() >= deadline ? AlignmentStatus::DeadlineExceeded : AlignmentStatus::InternalError; result.message=e.what();
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
    if (!mesh || vertex>=mesh->GetVertices().size()/3) return {};
    const auto &v=mesh->GetVertices();
    AlignmentDeviationSelection value;
    value.result=ref; value.vertex=vertex; value.sourcePoint={v[3*vertex],v[3*vertex+1],v[3*vertex+2]};
    value.alignedPoint=AlignmentMath::Point(AlignmentMath::Transform(result->sourceToTarget,AlignmentMath::Vector(value.sourcePoint)));
    value.sample=result->forward->samples[vertex]; value.isCurrent=result->isCurrent;
    return value;
}
