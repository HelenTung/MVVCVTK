#include "Interaction/CropHistory.h"
#include <cmath>
#include "Algorithms/CropGeometry.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace {
constexpr std::size_t nodeLimit=100000;
constexpr std::size_t depthLimit=4096;
std::atomic<std::uint64_t> nextIdentity{1};
}

CropNodeId CropHistory::CreateNodeId() noexcept
{
    auto current=nextIdentity.load(std::memory_order_relaxed);
    while (current!=0) {
        const auto next=current==std::numeric_limits<std::uint64_t>::max() ? 0 : current+1;
        if (nextIdentity.compare_exchange_weak(current,next,std::memory_order_relaxed)) return current;
    }
    return 0;
}

CropHistory CropHistory::Create(DataRevisionRef source, CropDocumentId documentId)
{
    CropHistory history;
    if (!GetDataRevisionRefValid(source)) return history;
    history.m_documentId=documentId ? documentId : CreateNodeId();
    history.m_root=CreateNodeId();
    if (!history.m_documentId || !history.m_root) return {};
    history.m_source=source;
    history.m_revision=1;
    history.m_appliedHead=history.m_requestedHead=history.m_root;
    history.m_nodes.emplace(history.m_root,CropNodeSnapshot{history.m_root,0,{}});
    history.m_children.emplace(history.m_root,std::vector<CropNodeId>{});
    return history;
}

CropHistorySnapshot CropHistory::GetSnapshot(CropNodeId after,std::size_t limit) const
{
    CropHistorySnapshot result;
    result.documentId=m_documentId;
    result.rootNodeId=m_root;
    result.sourceRevision=m_source;
    result.stateRevision=m_revision;
    result.requestedHead=m_requestedHead;
    result.appliedHead=m_appliedHead;
    result.totalNodeCount=m_nodes.size();
    result.results=m_results;
    const auto count=limit ? std::min(limit,nodeLimit) : nodeLimit;
    auto next=m_nodes.upper_bound(after);
    for (;next!=m_nodes.end() && result.nodes.size()<count;++next) result.nodes.push_back(next->second);
    if (next!=m_nodes.end() && !result.nodes.empty()) result.nextPageAfter=result.nodes.back().nodeId;
    return result;
}

std::optional<CropNodeSnapshot> CropHistory::GetNode(CropNodeId id) const
{
    const auto found=m_nodes.find(id);
    return found==m_nodes.end() ? std::optional<CropNodeSnapshot>{} : found->second;
}

std::vector<CropNodeId> CropHistory::GetChildren(CropNodeId id) const
{
    const auto found=m_children.find(id);
    return found==m_children.end() ? std::vector<CropNodeId>{} : found->second;
}

std::vector<CropNodeId> CropHistory::GetPathIds(CropNodeId id) const
{
    std::vector<CropNodeId> result;
    while (id) {
        const auto found=m_nodes.find(id);
        if (found==m_nodes.end() || result.size()>depthLimit) return {};
        result.push_back(id);
        id=found->second.parentNodeId;
    }
    if (result.empty() || result.back()!=m_root) return {};
    std::reverse(result.begin(),result.end());
    return result;
}

std::vector<CropOpItem> CropHistory::GetPath(CropNodeId id) const
{
    std::vector<CropOpItem> result;
    const auto path=GetPathIds(id);
    if (path.empty()) return result;
    result.reserve(path.size()-1);
    for (const auto node:path) {
        if (node!=m_root) result.push_back(*m_nodes.at(node).operation);
    }
    return result;
}

CropHistory::Stage CropHistory::BuildStage(CropNodeId head) const
{
    Stage stage;
    stage.documentId=m_documentId;
    stage.expectedRevision=m_revision;
    stage.head=head;
    if (!m_root || m_revision==std::numeric_limits<std::uint64_t>::max()) stage.failureReason=CropFailure::ResourceLimit;
    return stage;
}

