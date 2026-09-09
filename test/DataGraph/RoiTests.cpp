#include "Data/DataGraphStore.h"
#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool Check(bool value, const char* message)
{
    if (!value) std::cerr << message << '\n';
    return value;
}

struct Fixture final {
    DataGraphStore store;
    GridGeometry3D grid;
    DataRevisionRef source{store.CreateDataEntityId(),1};
    Fixture()
    {
        grid.extent={-2,2,5,7,10,12}; grid.dimensions={5,3,3};
        grid.spacing={0.5,2,3}; grid.origin={7,9,11};
        grid.direction={0,-1,0,1,0,0,0,0,1};
    }
    DataRevisionDraft GetImage() const
    {
        auto bytes=std::make_shared<const std::vector<std::uint8_t>>(45,1);
        return {source.entityId,0,DataTypes::imageGrid3D,{},
            std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,bytes)};
    }
    RoiDefinition GetBox() const
    {
        RoiDefinition definition; definition.source=source;
        RoiNode box;
        box.primitive.localToSource={0,-1,0,-5, 0.5,0,0,9, 0,0,1.5,44, 0,0,0,1};
        definition.nodes.push_back(box);
        return definition;
    }
    DataRevisionDraft GetRoi(const RoiDefinition& definition)
    {
        return {store.CreateDataEntityId(),0,DataTypes::roiGeometry,
            RoiEvaluator::GetInputs(definition),std::make_shared<const RoiGeometryPayload>(definition)};
    }
};

bool GetGeometryValid()
{
    Fixture f;
    DataTransaction tx;
    tx.outputs={f.GetImage(),f.GetRoi(f.GetBox())};
    const auto committed=f.store.SetDataCommit(std::move(tx));
    if (!Check(committed.status==DataCommitStatus::Succeeded,"same-transaction source/ROI failed")) return false;
    const auto result=RoiEvaluator::GetRoi(committed.graph,committed.published[1]->self,f.source);
    if (!Check(result.error==RoiError::None && result.roi,"ROI freeze failed")) return false;
    const auto& roi=*result.roi;
    if (!Check(roi.GetContains({-5,9,44}) && roi.GetContains({-6,9.5,45.5})
        && !roi.GetContains({-5,9.6,44}) && !roi.GetContains({-5,9,46})
        && !roi.GetContains({NAN,9,44}),"rotated box/source geometry is incorrect")) return false;
    const auto planes=roi.GetClipPlanes();
    if (!Check(planes.error==RoiError::None && planes.planes.size()==12
        && std::abs(roi.GetBoundaryDistance({-5,9,44})-0.5)<1e-10,"convex boundary contract failed")) return false;
    RoiMaskRequest request; request.region={{0,0,0},{5,3,3}}; request.maxBytes=7;
    std::vector<std::uint8_t> mask;
    while (true) {
        const auto chunk=roi.GetMaskChunk(request);
        if (!Check(chunk.error==RoiError::None && chunk.values.size()<=7,"mask chunk failed")) return false;
        mask.insert(mask.end(),chunk.values.begin(),chunk.values.end());
        if (chunk.isComplete) break;
        request.voxelOffset=chunk.nextOffset;
    }
    std::size_t selected=0; for (auto value:mask) selected+=value;
    if (!Check(mask.size()==45 && selected==3 && mask[22]==1,"nonzero extent rasterization is incorrect")) return false;
    request.voxelOffset=0; request.getCancelled=[] {return true;};
    const auto cancelled=roi.GetMaskChunk(request);
    if (!Check(cancelled.error==RoiError::Cancelled && cancelled.values.empty() && !cancelled.isComplete,"cancelled mask leaked a successful partial chunk")) return false;
    request.getCancelled=[]()->bool {throw 1;};
    if (!Check(roi.GetMaskChunk(request).error==RoiError::Cancelled,"cancel exception escaped")) return false;
    request.getCancelled={}; request.region.offset[0]=std::numeric_limits<std::size_t>::max();
    if (!Check(roi.GetMaskChunk(request).error==RoiError::InvalidRequest,"overflow region accepted")) return false;
    return Check(roi.GetPoints({{{-5,9,44}},{{-5,9,50}}},1).error==RoiError::TooLarge,"point budget ignored");
}

