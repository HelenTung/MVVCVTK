#include "Interaction/CropHistoryQueue.h"
#include <iostream>

namespace {
bool Check(bool value,const char* text) {if(!value)std::cerr<<"History queue: "<<text<<'\n';return value;}
CropHistory History() {DataEntityId id;id.bytes[0]=1;return CropHistory::Create({id,1});}
CropEditRequest Request(const CropHistory& h,CropRequestId id,CropNodeId node,CropEditKind kind=CropEditKind::Append)
{
    CropEditRequest q;q.documentId=h.GetDocumentId();q.requestId=id;q.expectedRevision=h.GetRevision();q.nodeId=node;q.kind=kind;
    q.operation.boxToInputModelMatrix[3]=static_cast<double>(id);return q;
}
bool Complete(CropHistory& h,CropHistoryQueue& queue)
{
    auto stage=queue.BuildNext(h);
    if(stage.failureReason!=CropFailure::None) {queue.SetFailed(h,stage.failureReason,std::move(stage.impact));return false;}
    queue.SetComplete(h,std::move(stage));return true;
}
bool GetFrozenBranches()
{
    auto h=History();CropHistoryQueue q;const auto root=h.GetRootId();
    const auto a=q.StartRequest(h,Request(h,1,root));
    const auto b=q.StartRequest(h,Request(h,2,a.nodeId));
    const auto c=q.StartRequest(h,Request(h,3,b.nodeId));
    const auto d=q.StartRequest(h,Request(h,4,a.nodeId));
    if(!Check(a.isAccepted&&b.isAccepted&&c.isAccepted&&d.isAccepted
        &&h.GetNodeCount()==1&&h.GetRequestedHead()==d.nodeId,"admission published nodes or lost requested head"))return false;
    if(!Complete(h,q)||!Complete(h,q)||!Complete(h,q)||!Complete(h,q))return false;
    if(!Check(h.GetPath(c.nodeId).size()==3&&h.GetPath(d.nodeId).size()==2
        &&h.GetNode(d.nodeId)->parentNodeId==a.nodeId&&h.GetNodeCount()==5,"queued branches were rebased onto latest head"))return false;
    auto replace=Request(h,5,b.nodeId,CropEditKind::Replace);
    replace.operation=*h.GetNode(b.nodeId)->operation;replace.operation.removalMode=CropRemovalMode::RemoveInside;
    const auto next=q.StartRequest(h,replace);
    if(!Complete(h,q))return false;
    if(!Check(h.GetNode(next.nodeId)->parentNodeId==a.nodeId&&h.GetNode(c.nodeId)->parentNodeId==b.nodeId,
        "replacement overwrote parent or child"))return false;
    auto same=Request(h,6,next.nodeId,CropEditKind::Replace);same.operation=*h.GetNode(next.nodeId)->operation;
    const auto identical=q.StartRequest(h,same);
    const auto child=q.StartRequest(h,Request(h,7,identical.nodeId));
    if(!Check(identical.nodeId==next.nodeId&&child.isAccepted,"identical replacement reserved an absent node"))return false;
    return Complete(h,q)&&Complete(h,q)&&Check(h.GetNodeCount()==7,"identical replacement added a node");
}
bool GetFailureAndReplay()
{
    auto h=History();CropHistoryQueue q;const auto root=h.GetRootId();
    const auto first=Request(h,1,root);const auto a=q.StartRequest(h,first);
    const auto b=q.StartRequest(h,Request(h,2,a.nodeId));
    const auto c=q.StartRequest(h,Request(h,3,b.nodeId));
    const auto d=q.StartRequest(h,Request(h,4,root));
    if(!Check(q.StartRequest(h,first).isReplay&&q.GetPendingCount()==4,"request replay duplicated a reservation"))return false;
    auto different=first;different.operation.radius=2;
    if(!Check(q.StartRequest(h,different).failureReason==CropFailure::InvalidRequest,"request ID accepted different content"))return false;
    q.SetFailed(h,CropFailure::PreviewNotReady);
    const auto failedB=q.BuildNext(h);q.SetFailed(h,failedB.failureReason);
    const auto failedC=q.BuildNext(h);q.SetFailed(h,failedC.failureReason);
    if(!Check(failedB.failureReason==CropFailure::ParentFailed&&failedC.failureReason==CropFailure::ParentFailed,
        "failed dependency silently reattached"))return false;
    if(!Complete(h,q))return false;
    if(!Check(q.GetOutcome(1)->status==CropEditStatus::Failed&&q.GetOutcome(2)->failureReason==CropFailure::ParentFailed
        &&q.GetOutcome(4)->status==CropEditStatus::Succeeded&&h.GetNodeCount()==2&&h.GetAppliedHead()==d.nodeId,
        "independent queued branch was cancelled"))return false;
    const auto again=q.StartRequest(h,first);
    return Check(again.isReplay&&q.GetPendingCount()==0,"completed failure replay was executed again");
}
bool GetPruneAtomic()
{
    auto h=History();CropHistoryQueue q;
    const auto a=q.StartRequest(h,Request(h,1,h.GetRootId()));if(!Complete(h,q))return false;
    const auto b=q.StartRequest(h,Request(h,2,a.nodeId));if(!Complete(h,q))return false;
    auto prune=Request(h,3,0,CropEditKind::Prune);prune.prune.nodeIds={b.nodeId};
    prune.prune.fallback=CropPruneFallback::NearestSurvivingAncestor;
    if(!Check(q.StartRequest(h,prune).isAccepted&&h.GetRequestedHead()==a.nodeId,"prune admission fallback"))return false;
    auto stage=q.BuildNext(h);
    if(!Check(stage.failureReason==CropFailure::None&&h.GetNode(b.nodeId).has_value(),"prune deleted before preview"))return false;
    q.SetFailed(h,CropFailure::PreviewNotReady);
    if(!Check(h.GetNode(b.nodeId).has_value()&&h.GetAppliedHead()==b.nodeId&&h.GetRequestedHead()==b.nodeId,
        "fallback preparation failure changed tree"))return false;
    prune.requestId=4;prune.expectedRevision=h.GetRevision();
    if(!q.StartRequest(h,prune).isAccepted||!Complete(h,q))return false;
    return Check(!h.GetNode(b.nodeId)&&h.GetAppliedHead()==a.nodeId&&q.GetOutcome(4)->prune.deletedCount==1,
        "prune commit lost deletion/impact");
}
bool GetQueueBounded()
{
    auto h=History();CropHistoryQueue q;const auto root=h.GetRootId();
    for(CropRequestId id=1;id<=256;++id)if(!q.StartRequest(h,Request(h,id,root)).isAccepted)return false;
    if(!Check(q.StartRequest(h,Request(h,257,root)).failureReason==CropFailure::ResourceLimit,"pending queue unbounded"))return false;
    q.SetCancelled(h);
    if(!Check(q.GetIsEmpty()&&h.GetRequestedHead()==root&&h.GetNodeCount()==1
        &&q.GetOutcome(256)->status==CropEditStatus::Cancelled,"cancel lost terminal outcomes"))return false;
    for(CropRequestId id=300;id<1400;++id) {
        auto request=Request(h,id,root,CropEditKind::Select);
        if(!q.StartRequest(h,request).isAccepted||!Complete(h,q))return false;
    }
    const auto expired=q.GetOutcome(1);
    return Check(expired&&expired->failureReason==CropFailure::RequestExpired
        &&q.StartRequest(h,Request(h,1,root)).failureReason==CropFailure::RequestExpired,
        "evicted request ID could execute again");
}
}
int GetCropHistoryQueueFailures()
{
    return !GetFrozenBranches()+!GetFailureAndReplay()+!GetPruneAtomic()+!GetQueueBounded();
}