CropHistory::Stage CropHistory::BuildAppend(CropNodeId parent,CropOpItem operation,CropNodeId reservedId) const
{
    auto stage=BuildStage(parent);
    if (stage.failureReason!=CropFailure::None) return stage;
    if (!m_nodes.count(parent)) { stage.failureReason=CropFailure::NodeNotFound; return stage; }
    const auto path=GetPathIds(parent);
    if (m_nodes.size()>=nodeLimit || path.size()>depthLimit) { stage.failureReason=CropFailure::ResourceLimit; return stage; }
    auto geometry=CropGeometry::Build(std::move(operation));
    if (!geometry) { stage.failureReason=CropFailure::BadInput; return stage; }
    const auto node=reservedId ? reservedId : CreateNodeId();
    if (!node || m_nodes.count(node)) { stage.failureReason=CropFailure::ResourceLimit; return stage; }
    operation=geometry->GetOperation();
    operation.operationIndex=node;
    stage.head=node;
    stage.operations=GetPath(parent);
    stage.operations.push_back(operation);
    stage.additions.emplace(node,CropNodeSnapshot{node,parent,std::move(operation)});
    stage.childAdditions.emplace(node,std::vector<CropNodeId>{});
    auto children=GetChildren(parent);
    children.push_back(node);
    stage.childChanges.emplace(parent,std::move(children));
    return stage;
}

CropHistory::Stage CropHistory::BuildReplace(CropNodeId original,CropOpItem operation,CropNodeId reservedId) const
{
    const auto node=GetNode(original);
    if (!node || !node->operation) {
        auto stage=BuildStage(original);
        stage.failureReason=CropFailure::NodeNotFound;
        return stage;
    }
    const auto geometry=CropGeometry::Build(operation);
    if (geometry && CropGeometry::GetOperationsSame(*node->operation,geometry->GetOperation())) return BuildSelection(original);
    return BuildAppend(node->parentNodeId,std::move(operation),reservedId);
}

CropHistory::Stage CropHistory::BuildSelection(CropNodeId nodeId) const
{
    auto stage=BuildStage(nodeId);
    if (!m_nodes.count(nodeId)) stage.failureReason=CropFailure::NodeNotFound;
    if (stage.failureReason==CropFailure::None) stage.operations=GetPath(nodeId);
    return stage;
}

bool CropHistory::SetSubtree(CropNodeId id,std::set<CropNodeId>& nodes) const
{
    if (!m_nodes.count(id)) return false;
    std::vector<CropNodeId> stack{id};
    while (!stack.empty()) {
        const auto current=stack.back(); stack.pop_back();
        if (!nodes.insert(current).second) continue;
        const auto& children=m_children.at(current);
        stack.insert(stack.end(),children.begin(),children.end());
    }
    return true;
}