bool GetDefinitionRejected()
{
    Fixture f;
    auto invalid=f.GetBox(); invalid.nodes[0].primitive.localToSource[15]=0;
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::InvalidGeometry,"projective matrix accepted")) return false;
    invalid=f.GetBox(); invalid.nodes[0].primitive.localToSource[4]=0;
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::InvalidGeometry,"singular matrix accepted")) return false;
    invalid=f.GetBox(); invalid.nodes[0].primitive={}; invalid.nodes[0].primitive.shape=RoiShape::HalfSpace; invalid.nodes[0].primitive.normal={0,0,0};
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::InvalidGeometry,"zero normal accepted")) return false;
    invalid=f.GetBox(); invalid.nodes.push_back({RoiNodeKind::Intersection,{},0,2});
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::InvalidGeometry,"forward node accepted")) return false;
    invalid=f.GetBox(); invalid.nodes.push_back({RoiNodeKind::SourceDomain});
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::InvalidGeometry,"unreachable node accepted")) return false;
    invalid=f.GetBox();
    for (std::uint32_t i=1;i<=roiDepthLimit;++i) invalid.nodes.push_back({RoiNodeKind::Intersection,{},i-1,i-1});
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::TooLarge,"DAG depth limit ignored")) return false;
    invalid.nodes.resize(roiNodeLimit+1);
    if (!Check(RoiEvaluator::GetDefinitionError(invalid)==RoiError::TooLarge,"node limit ignored")) return false;
    DataTransaction tx; tx.outputs={f.GetImage(),f.GetRoi(f.GetBox())}; tx.outputs[1].inputs.clear();
    const auto before=f.store.GetDataGraph().commitId;
    if (!Check(f.store.SetDataCommit(tx).status==DataCommitStatus::Rejected && f.store.GetDataGraph().commitId==before,"missing source relation committed partial data")) return false;
    auto oldDescriptor=DataTypeDescriptor{{DataTypes::roiGeometry.name,1},{},[](const IDataPayload&,std::string&){return true;}};
    return Check(!f.store.SetDataType(oldDescriptor),"legacy ROI schema registered through generic port");
}

