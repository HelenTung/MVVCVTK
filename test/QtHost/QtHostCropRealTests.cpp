#include "QtHostMethodCases.h"
#include "../TestTimer.h"
#include "Host/CropHostFeature.h"
#include "Host/VtkAppHostSession.h"
#include "Host/TrustedDataPort.h"
#include "Data/DataPayloads.h"

#include <vtkCommand.h>
#include <vtkImageData.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkWindowToImageFilter.h>
#include <vtkPNGWriter.h>
#include <vtkNew.h>
#ifndef GLAD_API_CALL_EXPORT
#define GLAD_API_CALL_EXPORT
#endif
#include <vtk_glad.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
#endif

namespace {
using Clock=std::chrono::steady_clock;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Env(const char* name){
#ifdef _WIN32
    char* value=nullptr;std::size_t size=0;if(_dupenv_s(&value,&size,name)!=0)return {};std::string result=value?value:"";std::free(value);return result;
#else
    const auto* value=std::getenv(name);return value?value:"";
#endif
}
std::string Quote(const std::string& value){std::string text="\"";for(char c:value){if(c=='\\'||c=='\"')text+='\\';if(c=='\n')text+="\\n";else text+=c;}return text+'"';}
std::string Ref(const DataRevisionRef& value){std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto byte:value.entityId.bytes)out<<std::setw(2)<<unsigned(byte);out<<':'<<std::dec<<value.generation;return out.str();}
double Millis(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
std::array<int,3> Dims(const std::string& value){std::array<int,3> result{};std::istringstream in(value);char a=0,b=0;Require(bool(in>>result[0]>>a>>result[1]>>b>>result[2])&&a==','&&b==','&&in.eof(),"dimensions must be X,Y,Z");for(auto n:result)Require(n>=32&&n<=4096&&n%16==0,"real audit dimensions must be multiples of 16 in [32,4096]");return result;}

template<class T> struct Completion {
    const std::thread::id owner=std::this_thread::get_id();
    std::atomic<bool> wrongThread{false};std::size_t count=0;T value;
    void Set(T result){if(std::this_thread::get_id()!=owner){wrongThread.store(true);return;}value=std::move(result);++count;}
    bool Ready() const{return wrongThread.load()||count!=0;}
    bool Once() const{return !wrongThread.load()&&count==1;}
};

struct Measurements {
    std::vector<double> ticks;
    std::uint64_t peakWorking=0,peakPrivate=0,gpuFreeFirst=0,gpuFreeMin=0;
    void Sample(){
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX info{};info.cb=sizeof(info);
        if(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),sizeof(info))){peakWorking=std::max<std::uint64_t>(peakWorking,info.PeakWorkingSetSize);peakPrivate=std::max<std::uint64_t>(peakPrivate,std::max(info.PrivateUsage,info.PeakPagefileUsage));}
#endif
    }
    void SampleGpu(vtkRenderWindow* window){
        auto* gl=vtkOpenGLRenderWindow::SafeDownCast(window);if(!gl)return;gl->MakeCurrent();
        if(!GLAD_GL_NVX_gpu_memory_info)return;
        GLint freeKiB=0;glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX,&freeKiB);
        if(freeKiB<=0)return;const auto bytes=std::uint64_t(freeKiB)*1024;
        if(!gpuFreeFirst)gpuFreeFirst=gpuFreeMin=bytes;else gpuFreeMin=std::min(gpuFreeMin,bytes);
    }
};
class Probe final:public HostFeature {
public:
    std::shared_ptr<TrustedDataPort> data;
    std::shared_ptr<FeatureViewDirectory> views;
    std::string_view GetFeatureId() const noexcept override{return "test.crop-real.reader";}
    bool AttachHost(const HostFeatureContext& context) override{data=context.data;views=context.views;return data&&views;}
    bool DetachHost() override{data.reset();views.reset();return true;}
    bool OnHostTick() override{return true;}
};
class Fixture {
public:
    static HostSessionConfig Config(){HostSessionConfig config;HostRenderViewConfig volume;volume.id="real-volume";volume.role=HostRenderViewRole::Primary3D;volume.window.width=320;volume.window.height=320;volume.window.viewInit.viewMode=HostRenderMode::Volume;config.renderViews.push_back(volume);volume.id="real-slice";volume.role=HostRenderViewRole::TopDownSlice;volume.window.viewInit.viewMode=HostRenderMode::SliceTopDown;config.renderViews.push_back(volume);return config;}
    VtkAppHostSession session{Config()};
    std::shared_ptr<CropHostFeature> crop=std::make_shared<CropHostFeature>();
    std::shared_ptr<Probe> probe=std::make_shared<Probe>();
    Measurements metrics;
    const HostRenderViewEndpoint* timer=nullptr;
    Fixture(){Require(session.BuildSession()&&session.AttachFeature(crop)&&session.AttachFeature(probe),"real fixture attach");for(const auto& id:{"real-volume","real-slice"}){const auto* view=session.GetRenderViewEndpoint(id);Require(view&&view->renderWindow,"real view");view->renderWindow->SetOffScreenRendering(1);}timer=session.GetRenderViewEndpoint("real-slice");HostTimerConfig tick;tick.isTimerEnabled=true;tick.targetView.viewId="real-slice";Require(session.AttachTimer(tick)&&session.Start(),"real fixture timer");}
    ~Fixture(){(void)Stop();}
    void Tick(){const auto start=Clock::now();int id=GetTestTimerId(timer->interactor);Require(id!=0,"real fixture timer missing");timer->interactor->InvokeEvent(vtkCommand::TimerEvent,&id);metrics.ticks.push_back(Millis(start));metrics.Sample();if(metrics.gpuFreeFirst)metrics.SampleGpu(session.GetRenderViewEndpoint("real-volume")->renderWindow);}
    bool Wait(const std::function<bool()>& ready){const auto limit=Clock::now()+std::chrono::minutes(10);while(!ready()){if(Clock::now()>limit)return false;Tick();std::this_thread::sleep_for(std::chrono::milliseconds(1));}return true;}
    bool Stop(){const auto limit=Clock::now()+std::chrono::seconds(30);do{if(session.Stop())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(Clock::now()<limit);return false;}
    CropHostTarget Target() const {CropHostTarget target;target.inputBinding=std::string(primaryVolumeBinding);target.referenceView.viewId="real-volume";target.targetViews.viewIds={"real-volume","real-slice"};return target;}
    void Save(const std::filesystem::path& path){const auto* view=session.GetRenderViewEndpoint("real-volume");view->renderWindow->Render();view->renderWindow->WaitForCompletion();vtkNew<vtkWindowToImageFilter> pixels;pixels->SetInput(view->renderWindow);pixels->ReadFrontBufferOff();pixels->ShouldRerenderOff();vtkNew<vtkPNGWriter> writer;writer->SetFileName(path.string().c_str());writer->SetInputConnection(pixels->GetOutputPort());writer->Write();Require(writer->GetErrorCode()==0,"real PNG output");metrics.SampleGpu(view->renderWindow);}
};
struct OracleResult {std::uint64_t kept=0,expected=0,mismatch=0;std::string sha;};
OracleResult VerifyMask(DataBytes mask,std::array<int,3> dims,const std::filesystem::path& artifact){
    OracleResult result;std::ofstream out(artifact,std::ios::binary);Require(bool(out),"mask artifact open");
    const std::int64_t side=std::min({dims[0],dims[1],dims[2]});
    const std::int64_t sphereRadius=side*5/8,cylinderRadius=side/8,cylinderHeight=side/4;
    std::size_t index=0;
    // Independent exact integer lattice oracle. None of these boundary
    // equalities is attainable on the chosen even-sized, half-index lattice.
    // It does not call CropGeometry, the ROI interpreter or the production mask.
    for(int z=0;z<dims[2];++z)for(int y=0;y<dims[1];++y)for(int x=0;x<dims[0];++x,++index){
        const std::array<std::int64_t,3> u{2LL*x-(dims[0]-1),2LL*y-(dims[1]-1),2LL*z-(dims[2]-1)};
        bool box=true;for(int axis=0;axis<3;++axis)box=box&&4*std::abs(u[axis])*1000000LL<=3LL*(dims[axis]-1)*1000001LL;
        const auto sx=u[0]-dims[0]/8,cyx=u[0]-dims[0]/4;
        const bool sphere=sx*sx+u[1]*u[1]+u[2]*u[2]<=sphereRadius*sphereRadius;
        const bool cylinder=cyx*cyx+u[1]*u[1]<=cylinderRadius*cylinderRadius&&std::abs(u[2])<=cylinderHeight;
        const bool expected=box&&u[0]>0&&sphere&&!cylinder,actual=(*mask)[index]!=0;
        result.expected+=expected;result.kept+=actual;result.mismatch+=actual!=expected;
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    struct Cleanup {BCRYPT_ALG_HANDLE& algorithm;BCRYPT_HASH_HANDLE& hash;
        ~Cleanup(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}} cleanup{algorithm,hash};
    Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider");
    DWORD objectSize=0,read=0;const auto property=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&read,0);
    Require(property>=0,"SHA256 object size");
    std::vector<unsigned char> object(objectSize);std::array<unsigned char,32> digest{};
    Require(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)>=0,"SHA256 create");
    bool valid=true;
#endif
    for(std::size_t offset=0;offset<mask->size();){const auto count=std::min<std::size_t>(8*1024*1024,mask->size()-offset);out.write(reinterpret_cast<const char*>(mask->data()+offset),static_cast<std::streamsize>(count));
#ifdef _WIN32
        valid=valid&&BCryptHashData(hash,const_cast<PUCHAR>(mask->data()+offset),static_cast<ULONG>(count),0)>=0;
#endif
        offset+=count;
    }
#ifdef _WIN32
    valid=valid&&BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;Require(valid,"SHA256 hash");
    std::ostringstream hex;hex<<std::hex<<std::setfill('0');for(auto byte:digest)hex<<std::setw(2)<<unsigned(byte);result.sha=hex.str();
#endif
    out.close();Require(bool(out),"mask artifact write");return result;
}
}