CropHistory::Stage CropHistory::BuildPrune(const CropPruneRequest& request, bool isQueuedCommand) const
{
    auto stage=BuildStage(m_appliedHead);
    stage.impact.stateRevision=m_revision;
    const auto fail=[&](CropFailure error) { stage.failureReason=stage.impact.failureReason=error; };
    if (stage.failureReason!=CropFailure::None) { fail(stage.failureReason); return stage; }
    if (request.nodeIds.empty()
        || (request.scope==CropPruneScope::Descendants && request.nodeIds.size()!=1)
        || (request.fallback!=CropPruneFallback::ExplicitNode && request.explicitFallbackNode!=0)) {
        fail(CropFailure::InvalidRequest); return stage;
    }
    if ((!isQueuedCommand && m_requestedHead!=m_appliedHead) || std::any_of(m_results.begin(),m_results.end(),
        [](const auto& result) { return result.status==CropResultStatus::Building; })) {
        fail(CropFailure::Busy); return stage;
    }
    switch (request.scope) {
    case CropPruneScope::Subtrees:
        for (const auto id:request.nodeIds) {
            if (id==m_root) { fail(CropFailure::InvalidRequest); return stage; }
            if (!SetSubtree(id,stage.deletions)) { fail(CropFailure::NodeNotFound); return stage; }
        }
        break;
    case CropPruneScope::Descendants: {
        const auto id=request.nodeIds.front();
        if (!m_nodes.count(id)) { fail(CropFailure::NodeNotFound); return stage; }
        for (const auto child:GetChildren(id)) SetSubtree(child,stage.deletions);
        break;
    }
    case CropPruneScope::OutsidePaths: {
        std::set<CropNodeId> keep{m_root};
        for (const auto id:request.nodeIds) {
            const auto path=GetPathIds(id);
            if (path.empty()) { fail(CropFailure::NodeNotFound); return stage; }
            keep.insert(path.begin(),path.end());
        }
        for (const auto& entry:m_nodes) if (!keep.count(entry.first)) stage.deletions.insert(entry.first);
        break;
    }
    default: fail(CropFailure::InvalidRequest); return stage;
    }
    stage.impact.deletedCount=stage.deletions.size();
    for (const auto id:stage.deletions) {
        const auto parent=m_nodes.at(id).parentNodeId;
        if (!stage.deletions.count(parent)) stage.impact.subtreeRoots.push_back(id);
    }
    for (const auto& result:m_results) {
        for (const auto id:GetPathIds(result.nodeId)) {
            if (stage.deletions.count(id)) stage.impact.blockers.push_back({result.resultId,id});
        }
    }
    if (!stage.impact.blockers.empty()) { fail(CropFailure::PublishedResultDependency); return stage; }
    if (stage.deletions.count(m_appliedHead)) {
        switch (request.fallback) {
        case CropPruneFallback::Reject: fail(CropFailure::PreviewNotReady); return stage;
        case CropPruneFallback::NearestSurvivingAncestor:
            while (stage.deletions.count(stage.head)) stage.head=m_nodes.at(stage.head).parentNodeId;
            break;
        case CropPruneFallback::ExplicitNode:
            if (!m_nodes.count(request.explicitFallbackNode) || stage.deletions.count(request.explicitFallbackNode)) {
                fail(CropFailure::NodeNotFound); return stage;
            }
            stage.head=request.explicitFallbackNode;
            break;
        default: fail(CropFailure::InvalidRequest); return stage;
        }
        if (stage.head==m_root && std::any_of(m_results.begin(),m_results.end(),
            [](const auto& result) { return result.status==CropResultStatus::Published; })) {
            fail(CropFailure::ReturnToSourceRequired); return stage;
        }
    }
    else if (request.fallback==CropPruneFallback::ExplicitNode
        && (!m_nodes.count(request.explicitFallbackNode) || stage.deletions.count(request.explicitFallbackNode))) {
        fail(CropFailure::NodeNotFound); return stage;
    }
    else if (request.fallback!=CropPruneFallback::Reject && request.fallback!=CropPruneFallback::NearestSurvivingAncestor
        && request.fallback!=CropPruneFallback::ExplicitNode) { fail(CropFailure::InvalidRequest); return stage; }
    stage.impact.fallbackNode=stage.head;
    stage.operations=GetPath(stage.head);
    for (const auto id:stage.impact.subtreeRoots) {
        const auto parent=m_nodes.at(id).parentNodeId;
        if (stage.childChanges.count(parent)) continue;
        auto children=GetChildren(parent);
        children.erase(std::remove_if(children.begin(),children.end(),
            [&](const auto child) { return stage.deletions.count(child)!=0; }),children.end());
        stage.childChanges.emplace(parent,std::move(children));
    }
    return stage;
}

CropPruneImpact CropHistory::GetPruneImpact(const CropPruneRequest& request) const
{
    return BuildPrune(request).impact;
}

bool CropHistory::GetStageReady(const Stage& stage) const noexcept
{
    return stage.failureReason==CropFailure::None && stage.documentId==m_documentId
        && stage.expectedRevision==m_revision && m_revision!=std::numeric_limits<std::uint64_t>::max();
}

void CropHistory::SetCommit(Stage&& stage) noexcept
{
    if (!GetStageReady(stage)) std::terminate();
    for (auto& change:stage.childChanges) m_children.at(change.first).swap(change.second);
    m_nodes.merge(stage.additions);
    m_children.merge(stage.childAdditions);
    for (const auto id:stage.deletions) { m_nodes.erase(id); m_children.erase(id); }
    m_appliedHead=stage.head;
    if (!m_requestedHead || stage.deletions.count(m_requestedHead)) m_requestedHead=stage.head;
    ++m_revision;
}

bool CropHistory::GetResultsValid(const std::vector<CropResultRecord>& results) const noexcept
{
    if (m_revision==std::numeric_limits<std::uint64_t>::max()) return false;
    if (results.size()>1024) return false;
    std::size_t published=0, building=0;
    for (std::size_t index=0;index<results.size();++index) {
        const auto& result=results[index];
        if (!result.resultId || (result.inputRoi && !GetDataRevisionRefValid(*result.inputRoi)) || (result.nodeId==m_root && !result.inputRoi) || !m_nodes.count(result.nodeId)
            || result.sourceRevision!=m_source) return false;
        for (std::size_t previous=0;previous<index;++previous) if (results[previous].resultId==result.resultId) return false;
        switch (result.status) {
        case CropResultStatus::Published:
            if (++published>1 || !GetDataEntityIdValid(result.scopeId)
                || !GetDataRevisionRefValid(result.outputRevision) || !GetDataRevisionRefValid(result.recipeRevision)) return false;
            break;
        case CropResultStatus::Building:
            if (++building>1) return false;
            break;
        case CropResultStatus::Releasing:
            if (!GetDataEntityIdValid(result.scopeId)) return false;
            break;
        default: return false;
        }
    }
    return true;
}