bool GetMasksAndBooleanValid()
{
    Fixture f;
    const DataRevisionRef maskRef{f.store.CreateDataEntityId(),1};
    auto values=std::make_shared<std::vector<std::uint8_t>>(45,0); (*values)[22]=7;
    auto definition=f.GetBox();
    RoiNode mask; mask.primitive.shape=RoiShape::MaskReference; mask.primitive.mask=maskRef;
    definition.nodes.push_back(mask); definition.nodes.push_back({RoiNodeKind::Difference,{},0,1});
    DataTransaction tx;
    tx.outputs={f.GetImage(),{maskRef.entityId,0,DataTypes::binaryMask3D,{},std::make_shared<const BinaryMask3DPayload>(f.grid,values)},f.GetRoi(definition)};
    auto invalid=tx; invalid.outputs[2].inputs.pop_back();
    if (!Check(f.store.SetDataCommit(invalid).status==DataCommitStatus::Rejected && f.store.GetDataGraph().commitId==0,"hidden mask dependency accepted")) return false;
    invalid=tx; auto wrongGrid=f.grid; wrongGrid.origin[0]+=1;
    invalid.outputs[1].payload=std::make_shared<const BinaryMask3DPayload>(wrongGrid,values);
    if (!Check(f.store.SetDataCommit(invalid).status==DataCommitStatus::Rejected,"same-size different-origin mask accepted")) return false;
    const auto committed=f.store.SetDataCommit(tx);
    if (!Check(committed.status==DataCommitStatus::Succeeded,"staged mask/ROI rejected")) return false;
    auto result=RoiEvaluator::GetRoi(committed.graph,committed.published.back()->self,f.source);
    if (!Check(result.roi && !result.roi->GetContains({-5,9,44}) && result.roi->GetContains({-5,9.5,44})
        && result.roi->GetClipPlanes().error==RoiError::UnsupportedRoi,"difference/mask selection or clipping capability is incorrect")) return false;
    (*values)[22]=0;
    if (!Check(!result.roi->GetContains({-5,9,44}),"ROI mask snapshot mutated via original buffer")) return false;
    if (!Check(RoiEvaluator::GetRoi(committed.graph,committed.published.back()->self,maskRef).error==RoiError::SourceMismatch,"wrong source identity accepted")) return false;
    definition.nodes.back().kind=RoiNodeKind::Union;
    tx.outputs={f.GetRoi(definition)};
    const auto unionCommit=f.store.SetDataCommit(tx);
    result=RoiEvaluator::GetRoi(unionCommit.graph,unionCommit.published.front()->self,f.source);
    return Check(result.roi && result.roi->GetContains({-5,9,44}),"union failed");
}
bool GetServiceValid()
{
    Fixture f; DataTransaction setup; setup.outputs={f.GetImage()}; f.store.SetDataCommit(setup);
    RoiService service(f.store);
    RoiRequest create; create.definition=f.GetBox(); create.metadata.name="region";
    const auto first=service.SetRoi(create);
    if (!Check(first.error==RoiError::None && first.roi,"headless ROI creation failed")) return false;
    const auto firstRef=first.roi->revision;
    DataTransaction bypass; auto draft=f.GetRoi(f.GetBox());
    draft.entityId=firstRef.entityId; draft.expectedGeneration=firstRef.generation; bypass.outputs={draft};
    const auto beforeBypass=f.store.GetDataGraph().commitId;
    if (!Check(f.store.SetDataCommit(bypass).status==DataCommitStatus::Rejected
        && f.store.GetDataGraph().commitId==beforeBypass,"generic geometry write bypassed atomic catalog update")) return false;
    const auto frozen=RoiEvaluator::GetRoi(f.store.GetDataGraph(),firstRef,f.source).roi;
    RoiRequest rename; rename.action=RoiAction::SetMetadata; rename.expectedRoi=firstRef;
    rename.expectedCatalogRevision=first.roi->catalogRevision; rename.metadata.name="renamed";
    const auto renamed=service.SetRoi(rename);
    if (!Check(renamed.error==RoiError::None && renamed.roi->revision==firstRef,"rename changed numerical revision")) return false;
    if (!Check(service.SetRoi(rename).error==RoiError::RevisionConflict,"stale catalog write accepted")) return false;
    RoiRequest change; change.action=RoiAction::SetGeometry; change.definition=f.GetBox();
    change.definition.nodes[0].primitive.localToSource[3]-=0.75;
    change.expectedRoi=firstRef; change.expectedCatalogRevision=renamed.roi->catalogRevision;
    const auto updated=service.SetRoi(change);
    if (!Check(updated.error==RoiError::None && updated.roi->revision.entityId==firstRef.entityId
        && updated.roi->revision.generation==2 && frozen->GetContains({-4.5,9,44}),"geometry edit/history stability failed")) return false;
    const auto historical=RoiService::GetDescriptor(f.store.GetDataGraph(),firstRef);
    if (!Check(historical && historical->currentRevision==updated.roi->revision && historical->metadata.name=="renamed","historical/current descriptor confused")) return false;
    change.expectedCatalogRevision=updated.roi->catalogRevision;
    if (!Check(service.SetRoi(change).error==RoiError::RevisionConflict,"stale geometry overwrote current")) return false;
    RoiRequest copy; copy.action=RoiAction::Copy; copy.copyFrom=firstRef; copy.metadata.name="copy";
    copy.expectedCatalogRevision=updated.roi->catalogRevision;
    const auto copied=service.SetRoi(copy);
    if (!Check(copied.error==RoiError::None && copied.roi->revision.entityId!=firstRef.entityId,"copy reused original entity")) return false;
    rename.expectedRoi=updated.roi->revision; rename.expectedCatalogRevision=copied.roi->catalogRevision;
    rename.metadata.isArchived=true;
    const auto archived=service.SetRoi(rename);
    return Check(archived.error==RoiError::None && RoiService::GetDescriptors(f.store.GetDataGraph()).size()==1
        && RoiService::GetDescriptors(f.store.GetDataGraph(),true).size()==2
        && f.store.GetData(f.store.GetDataGraph(),firstRef),"archive removed historical ROI or remained in active catalog");
}

