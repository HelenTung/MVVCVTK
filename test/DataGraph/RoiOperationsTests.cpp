#include "RoiArchiveTestReader.h"
#include "Support/RoiOperationDiagnostics.h"
#include <queue>
#include <cstring>
#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"
#include <set>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <future>
#include <chrono>
#include <climits>

namespace {
void Require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
struct Fixture {
    DataGraphStore store;RoiService service{store};GridGeometry3D grid;
    DataRevisionRef source{store.CreateDataEntityId(),1};
    explicit Fixture(std::array<double,9> direction={1,0,0,0,1,0,0,0,1}) {
        grid.extent={0,8,0,8,0,8};grid.dimensions={9,9,9};grid.spacing={1,2,3};
        grid.direction=direction;
        DataTransaction tx;tx.outputs={{source.entityId,0,DataTypes::imageGrid3D,{},std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,std::make_shared<const std::vector<std::uint8_t>>(729,10))}};
        Require(store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"source fixture failed");
    }
    Fixture(GridGeometry3D geometry,std::vector<std::uint8_t> values,DataBytes mask={}):grid(std::move(geometry)) {
        DataTransaction tx;tx.outputs={{source.entityId,0,DataTypes::imageGrid3D,{},
            std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,
                std::make_shared<const std::vector<std::uint8_t>>(std::move(values)),std::move(mask))}};
        Require(store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"source fixture failed");
    }
    RoiDescriptor Create(std::vector<RoiVoxel> voxels,std::string name="A") {
        RoiNode node;node.primitive.shape=RoiShape::VoxelSet;node.primitive.voxels=MakeRoiVoxelFixture(voxels);
        RoiRequest request;request.definition={source,{node}};request.metadata.name=std::move(name);const auto result=service.SetRoi(request);
        Require(result.error==RoiError::None && result.roi,"ROI fixture failed");return *result.roi;
    }
    RoiReadSnapshot Read(const RoiDescriptor& roi) {auto read=RoiEvaluator::GetRoi(store.GetDataGraph(),roi.revision,roi.definition.source);Require(read.error==RoiError::None && read.roi,"ROI read failed");return read.roi;}
    std::size_t Count(const RoiDescriptor& roi) {auto read=Read(roi);RoiMaskRequest r;r.region={{0,0,0},{std::size_t(grid.dimensions[0]),std::size_t(grid.dimensions[1]),std::size_t(grid.dimensions[2])}};const auto mask=read->GetMaskChunk(r);Require(mask.error==RoiError::None,"mask read failed");std::size_t count=0;for (auto v:mask.values) count+=v;return count;}
    RoiOperationRequest Operation(const RoiDescriptor& target,RoiOperationParams params) {RoiOperationRequest r;r.source=source;r.target=target.revision;r.params=std::move(params);return r;}
};
void Morphology() {
    Fixture f;auto a=f.Create({{{4,4,4},1}});
    auto request=f.Operation(a,RoiMorphologyParams{RoiMorphologyOperation::Dilate,2,RoiRadiusUnit::Millimetres});request.output=RoiOutputMode::CreateNew;
    const auto before=f.store.GetDataGraph().commitId;auto grown=f.service.SetOperation(request);
    Require(grown.error==RoiError::None && grown.rois.size()==1 && f.Count(grown.rois[0])==7,"physical sphere neighbourhood must contain 5 X and 2 Y neighbours");
    Require(f.store.GetDataGraph().commitId==before+1 && f.Count(a)==1,"morphology did not publish atomically or changed its input");
    request.params=RoiMorphologyParams{RoiMorphologyOperation::Close,2,RoiRadiusUnit::Millimetres};
    auto closed=f.service.SetOperation(request);Require(closed.error==RoiError::None && f.Count(closed.rois[0])==1,"closing a singleton changed its discrete support");
    auto boundary=f.Create({{{0,0,0},1}},"edge");request=f.Operation(boundary,RoiMorphologyParams{RoiMorphologyOperation::Close,1,RoiRadiusUnit::Voxels});
    closed=f.service.SetOperation(request);Require(closed.error==RoiError::None && f.Count(closed.rois[0])==1,"closing clipped its intermediate halo at the source boundary");
    request=f.Operation(a,RoiMorphologyParams{RoiMorphologyOperation::Dilate,2,RoiRadiusUnit::Millimetres});request.getCancelled=[] {return true;};
    const auto cancelledBefore=f.store.GetDataGraph().commitId;Require(f.service.SetOperation(request).error==RoiError::Cancelled && f.store.GetDataGraph().commitId==cancelledBefore,"cancelled morphology published partial output");
}
void ComponentsAndHoles() {
    Fixture f;auto a=f.Create({{{1,1,1},1},{{2,1,1},1},{{3,1,1},1},{{7,7,7},1}});
    RoiComponentParams split;split.operation=RoiComponentOperation::Split;
    const auto before=f.store.GetDataGraph().commitId;auto result=f.service.SetOperation(f.Operation(a,split));
    Require(result.error==RoiError::None && result.rois.size()==2 && f.store.GetDataGraph().commitId==before+1,"split was not one complete batch");
    Require(f.Count(result.rois[0])==3 && f.Count(result.rois[1])==1 && f.Count(a)==4,"split counts or input retention are wrong");
    Require(result.rois[0].definition.nodes[0].primitive.voxels==result.rois[1].definition.nodes[0].primitive.voxels,"split copied its shared label support");
    RoiComponentParams clean;clean.minimumVolume=12;
    result=f.service.SetOperation(f.Operation(a,clean));Require(result.error==RoiError::None && f.Count(result.rois[0])==3,"component cleaning did not use global mm^3 volume");
    std::vector<RoiVoxel> shell;for (int z=2;z<=6;++z) for (int y=2;y<=6;++y) for (int x=2;x<=6;++x) if (x!=4 || y!=4 || z!=4) shell.push_back({{x,y,z},1});
    auto hollow=f.Create(shell,"hollow");RoiComponentParams fill;fill.operation=RoiComponentOperation::FillHoles;
    result=f.service.SetOperation(f.Operation(hollow,fill));Require(result.error==RoiError::None && f.Count(result.rois[0])==125,"closed cavity was not filled");
    shell.erase(std::remove_if(shell.begin(),shell.end(),[](const auto& v){return v.index[1]==4 && v.index[2]==4 && v.index[0]<4;}),shell.end());
    auto open=f.Create(shell,"open");result=f.service.SetOperation(f.Operation(open,fill));Require(result.error==RoiError::None && f.Count(result.rois[0])==shell.size(),"background connected to the exterior was filled");
}
void LocalConflictAndHistory() {
    Fixture f;auto a=f.Create({{{4,4,4},1}}),b=f.Create({{{1,1,1},1}},"B");
    RoiRequest rename;rename.action=RoiAction::SetMetadata;rename.expectedRoi=b.revision;rename.metadata.name="renamed B";
    Require(f.service.SetRoi(rename).error==RoiError::None,"rename failed");auto request=f.Operation(a,RoiClearParams{});
    auto cleared=f.service.SetOperation(request);Require(cleared.error==RoiError::None && cleared.rois[0].revision.entityId==a.revision.entityId && f.Count(cleared.rois[0])==0,"an unrelated rename blocked a local ROI update");
    const auto before=f.store.GetDataGraph().commitId;Require(f.service.SetOperation(request).error==RoiError::RevisionConflict && f.store.GetDataGraph().commitId==before,"stale target overwrote a newer ROI");
    auto pinned=f.Read(a);RoiRequest remove;remove.action=RoiAction::Remove;remove.expectedRoi=cleared.rois[0].revision;
    Require(f.service.SetRoi(remove).error==RoiError::None && pinned->GetContains({4,8,12}) && !RoiService::GetDescriptor(f.store.GetDataGraph(),a.revision),"catalog deletion destroyed an existing immutable read view");
}
void ShapesAndTriangles() {
    Fixture f;RoiPrimitive contour;contour.shape=RoiShape::Contour;contour.depth={11,13};
    contour.contours={{{1,2},{7,2},{7,14},{4,14},{4,8},{1,8}}};
    auto read=RoiEvaluator::GetPreview(f.store.GetDataGraph(),{f.source,{{RoiNodeKind::Primitive,contour}}});
    Require(read.roi && read.roi->GetContains({6,12,12}) && !read.roi->GetContains({2,12,12}) && !read.roi->GetContains({6,12,14}),"concave contour or finite depth was reduced to a box");
    RoiPrimitive sphere;sphere.shape=RoiShape::Sphere;sphere.origin={4,8,12};sphere.radius=.6;
    read=RoiEvaluator::GetPreview(f.store.GetDataGraph(),{f.source,{{RoiNodeKind::Primitive,sphere}}});
    const std::array<std::array<double,3>,3> triangle{{{1,2,12},{7,2,12},{4,14,12}}};
    for (const auto& point:triangle) Require(!read.roi->GetContains(point),"triangle fixture has an interior vertex");
    const auto selection=read.roi->GetTriangleSelection(triangle);Require(selection.error==RoiError::None && !selection.triangles.empty() && selection.boundaryTolerance<=.15,"ROI wholly inside a triangle was missed");
    for (const auto& piece:selection.triangles) for (const auto& point:piece) Require(std::abs(point[2]-12)<1e-12,"ROI selection displaced the measurement surface");
}
void Archive() {
    Fixture f;auto a=f.Create({{{2,3,4},1},{{6,7,8},2}});auto archive=RoiService::GetArchiveReader(f.store.GetDataGraph(),{{a.revision,"ct"}});
    Require(archive.error==RoiError::None&&archive.reader&&archive.reader->GetMetadata().voxels.size()==1
        &&!archive.reader->GetMetadata().rois[0].nodes[0].primitive.voxels,"sparse archive retained a process pointer or lost its data");
    Fixture other;auto restored=other.service.LoadArchiveReader(*archive.reader,{{"ct",other.source}});
    Require(restored.error==RoiError::None&&restored.rois.size()==1&&other.Count(restored.rois[0])==2,"sparse archive did not restore exact membership");
    const DataRevisionRef labels{f.store.CreateDataEntityId(),1};auto values=std::make_shared<std::vector<std::uint64_t>>(729,0);(*values)[364]=0x100000001ULL;
    DataTransaction tx;tx.outputs={{labels.entityId,0,DataTypes::labelMap3D,{},std::make_shared<const LabelMap3DPayload>(f.grid,std::shared_ptr<const std::vector<std::uint64_t>>(values))}};Require(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"wide labels failed");
    RoiPrimitive primitive;primitive.shape=RoiShape::MaskReference;primitive.mask=labels;primitive.labels={0x100000001ULL};RoiRequest request;request.metadata.name="wide";request.definition={f.source,{{RoiNodeKind::Primitive,primitive}}};auto made=f.service.SetRoi(request);
    archive=RoiService::GetArchiveReader(f.store.GetDataGraph(),{{made.roi->revision,"ct"}});RoiArchiveChunkRequest chunk;chunk.elementOffset=364;chunk.chunkElements=1;
    Require(archive.reader&&archive.reader->GetMaskChunk(chunk).values==std::vector<std::uint64_t>{0x100000001ULL},"archive truncated a 64-bit label");
    restored=other.service.LoadArchiveReader(*archive.reader,{{"ct",other.source}});
    Require(restored.rois.size()==1&&other.Count(restored.rois[0])==1,"restored wide label selector changed its membership");
}
void ArchiveBundleAndTransform() {
    Fixture f;auto input=f.Create({{{1,1,1},1},{{6,6,6},1}},std::string(256,'A'));RoiComponentParams split;split.operation=RoiComponentOperation::Split;
    const auto parts=f.service.SetOperation(f.Operation(input,split));Require(parts.error==RoiError::None && parts.rois.size()==2,"long-name split failed");
    std::vector<RoiArchiveSelection> selection;for(const auto& roi:parts.rois){Require(roi.metadata.name.size()<=256,"split name exceeds the metadata limit");selection.push_back({roi.revision,"ct"});}
    const auto saved=RoiService::GetArchiveReader(f.store.GetDataGraph(),selection);
    Require(saved.reader && saved.reader->GetMetadata().voxels.size()==1 && saved.reader->GetMetadata().rois.size()==2,"bundle duplicated a shared split payload");
    Fixture target;const auto before=target.store.GetDataGraph().commitId;
    const auto restored=target.service.LoadArchiveReader(*saved.reader,{{"ct",target.source}});
    Require(restored.error==RoiError::None && restored.rois.size()==2 && restored.revisionMapping.size()==2 && target.store.GetDataGraph().commitId==before+1,"bundle restore was not one atomic mapping transaction");
    Require(target.Count(restored.rois[0])==1 && target.Count(restored.rois[1])==1 && restored.rois[0].definition.nodes[0].primitive.voxels==restored.rois[1].definition.nodes[0].primitive.voxels,"bundle restore lost shared membership");
    RoiArchiveTestReader bad(saved.reader);bad.metadata.rois.back().voxels[0].sharedPayload=1234;const auto stable=target.store.GetDataGraph().commitId;
    Require(target.service.LoadArchiveReader(bad,{{"ct",target.source}}).error!=RoiError::None && target.store.GetDataGraph().commitId==stable,"invalid bundle published a partial batch");
    RoiPrimitive sphere;sphere.shape=RoiShape::Sphere;sphere.origin={4,8,12};sphere.radius=.6;RoiRequest create;create.metadata.name="sphere";create.definition={f.source,{{RoiNodeKind::Primitive,sphere}}};const auto roi=f.service.SetRoi(create);
    RoiTransformParams transform;transform.sourceToTarget[3]=1.1;auto request=f.Operation(*roi.roi,transform);request.output=RoiOutputMode::CreateNew;
    const auto moved=f.service.SetOperation(request);Require(moved.error==RoiError::None && f.Read(moved.rois[0])->GetContains({5.1,8,12}) && !f.Read(moved.rois[0])->GetContains({4,8,12}),"analytic transform silently rasterized a sub-voxel sphere");
    request=f.Operation(input,transform);const auto noResample=f.store.GetDataGraph().commitId;Require(f.service.SetOperation(request).error==RoiError::UnsupportedRoi && f.store.GetDataGraph().commitId==noResample,"voxel transform did not require an explicit precision choice");
    const auto prepared=RoiService::GetOperation(f.store.GetDataGraph(),f.Operation(input,RoiClearParams{}));auto changed=f.Operation(input,RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels});
    Require(f.service.SetOperationCommit(changed,prepared).error==RoiError::InvalidRequest,"prepared output was submitted with different operation parameters");
    transform.rasterize=true;request.params=transform;request.output=RoiOutputMode::CreateNew;Require(f.service.SetOperation(request).error==RoiError::None,"explicit voxel resampling failed");
    auto archive=RoiService::GetArchiveReader(f.store.GetDataGraph(),{{moved.rois[0].revision,"ct"}});Require(archive.reader && archive.reader->GetMetadata().rois[0].provenance.operationId=="transform-analytic" && !archive.reader->GetMetadata().rois[0].provenance.canonicalParameters.empty(),"archive operation provenance or parameters were lost");
}

