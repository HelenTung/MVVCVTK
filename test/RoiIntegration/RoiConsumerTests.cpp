// 测试用途：真实公共 ROI 读取驱动大父三角形内的局部选择，核对面积、来源与无盖面语义。
#include "Data/RoiService.h"
#include "Geometry/RoiEvaluator.h"
#include "Host/VtkAppHostSession.h"
#include "Host/GapHostFeature.h"
#include "MVVCVTK/features/OrthogonalCrop/include/Algorithms/CropAlgorithm.h"
#include "MVVCVTK/features/GapAnalysis/src/GapParentRecord.h"
#include <vtkRenderWindow.h>
#include <thread>
#include "MVVCVTK/features/MetrologyAlignment/src/AlignmentDeviation.h"
#include "MVVCVTK/features/MetrologyAlignment/src/AlignmentSurfaceSamples.h"
#include "MVVCVTK/features/GapAnalysis/include/Render/GapDisplayData.h"
#ifdef ROI_TEST_WALL
#include "MVVCVTK/features/WallThickness/src/ThicknessAlgorithm.h"
#endif
#include <vtkCell.h>
#include <iostream>
#include <stdexcept>
#include <cmath>
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
DataRevisionRef Ref(unsigned byte){DataRevisionRef r;r.entityId.bytes[0]=static_cast<std::uint8_t>(byte);r.generation=1;return r;}
struct Fixture {
    DataGraphStore store;RoiService service{store};DataRevisionRef source{store.CreateDataEntityId(),1};GridGeometry3D grid;
    Fixture(){grid.extent={0,7,0,7,0,7};grid.dimensions={8,8,8};DataTransaction tx;
        tx.outputs.push_back({source.entityId,0,DataTypes::imageGrid3D,{},std::make_shared<const ImageGrid3DPayload>(grid,ImageValueType::UInt8,1,std::make_shared<const std::vector<std::uint8_t>>(512,10))});Require(store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"source transaction failed");}
    RoiReadSnapshot Box(double z=2){RoiPrimitive p;p.localToSource={.2,0,0,2,0,.2,0,2,0,0,.2,z,0,0,0,1};RoiRequest request;request.metadata.name="small";request.definition={source,{{RoiNodeKind::Primitive,p}}};const auto result=service.SetRoi(request);Require(result.roi.has_value(),"ROI transaction failed");return RoiEvaluator::GetRoi(store.GetDataGraph(),result.roi->revision,source).roi;}
};
void Metrology(){Fixture f;const auto roi=f.Box();AlignmentWork work;work.input.source=f.source;work.input.mesh=Ref(91);work.cancelled=std::make_shared<std::atomic<bool>>(false);
    work.mesh=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{.5,.5,2,6,.5,2,.5,6,2},std::vector<std::uint64_t>{0,1,2});
    work.referenceMesh=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{0,0,1.5,7,0,1.5,0,7,1.5},std::vector<std::uint64_t>{0,1,2});work.rois.emplace(roi->GetRevision(),roi);
    AlignmentRegion region;region.pinnedMesh=work.input.mesh;region.roi=roi->GetRevision();const auto samples=AlignmentGeometryFit::BuildSamples(work,region,alignmentIdentity);
    Require(samples.points.size()>=3,"geometry fit missed ROI inside a large triangle");for(const auto& p:samples.points)Require(roi->GetContains(p),"geometry fit sample is outside ROI");
    auto explicitRegion=region;explicitRegion.vertexIds={0,1,2};work.recipe.surfaceFit=AlignmentSurfaceFit{};work.recipe.surfaceFit->sourceRegion=explicitRegion;bool empty=false;try{(void)AlignmentSurfaceSampling::Build(work,alignmentIdentity,0,false);}catch(const std::invalid_argument&){empty=true;}Require(empty,"surface fitting ignored the explicit vertex selection while applying ROI");work.recipe.surfaceFit.reset();
    AlignmentDeviationRequest request;request.alignmentResult=Ref(92);request.reverseCoverage=false;request.maxDistance=3;request.evaluationRoi=roi->GetRevision();
    const auto result=AlignmentDeviation::Build(work,request,alignmentIdentity);Require(result.status==AlignmentStatus::FullyDetermined && result.forward,"deviation ROI computation failed");
    Require(result.forward->originalVertexCount==3 && result.forward->samples.size()>3 && std::abs(result.forward->statistics.selectedArea-.16)<1e-10 && std::abs(result.forward->statistics.mean-.5)<1e-10,"ROI deviation lost fragment area or complete reference support");
    for(const auto& sample:result.forward->samples)if(sample.sourcePosition)Require(sample.parentTriangle==0 && std::abs((*sample.sourcePosition)[2]-2)<1e-12,"deviation changed parent surface provenance");
}
void GapSurface(){Fixture f;auto image=vtkSmartPointer<vtkImageData>::New();image->SetExtent(f.grid.extent.data());image->AllocateScalars(VTK_INT,1);std::fill_n(static_cast<int*>(image->GetScalarPointer()),512,0);
    for(int z=1;z<=5;++z)for(int y=1;y<=5;++y)for(int x=1;x<=5;++x)*static_cast<int*>(image->GetScalarPointer(x,y,z))=7;
    VoidRegion parent;parent.id=7;parent.bbox={1,5,1,5,1,5};parent.voxelCount=125;parent.volumeMM3=125;
    auto selected=parent;selected.bbox[1]=3;selected.voxelCount=75;selected.volumeMM3=75;
    RoiPrimitive p;p.shape=RoiShape::HalfSpace;p.origin={3.2,0,0};p.normal={-1,0,0};const auto roi=RoiEvaluator::GetPreview(f.store.GetDataGraph(),{f.source,{{RoiNodeKind::Primitive,p}}}).roi;
    const std::vector<VoidRegion> full{parent};std::atomic<bool> cancel{false};const auto display=GapDisplayData::Build(image,{selected},cancel,roi,&full);
    Require(display && display->mesh && display->mesh->GetNumberOfCells()>0,"gap selected surface is empty");
    for(vtkIdType i=0;i<display->mesh->GetNumberOfCells();++i){auto* cell=display->mesh->GetCell(i);bool cap=true;for(vtkIdType k=0;k<cell->GetNumberOfPoints();++k){double q[3];display->mesh->GetPoint(cell->GetPointId(k),q);Require(q[0]<=3.2+1e-10,"gap surface exceeds evaluation ROI");cap=cap && std::abs(q[0]-3.2)<1e-10;}Require(!cap,"gap ROI introduced an artificial cut cap");}
    Require(display->regionVolumes.size()==1 && display->regionVolumes[0].first==7 && display->regionVolumes[0].second==75,"gap display lost parent ID or selected volume");
}