bool GetArchiveValid()
{
    Fixture f; DataTransaction setup; setup.outputs={f.GetImage()}; f.store.SetDataCommit(setup);
    const DataRevisionRef mask{f.store.CreateDataEntityId(),1};
    setup.outputs={{mask.entityId,0,DataTypes::binaryMask3D,{},
        std::make_shared<const BinaryMask3DPayload>(f.grid,std::make_shared<const std::vector<std::uint8_t>>(45,1))}};
    f.store.SetDataCommit(setup);
    RoiNode node; node.primitive.shape=RoiShape::MaskReference; node.primitive.mask=mask;
    RoiRequest create; create.definition={f.source,{node}}; create.metadata.name="persisted";
    RoiService service(f.store); const auto first=service.SetRoi(create);
    if (!Check(first.error==RoiError::None && first.roi,"archive fixture failed")) return false;
    const auto exported=RoiService::GetArchive(f.store.GetDataGraph(),first.roi->revision,"sample-key",roiCopyLimit);
    if (!Check(exported.error==RoiError::None && exported.archive && !exported.archive->nodes[0].primitive.mask
        && exported.archive->masks.size()==1,"archive retained runtime mask identity")) return false;
    Fixture target; DataTransaction source; source.outputs={target.GetImage()}; target.store.SetDataCommit(source);
    RoiService restored(target.store);
    const auto before=target.store.GetDataGraph().commitId;
    if (!Check(restored.LoadArchive(*exported.archive,"wrong-key",target.source,0,roiCopyLimit).error==RoiError::SourceUnresolved
        && target.store.GetDataGraph().commitId==before,"unresolved source restore wrote data")) return false;
    auto wrong=*exported.archive; wrong.source.image->origin[0]+=1;
    if (!Check(restored.LoadArchive(wrong,"sample-key",target.source,0,roiCopyLimit).error==RoiError::SourceMismatch
        && target.store.GetDataGraph().commitId==before,"wrong physical source accepted")) return false;
    if (!Check(restored.LoadArchive(*exported.archive,"sample-key",target.source,0,1).error==RoiError::TooLarge
        && target.store.GetDataGraph().commitId==before,"restore budget failed after commit")) return false;
    wrong=*exported.archive; wrong.masks[0].nodeIndices.push_back(0);
    if (!Check(restored.LoadArchive(wrong,"sample-key",target.source,0,roiCopyLimit).error==RoiError::InvalidRequest
        && target.store.GetDataGraph().commitId==before,"duplicate mask mapping partially restored")) return false;
    const auto loaded=restored.LoadArchive(*exported.archive,"sample-key",target.source,0,roiCopyLimit);
    if (!Check(loaded.error==RoiError::None && loaded.roi && loaded.roi->definition.source==target.source
        && target.store.GetDataGraph().commitId==before+1,"restore was not one atomic transaction")) return false;
    const auto roi=RoiEvaluator::GetRoi(target.store.GetDataGraph(),loaded.roi->revision,target.source);
    return Check(roi.roi && roi.roi->GetContains({-5,9,44})
        && loaded.roi->definition.nodes[0].primitive.mask!=mask,"restored ROI lost selection or reused runtime IDs");
}

bool GetArchiveSourceBudgetValid()
{
    Fixture f;
    auto bytes=std::make_shared<const std::vector<std::uint8_t>>(45,1);
    ImageMetadata metadata; metadata.identity.datasetId.assign(roiCopyLimit,'x');
    DataTransaction tx; tx.outputs={{f.source.entityId,0,DataTypes::imageGrid3D,{},
        std::make_shared<const ImageGrid3DPayload>(f.grid,ImageValueType::UInt8,1,bytes,DataBytes{},std::array<double,2>{0,1},metadata)}};
    if (!Check(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"large identity source fixture failed")) return false;
    RoiService service(f.store); RoiRequest request; request.definition=f.GetBox(); request.metadata.name="budget";
    const auto made=service.SetRoi(request);
    if (!Check(made.roi.has_value(),"large identity ROI fixture failed")) return false;
    const auto before=f.store.GetDataGraph().commitId;
    const auto archive=RoiService::GetArchive(f.store.GetDataGraph(),made.roi->revision,"key",roiCopyLimit);
    return Check(archive.error==RoiError::TooLarge && !archive.archive && archive.requiredBytes>roiCopyLimit
        && f.store.GetDataGraph().commitId==before,"source descriptor escaped archive work budget");
}