int GetCropRealFailCount()
{
    const auto input=Env("MVVCVTK_CROP_REAL_RAW"),size=Env("MVVCVTK_CROP_REAL_DIMS"),directory=Env("MVVCVTK_CROP_REAL_OUT");
    if(input.empty()||size.empty()||directory.empty()){std::cerr<<"NOT RUN: crop-real requires MVVCVTK_CROP_REAL_RAW/DIMS/OUT.\n";return 2;}
    std::ostringstream report;report<<std::setprecision(17);bool passed=false;std::string failure;
    const auto output=std::filesystem::u8path(directory);std::filesystem::create_directories(output);
    report<<"{\n  \"sha\": "<<Quote(Env("MVVCVTK_CROP_AUDIT_SHA"))<<",\n  \"input\": "<<Quote(input)<<",\n  \"dimensions\": "<<Quote(size)<<",\n";
    try {
        const auto dims=Dims(size);const auto count=std::uint64_t(dims[0])*dims[1]*dims[2];
        Require(std::filesystem::file_size(std::filesystem::u8path(input))==count*sizeof(float),"real float32 input size mismatch");
        const auto budgetText=Env("MVVCVTK_CROP_REAL_RAM_MIB");const auto mib=budgetText.empty()?65536ULL:std::stoull(budgetText);
        Require(mib>0&&mib<=131072,"real RAM MiB range");const auto budget=static_cast<std::size_t>(mib*1024*1024);
        Fixture fixture;const auto start=Clock::now();const auto loaded=std::make_shared<Completion<HostResult>>();
        HostLoadRequest load;load.filePath=input;load.geometry.dimensions=dims;load.geometry.spacing={0.1537f,0.1537f,0.1537f};
        const auto originText=Env("MVVCVTK_CROP_REAL_ORIGIN");
        if(!originText.empty()){std::istringstream coordinates(originText);char a=0,b=0;Require(bool(coordinates>>load.geometry.origin[0]>>a>>load.geometry.origin[1]>>b>>load.geometry.origin[2])&&a==','&&b==','&&coordinates.eof(),"real origin must be X,Y,Z");}
        load.metadata.identity.datasetId="crop-real-"+size;load.metadata.source.kind=ImageSourceKind::RawFile;load.metadata.source.uri=input;
        const auto inputOrigin=load.geometry.origin;
        Require(fixture.session.SendRequestResult(std::move(load),[loaded](HostResult result){loaded->Set(std::move(result));}),"real load admission");
        Require(fixture.Wait([&]{return loaded->Ready();})&&loaded->Once()&&loaded->value.isSucceeded,"real load completion");report<<"  \"load_ms\": "<<Millis(start)<<",\n";
        const auto rootView=fixture.probe->data->GetPrimaryImage();Require(rootView&&rootView->data,"real Root read");
        const auto root=std::dynamic_pointer_cast<const ImageGrid3DPayload>(rootView->data->payload);
        Require(root&&root->GetValueType()==ImageValueType::Float32&&root->GetComponentCount()==1&&!root->GetValidityMask(),"real Root scalar format");
        const auto geometry=root->GetGeometry();Require(geometry.dimensions==dims,"real Root dimensions");
        Require(geometry.coordinateFrame=="RAS","Root coordinate frame");
        for(int axis=0;axis<3;++axis)Require(geometry.spacing[axis]==double(0.1537f),"declared RAW spacing changed");
        Require(geometry.spacing[0]==geometry.spacing[1]&&geometry.spacing[1]==geometry.spacing[2],"oracle requires isotropic lattice");
        for(int row=0;row<3;++row){int units=0;for(int col=0;col<3;++col)units+=geometry.direction[row*3+col]!=0;Require(units==1,"oracle direction row");}
        for(int col=0;col<3;++col){int units=0;for(int row=0;row<3;++row){const auto value=geometry.direction[row*3+col];Require(value==0||value==1||value==-1,"oracle requires axis-permutation direction");units+=value!=0;}Require(units==1,"oracle direction column");}
        // Independently map 64 canonical RAS samples back to the declared LPS
        // RAW lattice and compare the original float32 bits. This validates
        // import geometry/axis normalization as well as Root scalar identity.
        std::ifstream raw(std::filesystem::u8path(input),std::ios::binary);Require(bool(raw),"RAW sample reader");
        for(std::uint64_t sample=0;sample<64;++sample){const auto linear=(sample*2654435761ULL)%count;
            const std::array<std::int64_t,3> ijk{std::int64_t(linear%dims[0]),std::int64_t((linear/dims[0])%dims[1]),std::int64_t(linear/(std::uint64_t(dims[0])*dims[1]))};
            std::array<std::int64_t,3> original{};
            for(int row=0;row<3;++row){double point=geometry.origin[row];for(int col=0;col<3;++col)point+=geometry.direction[row*3+col]*geometry.spacing[col]*(geometry.extent[col*2]+ijk[col]);
                const double index=((row<2?-point:point)-inputOrigin[row])/double(0.1537f);original[row]=std::llround(index);
                Require(std::abs(index-original[row])<1e-7&&original[row]>=0&&original[row]<dims[row],"RAW/RAS sample geometry mismatch");}
            const auto rawIndex=(std::uint64_t(original[2])*dims[1]+original[1])*dims[0]+original[0];std::array<char,4> bytes{};
            raw.seekg(static_cast<std::streamoff>(rawIndex*4));raw.read(bytes.data(),4);Require(bool(raw)&&std::memcmp(bytes.data(),root->GetValues()->data()+linear*4,4)==0,"imported scalar bits changed");
        }
        report<<"  \"source_scalar_samples_verified\": 64,\n";
        CropVectorDouble3Array center=geometry.origin;for(int row=0;row<3;++row)for(int col=0;col<3;++col)center[row]+=geometry.direction[row*3+col]*geometry.spacing[col]*(geometry.extent[col*2]+0.5*(dims[col]-1));
        report<<"  \"source_revision\": "<<Quote(Ref(rootView->data->self))<<",\n  \"spacing\": ["<<geometry.spacing[0]<<','<<geometry.spacing[1]<<','<<geometry.spacing[2]<<"],\n  \"origin\": ["<<geometry.origin[0]<<','<<geometry.origin[1]<<','<<geometry.origin[2]<<"],\n";
        const auto openedAt=Clock::now();CropDocumentRequest create;create.action=CropDocumentAction::CreateDocument;create.requestId=CropHostFeature::CreateRequestId();create.target=fixture.Target();create.sourceRevision=rootView->data->self;
        const auto opened=fixture.crop->SendRequest(create);Require(bool(opened),"real document admission");
        Require(fixture.Wait([&]{const auto out=fixture.crop->GetDocumentOutcome(opened.documentId,create.requestId);return out&&out->status!=CropEditStatus::Queued;}),"real document timeout");
        Require(fixture.crop->GetDocumentOutcome(opened.documentId,create.requestId)->status==CropEditStatus::Succeeded,"real document prepare");
        report<<"  \"document_open_ms\": "<<Millis(openedAt)<<",\n";
        fixture.Save(output/"root.png");
        const auto document=opened.documentId,rootNode=opened.rootNodeId;std::vector<CropNodeId> nodes;
        const auto append=[&](CropNodeId parent,CropOpItem operation){CropEditRequest request;request.documentId=document;request.nodeId=parent;request.requestId=CropHostFeature::CreateRequestId();request.expectedRevision=fixture.crop->GetHistory(document,0,1).stateRevision;request.kind=CropEditKind::Append;request.operation=std::move(operation);const auto accepted=fixture.crop->SendRequest(request);Require(bool(accepted),"real edit admission");Require(fixture.Wait([&]{const auto out=fixture.crop->GetOutcome(document,request.requestId);return out&&out->status!=CropEditStatus::Queued;}),"real edit timeout");const auto done=fixture.crop->GetOutcome(document,request.requestId);if(done->status!=CropEditStatus::Succeeded)std::cerr<<"real edit failure="<<static_cast<int>(done->failureReason)<<'\n';Require(done->status==CropEditStatus::Succeeded,"real edit preparation");return accepted.nodeId;};
        CropOpItem box;box.geometryType=CropShape::Box;box.boxToInputModelMatrix={};box.boxToInputModelMatrix[15]=1;
        for(int row=0;row<3;++row){box.boxToInputModelMatrix[row*4+3]=center[row];for(int col=0;col<3;++col)box.boxToInputModelMatrix[row*4+col]=geometry.direction[row*3+col]*geometry.spacing[col]*(dims[col]-1)*3.0/8.0;}
        nodes.push_back(append(rootNode,box));
        CropOpItem plane;plane.geometryType=CropShape::Plane;plane.planeCenterInInputModel=center;for(int row=0;row<3;++row)plane.planeNormalInInputModel[row]=geometry.direction[row*3];
        nodes.push_back(append(nodes.back(),plane));const auto side=std::min({dims[0],dims[1],dims[2]});
        CropOpItem sphere;sphere.geometryType=CropShape::Sphere;sphere.centerInInputModel=center;sphere.radius=geometry.spacing[0]*side*5.0/16.0;
        for(int row=0;row<3;++row)sphere.centerInInputModel[row]+=geometry.direction[row*3]*geometry.spacing[0]*dims[0]/16.0;
        nodes.push_back(append(nodes.back(),sphere));
        CropOpItem cylinder;cylinder.geometryType=CropShape::Cylinder;cylinder.removalMode=CropRemovalMode::RemoveInside;cylinder.centerInInputModel=center;cylinder.radius=geometry.spacing[0]*side/16.0;cylinder.height=geometry.spacing[0]*side/4.0;
        for(int row=0;row<3;++row){cylinder.centerInInputModel[row]+=geometry.direction[row*3]*geometry.spacing[0]*dims[0]/8.0;cylinder.axisInInputModel[row]=geometry.direction[row*3+2];}
        nodes.push_back(append(nodes.back(),cylinder));
        auto alternate=plane;for(auto& value:alternate.planeNormalInInputModel)value=-value;const auto alternateNode=append(rootNode,alternate);
        const auto buildAt=Clock::now();CropBuildRequest build;build.documentId=document;build.nodeId=nodes.back();build.requestId=CropHostFeature::CreateRequestId();build.expectedRevision=fixture.crop->GetHistory(document,0,1).stateRevision;build.options.availableRamBytes=budget;const auto completed=std::make_shared<Completion<CropBuildResult>>();
        const auto accepted=fixture.crop->SendRequest(build,[completed](auto result){completed->Set(std::move(result));});Require(bool(accepted),"real build admission");
        Require(fixture.Wait([&]{return completed->Ready();})&&completed->Once(),"real build timeout/owner callback");const auto built=completed->value;if(!built.isSucceeded)std::cerr<<"real build failure="<<static_cast<int>(built.failureReason)<<" "<<built.message<<'\n';Require(built.isSucceeded&&built.nodeCount==4,"real build completion");
        report<<"  \"build_ms\": "<<Millis(buildAt)<<",\n  \"mask_bytes\": "<<count<<",\n  \"output_revision\": "<<Quote(Ref(built.outputRevision))<<",\n";
        Require(fixture.crop->GetHistory().appliedHead==alternateNode,"building C changed preview D");
        Require(fixture.crop->SendRequest(build).isReplay,"real build replay identity");
        if(count<=256ULL*256*256) {
            auto outside=plane;for(int row=0;row<3;++row)outside.planeCenterInInputModel[row]+=geometry.direction[row*3]*geometry.spacing[0]*dims[0];
            const auto emptyNode=append(rootNode,outside);auto empty=build;empty.nodeId=emptyNode;empty.requestId=CropHostFeature::CreateRequestId();empty.expectedRevision=fixture.crop->GetHistory().stateRevision;
            const auto emptyDone=std::make_shared<Completion<CropBuildResult>>();Require(bool(fixture.crop->SendRequest(empty,[emptyDone](auto result){emptyDone->Set(std::move(result));})),"real empty admission");
            Require(fixture.Wait([&]{return emptyDone->Ready();})&&emptyDone->Once()&&emptyDone->value.failureReason==CropFailure::EmptyResult,"real EmptyResult contract");
            Require(fixture.probe->data->GetDataLifetime(built.scopeId).status==DataLifetimeStatus::Published&&fixture.crop->GetState().outputRevision==built.outputRevision,"empty result replaced the original");
            CropEditRequest preview;preview.documentId=document;preview.nodeId=alternateNode;preview.requestId=CropHostFeature::CreateRequestId();preview.expectedRevision=fixture.crop->GetHistory().stateRevision;
            Require(bool(fixture.crop->SendRequest(preview)),"real alternate restoration admission");Require(fixture.Wait([&]{const auto out=fixture.crop->GetOutcome(document,preview.requestId);return out&&out->status==CropEditStatus::Succeeded;}),"real alternate restoration");
            report<<"  \"real_empty_result_preserves_old\": true,\n";
        }

        const auto graph=fixture.probe->data->GetDataGraph();DataBytes heldMask;
        {
            const auto result=fixture.probe->data->GetData(graph,built.outputRevision);const auto payload=result?std::dynamic_pointer_cast<const ImageGrid3DPayload>(result->payload):nullptr;
            Require(payload&&payload->GetValues()==root->GetValues()&&payload->GetValidityMask()&&payload->GetValidityMask()->size()==count,"fixed Root scalars and full-resolution mask");
            const auto& g=payload->GetGeometry();Require(g.extent==geometry.extent&&g.dimensions==geometry.dimensions&&g.spacing==geometry.spacing&&g.origin==geometry.origin&&g.direction==geometry.direction,"result geometry changed");
            heldMask=payload->GetValidityMask();
            const auto verifyAt=Clock::now();auto worker=std::async(std::launch::async,VerifyMask,heldMask,dims,output/"crop.mask");
            Require(fixture.Wait([&]{return worker.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;}),"oracle timeout");const auto oracle=worker.get();
            report<<"  \"oracle_ms\": "<<Millis(verifyAt)<<",\n  \"kept\": "<<oracle.kept<<",\n  \"expected_kept\": "<<oracle.expected<<",\n  \"mismatch\": "<<oracle.mismatch<<",\n  \"mask_sha256\": "<<Quote(oracle.sha)<<",\n";
            Require(oracle.kept>0&&oracle.kept<count&&oracle.mismatch==0,"integer lattice oracle mismatch");
            const auto recipe=fixture.probe->data->GetData(graph,built.recipeRevision);const auto roi=recipe?std::dynamic_pointer_cast<const RoiGeometryPayload>(recipe->payload):nullptr;Require(roi&&roi->GetPrimitives().size()==4,"formal four-shape recipe");
        }
        fixture.Save(output/"alternate-preview.png");
        std::vector<CropVectorDouble3Array> samples{center,center};for(int row=0;row<3;++row){samples[0][row]-=geometry.direction[row*3]*geometry.spacing[0]*dims[0]/4.0;samples[1][row]+=geometry.direction[row*3]*geometry.spacing[0]*dims[0]/4.0;}
        Require(fixture.Wait([&]{return fixture.crop->GetPreviewPrecision(document,"real-volume",samples).renderedHead==alternateNode;}),"presented alternate preview");
        const auto precision=fixture.crop->GetPreviewPrecision(document,"real-volume",samples);Require(precision.failureReason==CropFailure::None&&precision.samples==std::vector<CropPointClassification>{CropPointClassification::Kept,CropPointClassification::Removed},"preview did not recover the alternate Root region");
        report<<"  \"coordinate_error\": ["<<precision.coordinates.inputError[0]<<','<<precision.coordinates.inputError[1]<<','<<precision.coordinates.inputError[2]<<"],\n";
        const auto selectAt=Clock::now();HostDataSelectRequest select;select.dataRevision=built.outputRevision;select.expectedBindingRevision=fixture.session.GetImageDescriptor()->bindingRevision;Require(fixture.session.SendRequest(std::move(select)),"materialized result selection");
        Require(fixture.Wait([&]{for(const auto& id:{"real-volume","real-slice"}){HostViewTarget target;target.viewId=id;const auto scene=fixture.session.GetSceneViewState(target);if(!scene||!scene->presentation||scene->presentation->dataRevision!=built.outputRevision)return false;}return true;}),"materialized result views");fixture.Save(output/"materialized.png");
        report<<"  \"select_result_ms\": "<<Millis(selectAt)<<",\n";
        const auto returnAt=Clock::now();CropDocumentRequest returning;returning.documentId=document;returning.requestId=CropHostFeature::CreateRequestId();returning.expectedRevision=fixture.crop->GetHistory(document,0,1).stateRevision;const auto returned=std::make_shared<Completion<CropDocumentOutcome>>();
        Require(bool(fixture.crop->SendRequest(returning,[returned](auto result){returned->Set(std::move(result));})),"real Return admission");
        Require(fixture.Wait([&]{return fixture.probe->data->GetDataLifetime(built.scopeId).status==DataLifetimeStatus::Releasing;}),"held mask did not enter Releasing");Require(!returned->Ready()&&!fixture.crop->GetHistory().results.empty(),"Return completed while a real full mask was held");
        heldMask.reset();Require(fixture.Wait([&]{return returned->Ready();}),"real Return release timeout");Require(returned->Once()&&returned->value.status==CropEditStatus::Succeeded&&fixture.probe->data->GetDataLifetime(built.scopeId).status==DataLifetimeStatus::Released&&!fixture.probe->data->GetData(graph,built.outputRevision),"real result release/old graph revocation");
        report<<"  \"return_release_ms\": "<<Millis(returnAt)<<",\n";
        const auto cancelAt=Clock::now();build.requestId=CropHostFeature::CreateRequestId();build.expectedRevision=fixture.crop->GetHistory().stateRevision;const auto cancelled=std::make_shared<Completion<CropBuildResult>>();
        Require(bool(fixture.crop->SendRequest(build,[cancelled](auto result){cancelled->Set(std::move(result));})),"real cancelled build admission");
        returning.requestId=CropHostFeature::CreateRequestId();returning.expectedRevision=fixture.crop->GetHistory().stateRevision;
        Require(bool(fixture.crop->SendRequest(returning)),"real cancel Return admission");
        Require(fixture.Wait([&]{const auto done=fixture.crop->GetDocumentOutcome(document,returning.requestId);return done&&done->status!=CropEditStatus::Queued&&cancelled->Ready();}),"real cancel timeout");Require(cancelled->Once()&&cancelled->value.failureReason==CropFailure::Cancelled&&fixture.crop->GetHistory().results.empty(),"real cancellation published a result");
        report<<"  \"cancel_ms\": "<<Millis(cancelAt)<<",\n";
        const auto stopAt=Clock::now();Require(fixture.Stop(),"real Session Stop");report<<"  \"stop_ms\": "<<Millis(stopAt)<<",\n";
        auto& metrics=fixture.metrics;std::sort(metrics.ticks.begin(),metrics.ticks.end());report<<"  \"owner_tick_p95_ms\": "<<(metrics.ticks.empty()?0:metrics.ticks[metrics.ticks.size()*95/100])<<",\n  \"owner_tick_max_ms\": "<<(metrics.ticks.empty()?0:metrics.ticks.back())<<",\n  \"peak_working_bytes\": "<<metrics.peakWorking<<",\n  \"peak_commit_bytes\": "<<metrics.peakPrivate<<",\n  \"gpu_free_first_bytes\": "<<metrics.gpuFreeFirst<<",\n  \"gpu_free_min_bytes\": "<<metrics.gpuFreeMin<<",\n";
        report<<"  \"gpu_measurement\": \"device-wide free-memory samples after owner ticks; includes other processes\",\n";
        report<<"  \"callbacks_on_owner_exactly_once\": true,\n";
        passed=true;
    }catch(const std::exception& error){failure=error.what();std::cerr<<"REAL CROP FAIL: "<<failure<<'\n';}
    report<<"  \"passed\": "<<(passed?"true":"false")<<",\n  \"failure\": "<<Quote(failure)<<"\n}\n";std::ofstream json(output/"report.json");json<<report.str();json.close();
    return GetCaseResult(passed&&bool(json),"Real CT public Crop pipeline, integer oracle, full mask, cancellation and release")?0:1;
}