void MeshCrop(){Fixture f;const DataRevisionRef source{f.store.CreateDataEntityId(),1};
    auto payload=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{.5,.5,2,6,.5,2,.5,6,2},std::vector<std::uint64_t>{0,1,2},
        std::vector<MeshAttribute>{{"value",1,{1.5,7,12.5}},{"measurement.valid",1,{1,0,1}}},"RAS",std::vector<MeshAttribute>{{"face",1,{7}}});
    DataTransaction tx;tx.outputs.push_back({source.entityId,0,DataTypes::surfaceMesh,{},payload});Require(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"mesh source failed");
    RoiPrimitive p;p.shape=RoiShape::Sphere;p.origin={2,2,2};p.radius=.2;RoiRequest request;request.definition={source,{{RoiNodeKind::Primitive,p}}};request.metadata.name="mesh-sphere";
    const auto created=f.service.SetRoi(request);Require(created.roi.has_value(),"mesh ROI failed");
    const auto archive=RoiService::GetArchive(f.store.GetDataGraph(),created.roi->revision,"mesh",0);Require(archive.archive && archive.archive->source.meshGeometryDigest=="77d72606163286d4011ae3be991abe3aba30542be67fd5d92ea50c7bbadf7455","mesh archive geometry SHA-256 is incorrect");
    auto altered=payload->GetVertices();altered[0]=1;const DataRevisionRef other{f.store.CreateDataEntityId(),1};tx={};tx.outputs.push_back({other.entityId,0,DataTypes::surfaceMesh,{},std::make_shared<const SurfaceMeshPayload>(altered,payload->GetTriangles())});Require(f.store.SetDataCommit(tx).status==DataCommitStatus::Succeeded,"changed mesh source failed");const auto beforeRestore=f.store.GetDataGraph().commitId;Require(f.service.LoadArchive(*archive.archive,"mesh",other,{},0).error==RoiError::SourceMismatch && f.store.GetDataGraph().commitId==beforeRestore,"same bounds/counts hid different mesh geometry at restore");
    const auto graph=f.store.GetDataGraph();CropInputSnapshot input;input.graph=graph;input.data=f.store.GetData(graph,source);
    const auto prepared=VtkPreparedDataView::BuildDataView(payload);input.mesh=std::make_shared<const VtkSurfaceMeshView>(VtkSurfaceMeshView{input.data,prepared->mesh->mesh});input.inputModelBounds={.5,6,.5,6,2,2};
    const auto roi=RoiEvaluator::GetRoi(graph,created.roi->revision,source).roi;
    const auto result=CropAlgorithm::GetRoiResult(input,roi,256ULL*1024*1024,{});const auto output=std::dynamic_pointer_cast<const SurfaceMeshPayload>(result.outputPayload);
    Require(result.isSucceeded && output && output->GetCoordinateFrame()=="RAS" && result.meshTriangleCount>0 && result.meshErrorBound>0,"generic mesh crop missed ROI wholly inside a parent triangle");
    const auto& points=output->GetVertices();const auto& attributes=output->GetPointAttributes();Require(attributes.size()==3,"mesh crop lost point attributes");
    const auto& cells=output->GetCellAttributes();Require(cells.size()==2,"mesh crop lost cell attributes");
    double area=0;for(std::size_t i=0;i<output->GetTriangles().size()/3;++i){const auto a=output->GetTriangles()[3*i],b=output->GetTriangles()[3*i+1],c=output->GetTriangles()[3*i+2];area+=.5*std::abs((points[3*b]-points[3*a])*(points[3*c+1]-points[3*a+1])-(points[3*b+1]-points[3*a+1])*(points[3*c]-points[3*a]));Require(cells[0].values[i]==7 && cells[1].values[i]==0,"crop changed parent cell attributes");}
    Require(area>.08 && area<.18,"mesh crop area is inconsistent with selected sphere disk");
    for(std::size_t i=0;i<points.size()/3;++i){Require(std::abs(points[3*i+2]-2)<1e-12 && std::abs(attributes[0].values[i]-(points[3*i]+2*points[3*i+1]))<1e-10,"crop displaced parent surface or lost interpolation");Require(attributes[1].values[i]==0,"crop interpolated invalid measurement into validity");double sum=0;for(int k=0;k<3;++k)sum+=attributes[2].values[3*i+k];Require(std::abs(sum-1)<1e-10,"crop lost barycentric provenance");}
}
class Probe final:public HostFeature{public:std::shared_ptr<TrustedDataPort> data;std::string_view GetFeatureId()const noexcept override{return "RoiConsumer.Probe";}bool AttachHost(const HostFeatureContext& c)override{data=c.data;return bool(data);}bool DetachHost()override{data.reset();return true;}bool OnHostTick()override{return false;}};
void GapSavedParent(){
    HostRenderViewConfig view;view.id="primary";view.role=HostRenderViewRole::Primary3D;view.window.viewInit.viewMode=HostRenderMode::Volume;view.inputMode=HostInputMode::HostInjected;view.renderWindow=vtkSmartPointer<vtkRenderWindow>::New();view.renderWindow->SetOffScreenRendering(1);
    HostSessionConfig config;config.renderViews={view};config.driveMode=HostDriveMode::HostDriven;config.onWorkAvailable=[]{};VtkAppHostSession session(config);struct Stop{VtkAppHostSession& s;~Stop(){s.Stop();}}stop{session};
    auto probe=std::make_shared<Probe>();GapHostConfig gapConfig;gapConfig.keys.switchOverlay.keySym="g";gapConfig.keys.exit.keySym="Escape";gapConfig.inputViews.viewIds={"primary"};gapConfig.defaultStart.targetViews.viewIds={"primary"};gapConfig.defaultStart.surface.isoMode=GapIsoMode::AbsoluteValue;gapConfig.defaultStart.surface.absoluteIsoValue=50;gapConfig.defaultStart.surface.backgroundMean=0;gapConfig.defaultStart.surface.materialMean=100;
    auto gap=std::make_shared<GapHostFeature>(gapConfig);Require(session.BuildSession() && session.AttachFeature(probe) && session.AttachFeature(gap),"gap host fixture failed");
    auto pump=[&](const std::function<bool()>& done){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60);while(!done() && std::chrono::steady_clock::now()<until){session.SendUpdates();std::this_thread::sleep_for(std::chrono::milliseconds(1));}Require(done(),"gap callback timed out");};
    HostReloadRequest load;load.geometry.dimensions={24,24,24};load.geometry.spacing={1,1,1};load.metadata.source.kind=ImageSourceKind::Memory;load.metadata.source.uri="memory://gap-saved-parent";load.metadata.identity.datasetId="gap-saved-parent";load.voxels.resize(24*24*24);
    for(int z=0;z<24;++z)for(int y=0;y<24;++y)for(int x=0;x<24;++x){const bool solid=x>=3 && x<=20 && y>=3 && y<=20 && z>=3 && z<=20;const bool cavity=(x-12)*(x-12)+(y-12)*(y-12)+(z-12)*(z-12)<=9;load.voxels[x+24*(y+24*z)]=solid&&!cavity?100.F:0.F;}
    bool loaded=false;HostResult loadResult;Require(session.SendRequest(std::move(load),[&](HostResult r){loadResult=r;loaded=true;}).status==HostAdmissionStatus::Accepted,"gap source refused");pump([&]{return loaded;});Require(loadResult.isSucceeded,"gap source failed");
    auto run=[&](GapHostAction action,GapHostStartParams parameters){bool done=false;GapHostResult result;GapHostRequest request;request.action=action;request.start=std::move(parameters);Require(gap->SendRequest(request,[&](GapHostResult r){result=std::move(r);done=true;}).status==GapAdmissionStatus::Accepted,"gap operation refused");pump([&]{return done;});if(result.status!=GapResultStatus::Succeeded && result.status!=GapResultStatus::SucceededWithDisplayFailure)throw std::runtime_error("gap operation failed: "+result.message);return result;};
    const auto first=run(GapHostAction::Start,gapConfig.defaultStart);auto graph=probe->data->GetDataGraph();const auto saved=probe->data->GetData(graph,first.resultSet);const auto* collection=saved?dynamic_cast<const DataCollectionPayload*>(saved->payload.get()):nullptr;Require(collection,"saved gap collection missing");
    DataRevisionRef parentRecord;for(const auto& item:collection->GetItems())if(item.role=="parent-detection")parentRecord=item.data;
    const auto record=probe->data->GetData(graph,parentRecord);const auto* parent=record?dynamic_cast<const GapParentRecord*>(record->payload.get()):nullptr;Require(parent && parent->GetValid() && !parent->voids->empty(),"full saved gap parent missing defects");
    // 保存正式结果必须建立图中的保留关系；仅保存运行期 ref 不阻止 owner 结果退役。
    DataTransaction keep;keep.bindings.push_back({"test.roi-saved-gap-parent",0,true,{},first.resultSet});Require(probe->data->SetDataCommit(keep).status==DataCommitStatus::Succeeded,"saved parent retention binding failed");
    auto secondParams=gapConfig.defaultStart;secondParams.surface.absoluteIsoValue=60;const auto second=run(GapHostAction::Start,secondParams);Require(second.resultSet!=first.resultSet,"gap did not publish independent parent results");
    const auto d=*session.GetImageDescriptor();RoiPrimitive primitive;primitive.shape=RoiShape::HalfSpace;primitive.origin=d.origin;for(int r=0;r<3;++r){primitive.origin[r]+=d.direction[r*3]*d.spacing[0]*12;primitive.normal[r]=d.direction[r*3];}
    RoiRequest roiRequest;roiRequest.metadata.name="saved-parent-selection";roiRequest.definition={d.dataRevision,{{RoiNodeKind::Primitive,primitive}}};const auto roi=session.SetRoi(roiRequest);Require(roi.roi.has_value(),"gap evaluation ROI failed");
    graph=probe->data->GetDataGraph();Require(probe->data->GetRoi(graph,roi.roi->revision,d.dataRevision).error==RoiError::None,"saved-parent ROI lookup failed");Require(bool(probe->data->GetData(graph,first.resultSet)),"old parent result retired during another detection");
    DataRevisionRef parentLabel;for(const auto& item:collection->GetItems())if(item.role=="parent-labels")parentLabel=item.data;const auto labelNode=probe->data->GetData(graph,parentLabel);const auto label=labelNode?std::dynamic_pointer_cast<const LabelMap3DPayload>(labelNode->payload):nullptr;Require(label && label->GetValues()==parent->labels->GetValues(),"saved parent label identity mismatch");const auto labelView=probe->data->GetLabelMap(graph,parentLabel);Require(labelView && labelView->labels && labelView->labels->GetScalarType()==VTK_INT,"saved parent label VTK type mismatch");Require(parent->parameters.isoValue==50 && parent->source==d.dataRevision,"saved parent parameters or source mismatch");
    auto evaluation=gapConfig.defaultStart;evaluation.parentResult=first.resultSet;evaluation.evaluationRoi=roi.roi->revision;evaluation.roiPolicy=GapRoiPolicy::InsideOnly;const auto evaluated=run(GapHostAction::Evaluate,evaluation);
    Require(!evaluated.statistics.isPorosityAvailable && evaluated.statistics.voidVoxelCount>0 && evaluated.statistics.voidVoxelCount<first.statistics.voidVoxelCount,"gap re-evaluation lost partial ROI statistics");
    const auto node=probe->data->GetData(probe->data->GetDataGraph(),evaluated.resultSet);Require(node && std::any_of(node->inputs.begin(),node->inputs.end(),[&](const auto& input){return input.role=="parent-result" && input.source==first.resultSet;}),"gap re-evaluation did not trace the explicitly saved parent");
    DataTransaction release;release.bindings.push_back({"test.roi-saved-gap-parent",1,true,first.resultSet,{}});Require(probe->data->SetDataCommit(release).status==DataCommitStatus::Succeeded,"saved parent retention cleanup failed");
}
#ifdef ROI_TEST_WALL
void Wall(){Fixture f;const auto roi=f.Box(1.5);ThicknessAlgorithm::Work work;work.evaluationRoi=roi;
    auto values=std::make_shared<std::vector<std::uint8_t>>(512,0);auto labels=std::make_shared<std::vector<std::uint64_t>>(512,0);
    for(int z=2;z<=3;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){const auto i=x+8*(y+8*z);(*values)[i]=100;(*labels)[i]=1;}
    work.source=std::make_shared<const ImageGrid3DPayload>(f.grid,ImageValueType::UInt8,1,values);work.labels=std::make_shared<const LabelMap3DPayload>(f.grid,std::shared_ptr<const std::vector<std::uint64_t>>(labels));
    work.mesh=std::make_shared<const SurfaceMeshPayload>(std::vector<double>{.5,.5,1.5,6,.5,1.5,.5,6,1.5},std::vector<std::uint64_t>{0,2,1},std::vector<MeshAttribute>{{"measurement.normal",3,{0,0,-1,0,0,-1,0,0,-1}}});
    work.archive.input={f.source,Ref(93),Ref(94),{}, {}, {}, {1},ThicknessUnit::Millimeter};work.archive.params.materialThreshold=50;work.archive.params.maxDistance=10;work.archive.params.sampleSpacing=10;work.archive.params.maxBoundaryError=.5;work.archive.params.directionCount=1;work.archive.params.evaluationRoi=roi->GetRevision();
    work.archive.evaluation={1.5,2.5,{0,4},8,0};work.cancelled=std::make_shared<std::atomic<bool>>(false);work.deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    const auto result=ThicknessAlgorithm::BuildField(work);Require(result.status==ThicknessStatus::Succeeded && result.field.samples && !result.field.samples->empty(),"wall missed ROI inside a large parent triangle");
    double area=0;for(const auto& sample:*result.field.samples){area+=sample.area;Require(sample.sourceTriangle==0 && std::abs(sample.source[2]-1.5)<1e-12,"wall displaced parent surface samples");}
    Require(std::abs(area-.16)<1e-10 && std::abs(result.statistics.evaluatedArea-.16)<1e-10,"wall weighted entire centroid patch rather than ROI area");
}
#endif
}
int main(){try{Metrology();GapSurface();MeshCrop();GapSavedParent();
#ifdef ROI_TEST_WALL
Wall();
#endif
return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