bool GetSourceBindingCasValid()
{
    Fixture f; DataTransaction setup; setup.outputs={f.GetImage()};
    setup.bindings={{std::string(primaryVolumeBinding),0,true,{},f.source}};
    if (f.store.SetDataCommit(setup).status!=DataCommitStatus::Succeeded) return false;
    const auto initial=f.store.GetDataGraph().view->GetDataBinding(primaryVolumeBinding);
    RoiRequest request; request.definition=f.GetBox(); request.metadata.name="binding-cas"; request.expectedSourceBinding=initial;
    DataTransaction clear; clear.bindings={{std::string(primaryVolumeBinding),initial->revision,true,f.source,{}}};
    if (f.store.SetDataCommit(clear).status!=DataCommitStatus::Succeeded) return false;
    DataTransaction restore; restore.bindings={{std::string(primaryVolumeBinding),initial->revision+1,true,{},f.source}};
    if (f.store.SetDataCommit(restore).status!=DataCommitStatus::Succeeded) return false;
    RoiService service(f.store); const auto before=f.store.GetDataGraph().commitId;
    if (!Check(service.SetRoi(request).error==RoiError::RevisionConflict && f.store.GetDataGraph().commitId==before,
        "source binding ABA escaped ROI CAS")) return false;
    request.expectedSourceBinding=f.store.GetDataGraph().view->GetDataBinding(primaryVolumeBinding);
    return Check(service.SetRoi(request).error==RoiError::None,"current source binding expectation rejected");
}

bool GetCropBoundariesValid()
{
    Fixture f;DataTransaction setup;setup.outputs={f.GetImage()};
    if(f.store.SetDataCommit(setup).status!=DataCommitStatus::Succeeded)return false;
    const auto freeze=[&](RoiPrimitive primitive) {
        RoiDefinition definition;definition.source=f.source;definition.nodes={{RoiNodeKind::Primitive,primitive}};
        DataTransaction tx;tx.outputs={f.GetRoi(definition)};const auto committed=f.store.SetDataCommit(tx);
        return committed.published.empty()?RoiReadSnapshot{}:RoiEvaluator::GetRoi(committed.graph,committed.published.front()->self,f.source).roi;
    };
    RoiPrimitive plane;plane.shape=RoiShape::HalfSpace;plane.origin={-5,9,44};plane.normal={1,0,0};
    const auto closed=freeze(plane);plane.boundaryPolicy=RoiBoundaryPolicy::CropV1;const auto strict=freeze(plane);
    if(!Check(closed&&strict&&closed->GetContains({-5,9,44})&&!strict->GetContains({-5,9,44})
        &&strict->GetContains({-4.9,9,44})&&strict->GetClipPlanes().error==RoiError::UnsupportedRoi,"Crop plane boundary migration"))return false;
    RoiPrimitive sphere;sphere.shape=RoiShape::Sphere;sphere.origin={-5,9,44};sphere.radius=.5;sphere.boundaryPolicy=RoiBoundaryPolicy::CropV1;
    const auto ball=freeze(sphere);
    if(!Check(ball&&ball->GetContains({-4.5,9,44})&&!ball->GetContains({-4.499,9,44})
        &&ball->GetClipPlanes().error==RoiError::UnsupportedRoi,"sphere exact boundary/capability"))return false;
    auto cylinder=sphere;cylinder.shape=RoiShape::Cylinder;cylinder.normal={0,0,2};cylinder.height=2;
    const auto tube=freeze(cylinder);
    if(!Check(tube&&tube->GetContains({-4.5,9,45})&&!tube->GetContains({-5,9,45.01}),"finite cylinder normalized axis/caps"))return false;
    auto box=f.GetBox().nodes.front().primitive;const auto oldBox=freeze(box);box.boundaryPolicy=RoiBoundaryPolicy::CropV1;const auto cropBox=freeze(box);
    if(!Check(oldBox&&cropBox&&!oldBox->GetContains({-5,9.50000025,44})&&cropBox->GetContains({-5,9.50000025,44}),"Crop Box tolerance preserved without changing Closed"))return false;
    sphere.height=1;if(!Check(!freeze(sphere),"inactive sphere height accepted"))return false;
    plane.boundaryPolicy=static_cast<RoiBoundaryPolicy>(99);
    return Check(!freeze(plane),"unknown boundary policy accepted");
}

}
int main()
{
    return GetCropBoundariesValid() && GetGeometryValid() && GetDefinitionRejected() && GetMasksAndBooleanValid() && GetServiceValid() && GetArchiveValid() && GetArchiveSourceBudgetValid() && GetSourceBindingCasValid() ? 0 : 1;
}