void MeshAndProjection(){
    Fixture f;std::vector<double> points;std::vector<std::uint64_t> triangles;
    const auto cube=[&](double low,double high){const auto base=points.size()/3;for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x){points.push_back(x?high:low);points.push_back((y?high:low)*2);points.push_back((z?high:low)*3);}for(auto vertex:std::vector<std::uint64_t>{0,2,3,0,3,1,4,5,7,4,7,6,0,1,5,0,5,4,2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5})triangles.push_back(base+vertex);};
    cube(1.5,6.5);cube(3.5,4.5);const DataRevisionRef mesh{f.store.CreateDataEntityId(),1};DataTransaction tx;tx.outputs.push_back({mesh.entityId,0,DataTypes::surfaceMesh,{},std::make_shared<const SurfaceMeshPayload>(points,triangles)});Require(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"mesh ROI fixture failed");
    RoiOperationRequest request;request.source=f.source;request.output=RoiOutputMode::CreateNew;RoiMeshParams params;params.mesh=mesh;request.params=params;
    const auto made=f.service.SetOperation(request);Require(made.error==RoiError::None && f.Count(made.rois[0])==124 && !f.Read(made.rois[0])->GetContains({4,8,12}),"nested closed shells lost their cavity");
    const DataRevisionRef open{f.store.CreateDataEntityId(),1};triangles.resize(3);tx={};tx.outputs.push_back({open.entityId,0,DataTypes::surfaceMesh,{},std::make_shared<const SurfaceMeshPayload>(points,triangles)});Require(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"open mesh fixture failed");params.mesh=open;request.params=params;const auto before=f.store.GetDataGraph().commitId;Require(f.service.SetOperation(request).error==RoiError::InvalidGeometry && f.store.GetDataGraph().commitId==before,"open surface was accepted as interior");
    params.mode=RoiMeshMode::Layer;params.inward=1;params.outward=1;request.params=params;Require(f.service.SetOperation(request).error==RoiError::None,"symmetric layer rejected an open surface");
    const DataRevisionRef scaledMesh{f.store.CreateDataEntityId(),1};tx={};tx.outputs.push_back({scaledMesh.entityId,0,DataTypes::surfaceMesh,{},std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0,0,4,10,0,4,0,10,4},std::vector<std::uint64_t>{0,1,2})});Require(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"scaled layer fixture failed");
    params={};params.mesh=scaledMesh;params.mode=RoiMeshMode::Layer;params.meshToSource[0]=2;params.meshToSource[5]=2;params.meshToSource[10]=3;params.outward=1.5;request.params=params;
    const auto layer=f.service.SetOperation(request);Require(layer.error==RoiError::None && f.Read(layer.rois[0])->GetContains({4,8,12}) && !f.Read(layer.rois[0])->GetContains({4,8,9}),"layer distance used mesh coordinates or nearest vertices");
    params.outward=4;request.params=params;const auto thick=f.service.SetOperation(request);Require(thick.error==RoiError::None && f.Read(thick.rois[0])->GetContains({4,8,9}),"thickness-only change reused an obsolete selection");
    params.outward=1.5;params.meshToSource[11]=3;request.params=params;const auto shifted=f.service.SetOperation(request);Require(shifted.error==RoiError::None && f.Read(shifted.rois[0])->GetContains({4,8,15}) && !f.Read(shifted.rois[0])->GetContains({4,8,12}),"mesh transform did not invalidate the spatial index");
    RoiPrimitive projected;projected.shape=RoiShape::ProjectedContour;projected.contours={{{-1,-1},{1,-1},{0,1}}};projected.depth={1,10};projected.viewToClip={};Require(RoiEvaluator::GetPreview(f.store.GetDataGraph(),{f.source,{{RoiNodeKind::Primitive,projected}}}).error==RoiError::InvalidGeometry,"singular frozen projection was accepted");
    RoiPrimitive contour;contour.shape=RoiShape::Contour;contour.contours={{{0,0},{1,1},{2,2}}};Require(RoiEvaluator::GetPreview(f.store.GetDataGraph(),{f.source,{{RoiNodeKind::Primitive,contour}}}).error==RoiError::InvalidGeometry,"collinear contour was accepted");
}

