#include "Interaction/CropHistory.h"
#include "Interaction/CropHistoryQueue.h"

#include <iostream>

namespace {
bool Check(bool value,const char* message)
{
    if (!value) std::cerr << "History: " << message << '\n';
    return value;
}
DataRevisionRef Ref(unsigned char value)
{
    DataEntityId id; id.bytes[0]=value; return {id,1};
}
CropNodeId Append(CropHistory& history,CropNodeId parent,double x)
{
    CropOpItem op; op.boxToInputModelMatrix[3]=x;
    auto stage=history.BuildAppend(parent,op);
    if (stage.failureReason!=CropFailure::None) return 0;
    const auto id=stage.head;
    history.SetRequestedHead(id); history.SetCommit(std::move(stage));
    return id;
}
bool Select(CropHistory& history,CropNodeId id)
{
    auto stage=history.BuildSelection(id);
    if (stage.failureReason!=CropFailure::None) return false;
    history.SetRequestedHead(id); history.SetCommit(std::move(stage));
    return true;
}
CropResultRecord Result(CropNodeId id)
{
    CropResultRecord result;
    result.resultId=100; result.nodeId=id; result.status=CropResultStatus::Published;
    result.scopeId=Ref(5).entityId; result.sourceRevision=Ref(1);
    result.recipeRevision=Ref(6); result.outputRevision=Ref(7);
    return result;
}
bool GetBranchesValid()
{
    auto history=CropHistory::Create(Ref(1)); const auto root=history.GetRootId();
    const auto a=Append(history,root,1),b=Append(history,a,2),c=Append(history,b,3);
    if (!Select(history,a)) return false;
    const auto d=Append(history,a,4);
    if (!Check(a&&b&&c&&d&&history.GetNodeCount()==5,"branch append lost nodes")) return false;
    const auto pathC=history.GetPath(c),pathD=history.GetPath(d);
    if (!Check(pathC.size()==3&&pathD.size()==2&&pathC[1].operationIndex==b&&pathD[1].operationIndex==d,
        "target path mixed sibling operations")) return false;
    auto replacement=*history.GetNode(b)->operation;
    replacement.removalMode=CropRemovalMode::RemoveInside;
    auto stage=history.BuildReplace(b,replacement); const auto next=stage.head;
    history.SetRequestedHead(next); history.SetCommit(std::move(stage));
    if (!Check(history.GetNode(next)->parentNodeId==a && history.GetNode(c)->parentNodeId==b
        && history.GetNode(b)->operation->removalMode==CropRemovalMode::KeepInside,"replacement changed original branch")) return false;
    const auto archive=history.GetArchive();
    CropFailure failure;
    const auto restored=CropHistory::CreateFromArchive(archive,failure);
    if (!Check(restored && restored->GetDocumentId()!=history.GetDocumentId()
        && restored->GetNodeCount()==history.GetNodeCount() && restored->GetPath(restored->GetAppliedHead()).size()==2,
        "archive relationships or runtime identity")) return false;
    auto invalid=archive; invalid.nodes.back().parentNodeId=invalid.nodes.back().nodeId;
    return Check(!CropHistory::CreateFromArchive(invalid,failure),"archive cycle accepted");
}
bool GetPruneProtected()
{
    auto history=CropHistory::Create(Ref(1)); const auto root=history.GetRootId();
    const auto a=Append(history,root,1),b=Append(history,a,2),c=Append(history,b,3),e=Append(history,c,5);
    const auto d=Append(history,a,4);
    history.SetResults({Result(c)});
    CropPruneRequest request; request.nodeIds={b,d};
    const auto before=history.GetNodeCount();
    if (!Check(history.GetPruneImpact(request).failureReason==CropFailure::PublishedResultDependency
        && history.GetNodeCount()==before && history.GetNode(d).has_value(),"batch protection not atomic")) return false;
    request.scope=CropPruneScope::Descendants; request.nodeIds={c};
    auto descendants=history.BuildPrune(request);
    if (!Check(descendants.failureReason==CropFailure::None&&descendants.impact.deletedCount==1,"published node descendants incorrectly protected")) return false;
    history.SetCommit(std::move(descendants));
    if (!Check(!history.GetNode(e)&&history.GetNode(c).has_value(),"descendants removed endpoint")) return false;
    request.scope=CropPruneScope::OutsidePaths; request.nodeIds={d};
    if (!Check(history.GetPruneImpact(request).failureReason==CropFailure::PublishedResultDependency,"outside paths bypassed protection")) return false;
    auto releasing=Result(c); releasing.status=CropResultStatus::Releasing;
    history.SetResults({releasing});
    request.scope=CropPruneScope::Subtrees; request.nodeIds={b};
    if (!Check(history.GetPruneImpact(request).failureReason==CropFailure::PublishedResultDependency,"releasing path lost protection")) return false;
    history.SetResults({});
    auto prune=history.BuildPrune(request);
    if (!Check(prune.failureReason==CropFailure::None,"released path remained protected")) return false;
    history.SetCommit(std::move(prune));
    return Check(!history.GetNode(b)&&!history.GetNode(c)&&history.GetNode(d).has_value(),"released subtree prune damaged sibling");
}
bool GetFallbackValid()
{
    auto history=CropHistory::Create(Ref(1)); const auto root=history.GetRootId();
    const auto c=Append(history,root,1),d=Append(history,root,2);
    history.SetResults({Result(c)});
    CropPruneRequest request; request.nodeIds={d}; request.fallback=CropPruneFallback::NearestSurvivingAncestor;
    if (!Check(history.GetPruneImpact(request).failureReason==CropFailure::ReturnToSourceRequired,"prune silently returned to Root")) return false;
    request.fallback=CropPruneFallback::ExplicitNode; request.explicitFallbackNode=c;
    auto stage=history.BuildPrune(request);
    if (!Check(stage.failureReason==CropFailure::None&&stage.head==c,"explicit surviving fallback rejected")) return false;
    history.SetCommit(std::move(stage));
    if (!Check(history.GetAppliedHead()==c&&history.GetRequestedHead()==c,"fallback heads diverged")) return false;
    auto stale=history.BuildSelection(root);
    history.SetResults({});
    return Check(!history.GetStageReady(stale),"stage ignored protection revision change");
}
bool GetNoPartialStage()
{
    auto history=CropHistory::Create(Ref(1)); const auto root=history.GetRootId();
    CropOpItem op;
    const auto abandoned=history.BuildAppend(root,op);
    if (!Check(history.GetNodeCount()==1&&!history.GetNode(abandoned.head),"prepared node published before preview commit")) return false;
    const auto id=Append(history,root,1);
    CropPruneRequest request; request.nodeIds={id}; request.fallback=CropPruneFallback::NearestSurvivingAncestor;
    auto prepared=history.BuildPrune(request);
    if (!Check(history.GetNode(id).has_value(),"prune preparation deleted history")) return false;
    history.SetCommit(std::move(prepared));
    const auto next=Append(history,root,2);
    return Check(next!=id&&next!=abandoned.head,"deleted/reserved identity reused");
}
bool GetLargeHistoryValid()
{
    const auto makeArchive=[](CropNodeId count,bool isChain) {
        CropDocumentArchive archive;archive.sourceRevision=Ref(1);archive.rootNodeId=1;
        archive.requestedHead=archive.appliedHead=isChain?count:1;
        archive.nodes.reserve(static_cast<std::size_t>(count));
        archive.nodes.push_back({1,0,{}});
        for(CropNodeId id=2;id<=count;++id) {
            CropOpItem operation;operation.operationIndex=id;
            archive.nodes.push_back({id,isChain?id-1:1,operation});
        }
        return archive;
    };
    const auto checkAppend=[](CropHistory& history,CropNodeId parent) {
        const auto next=Append(history,parent,1);
        if(!next)return false;
        CropHistoryQueue queue;CropEditRequest request;
        request.documentId=history.GetDocumentId();request.requestId=1;
        request.expectedRevision=history.GetRevision();request.nodeId=next;request.kind=CropEditKind::Append;
        const auto admitted=queue.StartRequest(history,request);
        if(!admitted)return false;
        auto stage=queue.BuildNext(history);
        if(stage.failureReason!=CropFailure::None)return false;
        queue.SetComplete(history,std::move(stage));
        return queue.GetOutcome(1)->status==CropEditStatus::Succeeded;
    };
    CropFailure failure;
    auto deepArchive=makeArchive(5001,true);
    auto deep=CropHistory::CreateFromArchive(deepArchive,failure);
    if(!Check(deep&&failure==CropFailure::None
        &&deep->GetPath(deep->GetAppliedHead()).size()==5000
        &&checkAppend(*deep,deep->GetAppliedHead())
        &&deep->GetPath(deep->GetAppliedHead()).size()==5002,"deep history restore/append/queue still has a 4096 limit"))return false;
    deepArchive.nodes.back().parentNodeId=deepArchive.nodes.back().nodeId;
    if(!Check(!CropHistory::CreateFromArchive(deepArchive,failure),"deep archive cycle accepted"))return false;
    deepArchive.nodes.back().parentNodeId=999999;
    if(!Check(!CropHistory::CreateFromArchive(deepArchive,failure),"deep archive missing parent accepted"))return false;
    auto wide=CropHistory::CreateFromArchive(makeArchive(100001,false),failure);
    if(!Check(wide&&failure==CropFailure::None&&checkAppend(*wide,wide->GetRootId())
        &&wide->GetNodeCount()==100003,"large history restore/append/queue still has a 100000 limit"))return false;
    const auto all=wide->GetSnapshot(0,0),explicitPage=wide->GetSnapshot(0,100003);
    return Check(all.nodes.size()==100003&&!all.nextPageAfter
        &&explicitPage.nodes.size()==100003&&!explicitPage.nextPageAfter
        &&wide->GetSnapshot(0,17).nodes.size()==17,"history pagination silently truncates at 100000");
}
}

int GetCropHistoryQueueFailures();
int GetCropHistoryFailures()
{
    return GetCropHistoryQueueFailures()+(!GetBranchesValid())+(!GetPruneProtected())+(!GetFallbackValid())+(!GetNoPartialStage())+(!GetLargeHistoryValid());
}
