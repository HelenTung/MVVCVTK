#include "Host/VtkAppHostSession.h"
#include "Host/CropHostFeature.h"
#include "Host/ArtifactReductionHostFeature.h"
#include "Host/PartSegmentationHostFeature.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include <vtkImageData.h>
#include <vtkRenderWindow.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool value,const std::string& name)
{
    if (!value) throw std::runtime_error(name);
    std::cout << "[PASS] " << name << std::endl;
}
class Probe final : public HostFeature {
public:
    std::shared_ptr<TrustedDataPort> data;
    std::string_view GetFeatureId() const noexcept override { return "RoiIntegration.Probe"; }
    bool AttachHost(const HostFeatureContext& c) override { data=c.data; return bool(data); }
    bool DetachHost() override { data.reset(); return true; }
    bool OnHostTick() override { return false; }
};
void Pump(VtkAppHostSession& session,const std::function<bool()>& done)
{
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while (!done() && std::chrono::steady_clock::now()<until) {
        session.SendUpdates(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(done(),"owner 完成回调/工作状态在期限内兑现");
}
bool Selected(std::size_t i,int n)
{
    const int x=int(i%n),y=int(i/n%n),z=int(i/(n*n));
    return x>=n/4 && x<3*n/4 && y>=n/4 && y<3*n/4 && z>=n/4 && z<3*n/4;
}
std::array<double,3> Point(const ImageDescriptor& d,double x,double y,double z)
{
    std::array<double,3> p=d.origin;
    const double index[3]={x+d.extent[0],y+d.extent[2],z+d.extent[4]};
    for (int r=0;r<3;++r) for (int c=0;c<3;++c) p[r]+=d.direction[r*3+c]*d.spacing[c]*index[c];
    return p;
}
bool HasInput(const DataSnapshot& data,const std::string& role,const DataRevisionRef& ref)
{
    if (!data) return false;
    return std::any_of(data->inputs.begin(),data->inputs.end(),[&](const auto& input) { return input.role==role && input.source==ref; });
}
void Run(const char* sample)
{
    const int n=sample ? 64:24; const std::size_t count=std::size_t(n)*n*n;
    HostRenderViewConfig view; view.id="primary"; view.role=HostRenderViewRole::Primary3D;
    view.window.viewInit.viewMode=HostRenderMode::Volume; view.inputMode=HostInputMode::HostInjected;
    view.renderWindow=vtkSmartPointer<vtkRenderWindow>::New(); view.renderWindow->SetOffScreenRendering(1);
    HostSessionConfig config; config.renderViews={view}; config.driveMode=HostDriveMode::HostDriven;
    config.onWorkAvailable=[] {};
    VtkAppHostSession session(config);
    struct StopGuard { VtkAppHostSession& session; ~StopGuard() { session.Stop(); } } stopGuard{session};
    auto probe=std::make_shared<Probe>(); auto crop=std::make_shared<CropHostFeature>();
    auto artifact=std::make_shared<ArtifactReductionHostFeature>();
    auto part=std::make_shared<PartSegmentationHostFeature>(); SurfaceDeterminationConfig surfaceConfig; surfaceConfig.maxWorkingBytes=1024ULL*1024*1024;
    auto surface=std::make_shared<SurfaceDeterminationHostFeature>(surfaceConfig);
    Check(session.BuildSession() && session.AttachFeature(probe) && session.AttachFeature(crop)
        && session.AttachFeature(artifact) && session.AttachFeature(part) && session.AttachFeature(surface),"真实 Session 挂载四个消费者");
    bool loaded=false; HostResult loadResult;
    const auto complete=[&](HostResult r) { loadResult=std::move(r); loaded=true; };
    if (sample) {
        HostLoadRequest load; load.filePath=sample; load.geometry.dimensions={n,n,n};
        load.geometry.spacing={.1537F,.1537F,.1537F}; load.geometry.origin={113.1232F,87.3016F,113.1232F};
        load.metadata.source.kind=ImageSourceKind::RawFile; load.metadata.identity.datasetId="roi-real-1536-region"; load.metadata.source.uri=sample;
        Check(session.SendRequestResult(std::move(load),complete),"真实 CT RAW 加载接纳");
    } else {
        HostReloadRequest load; load.geometry.dimensions={n,n,n}; load.geometry.spacing={.7F,1.1F,1.3F};
        load.geometry.origin={12.F,-17.F,23.F}; load.geometry.direction={0,-1,0,1,0,0,0,0,1};
        load.metadata.source.kind=ImageSourceKind::Memory; load.metadata.identity.datasetId="roi-synthetic"; load.metadata.source.uri="memory://roi-synthetic";
        load.voxels.resize(count);
        for (std::size_t i=0;i<count;++i) {
            const double x=double(i%n)-n*.5,y=double(i/n%n)-n*.5,z=double(i/(n*n))-n*.5;
            load.voxels[i]=float(std::sqrt(x*x+y*y+z*z)+.12*std::sin(double(i)*.73));
        }
        Check(session.SendRequestResult(std::move(load),complete),"旋转各向异性体数据加载接纳");
    }
    Pump(session,[&] { return loaded; }); Check(loadResult.isSucceeded,"来源加载完成");
    const auto descriptor=session.GetImageDescriptor(); Check(bool(descriptor),"读取规范 RAS 来源描述");
    const auto d=*descriptor; const auto source=d.dataRevision;
    auto graph=probe->data->GetDataGraph(); const auto original=probe->data->GetImageGrid(graph,source);
    Check(original && original->image,"读取来源不可变快照");
    const auto* originalValues=static_cast<const float*>(original->image->GetScalarPointer());
    RoiRequest create; create.definition.source=source; create.metadata.name="shared-roi";
    RoiNode box; box.kind=RoiNodeKind::Primitive;
    const double mid=(n-1)*.5,half=(n/2-1)*.5;
    const auto center=Point(d,mid,mid,mid);
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c) box.primitive.localToSource[r*4+c]=d.direction[r*3+c]*d.spacing[c]*half;
        box.primitive.localToSource[r*4+3]=center[r];
    }
    create.definition.nodes={box};
    RoiResult wrong; std::thread worker([&] { wrong=session.SetRoi(create); }); worker.join();
    Check(wrong.error==RoiError::WrongThread,"Session 非 owner 写入明确拒绝");
    const auto created=session.SetRoi(create); Check(created.error==RoiError::None && created.roi,"公共入口创建正式 ROI");
    const auto ref=created.roi->revision;
    graph=probe->data->GetDataGraph(); const auto frozen=probe->data->GetRoi(graph,ref,source);
    Check(frozen.error==RoiError::None && frozen.roi,"冻结公共 ROI");
    RoiMaskRequest maskRequest; maskRequest.region.size={std::size_t(n),std::size_t(n),std::size_t(n)};
    const auto mask=frozen.roi->GetMaskChunk(maskRequest);
    bool exact=mask.error==RoiError::None && mask.isComplete && mask.values.size()==count;
    for (std::size_t i=0;exact && i<count;++i) exact=(mask.values[i]!=0)==Selected(i,n);
    Check(exact,"独立整数索引基准与源物理 ROI 完全一致");
    const HostViewTargets targets{{"primary"},{}};
    CropHostTarget target; target.inputBinding=std::string(primaryVolumeBinding); target.referenceView.viewId="primary"; target.targetViews=targets;
    CropDocumentRequest openCrop;openCrop.action=CropDocumentAction::CreateDocument;
    openCrop.requestId=CropHostFeature::CreateRequestId();openCrop.target=target;openCrop.sourceRevision=source;
    bool opened=false;CropDocumentOutcome openResult;
    Check(bool(crop->SendRequest(openCrop,[&](CropDocumentOutcome value){openResult=std::move(value);opened=true;})),"Crop 显式创建来源文档");
    Pump(session,[&]{return opened;});Check(openResult.status==CropEditStatus::Succeeded,"Crop 文档准备全部视图");
    const auto cropHistory=crop->GetHistory();
    CropBuildRequest cropRequest;cropRequest.documentId=cropHistory.documentId;cropRequest.nodeId=cropHistory.rootNodeId;
    cropRequest.requestId=CropHostFeature::CreateRequestId();cropRequest.expectedRevision=cropHistory.stateRevision;cropRequest.inputRoi=ref;
    bool cropped=false; CropBuildResult cropResult;
    Check(bool(crop->SendRequest(cropRequest,[&](CropBuildResult r) { cropResult=std::move(r); cropped=true; })),"Crop 接纳同一 ROI");
    Pump(session,[&] { return cropped; }); Check(cropResult.isSucceeded,"Crop 生成派生结果："+cropResult.message);
    graph=probe->data->GetDataGraph(); auto croppedImage=probe->data->GetImageGrid(graph,cropResult.outputRevision);
    exact=croppedImage && croppedImage->validityMask && std::memcmp(originalValues,croppedImage->image->GetScalarPointer(),count*sizeof(float))==0;
    const auto* validity=exact ? static_cast<const unsigned char*>(croppedImage->validityMask->GetScalarPointer()):nullptr;
    for (std::size_t i=0;exact && i<count;++i) exact=(validity[i]!=0)==Selected(i,n);
    Check(exact && HasInput(croppedImage->data,"crop-roi",ref),"Crop 保留原标量并精确裁切有效域，追溯同一修订");
    const auto savedCrop=crop->GetArchive(cropHistory.documentId);
    Check(savedCrop&&savedCrop->result&&savedCrop->result->inputRoi==ref,"ROI 构建归档保留精确输入身份");
    CropDocumentRequest restoreCrop;restoreCrop.action=CropDocumentAction::RestoreDocument;
    restoreCrop.requestId=CropHostFeature::CreateRequestId();restoreCrop.target=target;
    restoreCrop.sourceRevision=source;restoreCrop.archive=*savedCrop;
    bool restoredCrop=false;CropDocumentOutcome restoreCropResult;
    Check(bool(crop->SendRequest(restoreCrop,[&](CropDocumentOutcome value){restoreCropResult=std::move(value);restoredCrop=true;})),"ROI 裁切结果归档恢复接纳");
    Pump(session,[&]{return restoredCrop;});
    Check(restoreCropResult.status==CropEditStatus::Succeeded&&restoreCropResult.restoreStatus==CropRestoreStatus::ResultRestored,"ROI 归档恢复独立结果");
    {
        const auto restoredState=crop->GetState(restoreCropResult.documentId);
        const auto restoredImage=probe->data->GetImageGrid(probe->data->GetDataGraph(),restoredState.outputRevision);
        Check(restoredImage&&restoredImage->validityMask&&restoredImage->validityMask->GetScalarPointer()!=croppedImage->validityMask->GetScalarPointer()
            &&std::memcmp(restoredImage->validityMask->GetScalarPointer(),validity,count)==0,"恢复结果复制掩码，逐体素一致且生命周期独立");
    }
    CropDocumentRequest closeRestored;closeRestored.action=CropDocumentAction::CloseDocument;closeRestored.documentId=restoreCropResult.documentId;
    closeRestored.requestId=CropHostFeature::CreateRequestId();closeRestored.expectedRevision=crop->GetHistory(closeRestored.documentId).stateRevision;
    bool closedRestored=false;CropDocumentOutcome closeResult;
    Check(bool(crop->SendRequest(closeRestored,[&](CropDocumentOutcome value){closeResult=std::move(value);closedRestored=true;})),"关闭恢复文档接纳");
    Pump(session,[&]{return closedRestored;});Check(closeResult.status==CropEditStatus::Succeeded,"关闭恢复结果不影响原结果");
    CropDocumentRequest activateOriginal;activateOriginal.action=CropDocumentAction::ActivateDocument;activateOriginal.documentId=cropHistory.documentId;
    activateOriginal.requestId=CropHostFeature::CreateRequestId();activateOriginal.expectedRevision=crop->GetHistory(cropHistory.documentId).stateRevision;activateOriginal.target=target;
    bool activatedOriginal=false;CropDocumentOutcome activateResult;
    Check(bool(crop->SendRequest(activateOriginal,[&](CropDocumentOutcome value){activateResult=std::move(value);activatedOriginal=true;})),"重新激活原裁切文档");
    Pump(session,[&]{return activatedOriginal;});Check(activateResult.status==CropEditStatus::Succeeded,"原裁切文档仍可独立使用");
    ArtifactRequest reduce; reduce.source=source; reduce.processingRoi=ref; reduce.qualityRoi=ref;
    ArtifactDiffusionParams diffusion; diffusion.iterations=1; diffusion.threshold=100; reduce.diffusion=diffusion;
    ArtifactHostRequest prepare; prepare.prepare=reduce;
    auto admission=artifact->SendRequest(prepare); Check(admission.error==ArtifactError::None,"Artifact 接纳同一 ROI");
    Pump(session,[&] { return artifact->GetState().status!=ArtifactStatus::Running; });
    Check(artifact->GetState().status==ArtifactStatus::Ready,"Artifact 候选完成");
    // 元数据修订不改变几何引用，因此不能使已冻结任务过期。
    RoiRequest rename; rename.action=RoiAction::SetMetadata; rename.expectedRoi=ref;
    rename.expectedCatalogRevision=created.roi->catalogRevision; rename.metadata.name="renamed";
    const auto renamed=session.SetRoi(rename); Check(renamed.error==RoiError::None,"运行后重命名不产生几何修订");
    ArtifactHostRequest commit; commit.action=ArtifactAction::Commit; commit.requestId=admission.requestId;
    Check(artifact->SendRequest(commit).error==ArtifactError::None,"重命名后候选仍可正式提交");
    const auto corrected=artifact->GetState().correctedVolume; Check(bool(corrected),"Artifact 正式输出存在");
    graph=probe->data->GetDataGraph(); const auto filtered=probe->data->GetImageGrid(graph,*corrected);
    const auto* filteredValues=static_cast<const float*>(filtered->image->GetScalarPointer());
    std::size_t changed=0; exact=true;
    for (std::size_t i=0;i<count;++i) if (std::memcmp(originalValues+i,filteredValues+i,sizeof(float))!=0) {
        ++changed; if (!Selected(i,n)) exact=false;
    }
    Check(exact && changed>0 && HasInput(filtered->data,"processing-roi",ref),"Artifact 仅改 ROI 内体素且记录同一修订");
    PartSegmentationStartParams partParams; partParams.targetViews=targets;
    partParams.threshold=*std::min_element(originalValues,originalValues+count)-1;
    PartSegmentationRequest segment; segment.action=PartSegmentationAction::Start; segment.start=partParams;
    bool segmented=false; PartSegmentationResult segmentedResult;
    Check(part->SendRequest(segment,[&](PartSegmentationResult r) { segmentedResult=std::move(r); segmented=true; }).status==PartAdmissionStatus::Accepted,"Part 正常分割接纳");
    Pump(session,[&] { return segmented; });
    const auto parts=part->GetPartSetSnapshot(); Check(segmentedResult.status==PartResultStatus::Succeeded && parts && parts->parts.size()==1,"普通分割保持完整来源域");
    PartEditRequest edit; edit.expectedLabelMap=segmentedResult.labelMap; edit.expectedCatalogRevision=parts->catalogRevision; edit.scope.editRoi=ref;
    PartBrushEdit brush; brush.target=parts->parts.front().binding; brush.isErase=true; brush.radiusMM=100000; brush.sourcePoints={center}; edit.operation=brush;
    bool edited=false; PartSegmentationResult editResult;
    Check(part->SendEditRequest(edit,[&](PartSegmentationResult r) { editResult=std::move(r); edited=true; }).status==PartAdmissionStatus::Accepted,"Part 编辑接纳同一 ROI");
    Pump(session,[&] { return edited; }); const auto preview=part->GetEditPreview();
    Check(editResult.status==PartResultStatus::PreviewReady && preview,"Part 编辑只生成预览");
    exact=preview->labels && preview->labels->size()==count;
    for (std::size_t i=0;exact && i<count;++i) exact=((*preview->labels)[i]==0)==Selected(i,n);
    Check(exact,"Part 擦除与公共 ROI 整数索引基准逐体素一致");
    bool partCommitted=false; PartSegmentationResult partResult;
    Check(part->SetEditCommit(preview->previewId,[&](PartSegmentationResult r) { partResult=std::move(r); partCommitted=true; }).status==PartAdmissionStatus::Accepted,"Part 预览确认接纳");
    Pump(session,[&] { return partCommitted; });
    graph=probe->data->GetDataGraph();
    Check(partResult.status==PartResultStatus::Succeeded && HasInput(probe->data->GetData(graph,partResult.labelMap),"edit-roi",ref),"Part 正式标签追溯同一 ROI");
    SurfaceDeterminationStartParams surfaceParams; surfaceParams.targetViews=targets; surfaceParams.analysisRoi=ref;
    surfaceParams.method=SurfaceDeterminationMethod::LocalAdaptiveIso50; surfaceParams.componentSelection=SurfaceComponentSelection::All;
    double minimum=INFINITY,maximum=-INFINITY;
    for (std::size_t i=0;i<count;++i) if (Selected(i,n)) { minimum=std::min(minimum,double(originalValues[i])); maximum=std::max(maximum,double(originalValues[i])); }
    surfaceParams.initialIsoValue=(minimum+maximum)*.5;
    SurfaceDeterminationRequest extract; extract.action=SurfaceDeterminationAction::Start; extract.start=surfaceParams;
    bool surfaced=false; SurfaceDeterminationResult surfaceResult;
    Check(surface->SendRequest(extract,[&](SurfaceDeterminationResult r) { surfaceResult=std::move(r); surfaced=true; }).status==SurfaceAdmissionStatus::Accepted,"Surface 接纳同一凸 ROI");
    Pump(session,[&] { return surfaced; });
    const auto mesh=surface->GetSurfaceSnapshot();
    Check(surfaceResult.status==SurfaceResultStatus::Succeeded && surfaceResult.isPublished
        && mesh && mesh->purpose==SurfaceTaskPurpose::Determine && mesh->points && !mesh->points->empty(),"Surface 生成局部表面："+surfaceResult.message);
    exact=true; for (const auto& p:*mesh->points) if (!frozen.roi->GetContains(p.positionModel)) exact=false;
    graph=probe->data->GetDataGraph();
    Check(exact && HasInput(probe->data->GetData(graph,mesh->meshRevision),"analysis-roi",ref),"Surface 所有点位于同一精确 ROI 并记录修订");
    const auto archive=session.GetRoiArchive(ref,d.metadata.identity.datasetId);
    Check(archive.error==RoiError::None && archive.archive,"带来源 URI 的纯值归档成功");
    const auto restored=session.LoadRoiArchive(*archive.archive,d.metadata.identity.datasetId,source,renamed.roi->catalogRevision);
    Check(restored.error==RoiError::None && restored.roi && restored.roi->revision.entityId!=ref.entityId,"归档显式映射来源并恢复新身份");
    // 精确几何头改变后，CurrentPrimary 模式不能提交旧 ROI 的候选。
    Check(artifact->SendRequest(prepare).error==ArtifactError::None,"再次冻结旧 ROI 候选");
    Pump(session,[&] { return artifact->GetState().status!=ArtifactStatus::Running; });
    Check(artifact->GetState().status==ArtifactStatus::Ready,"几何冲突前候选已就绪");
    RoiRequest change=create; change.metadata={}; change.action=RoiAction::SetGeometry; change.expectedRoi=ref;
    change.expectedCatalogRevision=restored.roi->catalogRevision;
    // 旋转数据的第一列可能有零项，改动非零列方向。
    for (int r=0;r<3;++r) change.definition.nodes[0].primitive.localToSource[r*4]*=.8;
    Check(session.SetRoi(change).error==RoiError::None,"公共几何修改创建新修订");
    commit.requestId=artifact->GetState().requestId;
    Check(artifact->SendRequest(commit).error==ArtifactError::SourceChanged,"旧 ROI 候选在提交时拒绝过期头");
    Check(surface->GetResultValidity(mesh->dataRevision).status==SurfaceRestoreStatus::Historical,
        "ROI 几何更新使冻结表面成为历史结果");
    Check(frozen.roi->GetMaskChunk(maskRequest).values==mask.values,"旧 ROI 读取快照保持不变");
    // 正向组合：裁切输出上的正式 Surface 必须由消费者释放，不能永久阻塞原文档。
    bool selectedCrop=false;HostResult selectCropResult;
    HostDataSelectRequest selectCrop;selectCrop.dataRevision=cropResult.outputRevision;
    selectCrop.expectedBindingRevision=session.GetImageDescriptor()->bindingRevision;
    Check(session.SendRequestResult(std::move(selectCrop),[&](HostResult value){selectCropResult=std::move(value);selectedCrop=true;}),"选择裁切结果作为 Surface 来源");
    Pump(session,[&]{return selectedCrop;});Check(selectCropResult.isSucceeded,"裁切结果选择完成");
    surfaceParams.analysisRoi.reset();surfaceParams.sourceVolume=cropResult.outputRevision;
    extract.start=surfaceParams;surfaced=false;surfaceResult={};
    Check(surface->SendRequest(extract,[&](SurfaceDeterminationResult value){surfaceResult=std::move(value);surfaced=true;}).status==SurfaceAdmissionStatus::Accepted,"Surface 接纳临时裁切来源");
    Pump(session,[&]{return surfaced;});auto scopedSurface=surface->GetSurfaceSnapshot();
    Check(surfaceResult.isPublished&&scopedSurface&&scopedSurface->points&&!scopedSurface->points->empty(),"临时裁切来源生成非空正式表面："+surfaceResult.message);
    auto scopedData=probe->data->GetData(probe->data->GetDataGraph(),scopedSurface->dataRevision);
    Check(scopedData&&GetDataEntityIdValid(scopedData->lifetimeScope),"正式 Surface 继承临时来源生命周期");
    auto heldPoints=scopedSurface->points;scopedSurface.reset();scopedData.reset();
    Check(!session.DetachFeature(*surface),"公开表面点数组仍持有时不提前报告释放");
    heldPoints.reset();
    bool surfaceDetached=false;Pump(session,[&]{if(!surfaceDetached)surfaceDetached=session.DetachFeature(*surface);return surfaceDetached;});
    croppedImage.reset();
    Pump(session,[&]{return session.Stop();});
    Check(session.Stop(),"Session 停止完整释放消费者");
    std::cout << "EVIDENCE voxels=" << count << " selected=" << count/8 << " artifactChanged=" << changed
        << " surfacePoints=" << mesh->points->size() << " real=" << bool(sample) << std::endl;
}
}
int main(int argc,char** argv)
{
    try { Run(argc>1 ? argv[1]:nullptr); return 0; }
    catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << std::endl; return 1; }
}
