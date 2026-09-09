#include "Interaction/CropHistoryQueue.h"
#include "Algorithms/CropGeometry.h"
#include <algorithm>
#include <limits>

namespace {
constexpr std::size_t pendingLimit=256,terminalLimit=1024,nodeLimit=100000,depthLimit=4096;
bool GetSame(const CropEditRequest& a,const CropEditRequest& b)
{
    const auto& x=a.operation;const auto& y=b.operation;
    return a.documentId==b.documentId&&a.requestId==b.requestId&&a.expectedRevision==b.expectedRevision
        &&a.kind==b.kind&&a.nodeId==b.nodeId&&a.prune.scope==b.prune.scope
        &&a.prune.nodeIds==b.prune.nodeIds&&a.prune.fallback==b.prune.fallback
        &&a.prune.explicitFallbackNode==b.prune.explicitFallbackNode
        &&x.operationIndex==y.operationIndex&&x.geometryType==y.geometryType&&x.removalMode==y.removalMode
        &&x.boxToInputModelMatrix==y.boxToInputModelMatrix&&x.planeCenterInInputModel==y.planeCenterInInputModel
        &&x.planeNormalInInputModel==y.planeNormalInInputModel&&x.centerInInputModel==y.centerInInputModel
        &&x.axisInInputModel==y.axisInInputModel&&x.radius==y.radius&&x.height==y.height
        &&x.recipeVersion==y.recipeVersion&&x.boundaryPolicyVersion==y.boundaryPolicyVersion;
}
}

std::optional<CropEditOutcome> CropHistoryQueue::GetOutcome(CropRequestId id) const
{
    const auto found=m_entries.find(id);
    if(found!=m_entries.end())return found->second.outcome;
    if(id&&id<=m_expiredThrough)return CropEditOutcome{id,0,0,CropEditStatus::Failed,CropFailure::RequestExpired,{}};
    return std::nullopt;
}

std::optional<CropNodeSnapshot> CropHistoryQueue::GetNode(const CropHistory& history,CropNodeId id) const
{
    if(auto node=history.GetNode(id))return node;
    for(const auto request:m_pending) {
        const auto& entry=m_entries.at(request);
        if(entry.reserved&&entry.reserved->nodeId==id)return entry.reserved;
    }
    return std::nullopt;
}

void CropHistoryQueue::ClearExpired()
{
    while(m_entries.size()-m_pending.size()>=terminalLimit) {
        auto oldest=m_entries.end();
        for(auto it=m_entries.begin();it!=m_entries.end();++it) {
            if(it->second.outcome.status==CropEditStatus::Queued)continue;
            if(oldest==m_entries.end()||it->second.admissionOrder<oldest->second.admissionOrder)oldest=it;
        }
        if(oldest==m_entries.end())break;
        m_expiredThrough=std::max(m_expiredThrough,oldest->first);
        m_entries.erase(oldest);
    }
}

CropEditAdmission CropHistoryQueue::StartRequest(CropHistory& history,CropEditRequest request)
{
    CropEditAdmission admission;
    admission.requestId=request.requestId;admission.stateRevision=history.GetRevision();
    const auto reject=[&](CropFailure failure) { admission.failureReason=failure;return admission; };
    if(!request.requestId||!request.documentId||request.documentId!=history.GetDocumentId())return reject(CropFailure::InvalidRequest);
    const auto previous=m_entries.find(request.requestId);
    if(previous!=m_entries.end()) {
        if(!GetSame(previous->second.request,request))return reject(CropFailure::InvalidRequest);
        admission.isAccepted=admission.isReplay=true;admission.nodeId=previous->second.outcome.nodeId;
        return admission;
    }
    if(request.requestId<=m_expiredThrough)return reject(CropFailure::RequestExpired);
    if(request.expectedRevision!=history.GetRevision())return reject(CropFailure::StateVersionMismatch);
    if(m_pending.size()>=pendingLimit||!m_nextOrder)return reject(CropFailure::ResourceLimit);
    if(!m_pending.empty()&&m_entries.at(m_pending.front()).request.kind==CropEditKind::Prune)return reject(CropFailure::Busy);
    Entry entry;entry.request=request;entry.admissionOrder=m_nextOrder;
    entry.outcome.requestId=request.requestId;entry.outcome.stateRevision=history.GetRevision();
    entry.outcome.nodeId=request.nodeId;
    if(request.kind==CropEditKind::Append||request.kind==CropEditKind::Replace) {
        const auto base=GetNode(history,request.nodeId);
        if(!base||(request.kind==CropEditKind::Replace&&!base->operation))return reject(CropFailure::NodeNotFound);
        const auto geometry=CropGeometry::Build(request.operation);
        if(!geometry)return reject(CropFailure::BadInput);
        entry.hadPendingDependency=!history.GetNode(request.nodeId).has_value();
        const bool isIdentical=request.kind==CropEditKind::Replace
            &&CropGeometry::GetOperationsSame(*base->operation,geometry->GetOperation());
        if(!isIdentical) {
            std::size_t reservedCount=0;
            for(const auto id:m_pending)if(m_entries.at(id).reserved)++reservedCount;
            if(history.GetNodeCount()+reservedCount>=nodeLimit)return reject(CropFailure::ResourceLimit);
            const auto parent=request.kind==CropEditKind::Append?base->nodeId:base->parentNodeId;
            auto ancestor=GetNode(history,parent);std::size_t depth=0;
            while(ancestor&&ancestor->parentNodeId) {
                if(++depth>=depthLimit)return reject(CropFailure::ResourceLimit);
                ancestor=GetNode(history,ancestor->parentNodeId);
            }
            if(!ancestor||ancestor->nodeId!=history.GetRootId())return reject(CropFailure::NodeNotFound);
            const auto node=CropHistory::CreateNodeId();
            if(!node)return reject(CropFailure::ResourceLimit);
            auto operation=geometry->GetOperation();operation.operationIndex=node;
            entry.reserved=CropNodeSnapshot{node,parent,std::move(operation)};
            entry.outcome.nodeId=node;
        }
    }
    else if(request.kind==CropEditKind::Select) {
        if(!GetNode(history,request.nodeId))return reject(CropFailure::NodeNotFound);
        if(request.nodeId==history.GetRootId()&&std::any_of(history.GetResults().begin(),history.GetResults().end(),
            [](const auto& result) {return result.status==CropResultStatus::Published;}))return reject(CropFailure::ReturnToSourceRequired);
        entry.hadPendingDependency=!history.GetNode(request.nodeId).has_value();
    }
    else if(request.kind==CropEditKind::Prune) {
        if(!m_pending.empty())return reject(CropFailure::Busy);
        const auto stage=history.BuildPrune(request.prune);
        if(stage.failureReason!=CropFailure::None)return reject(stage.failureReason);
        entry.outcome.nodeId=stage.head;
    }
    else return reject(CropFailure::InvalidRequest);
    ClearExpired();
    const auto inserted=m_entries.emplace(request.requestId,std::move(entry));
    try {m_pending.push_back(request.requestId);}catch(...) {m_entries.erase(inserted.first);throw;}
    m_nextOrder=m_nextOrder==std::numeric_limits<std::uint64_t>::max()?0:m_nextOrder+1;
    admission.isAccepted=true;admission.nodeId=inserted.first->second.outcome.nodeId;
    SetRequestedHead(history);
    return admission;
}