void PrecisionAndLimits(){
    Fixture f;RoiPrimitive ellipse;ellipse.shape=RoiShape::EllipticCylinder;ellipse.localToSource={.03,0,0,4.2,0,.05,0,8.3,0,0,1,12,0,0,0,1};ellipse.depth={-.1,.1};
    auto read=RoiEvaluator::GetPreview(f.store.GetDataGraph(),{f.source,{{RoiNodeKind::Primitive,ellipse}}});Require(read.roi && read.roi->GetContains({4.2,8.3,12}) && !read.roi->GetContains({4.24,8.3,12}),"finite ellipse predicate is invalid");
    const std::array<std::array<double,3>,3> triangle{{{1,2,12},{7,2,12},{4,14,12}}};const auto pieces=read.roi->GetTriangleSelection(triangle);
    Require(pieces.error==RoiError::None && !pieces.triangles.empty() && pieces.boundaryTolerance<=.03/4,"sub-voxel ellipse wholly inside a triangle was missed");
    double area=0;for(const auto& piece:pieces.triangles){const auto& a=piece[0];const auto& b=piece[1];const auto& c=piece[2];area+=std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))*.5;}
    Require(area>.003 && area<.0065,"triangle sampling counted both sides of a shared sub-cell face");
    auto a=f.Create({{{4,4,4},1}});auto request=f.Operation(a,RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels});request.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,-1,RoiRadiusUnit::Voxels};const auto before=f.store.GetDataGraph().commitId;
    Require(f.service.SetOperation(request).error==RoiError::InvalidRequest && f.store.GetDataGraph().commitId==before,"invalid morphology changed the original ROI");
    request.params=RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels};request.deadline=std::chrono::steady_clock::now();Require(f.service.SetOperation(request).error==RoiError::DeadlineExceeded,"expired ROI deadline was ignored");
    RoiThresholdParams grow;grow.mode=RoiThresholdMode::SeedGrow;grow.range={10,10};request=f.Operation(a,grow);Require(f.service.SetOperation(request).error==RoiError::InvalidRequest,"missing seed became a whole threshold domain");
    grow.seeds={{4,8,12}};request.params=grow;const auto grown=f.service.SetOperation(request);Require(grown.error==RoiError::None && f.Count(grown.rois[0])==1,"seed growth escaped its fixed permitted ROI");
    RoiMaskRequest block;block.region={{0,0,0},{9,9,9}};block.chunkBytes=3;Require(f.Read(a)->GetBlockClassification(block).classification==RoiBlockClassification::Boundary,"block classification lost mixed membership across chunks");block.getCancelled=[](){return true;};Require(f.Read(a)->GetBlockClassification(block).classification==RoiBlockClassification::Unavailable,"cancelled block was classified as an empty ROI");
}