void CropHistory::SetResults(std::vector<CropResultRecord>&& results) noexcept
{
    if (!GetResultsValid(results)) std::terminate();
    m_results.swap(results);
    ++m_revision;
}

CropDocumentArchive CropHistory::GetArchive() const
{
    CropDocumentArchive archive;
    archive.sourceRevision=m_source;
    archive.rootNodeId=m_root;
    archive.requestedHead=m_requestedHead;
    archive.appliedHead=m_appliedHead;
    for (const auto& node:m_nodes) archive.nodes.push_back(node.second);
    for (const auto& result:m_results) if (result.status==CropResultStatus::Published) archive.result=result;
    return archive;
}

std::optional<CropHistory> CropHistory::CreateFromArchive(const CropDocumentArchive& archive,CropFailure& failure,
    std::vector<CropNodeMapping>* mappings)
{
    failure=CropFailure::BadInput;
    if(mappings)mappings->clear();
    if(archive.nodes.size()>nodeLimit){failure=CropFailure::ResourceLimit;return {};}
    if (archive.schemaVersion!=1 || !GetDataRevisionRefValid(archive.sourceRevision)
        || archive.nodes.empty() || archive.nodes.size()>nodeLimit) return {};
    std::map<CropNodeId,CropNodeSnapshot> nodes;
    for (const auto& node:archive.nodes) {
        if (!node.nodeId || !nodes.emplace(node.nodeId,node).second) return {};
    }
    const auto root=nodes.find(archive.rootNodeId);
    if (root==nodes.end() || root->second.parentNodeId || root->second.operation
        || !nodes.count(archive.requestedHead) || !nodes.count(archive.appliedHead)) return {};
    std::map<CropNodeId,std::size_t> depths{{archive.rootNodeId,0}};
    for (auto& entry:nodes) {
        if (entry.first==archive.rootNodeId) continue;
        if (!entry.second.operation || entry.second.operation->operationIndex!=entry.first) return {};
        const auto geometry=CropGeometry::Build(*entry.second.operation);
        if (!geometry) return {};
        entry.second.operation=geometry->GetOperation();
        // 缓存已验证父链深度，避免每个节点重复遍历全部祖先。
        std::vector<CropNodeId> path;
        std::set<CropNodeId> pending;
        auto id=entry.first;
        while (!depths.count(id)) {
            const auto found=nodes.find(id);
            if (found==nodes.end() || !pending.insert(id).second || path.size()>=depthLimit) return {};
            path.push_back(id);
            id=found->second.parentNodeId;
        }
        auto depth=depths.at(id);
        for (auto node=path.rbegin();node!=path.rend();++node) {
            if (++depth>depthLimit) return {};
            depths.emplace(*node,depth);
        }
    }
    if (archive.result) {
        const auto& result=*archive.result;
        if (!result.resultId || (result.inputRoi && !GetDataRevisionRefValid(*result.inputRoi)) || (result.nodeId==archive.rootNodeId && !result.inputRoi) || !nodes.count(result.nodeId)
            || result.status!=CropResultStatus::Published || result.sourceRevision!=archive.sourceRevision
            || !GetDataEntityIdValid(result.scopeId) || !GetDataRevisionRefValid(result.recipeRevision)
            || !GetDataRevisionRefValid(result.outputRevision)||!result.options.availableRamBytes
            ||!std::isfinite(result.options.meshTolerance)||result.options.meshTolerance<=0||!result.options.maxCells
            ||!result.options.maxDepth||result.options.maxDepth>128||!std::isfinite(result.meshErrorBound)||result.meshErrorBound<0
            ||!std::isfinite(result.meshAreaErrorBound)||result.meshAreaErrorBound<0) return {};
    }
    auto history=Create(archive.sourceRevision);
    if (!history.m_root) { failure=CropFailure::ResourceLimit; return {}; }
    std::map<CropNodeId,CropNodeId> identities{{archive.rootNodeId,history.m_root}};
    for (const auto& node:nodes) if (node.first!=archive.rootNodeId) {
        const auto id=CreateNodeId();
        if (!id) { failure=CropFailure::ResourceLimit; return {}; }
        identities.emplace(node.first,id);
    }
    for (const auto& entry:nodes) {
        if (entry.first==archive.rootNodeId) continue;
        auto node=entry.second;
        node.nodeId=identities.at(entry.first);
        node.parentNodeId=identities.at(node.parentNodeId);
        node.operation->operationIndex=node.nodeId;
        history.m_nodes.emplace(node.nodeId,node);
        history.m_children.try_emplace(node.nodeId);
        history.m_children[node.parentNodeId].push_back(node.nodeId);
    }
    history.m_requestedHead=identities.at(archive.requestedHead);
    history.m_appliedHead=identities.at(archive.appliedHead);
    // 这里只恢复历史；有效结果的载荷授权及关联由 Host 原子验证后另行接管。
    if(mappings){mappings->reserve(identities.size());for(const auto& item:identities)mappings->push_back({item.first,item.second});}
    failure=CropFailure::None;
    return history;
}