const CropEditRequest* CropHistoryQueue::GetNext() const noexcept
{
    return m_pending.empty()?nullptr:&m_entries.at(m_pending.front()).request;
}

CropHistory::Stage CropHistoryQueue::BuildNext(const CropHistory& history) const
{
    if(m_pending.empty()) {CropHistory::Stage stage;stage.failureReason=CropFailure::InvalidRequest;return stage;}
    const auto& entry=m_entries.at(m_pending.front());const auto& request=entry.request;
    CropHistory::Stage stage;
    if(request.kind==CropEditKind::Prune)stage=history.BuildPrune(request.prune,true);
    else if(!history.GetNode(request.nodeId))stage.failureReason=entry.hadPendingDependency?CropFailure::ParentFailed:CropFailure::NodeNotFound;
    else if(request.kind==CropEditKind::Select) {
        stage=history.BuildSelection(request.nodeId);
        if(request.nodeId==history.GetRootId()&&std::any_of(history.GetResults().begin(),history.GetResults().end(),
            [](const auto& result) {return result.status==CropResultStatus::Published;}))stage.failureReason=CropFailure::ReturnToSourceRequired;
    }
    else if(entry.reserved)stage=history.BuildAppend(entry.reserved->parentNodeId,*entry.reserved->operation,entry.reserved->nodeId);
    else stage=history.BuildSelection(entry.outcome.nodeId);
    return stage;
}

void CropHistoryQueue::SetRequestedHead(CropHistory& history) const noexcept
{
    history.SetRequestedHead(m_pending.empty()?history.GetAppliedHead():m_entries.at(m_pending.back()).outcome.nodeId);
}

void CropHistoryQueue::SetComplete(CropHistory& history,CropHistory::Stage&& stage) noexcept
{
    if(m_pending.empty()||!history.GetStageReady(stage))std::terminate();
    auto& entry=m_entries.at(m_pending.front());
    if(stage.head!=entry.outcome.nodeId)std::terminate();
    history.SetCommit(std::move(stage));
    entry.outcome.status=CropEditStatus::Succeeded;entry.outcome.stateRevision=history.GetRevision();
    entry.outcome.prune=std::move(stage.impact);
    m_pending.pop_front();SetRequestedHead(history);
}

void CropHistoryQueue::SetFailed(CropHistory& history,CropFailure failure,CropPruneImpact impact) noexcept
{
    if(m_pending.empty())return;
    auto& entry=m_entries.at(m_pending.front());
    entry.outcome.status=failure==CropFailure::Cancelled?CropEditStatus::Cancelled:CropEditStatus::Failed;
    entry.outcome.failureReason=failure;entry.outcome.stateRevision=history.GetRevision();entry.outcome.prune=std::move(impact);
    m_pending.pop_front();SetRequestedHead(history);
}

void CropHistoryQueue::SetCancelled(CropHistory& history) noexcept
{
    while(!m_pending.empty())SetFailed(history,CropFailure::Cancelled);
}

bool CropHistoryQueue::GetRequestsSame(const CropEditRequest& a,const CropEditRequest& b) { return GetSame(a,b); }
void CropHistoryQueue::ForgetOutcome(CropRequestId id) noexcept
{
    const auto found=m_entries.find(id);
    if(found!=m_entries.end()&&found->second.outcome.status!=CropEditStatus::Queued) {
        m_expiredThrough=std::max(m_expiredThrough,id);m_entries.erase(found);
    }
}