void ProcessingRegression(){
    Fixture f;
    RoiComponentParams split;split.operation=RoiComponentOperation::Split;
    auto ordered=f.Create({{{2,1,1},1},{{1,7,7},1},{{2,7,7},1}});
    auto parts=f.service.SetOperation(f.Operation(ordered,split));
    Require(parts.error==RoiError::None && parts.componentVoxelCounts==std::vector<std::size_t>{2,1}
        && f.Read(parts.rois.front())->GetContains({1,14,21}),"compressed components changed lexicographic label order");
    auto diagonal=f.Create({{{1,1,1},1},{{2,2,2},1}});
    parts=f.service.SetOperation(f.Operation(diagonal,split));Require(parts.rois.size()==2,"face connectivity joined diagonal voxels");
    split.connectivity=RoiConnectivity::Full;parts=f.service.SetOperation(f.Operation(diagonal,split));
    Require(parts.error==RoiError::None && parts.componentVoxelCounts==std::vector<std::size_t>{2},"full connectivity separated corner neighbours");

    // 独立的离散集合真值，包含空洞与源边界；开闭仅在最终输出裁回有效域。
    using Cell=std::array<int,3>;std::set<Cell> input;std::vector<RoiVoxel> voxels;
    for(int z=2;z<=6;++z)for(int y=2;y<=6;++y)for(int x=2;x<=6;++x)if(Cell{x,y,z}!=Cell{3,3,3} && Cell{x,y,z}!=Cell{5,4,4})input.insert({x,y,z});
    input.insert({0,0,0});for(const auto& cell:input)voxels.push_back({cell,1});const auto original=f.Create(std::move(voxels));
    std::vector<Cell> kernel{{0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const auto reference=[&](const std::set<Cell>& cells,bool dilate){std::set<Cell> result;
        for(const auto& cell:cells){bool kept=true;for(const auto& offset:kernel){Cell point{cell[0]+offset[0],cell[1]+offset[1],cell[2]+offset[2]};
            if(dilate)result.insert(point);else if(!cells.count(point))kept=false;}if(!dilate && kept)result.insert(cell);}return result;};
    for(const auto operation:{RoiMorphologyOperation::Dilate,RoiMorphologyOperation::Erode,RoiMorphologyOperation::Open,RoiMorphologyOperation::Close}){
        auto expected=reference(input,operation==RoiMorphologyOperation::Dilate || operation==RoiMorphologyOperation::Close);
        if(operation==RoiMorphologyOperation::Open || operation==RoiMorphologyOperation::Close)expected=reference(expected,operation==RoiMorphologyOperation::Open);
        auto request=f.Operation(original,RoiMorphologyParams{operation,1,RoiRadiusUnit::Voxels});request.output=RoiOutputMode::CreateNew;
        const auto result=f.service.SetOperation(request);Require(result.error==RoiError::None && result.rois.size()==1,"interval morphology failed");
        const auto query=f.Read(result.rois.front())->GetQuery();Require(query.query!=nullptr,"morphology query unavailable");
        for(int z=0;z<9;++z)for(int y=0;y<9;++y)for(int x=0;x<9;++x)
            Require(query.query->GetContains({double(x),double(y*2),double(z*3)})==bool(expected.count({x,y,z})),"interval morphology differs from discrete neighbourhood truth");
    }
    Fixture skewed({1,.2,0,0,1,.3,0,0,1});std::vector<RoiVoxel> skewedInput;for(const auto& cell:input)skewedInput.push_back({cell,1});
    const auto skewedRoi=skewed.Create(std::move(skewedInput));kernel.clear();
    const auto physical=[&](Cell index){std::array<double,3> p{};for(int r=0;r<3;++r)for(int a=0;a<3;++a)p[r]+=skewed.grid.direction[r*3+a]*skewed.grid.spacing[a]*index[a];return p;};
    // 3 mm 半径的独立逐偏移真值；此网格逆矩阵给出的各轴包围范围均小于 4。
    for(int z=-4;z<=4;++z)for(int y=-4;y<=4;++y)for(int x=-4;x<=4;++x){const auto p=physical({x,y,z});if(std::hypot(p[0],p[1],p[2])<=3)kernel.push_back({x,y,z});}
    for(const auto operation:{RoiMorphologyOperation::Dilate,RoiMorphologyOperation::Erode,RoiMorphologyOperation::Open,RoiMorphologyOperation::Close}) {
        auto expected=reference(input,operation==RoiMorphologyOperation::Dilate||operation==RoiMorphologyOperation::Close);
        if(operation==RoiMorphologyOperation::Open||operation==RoiMorphologyOperation::Close)expected=reference(expected,operation==RoiMorphologyOperation::Open);
        auto request=skewed.Operation(skewedRoi,RoiMorphologyParams{operation,3,RoiRadiusUnit::Millimetres});request.output=RoiOutputMode::CreateNew;
        const auto result=skewed.service.SetOperation(request);Require(result.error==RoiError::None&&!result.rois.empty(),"physical interval kernel failed");
        const auto query=skewed.Read(result.rois.front())->GetQuery();
        for(int z=0;z<9;++z)for(int y=0;y<9;++y)for(int x=0;x<9;++x)
            Require(query.query->GetContains(physical({x,y,z}))==bool(expected.count({x,y,z})),"compressed physical kernel changed shear or radius inclusion");
    }

    const DataRevisionRef gray{f.store.CreateDataEntityId(),1};std::vector<float> values(729,10);
    for(int z=0;z<9;++z)for(int y=0;y<9;++y)values[4+9*(y+9*z)]=0;
    values[7+9*(7+9*7)]=std::numeric_limits<float>::quiet_NaN();
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(values.size()*sizeof(float));std::memcpy(bytes->data(),values.data(),bytes->size());
    DataTransaction transaction;transaction.outputs={{gray.entityId,0,DataTypes::imageGrid3D,{},
        std::make_shared<const ImageGrid3DPayload>(f.grid,ImageValueType::Float32,1,bytes)}};
    Require(f.store.SetDataCommit(transaction).status==DataCommitStatus::Succeeded,"gray growth fixture failed");
    RoiOperationRequest grow;grow.source=gray;grow.output=RoiOutputMode::CreateNew;RoiThresholdParams parameters;parameters.mode=RoiThresholdMode::SeedGrow;parameters.range={10,10};parameters.seeds={{1,2,3},{1,2,3}};grow.params=parameters;
    const auto before=f.store.GetDataGraph().commitId;
    Require(f.service.SetOperation(grow).error==RoiError::InvalidGeometry && f.store.GetDataGraph().commitId==before,"unreachable invalid gray no longer rejects the permitted domain");
    auto mask=std::make_shared<std::vector<std::uint8_t>>(729,1);(*mask)[7+9*(7+9*7)]=0;
    transaction.outputs={{gray.entityId,1,DataTypes::imageGrid3D,{},std::make_shared<const ImageGrid3DPayload>(f.grid,ImageValueType::Float32,1,bytes,mask)}};
    Require(f.store.SetDataCommit(transaction).status==DataCommitStatus::Succeeded,"masked growth fixture failed");grow.source.generation=2;
    const auto grown=f.service.SetOperation(grow);Require(grown.error==RoiError::None && f.Count(grown.rois.front())==4*9*9,"frontier growth crossed a gray barrier or duplicated seeds");
    parameters.seeds={{7,14,21}};grow.params=parameters;Require(f.service.SetOperation(grow).error==RoiError::InvalidRequest,"invalid source-mask seed was accepted");

    std::weak_ptr<const void> meshStorage;std::weak_ptr<const DataRevision> meshRevision;
    {Fixture owner;const DataRevisionRef mesh{owner.store.CreateDataEntityId(),1};
        auto payload=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{1,2,12,7,2,12,4,14,12},std::vector<std::uint64_t>{0,1,2});
        meshStorage=payload->GetDataResources().front();DataTransaction data;data.outputs={{mesh.entityId,0,DataTypes::surfaceMesh,{},payload}};
        Require(owner.store.SetDataCommit(data).status==DataCommitStatus::Succeeded,"mesh lifetime fixture failed");meshRevision=owner.store.GetDataGraph().view->GetData(mesh);
        RoiOperationRequest request;request.source=owner.source;request.output=RoiOutputMode::CreateNew;RoiMeshParams selection;selection.mesh=mesh;selection.mode=RoiMeshMode::Layer;selection.outward=.5;request.params=selection;
        const auto first=owner.service.SetOperation(request),second=owner.service.SetOperation(request);
        Require(first.error==RoiError::None && second.error==RoiError::None && owner.Count(first.rois.front())==owner.Count(second.rois.front()) && owner.Count(first.rois.front())>0,"repeated mesh selection changed membership");
    }
    Require(meshStorage.expired() && meshRevision.expired(),"ROI index cache retained mesh data after its graph owner was destroyed");
}

void Compressed() {
    std::vector<RoiVoxelRun> owned{{{0,0,0},31,3},{{0,1,0},31,4}};const auto* address=owned.data();RoiVoxelSet adopted(std::move(owned));
    Require(adopted.GetRuns().data()==address&&adopted.GetVoxelCount()==64,"ordered runs were copied instead of adopted");
    for(std::size_t width:{1,3,17}){
        RoiVoxelBuilder builder;
        std::vector<RoiVoxel> batch;for(int x=31;x>=0;--x){batch.push_back({{x,2,3},5});if(batch.size()==width){builder.AppendVoxels(batch);batch.clear();}}
        if(!batch.empty())builder.AppendVoxels(batch);const auto compressed=builder.Finish();
        Require(compressed->GetRuns().size()==1&&compressed->GetVoxelCount()==32&&compressed->GetLabel({17,2,3})==5,"voxel input blocks changed compression or membership");
    }
    RoiVoxelBuilder failed;failed.AppendRuns({{{0,0,0},2,1}});bool duplicate=false,unpublished=false;
    try{failed.AppendRuns({{{2,0,0},4,1}});}catch(const std::invalid_argument&){duplicate=true;}
    try{failed.Finish();}catch(const std::logic_error&){unpublished=true;}
    Require(duplicate&&unpublished,"failed incremental candidate remained publishable");
    // 十亿成员由一条区间表示；仅取末尾三项，不物化完整体素向量。
    RoiVoxelSet large(std::vector<RoiVoxelRun>{{{0,1,2},999999999,7}});
    Require(large.GetRuns().size()==1&&large.GetVoxelCount()==1000000000,"dense ROI lost compressed storage");
    const auto tail=large.GetVoxelChunk(999999997,3);
    Require(tail.size()==3&&tail[0].index==std::array<int,3>{999999997,1,2}&&tail[2].index[0]==999999999
        &&large.GetLabel({999999999,1,2})==7&&large.GetLabel({1000000000,1,2})==0,"compressed chunk or label boundary changed");
    RoiVoxelSet small(std::vector<RoiVoxelRun>{{{-3,2,1},3,4},{{2,0,0},4,9},{{4,2,1},5,4}});
    Require(small.GetRuns().size()==2&&small.GetVoxelCount()==12,"adjacent runs were not coalesced");
    const auto read=[&]{return small.GetVoxelChunk(2,3);};
    auto first=std::async(std::launch::async,read),second=std::async(std::launch::async,read);
    const auto chunkA=first.get(),chunkB=second.get();Require(chunkA.size()==3&&chunkB.size()==3
        &&chunkA.front().index==chunkB.front().index&&small.GetRuns().size()==2,"concurrent bounded reads changed compressed payload");
    bool rejected=false;try{RoiVoxelSet invalid(std::vector<RoiVoxelRun>{{{0,0,0},3,1},{{3,0,0},4,2}});}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"overlapping labels were accepted");
    Fixture f;RoiOperationRequest request;request.source=f.source;request.output=RoiOutputMode::CreateNew;request.params=RoiThresholdParams{RoiThresholdMode::Threshold,{10,10}};
    const auto result=f.service.SetOperation(request);
    Require(result.error==RoiError::None&&result.rois.size()==1&&f.Count(result.rois.front())==729,"dense threshold publication changed membership");
    const auto payload=result.rois.front().definition.nodes.front().primitive.voxels;
    Require(payload&&payload->GetRuns().size()==81&&payload->GetVoxelCount()==729,"operation output expanded row intervals");
    const auto archive=RoiService::GetArchiveReader(f.store.GetDataGraph(),{{result.rois.front().revision,"dense"}});
    Require(archive.error==RoiError::None&&archive.reader&&archive.reader->GetVoxelPayloadCount(0)==81,"compressed archive count failed");
    RoiArchiveChunkRequest chunk;chunk.chunkElements=7;std::size_t count=0;
    do {const auto page=archive.reader->GetVoxelPayloadChunk(chunk);Require(page.error==RoiError::None,"compressed archive chunk failed");
        for(const auto& run:page.runs)count+=std::size_t(std::int64_t(run.last)-run.first[0]+1);chunk.elementOffset=page.nextOffset;if(page.isComplete)break;}while(true);
    Require(count==729&&payload->GetRuns().size()==81,"archive paging lost members or changed compressed storage");
    std::cout<<"ROI compressed: members="<<large.GetVoxelCount()<<" runs="<<large.GetRuns().size()<<" publishedMembers=729 publishedRuns="<<payload->GetRuns().size()<<'\n';
}
void Islands() {
    // 分散到不同的行并打乱 X 顺序，独立核对编号；耗时只作同条件比较，不设机器相关门限。
    for(const int count:{4000,8000,16000}) {
        GridGeometry3D grid;grid.extent={0,2,0,count*2-1,0,0};grid.dimensions={3,count*2,1};
        Fixture f(grid,std::vector<std::uint8_t>(std::size_t(count)*6,10));
        std::vector<RoiVoxel> voxels;std::vector<std::array<int,3>> ordered;
        for(int i=0;i<count;++i){const std::array<int,3> point{(count-i)%3,i*2,0};voxels.push_back({point,1});ordered.push_back(point);}
        std::sort(ordered.begin(),ordered.end());const auto input=f.Create(std::move(voxels));
        RoiComponentParams params;params.operation=RoiComponentOperation::RemoveSmall;
        auto request=f.Operation(input,params);request.output=RoiOutputMode::CreateNew;
        const auto start=std::chrono::steady_clock::now();
        const auto result=RoiService::GetOperation(f.store.GetDataGraph(),request);
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        Require(result.error==RoiError::None && result.rois.size()==1 && result.componentVoxelCounts==std::vector<std::size_t>(count,1),"island membership/counts changed");
        const auto& payload=result.rois.front().definition.nodes.front().primitive.voxels;
        Require(payload && payload->GetRuns().size()==std::size_t(count),"islands lost compressed storage");
        for(int i=0;i<count;++i)Require(payload->GetLabel(ordered[i])==std::uint32_t(i+1),"island labels changed lexicographic order");
        std::cout<<"ROI islands="<<count<<" elapsedMs="<<elapsed<<std::endl;
    }
}
void ComponentSuccessors() {
    using Cell=std::array<int,3>;
    for(const auto connectivity:{RoiConnectivity::Faces,RoiConnectivity::Full})for(int pattern=0;pattern<3;++pattern) {
        std::size_t previous=0;
        for(const int count:{128,256,512}) {
            GridGeometry3D grid;grid.extent={0,count*2,0,4,0,2};grid.dimensions={count*2+1,5,3};
            if(pattern==1){grid.extent={0,2,0,count*2-1,0,0};grid.dimensions={3,count*2,1};}
            Fixture f(grid,std::vector<std::uint8_t>(std::size_t(grid.dimensions[0])*grid.dimensions[1]*grid.dimensions[2],10));std::set<Cell> input;
            for(int i=0;i<count;++i)input.insert(pattern==1?Cell{i%3,i*2,0}:Cell{i*2,0,0});
            if(pattern==2){for(int x=count/2;x<count;++x)input.insert({x,1,0});for(int i=0;i<count/4;++i)input.insert({i*3,3,2});}
            std::vector<RoiVoxel> voxels;for(const auto& cell:input)voxels.push_back({cell,1});auto remaining=input;
            std::vector<std::set<Cell>> expected;
            while(!remaining.empty()) {
                std::set<Cell> component;std::queue<Cell> queue;queue.push(*remaining.begin());remaining.erase(remaining.begin());
                while(!queue.empty()){const auto cell=queue.front();queue.pop();component.insert(cell);
                    for(int z=-1;z<=1;++z)for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
                        if(!(x||y||z)||(connectivity==RoiConnectivity::Faces && std::abs(x)+std::abs(y)+std::abs(z)!=1))continue;
                        const Cell next{cell[0]+x,cell[1]+y,cell[2]+z};const auto found=remaining.find(next);if(found!=remaining.end()){queue.push(next);remaining.erase(found);}}}
                expected.push_back(std::move(component));
            }
            std::sort(expected.begin(),expected.end(),[](const auto& a,const auto& b){return *a.begin()<*b.begin();});
            const auto original=f.Create(voxels);RoiComponentParams params;params.operation=RoiComponentOperation::RemoveSmall;params.connectivity=connectivity;
            RoiOperationDiagnostics::Reset();const auto result=RoiService::GetOperation(f.store.GetDataGraph(),f.Operation(original,params));const auto counters=RoiOperationDiagnostics::Get();
            Require(result.error==RoiError::None && result.rois.size()==1 && result.componentVoxelCounts.size()==expected.size(),"successor components changed population");
            const auto payload=result.rois.front().definition.nodes.front().primitive.voxels;
            Require(payload && payload->GetVoxelCount()==input.size(),"successor components lost or added members");
            for(std::size_t i=0;i<expected.size();++i){Require(result.componentVoxelCounts[i]==expected[i].size(),"successor component size/order differs from BFS");
                for(const auto& cell:expected[i])Require(payload->GetLabel(cell)==std::uint32_t(i+1),"successor component membership/number differs from BFS");}
            const auto work=counters.componentComparisons+counters.componentSuccessorSteps;
            // 本组每行至多 512 个区间，8 邻行查询及 successor 跳转采用离散访问上界，独立于机器耗时。
            Require(counters.componentConsumedRuns>0 && work<256*input.size(),"component interval accesses grew beyond the logarithmic search bound");
            if(previous && pattern!=1)Require(work<=previous*3+128,"doubling islands caused quadratic interval access growth");previous=work;
        }
    }
}
void FinalClipping() {
    // 带孔洞有效域的独立集合真值：中间运算不裁剪，最后才求源域交集。
    GridGeometry3D grid;grid.extent={0,8,0,8,0,8};grid.dimensions={9,9,9};
    auto mask=std::make_shared<std::vector<std::uint8_t>>(729,1);(*mask)[4+9*(4+9*4)]=0;(*mask)[0]=0;
    Fixture f(grid,std::vector<std::uint8_t>(729,10),mask);using Cell=std::array<int,3>;std::set<Cell> input;std::vector<RoiVoxel> voxels;
    for(int z=0;z<6;++z)for(int y=0;y<6;++y)for(int x=0;x<6;++x){voxels.push_back({{x,y,z},1});if((*mask)[x+9*(y+9*z)])input.insert({x,y,z});}
    const auto original=f.Create(voxels);const std::array<Cell,7> kernel{{{0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    const auto morph=[&](const std::set<Cell>& cells,bool dilate){std::set<Cell> out;for(const auto& cell:cells){bool keep=true;for(const auto& d:kernel){const Cell at{cell[0]+d[0],cell[1]+d[1],cell[2]+d[2]};if(dilate)out.insert(at);else if(!cells.count(at))keep=false;}if(!dilate&&keep)out.insert(cell);}return out;};
    for(const auto operation:{RoiMorphologyOperation::Dilate,RoiMorphologyOperation::Erode,RoiMorphologyOperation::Open,RoiMorphologyOperation::Close}) {
        auto expected=morph(input,operation==RoiMorphologyOperation::Dilate||operation==RoiMorphologyOperation::Close);
        if(operation==RoiMorphologyOperation::Open||operation==RoiMorphologyOperation::Close)expected=morph(expected,operation==RoiMorphologyOperation::Open);
        const auto result=RoiService::GetOperation(f.store.GetDataGraph(),f.Operation(original,RoiMorphologyParams{operation,1,RoiRadiusUnit::Voxels}));
        Require(result.error==RoiError::None && result.rois.size()==1,"final masked clipping failed");const auto payload=result.rois.front().definition.nodes.front().primitive.voxels;
        for(int z=0;z<9;++z)for(int y=0;y<9;++y)for(int x=0;x<9;++x)Require(bool(payload->GetLabel({x,y,z}))==(expected.count({x,y,z}) && (*mask)[x+9*(y+9*z)]),"final clipping changed morphology boundary/mask membership");
    }
    constexpr int width=131100;GridGeometry3D line;line.extent={0,width-1,0,0,0,0};line.dimensions={width,1,1};
    for(const bool masked:{false,true}) {
        auto validity=masked?std::make_shared<std::vector<std::uint8_t>>(width,1):std::shared_ptr<std::vector<std::uint8_t>>{};
        if(validity)for(const int x:{0,65535,65536,65537,width-1})(*validity)[x]=0;
        Fixture owner(line,std::vector<std::uint8_t>(width,10),validity);RoiNode node;node.primitive.shape=RoiShape::VoxelSet;
        node.primitive.voxels=std::make_shared<const RoiVoxelSet>(std::vector<RoiVoxelRun>{{{0,0,0},width-1,1}});
        RoiRequest create;create.metadata.name="final-line";create.definition={owner.source,{node}};const auto roi=owner.service.SetRoi(create);Require(roi.roi.has_value(),"line clip fixture failed");
        auto request=owner.Operation(*roi.roi,RoiMorphologyParams{RoiMorphologyOperation::Dilate,2,RoiRadiusUnit::Voxels});request.output=RoiOutputMode::CreateNew;
        RoiOperationDiagnostics::Reset();const auto result=RoiService::GetOperation(owner.store.GetDataGraph(),request);const auto stats=RoiOperationDiagnostics::Get();
        Require(result.error==RoiError::None && result.rois.size()==1,"long final interval clipping failed");const auto payload=result.rois.front().definition.nodes.front().primitive.voxels;
        for(int x=0;x<width;++x)Require(bool(payload->GetLabel({x,0,0}))==(!validity||(*validity)[x]),"mask chunk boundary changed final membership");
        if(!masked)Require(stats.clipMaskValues==0 && stats.clipRuns==1,"unmasked final crop expanded its full interval");
        else {Require(stats.clipMaskValues==width && stats.clipCheckpoints>=3,"masked clipping did not visit the complete bounded chunks");
            RoiOperationDiagnostics::Reset();const auto before=owner.store.GetDataGraph().commitId;
            request.getCancelled=[] {return RoiOperationDiagnostics::Get().clipMaskValues>0;};
            const auto cancelled=owner.service.SetOperation(request);
            Require(cancelled.error==RoiError::Cancelled && cancelled.rois.empty() && owner.store.GetDataGraph().commitId==before,"cancellation during final clipping published a partial candidate");}
    }
}
template<class T> void GrayCase(ImageValueType type,const std::vector<T>& values,std::array<double,2> range,const std::vector<bool>& expected) {
    Fixture owner;GridGeometry3D grid;grid.extent={0,int(values.size())-1,0,0,0,0};grid.dimensions={int(values.size()),1,1};
    auto bytes=std::make_shared<std::vector<std::uint8_t>>(values.size()*sizeof(T));std::memcpy(bytes->data(),values.data(),bytes->size());
    const DataRevisionRef source{owner.store.CreateDataEntityId(),1};DataTransaction transaction;transaction.outputs={{source.entityId,0,DataTypes::imageGrid3D,{},std::make_shared<const ImageGrid3DPayload>(grid,type,1,bytes)}};
    Require(owner.store.SetDataCommit(transaction).status==DataCommitStatus::Succeeded,"typed gray fixture failed");
    RoiThresholdParams params;params.range=range;RoiOperationRequest request;request.source=source;request.output=RoiOutputMode::CreateNew;request.params=params;
    const auto threshold=RoiService::GetOperation(owner.store.GetDataGraph(),request);Require(threshold.error==RoiError::None && threshold.rois.size()==1,"typed threshold failed");
    const auto payload=threshold.rois.front().definition.nodes.front().primitive.voxels;
    for(std::size_t i=0;i<values.size();++i){Require(bool(payload->GetLabel({int(i),0,0}))==expected[i],"typed threshold rounded an original scalar");
        params.mode=RoiThresholdMode::SeedGrow;params.seeds={{{double(i),0,0}}};request.params=params;
        const auto grown=RoiService::GetOperation(owner.store.GetDataGraph(),request);
        if(!expected[i])Require(grown.error==RoiError::InvalidRequest,"seed range disagrees with integer threshold rejection");
        else {Require(grown.error==RoiError::None && grown.rois.size()==1,"valid typed seed rejected");const auto members=grown.rois.front().definition.nodes.front().primitive.voxels;
            Require(members->GetLabel({int(i),0,0})!=0,"typed seed omitted itself");for(std::size_t j=0;j<values.size();++j)Require(!members->GetLabel({int(j),0,0})||expected[j],"typed seed grew through a value rejected by threshold");}}
}
void IntegerRanges() {
    GrayCase<std::int8_t>(ImageValueType::Int8,{-2,-1,0,1,2},{-1.9,.9},{false,true,true,false,false});
    GrayCase<std::uint8_t>(ImageValueType::UInt8,{0,1,2,255},{-.9,1.1},{true,true,false,false});
    GrayCase<std::int16_t>(ImageValueType::Int16,{-32768,0,32767},{-1e20,1e20},{true,true,true});
    GrayCase<std::uint16_t>(ImageValueType::UInt16,{0,1,65535},{.1,.9},{false,false,false});
    GrayCase<std::int32_t>(ImageValueType::Int32,{INT_MIN,0,INT_MAX},{double(INT_MIN),double(INT_MAX)},{true,true,true});
    GrayCase<std::uint32_t>(ImageValueType::UInt32,{0,1,UINT32_MAX},{double(UINT32_MAX),1e20},{false,false,true});
    constexpr std::int64_t exact=9007199254740992LL;const auto low=std::numeric_limits<std::int64_t>::min(),high=std::numeric_limits<std::int64_t>::max();
    GrayCase<std::int64_t>(ImageValueType::Int64,{exact-1,exact,exact+1,exact+2},{double(exact),double(exact)},{false,true,false,false});
    GrayCase<std::int64_t>(ImageValueType::Int64,{-exact-2,-exact-1,-exact,-exact+1},{-double(exact),-double(exact)},{false,false,true,false});
    GrayCase<std::uint64_t>(ImageValueType::UInt64,{std::uint64_t(exact-1),std::uint64_t(exact),std::uint64_t(exact+1),std::uint64_t(exact+2)},{double(exact),double(exact)},{false,true,false,false});
    GrayCase<std::int64_t>(ImageValueType::Int64,{low,low+1,0,high},{-std::ldexp(1.,63),-std::ldexp(1.,63)},{true,false,false,false});
    GrayCase<std::int64_t>(ImageValueType::Int64,{low,0,high-1024,high-1023,high},{std::ldexp(1.,63)-1024,std::ldexp(1.,63)},{false,false,false,true,true});
    GrayCase<std::int64_t>(ImageValueType::Int64,{low,high},{std::ldexp(1.,63),std::ldexp(1.,63)},{false,false});
    constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
    GrayCase<std::uint64_t>(ImageValueType::UInt64,{0,maximum-2048,maximum-2047,maximum-1,maximum},{std::ldexp(1.,64)-2048,std::ldexp(1.,64)},{false,false,true,true,true});
    GrayCase<std::uint64_t>(ImageValueType::UInt64,{0,maximum},{std::ldexp(1.,64),std::ldexp(1.,64)},{false,false});
    GrayCase<float>(ImageValueType::Float32,{1,1.5f,2,2.5f,3},{1.5,2.5},{false,true,true,true,false});
    GrayCase<double>(ImageValueType::Float64,{1,1.5,2,2.5,3},{1.5,2.5},{false,true,true,true,false});
}
void IndexEndpoints() {
    for(const int low:{INT_MIN,INT_MAX-2}) {
        GridGeometry3D grid;grid.extent={low,low+2,low,low+2,low,low+2};grid.dimensions={3,3,3};
        grid.origin={-double(low),-double(low),-double(low)};Fixture f(grid,std::vector<std::uint8_t>(27,10));
        const auto center=f.Create({{{low+1,low+1,low+1},1}});
        const auto countOperation=[&](const RoiDescriptor& roi,RoiOperationParams params){auto request=f.Operation(roi,std::move(params));request.output=RoiOutputMode::CreateNew;const auto result=f.service.SetOperation(request);Require(result.error==RoiError::None && result.rois.size()==1,"legal endpoint ROI operation failed");return f.Count(result.rois.front());};
        Require(countOperation(center,RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels})==7,"endpoint dilation changed neighbourhood");
        Require(countOperation(center,RoiMorphologyParams{RoiMorphologyOperation::Erode,1,RoiRadiusUnit::Voxels})==0,"endpoint erosion changed neighbourhood");
        Require(countOperation(center,RoiMorphologyParams{RoiMorphologyOperation::Open,1,RoiRadiusUnit::Voxels})==0,"endpoint opening changed support");
        for(const int index:{low,low+2}) {
            const auto corner=f.Create({{{index,index,index},1}});
            Require(countOperation(corner,RoiMorphologyParams{RoiMorphologyOperation::Close,1,RoiRadiusUnit::Voxels})==1,"endpoint closing clipped intermediate halo");
            Require(countOperation(corner,RoiMorphologyParams{RoiMorphologyOperation::Dilate,1,RoiRadiusUnit::Voxels})==4,"endpoint dilation did not clip final support");
        }
        const auto corners=f.Create({{{low,low,low},1},{{low+2,low+2,low+2},1}});
        for(const auto connectivity:{RoiConnectivity::Faces,RoiConnectivity::Full}) {
            RoiComponentParams split;split.operation=RoiComponentOperation::Split;split.connectivity=connectivity;
            const auto result=f.service.SetOperation(f.Operation(corners,split));
            Require(result.error==RoiError::None && result.componentVoxelCounts==std::vector<std::size_t>{1,1}
                && f.Read(result.rois.front())->GetContains({0,0,0}),"endpoint components or numbering changed");
            RoiThresholdParams grow;grow.mode=RoiThresholdMode::SeedGrow;grow.range={10,10};grow.seeds={{2,2,2}};grow.connectivity=connectivity;
            RoiOperationRequest request;request.source=f.source;request.output=RoiOutputMode::CreateNew;request.params=grow;
            const auto grown=f.service.SetOperation(request);Require(grown.error==RoiError::None && f.Count(grown.rois.front())==27,"endpoint seed growth rejected legal terminal index");
        }
        std::vector<RoiVoxel> shell;
        for(int z=0;z<3;++z)for(int y=0;y<3;++y)for(int x=0;x<3;++x)
            if(std::array<int,3>{x,y,z}!=std::array<int,3>{1,1,1} && std::array<int,3>{x,y,z}!=std::array<int,3>{2,2,2})shell.push_back({{low+x,low+y,low+z},1});
        RoiComponentParams fill;fill.operation=RoiComponentOperation::FillHoles;
        Require(countOperation(f.Create(std::move(shell)),fill)==26,"endpoint fill did not distinguish cavity from exterior");
    }
}
}
int main(int argc,char** argv) {try {if(argc==2 && std::string(argv[1])=="successors"){ComponentSuccessors();return 0;}if(argc==2 && std::string(argv[1])=="clipping"){FinalClipping();return 0;}if(argc==2 && std::string(argv[1])=="integer-range"){IntegerRanges();return 0;}if(argc==2 && std::string(argv[1])=="endpoints"){IndexEndpoints();return 0;}if(argc==2 && std::string(argv[1])=="islands"){Islands();return 0;}if(argc==2 && std::string(argv[1])=="processing"){ProcessingRegression();return 0;}if(argc==2 && std::string(argv[1])=="compressed"){Compressed();return 0;}Morphology();ComponentsAndHoles();LocalConflictAndHistory();ShapesAndTriangles();Archive();ArchiveBundleAndTransform();MeshAndProjection();PrecisionAndLimits();return 0;}catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