bool CropHistory::GetGeometrySame(const GridGeometry3D& a,const GridGeometry3D& b) noexcept {
    return a.extent==b.extent&&a.dimensions==b.dimensions&&a.spacing==b.spacing&&a.origin==b.origin
        &&a.direction==b.direction&&a.coordinateFrame==b.coordinateFrame;
}
bool CropHistory::GetArchivesSame(const CropDocumentArchive& a,const CropDocumentArchive& b) {
    if(a.schemaVersion!=b.schemaVersion||a.sourceRevision!=b.sourceRevision||a.sourceType!=b.sourceType
        ||a.coordinateFrame!=b.coordinateFrame||a.maskSourceRevision!=b.maskSourceRevision
        ||a.rootNodeId!=b.rootNodeId||a.requestedHead!=b.requestedHead||a.appliedHead!=b.appliedHead
        ||a.nodes.size()!=b.nodes.size()||bool(a.imageGeometry)!=bool(b.imageGeometry)||bool(a.result)!=bool(b.result))return false;
    if(a.imageGeometry&&!GetGeometrySame(*a.imageGeometry,*b.imageGeometry))return false;
    for(std::size_t index=0;index<a.nodes.size();++index) {
        const auto& x=a.nodes[index];const auto& y=b.nodes[index];
        if(x.nodeId!=y.nodeId||x.parentNodeId!=y.parentNodeId||bool(x.operation)!=bool(y.operation))return false;
        if(x.operation) {
            const auto& p=*x.operation;const auto& q=*y.operation;
            if(p.operationIndex!=q.operationIndex||p.geometryType!=q.geometryType||p.removalMode!=q.removalMode
                ||p.boxToInputModelMatrix!=q.boxToInputModelMatrix||p.planeCenterInInputModel!=q.planeCenterInInputModel
                ||p.planeNormalInInputModel!=q.planeNormalInInputModel||p.centerInInputModel!=q.centerInInputModel
                ||p.axisInInputModel!=q.axisInInputModel||p.radius!=q.radius||p.height!=q.height
                ||p.recipeVersion!=q.recipeVersion||p.boundaryPolicyVersion!=q.boundaryPolicyVersion)return false;
        }
    }
    if(a.result) {
        const auto& x=*a.result;const auto& y=*b.result;
        if(!GetRecordsSame(x,y))return false;
    }
    return true;
}

bool CropHistory::GetRecordsSame(const CropResultRecord& x,const CropResultRecord& y) noexcept {
    return x.resultId==y.resultId&&x.nodeId==y.nodeId&&x.status==y.status&&x.scopeId==y.scopeId
        &&x.sourceRevision==y.sourceRevision&&x.recipeRevision==y.recipeRevision&&x.outputRevision==y.outputRevision
        &&x.inputRoi==y.inputRoi &&x.publicationGeneration==y.publicationGeneration&&x.options==y.options
        &&x.meshErrorBound==y.meshErrorBound&&x.meshAreaErrorBound==y.meshAreaErrorBound&&x.meshTriangleCount==y.meshTriangleCount;
}
