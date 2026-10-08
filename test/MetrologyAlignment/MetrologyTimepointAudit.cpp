// 真实 ROI 的公共 Feature 组合实验；不把跨采集实测件当作独立名义真值。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include "../TestDataPort.h"
#include "Host/SurfaceDeterminationHostFeature.h"
#include "Host/MetrologyAlignmentHostFeature.h"
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPolyData.h>
#include <vtkXMLPolyDataWriter.h>
#include "Render/Contracts/OverlayService.h"
#include <vtkRenderer.h>
#include <vtkCamera.h>
#include <vtkRenderWindow.h>
#include <vtkPointData.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <vtkPNGWriter.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cmath>
#include <thread>

namespace {
void Require(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
std::string Json(const std::string &value) {
    std::string text="\""; const char *hex="0123456789abcdef";
    for (const unsigned char c:value) {
        if (c=='"' || c=='\\') { text+='\\'; text+=static_cast<char>(c); }
        else if (c<32) { text+="\\u00"; text+=hex[c>>4]; text+=hex[c&15]; }
        else text+=static_cast<char>(c);
    }
    return text+'"';
}
std::string Id(const DataRevisionRef &r) {
    std::ostringstream out; out<<std::hex<<std::setfill('0');
    for (auto b:r.entityId.bytes) out<<std::setw(2)<<unsigned(b);
    out<<':'<<std::dec<<r.generation; return out.str();
}
void WriteSupport(std::ostream &out,const AlignmentGeometry &geometry) {
    Require(geometry.support.has_value(),"New measured geometry has no sample support.");
    const auto &s=*geometry.support;
    out<<",\"support\":{\"principalRmsSpreads\":[";
    for (std::size_t i=0;i<3;++i) { if (i) out<<','; out<<s.principalRmsSpreads[i]; } out<<']';
    if (s.axialSpan) out<<",\"axialSpan\":"<<*s.axialSpan;
    if (s.angularSpanDegrees) out<<",\"angularSpanDegrees\":"<<*s.angularSpanDegrees;
    if (s.radialDirectionMeanLength) out<<",\"radialDirectionMeanLength\":"<<*s.radialDirectionMeanLength;
    out<<'}';
}
struct Memory final { std::size_t working=0,privateBytes=0,peak=0,peakCommit=0; };
Memory ReadMemory() {
    PROCESS_MEMORY_COUNTERS_EX m{}; m.cb=sizeof(m);
    Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),sizeof(m)),"Memory counters unavailable.");
    return {m.WorkingSetSize,m.PrivateUsage,m.PeakWorkingSetSize,m.PeakPagefileUsage};
}
class Hash final {
  public:
    Hash() {
        Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA provider failed.");
        if (BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0) { BCryptCloseAlgorithmProvider(algorithm,0); algorithm=nullptr; throw std::runtime_error("SHA create failed."); }
    }
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    void Add(unsigned char *data,ULONG count) { Require(BCryptHashData(hash,data,count,0)>=0,"SHA update failed."); }
    std::string Finish() {
        std::array<unsigned char,32> bytes{}; Require(BCryptFinishHash(hash,bytes.data(),32,0)>=0,"SHA finish failed.");
        std::ostringstream out; out<<std::hex<<std::setfill('0'); for (auto b:bytes) out<<std::setw(2)<<unsigned(b); return out.str();
    }
  private:
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
};
class Views final : public FeatureViewDirectory {
    std::vector<HostFeatureView> GetViews(const HostViewTargets &) const override { return {}; }
    std::shared_ptr<FeatureViewService> GetFeaturePort(const std::string &) const override { return {}; }
    std::shared_ptr<OverlayService> GetOverlayPort(const std::string &) const override { return {}; }
    std::optional<HostInputView> GetInputView(const HostViewTarget &) const override { return {}; }
};
class InspectionOverlayPort final:public OverlayService {
public:
    vtkSmartPointer<vtkRenderer> renderer=vtkSmartPointer<vtkRenderer>::New();
    bool AttachOverlay(std::shared_ptr<FeatureOverlay> overlay) override {overlay->SetOverlayState({});overlay->AttachRenderer(renderer);overlays.push_back(overlay);return true;}
    void RemoveOverlay(std::shared_ptr<FeatureOverlay> overlay) noexcept override {overlay->DetachRenderer(renderer);overlays.erase(std::remove(overlays.begin(),overlays.end(),overlay),overlays.end());}
    void ClearOverlays() noexcept override {while(!overlays.empty())RemoveOverlay(overlays.back());}
    std::vector<std::shared_ptr<FeatureOverlay>> overlays;
};
class InspectionViews final:public FeatureViewDirectory {
public:
    std::shared_ptr<InspectionOverlayPort> port=std::make_shared<InspectionOverlayPort>();
    vtkSmartPointer<vtkRenderWindow> window=vtkSmartPointer<vtkRenderWindow>::New();
    InspectionViews(){window->SetOffScreenRendering(1);window->SetSize(640,480);window->SetMultiSamples(0);window->AddRenderer(port->renderer);port->renderer->SetBackground(.025,.025,.025);}
    std::vector<HostFeatureView> GetViews(const HostViewTargets &) const override{return {{"inspection-audit",HostRenderViewRole::Primary3D}};}
    std::shared_ptr<FeatureViewService> GetFeaturePort(const std::string &) const override{return {};}
    std::shared_ptr<OverlayService> GetOverlayPort(const std::string &) const override{return port;}
    std::optional<HostInputView> GetInputView(const HostViewTarget &) const override{return {};}
};
class Control final : public FeatureHostControl {
    bool SetActiveViews(const std::vector<std::string> &) override { return true; }
    bool SetViewStatus(const std::vector<std::string> &,const std::string &) override { return true; }
    bool SendSceneDelta(FeatureSceneDelta) override { return true; }
    bool AttachInput(HostInputBinding) override { return true; }
    bool DetachInput(std::string_view) override { return true; }
    bool SendOwnerComplete(std::function<void()> complete) override {
        if (!complete || std::this_thread::get_id()!=owner) return false;
        complete(); return true;
    }
    bool SendWorkAvailable() override { return true; }
    const std::thread::id owner=std::this_thread::get_id();
};
struct Scan final {
    std::string id,parentSha,roiSha;
    std::filesystem::path roiPath;
    double spacing=0;
    std::array<int,6> nativeRoi{};
};
struct Manifest final {
    std::size_t inputLimit=0,workingLimit=0,deadlineMs=0;
    std::vector<Scan> scans;
};
Manifest ReadManifest(const std::filesystem::path &path) {
    Require(std::filesystem::file_size(path)<1024*1024,"Manifest too large.");
    std::ifstream in(path); in.imbue(std::locale::classic()); std::string key; unsigned version=0; Manifest m;
    Require(bool(in>>key>>version) && key=="ct-timepoints" && version==1,"Invalid timepoint manifest.");
    Require(bool(in>>key>>m.inputLimit>>m.workingLimit>>m.deadlineMs) && key=="limits" && m.inputLimit && m.workingLimit &&
        m.deadlineMs>0 && m.deadlineMs<=3600000,"Invalid audit limits.");
    while (in>>key) {
        Scan s; std::string file;
        Require(key=="scan" && bool(in>>std::quoted(s.id)>>std::quoted(file)>>std::quoted(s.parentSha)>>std::quoted(s.roiSha)>>s.spacing),"Invalid scan entry.");
        Require(s.id.size()>0 && s.id.size()<64 && s.parentSha.size()==64 && s.roiSha.size()==64 && std::isfinite(s.spacing) && s.spacing>0,"Invalid scan identity or spacing.");
        Require(std::all_of(s.id.begin(),s.id.end(),[](char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_'; }),"Scan id must be a filename-safe label.");
        Require(std::none_of(m.scans.begin(),m.scans.end(),[&](const auto &scan) { return scan.id==s.id; }),"Duplicate scan id.");
        for (const auto &digest:{s.parentSha,s.roiSha}) Require(std::all_of(digest.begin(),digest.end(),[](char c) { return (c>='0' && c<='9') || (c>='a' && c<='f'); }),"Invalid SHA256 text.");
        for (auto &v:s.nativeRoi) Require(bool(in>>v),"Missing native ROI bounds.");
        for (std::size_t a=0;a<3;++a) Require(s.nativeRoi[2*a]>=0 && s.nativeRoi[2*a+1]<1536 &&
            s.nativeRoi[2*a+1]>s.nativeRoi[2*a],"ROI outside parent RAW.");
        s.roiPath=std::filesystem::u8path(file); if (s.roiPath.is_relative()) s.roiPath=path.parent_path()/s.roiPath;
        Require(m.scans.size()<8,"Too many scans in one bounded experiment."); m.scans.push_back(std::move(s));
    }
    Require(in.eof() && !m.scans.empty(),"Incomplete manifest."); return m;
}
DataRevisionRef Load(TestDataPort &data,const Scan &scan,std::size_t inputLimit) {
    const std::uint16_t endian=1; Require(*reinterpret_cast<const unsigned char *>(&endian)==1,"Little-endian host required.");
    std::array<int,3> dims{}; std::size_t count=1;
    for (std::size_t a=0;a<3;++a) { dims[a]=scan.nativeRoi[2*a+1]-scan.nativeRoi[2*a]+1; count*=static_cast<std::size_t>(dims[a]); }
    Require(count<=inputLimit/4 && std::filesystem::file_size(scan.roiPath)==count*4,"ROI input size or budget mismatch.");
    auto image=vtkSmartPointer<vtkImageData>::New(); image->SetDimensions(dims.data());
    image->SetSpacing(scan.spacing,scan.spacing,scan.spacing);
    image->SetOrigin(-scan.nativeRoi[0]*scan.spacing,-scan.nativeRoi[2]*scan.spacing,scan.nativeRoi[4]*scan.spacing);
    auto direction=vtkSmartPointer<vtkMatrix3x3>::New(); direction->Identity(); direction->SetElement(0,0,-1); direction->SetElement(1,1,-1);
    image->SetDirectionMatrix(direction); image->AllocateScalars(VTK_FLOAT,1);
    auto *buffer=static_cast<unsigned char *>(image->GetScalarPointer()); Require(buffer,"ROI allocation failed.");
    std::ifstream input(scan.roiPath,std::ios::binary); Hash hash;
    for (std::size_t offset=0;offset<count*4;) {
        const auto amount=std::min<std::size_t>(1024*1024,count*4-offset);
        Require(bool(input.read(reinterpret_cast<char *>(buffer+offset),static_cast<std::streamsize>(amount))),"ROI read failed.");
        hash.Add(buffer+offset,static_cast<ULONG>(amount)); offset+=amount;
    }
    Require(hash.Finish()==scan.roiSha,"ROI SHA256 mismatch.");
    VtkDataBridge bridge; const auto payload=bridge.CreateImagePayload(image); Require(bool(payload),"ROI image adapter rejected geometry.");
    const DataRevisionRef ref{data.CreateDataEntityId(),1};
    std::ostringstream provenance; provenance<<std::setprecision(17)<<"parent-sha256="<<scan.parentSha<<";roi-sha256="<<scan.roiSha<<";spacing-mm="<<scan.spacing<<";native-inclusive-roi=";
    for (auto v:scan.nativeRoi) provenance<<v<<',';
    DataTransaction tx; tx.outputs.push_back({ref.entityId,0,DataTypes::imageGrid3D,{},payload,
        DataProvenance{"timepoint-audit","native-raw-roi","1",provenance.str()}});
    Require(data.SetDataCommit(std::move(tx)).status==DataCommitStatus::Succeeded,"ROI publication failed."); return ref;
}
template<class Feature,class Completion> void Wait(Feature &feature,const std::shared_ptr<Completion> &completion,std::size_t milliseconds) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while (!completion->done && std::chrono::steady_clock::now()<deadline) {
        Require(feature.OnHostTick(),"Owner tick failed."); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(completion->done,"Audit operation deadline exceeded.");
}
struct SurfaceCompletion final { bool done=false; SurfaceDeterminationResult value; };
struct AlignmentCompletion final { bool done=false; AlignmentResult value; };
struct Surface final {
    DataRevisionRef source,mesh,generation;
    std::shared_ptr<const SurfaceMeshPayload> payload;
    std::vector<std::uint64_t> validVertices,validTriangles;
    std::shared_ptr<const std::vector<SurfaceObjectRecord>> objects;
    std::optional<double> iso;
};
Surface Determine(TestDataPort &data,SurfaceDeterminationHostFeature &feature,const Scan &scan,const Manifest &m,std::ostream &out,const std::filesystem::path &folder) {
    std::cout<<"SURFACE_START "<<scan.id<<'\n'<<std::flush;
    Surface surface; const auto start=std::chrono::steady_clock::now(); surface.source=Load(data,scan,m.inputLimit);
    SurfaceDeterminationRequest request; request.action=SurfaceDeterminationAction::Start;
    request.start=SurfaceDeterminationStartParams{}; request.start->sourceVolume=surface.source; request.start->modelUnit="mm";
    request.start->resultScope=scan.id; request.start->componentSelection=SurfaceComponentSelection::All;
    auto completion=std::make_shared<SurfaceCompletion>();
    const auto admission=feature.SendRequest(request,[completion](SurfaceDeterminationResult r) { completion->value=std::move(r); completion->done=true; });
    Require(admission.status==SurfaceAdmissionStatus::Accepted,"Surface request was not admitted.");
    Wait(feature,completion,m.deadlineMs);
    const auto &result=completion->value;
    out<<"{\"id\":"<<Json(scan.id)<<",\"parentSha256\":"<<Json(scan.parentSha)<<",\"roiSha256\":"<<Json(scan.roiSha)
       <<",\"roiFile\":"<<Json(std::filesystem::absolute(scan.roiPath).lexically_normal().u8string())
       <<",\"spacingMm\":"<<scan.spacing<<",\"source\":"<<Json(Id(surface.source))<<",\"status\":"<<unsigned(result.status)
       <<",\"failure\":"<<unsigned(result.failureReason)<<",\"message\":"<<Json(result.message)<<",\"nativeInclusiveRoi\":[";
    for (std::size_t i=0;i<6;++i) { if (i) out<<','; out<<scan.nativeRoi[i]; }
    out<<"],\"coordinateFrame\":\"RAS\",\"nativeToRasDirection\":[-1,0,0,0,-1,0,0,0,1]";
    if (result.status==SurfaceResultStatus::Succeeded && result.isPublished) {
        const auto snapshot=feature.GetSurfaceSnapshot(result.dataRevision);
        const auto validity=feature.GetResultValidity(result.dataRevision);
        out<<",\"restoreStatus\":"<<unsigned(validity.status)<<",\"canMeasure\":"<<(validity.canMeasure ? "true" : "false")<<",\"validityMessage\":"<<Json(validity.message);
        Require(snapshot && snapshot->modelUnit=="mm" && validity.canMeasure,"Published surface is not measurable/current.");
        surface.mesh=result.meshRevision; surface.generation=result.dataRevision;
        surface.objects=snapshot->objects;
        if(snapshot->isoEstimate)surface.iso=snapshot->isoEstimate->isoValue;
        const auto node=data.GetDataGraph().view->GetData(surface.mesh);
        surface.payload=node ? std::dynamic_pointer_cast<const SurfaceMeshPayload>(node->payload) : nullptr;
        Require(bool(surface.payload),"Formal surface mesh unavailable.");
        const std::array<double,6> expectedBounds{-scan.nativeRoi[1]*scan.spacing,-scan.nativeRoi[0]*scan.spacing,
            -scan.nativeRoi[3]*scan.spacing,-scan.nativeRoi[2]*scan.spacing,scan.nativeRoi[4]*scan.spacing,scan.nativeRoi[5]*scan.spacing};
        const auto &positions=surface.payload->GetVertices();
        for (std::size_t i=0;i<positions.size();++i) Require(positions[i]>=expectedBounds[2*(i%3)]-1e-8 &&
            positions[i]<=expectedBounds[2*(i%3)+1]+1e-8,"Published point violates native-ROI physical bounds.");
        out<<",\"physicalBoundsChecked\":true,\"inputIsCroppedFromParent\":true,\"voxelCenterBoundsRAS\":[";
        for (std::size_t i=0;i<6;++i) { if (i) out<<','; out<<expectedBounds[i]; } out<<']';
        const MeshAttribute *valid=nullptr;
        for (const auto &a:surface.payload->GetPointAttributes()) if (a.name=="measurement.valid") valid=&a;
        Require(valid && valid->values.size()==surface.payload->GetVertices().size()/3,"Measurement validity attributes missing.");
        for (std::size_t i=0;i<valid->values.size();++i) if (valid->values[i]==1) surface.validVertices.push_back(i);
        const auto &tri=surface.payload->GetTriangles();
        const MeshAttribute *faceValidity=nullptr;
        for (const auto &a:surface.payload->GetCellAttributes()) if (a.name=="measurement.valid") faceValidity=&a;
        Require(faceValidity && faceValidity->componentCount==1 && faceValidity->values.size()==tri.size()/3 &&
            snapshot->triangleValidity && faceValidity->values.size()==snapshot->triangleValidity->size() &&
            std::equal(faceValidity->values.begin(),faceValidity->values.end(),snapshot->triangleValidity->begin()),
            "Published cell quality does not preserve C3 triangle validity.");
        out<<",\"cellQualityChecked\":true";
        for (std::size_t i=0;i<tri.size()/3;++i) if (valid->values[tri[3*i]]==1 && valid->values[tri[3*i+1]]==1 && valid->values[tri[3*i+2]]==1 &&
            snapshot->triangleValidity && (*snapshot->triangleValidity)[i]) surface.validTriangles.push_back(i);
        out<<",\"mesh\":"<<Json(Id(surface.mesh))<<",\"generation\":"<<Json(Id(surface.generation))
           <<",\"algorithmRevision\":"<<snapshot->algorithmRevision<<",\"parameterFingerprint\":"<<snapshot->parameterFingerprint
           <<",\"resolvedRecipe\":"<<Json(snapshot->canonicalParameters)<<",\"points\":"<<valid->values.size()
           <<",\"validPoints\":"<<surface.validVertices.size()<<",\"triangles\":"<<tri.size()/3<<",\"validTriangles\":"<<surface.validTriangles.size()
           <<",\"objects\":"<<(snapshot->objects ? snapshot->objects->size() : 0)
           <<",\"workingEstimateBytes\":"<<snapshot->execution.estimatedWorkingBytes<<",\"retainedBytes\":"<<snapshot->execution.retainedBytes;
        std::size_t closed=0,truncated=0,nonManifold=0;
        if (snapshot->objects) for (const auto &object:*snapshot->objects) { closed+=object.isClosed ? 1 : 0; truncated+=object.isTruncated ? 1 : 0; nonManifold+=object.isManifold ? 0 : 1; }
        out<<",\"closedObjects\":"<<closed<<",\"truncatedObjects\":"<<truncated<<",\"nonManifoldObjects\":"<<nonManifold;
        out<<",\"objectGeometry\":[";
        if (surface.objects) for (std::size_t i=0;i<surface.objects->size();++i) {
            if (i) out<<','; const auto &object=(*surface.objects)[i];
            out<<"{\"index\":"<<object.objectIndex<<",\"firstTriangle\":"<<object.firstTriangle<<",\"triangleCount\":"<<object.triangleCount
               <<",\"closed\":"<<(object.isClosed ? "true" : "false")<<",\"manifold\":"<<(object.isManifold ? "true" : "false")
               <<",\"truncated\":"<<(object.isTruncated ? "true" : "false")<<",\"validAreaRatio\":"<<object.validAreaRatio<<",\"bounds\":[";
            for (std::size_t j=0;j<6;++j) { if (j) out<<','; out<<object.boundsModel[j]; } out<<"]}";
        }
        out<<']';
        if (snapshot->isoEstimate) out<<",\"background\":"<<snapshot->isoEstimate->backgroundValue<<",\"material\":"<<snapshot->isoEstimate->materialValue<<",\"iso\":"<<snapshot->isoEstimate->isoValue;
        const auto view=data.GetSurfaceMesh(data.GetDataGraph(),surface.mesh);
        Require(view && view->mesh,"Formal surface render view unavailable.");
        auto writer=vtkSmartPointer<vtkXMLPolyDataWriter>::New(); writer->SetFileName((folder/(scan.id+".vtp")).u8string().c_str());
        writer->SetInputData(view->mesh); Require(writer->Write()!=0,"VTP output failed.");
    }
    const auto memory=ReadMemory();
    out<<",\"elapsedMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()
       <<",\"workingSetBytes\":"<<memory.working<<",\"privateBytes\":"<<memory.privateBytes<<",\"peakWorkingSetBytes\":"<<memory.peak<<'}'; out.flush();
    std::cout<<"SURFACE "<<scan.id<<" status="<<unsigned(result.status)<<" valid="<<surface.validVertices.size()<<'\n'<<std::flush;
    return surface;
}
AlignmentResult Solve(MetrologyAlignmentHostFeature &feature,AlignmentRecipe recipe,AlignmentInput input,
    const Manifest &m,std::vector<AlignmentMatrix> poses={alignmentIdentity}) {
    AlignmentRequest save; save.action=AlignmentAction::SaveRecipe; save.recipe=std::move(recipe); DataRevisionRef recipeRef;
    Require(feature.SendRequest(std::move(save),[&](AlignmentResult r) { recipeRef=r.recipe; }).status==AlignmentAdmissionStatus::Accepted && GetDataRevisionRefValid(recipeRef),"Alignment recipe rejected.");
    AlignmentRequest run; run.recipeRef=recipeRef; run.input=std::move(input); run.initialPoses=std::move(poses);
    auto completion=std::make_shared<AlignmentCompletion>();
    Require(feature.SendRequest(run,[completion](AlignmentResult r) { completion->value=std::move(r); completion->done=true; }).status==AlignmentAdmissionStatus::Accepted,"Alignment was not admitted.");
    Wait(feature,completion,m.deadlineMs+1000); return completion->value;
}
bool IdentityControl(MetrologyAlignmentHostFeature &feature,const Surface &surface,const Manifest &m,std::ostream &out,
                     std::vector<DataRevisionRef> &deviations) {
    if (!surface.payload || surface.validTriangles.empty()) { out<<"{\"attempted\":false}"; return false; }
    const auto started=std::chrono::steady_clock::now();
    AlignmentRecipe recipe; recipe.id="real-surface-rigid-control"; recipe.targetFrameId="control-reference"; recipe.unit=AlignmentUnit::Millimeter;
    recipe.method=AlignmentMethod::ConstrainedBestFit; recipe.datumCount=0; recipe.nominalData=surface.mesh;
    recipe.requiresMeasurementQuality=true; recipe.surfaceFit=AlignmentSurfaceFit{};
    recipe.surfaceFit->referenceTriangles=surface.validTriangles;
    AlignmentRegion region; region.pinnedMesh=surface.mesh;
    std::vector<bool> selected(surface.payload->GetVertices().size()/3,false);
    const auto &tri=surface.payload->GetTriangles();
    for (auto i:surface.validTriangles) for (std::size_t j=0;j<3;++j) selected[tri[3*i+j]]=true;
    for (std::size_t i=0;i<selected.size();++i) if (selected[i]) region.vertexIds.push_back(i);
    recipe.surfaceFit->sourceRegion=std::move(region); recipe.surfaceFit->scales={{.55,4,60},{.275,.5,60}};
    recipe.maxPairDistance=.5; recipe.maxAlignmentRms=1e-5; recipe.minPairCoverage=1;
    auto pose=alignmentIdentity; const double a=.01; pose[0]=pose[5]=std::cos(a); pose[1]=-std::sin(a); pose[4]=std::sin(a);
    pose[3]=.15; pose[7]=-.12; pose[11]=.08;
    const auto result=Solve(feature,std::move(recipe),
        AlignmentInput{surface.source,surface.mesh,surface.source,"control-source","RAS","rigid-control",AlignmentUnit::Millimeter},m,{pose});
    out<<"{\"attempted\":true,\"scope\":\"same measured mesh; perturbed initialization; identity is exact numerical truth\",\"status\":"<<unsigned(result.status)
       <<",\"qualityPassed\":"<<(result.diagnostics.isQualityPassed ? "true" : "false")<<",\"rms\":"<<result.diagnostics.rms;
    bool passed=false;
    if (const auto archive=feature.GetArchive(result.result)) {
        const auto &t=archive->sourceToTarget;
        const double translation=std::hypot(t[3],t[7],t[11]);
        const double angle=std::atan2(std::hypot(t[9]-t[6],t[2]-t[8],t[4]-t[1])/2,(t[0]+t[5]+t[10]-1)/2);
        out<<",\"translationErrorMm\":"<<translation<<",\"rotationErrorRadians\":"<<angle;
        passed=result.status==AlignmentStatus::FullyDetermined && result.diagnostics.isQualityPassed && translation<1e-6 && angle<1e-6;
    }
    if(passed) {
        AlignmentRequest evaluate;evaluate.action=AlignmentAction::EvaluateSurface;evaluate.deviation=AlignmentDeviationRequest{};
        auto &p=*evaluate.deviation;p.alignmentResult=result.result;p.sourceTriangles=surface.validTriangles;p.referenceTriangles=surface.validTriangles;
        p.requiresMeasurementQuality=true;p.reverseCoverage=false;p.maxDistance=1e-6;p.tolerance=std::array<double,2>{0,1e-6};
        auto complete=std::make_shared<AlignmentCompletion>();
        Require(feature.SendRequest(evaluate,[complete](AlignmentResult value) { complete->value=std::move(value);complete->done=true; }).status==AlignmentAdmissionStatus::Accepted,"Identity deviation rejected.");
        Wait(feature,complete,m.deadlineMs+1000);const auto field=feature.GetDeviation(complete->value.deviationResult);
        Require(field && field->isCurrent && field->forward->statistics.hasValues && field->forward->statistics.maximum<=1e-6,
            "Identity deviation exceeds its numerical control tolerance.");
        deviations.push_back(field->result);out<<",\"deviationSamples\":"<<field->forward->samples.size();
    }
    out<<",\"numericalControlPassed\":"<<(passed ? "true" : "false")<<",\"elapsedMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'}';
    std::cout<<"RIGID_CONTROL status="<<unsigned(result.status)<<" rms="<<result.diagnostics.rms<<'\n'<<std::flush; return passed;
}
AlignmentResult NativeFrame(MetrologyAlignmentHostFeature &feature,const Surface &surface,const Manifest &m) {
    Require(surface.payload && surface.validVertices.size()>=3,"Native frame needs three valid surface points.");
    const auto &v=surface.payload->GetVertices();
    const auto point=[&](std::uint64_t id) { return AlignmentPoint{v[3*id],v[3*id+1],v[3*id+2]}; };
    const auto distance=[](const AlignmentPoint &a,const AlignmentPoint &b) { double d=0; for (int j=0;j<3;++j) d+=(a[j]-b[j])*(a[j]-b[j]); return d; };
    std::array<std::uint64_t,3> anchors{surface.validVertices.front(),0,0};
    double greatest=-1;
    for (auto i:surface.validVertices) if (const auto d=distance(point(i),point(anchors[0]));d>greatest) { greatest=d; anchors[1]=i; }
    const auto a=point(anchors[0]),b=point(anchors[1]); greatest=-1;
    for (auto i:surface.validVertices) {
        const auto c=point(i); double projection=0; for (int j=0;j<3;++j) projection+=(c[j]-a[j])*(b[j]-a[j]);
        const double d=distance(c,a)-projection*projection/distance(a,b);
        if (d>greatest) { greatest=d; anchors[2]=i; }
    }
    Require(greatest>1e-8,"Native frame anchors are degenerate.");
    AlignmentRecipe recipe; recipe.id="surface-native-frame"; recipe.targetFrameId="surface-native"; recipe.unit=AlignmentUnit::Millimeter;
    recipe.method=AlignmentMethod::Rps; recipe.datumCount=0; recipe.nominalData=surface.source; recipe.requiresMeasurementQuality=true;
    for (std::size_t i=0;i<3;++i) {
        AlignmentGeometrySpec spec; spec.id="native-anchor-"+std::to_string(i); spec.kind=AlignmentGeometryKind::Point;
        spec.region={surface.mesh,{anchors[i]}, {}}; recipe.geometries.push_back(spec);
        for (std::size_t axis=0;axis<3;++axis) {
            AlignmentConstraint c; c.geometryIndex=i; c.nominalPoint=point(anchors[i]); c.targetDirection={0,0,0}; c.targetDirection[axis]=1;
            recipe.constraints.push_back(c);
        }
    }
    return Solve(feature,std::move(recipe),
        AlignmentInput{surface.source,surface.mesh,surface.source,"surface-native","RAS","surface-native",AlignmentUnit::Millimeter},m);
}
bool RingFit(MetrologyAlignmentHostFeature &feature,const Surface &surface,const Manifest &m,std::ostream &out,
             std::vector<DataRevisionRef> &reports) {
    if (!surface.payload || surface.validVertices.size()<3) { out<<"{\"attempted\":false}"; return false; }
    const auto started=std::chrono::steady_clock::now(); bool passed=false;
    const auto &v=surface.payload->GetVertices();
    const auto point=[&](std::uint64_t id) { return AlignmentPoint{v[3*id],v[3*id+1],v[3*id+2]}; };
    const auto alignment=NativeFrame(feature,surface,m);
    out<<"{\"attempted\":true,\"scope\":\"native RAW coordinate assignment, not alignment to independent nominal geometry\",\"alignmentStatus\":"<<unsigned(alignment.status);
    if (alignment.status==AlignmentStatus::FullyDetermined && alignment.diagnostics.isQualityPassed) {
        InspectionPlan plan; plan.id="ring-local-wall"; plan.alignmentResult=alignment.result; plan.requiresMeasurementQuality=true;
        InspectionGeometrySpec cylinder; cylinder.fit.id="outer-wall-patch"; cylinder.fit.kind=AlignmentGeometryKind::Cylinder;
        cylinder.fit.targetDirection={1,0,0}; cylinder.fit.maxFitRms=.0778; cylinder.fit.region.pinnedMesh=surface.mesh;
        const MeshAttribute *normals=nullptr;
        for (const auto &attribute:surface.payload->GetPointAttributes()) if (attribute.name=="measurement.normal") normals=&attribute;
        Require(normals && normals->componentCount==3,"Ring source normals missing.");
        for (auto i:surface.validVertices) if (std::abs(normals->values[3*i])<.3) cylinder.fit.region.vertexIds.push_back(i);
        out<<",\"selectedPoints\":"<<cylinder.fit.region.vertexIds.size(); plan.geometries.push_back(std::move(cylinder));
        for (const auto metric:{InspectionMetric::Diameter,InspectionMetric::FitRms}) {
            InspectionItem item; item.id=metric==InspectionMetric::Diameter ? "local-cylinder-diameter" : "local-fit-residual";
            item.measured.metric=metric; plan.items.push_back(item);
        }
        AlignmentRequest inspect; inspect.action=AlignmentAction::Inspect; inspect.inspection=std::move(plan);
        auto completion=std::make_shared<AlignmentCompletion>();
        Require(feature.SendRequest(std::move(inspect),[completion](AlignmentResult r) { completion->value=std::move(r); completion->done=true; }).status==AlignmentAdmissionStatus::Accepted,"Ring inspection not admitted.");
        Wait(feature,completion,m.deadlineMs+1000);
        out<<",\"inspectionStatus\":"<<unsigned(completion->value.status);
        if (const auto report=feature.GetInspection(completion->value.inspectionResult)) {
            reports.push_back(report->result);
            out<<",\"geometryStatus\":"<<unsigned(report->report->geometries[0].status)<<",\"geometryMessage\":"<<Json(report->report->geometries[0].message)
               <<",\"verdict\":"<<unsigned(report->report->verdict)<<",\"values\":[";
            for (std::size_t i=0;i<report->report->items.size();++i) { if (i) out<<','; const auto &row=report->report->items[i]; out<<"{\"id\":"<<Json(row.id)<<",\"value\":"; if (row.measured) out<<*row.measured; else out<<"null"; out<<'}'; }
            out<<']';
            passed=completion->value.status==AlignmentStatus::FullyDetermined && report->isCurrent &&
                report->report->geometries[0].status==AlignmentStatus::FullyDetermined && report->report->verdict==AlignmentInspectionVerdict::NotEvaluated &&
                std::all_of(report->report->items.begin(),report->report->items.end(),[](const auto &item) { return item.measured && std::isfinite(*item.measured); });
            if (passed) {
                const auto &geometry=report->report->geometries[0].fitted;
                WriteSupport(out,geometry);
                const auto &axis=geometry.sourceDirection; const auto &center=geometry.sourceCenter;
                const AlignmentPoint helper=std::abs(axis[0])<.8 ? AlignmentPoint{1,0,0} : AlignmentPoint{0,1,0};
                AlignmentPoint u{axis[1]*helper[2]-axis[2]*helper[1],axis[2]*helper[0]-axis[0]*helper[2],axis[0]*helper[1]-axis[1]*helper[0]};
                const double length=std::hypot(u[0],u[1],u[2]); for (auto &component:u) component/=length;
                const AlignmentPoint w{axis[1]*u[2]-axis[2]*u[1],axis[2]*u[0]-axis[0]*u[2],axis[0]*u[1]-axis[1]*u[0]};
                std::vector<double> angles; double low=std::numeric_limits<double>::infinity(),high=-low;
                for (auto id:report->plan->geometries[0].fit.region.vertexIds) {
                    const auto p=point(id); double axial=0,x=0,y=0;
                    for (std::size_t j=0;j<3;++j) { const double d=p[j]-center[j]; axial+=d*axis[j]; x+=d*u[j]; y+=d*w[j]; }
                    low=std::min(low,axial); high=std::max(high,axial); angles.push_back(std::atan2(y,x));
                }
                std::sort(angles.begin(),angles.end()); const double pi=std::acos(-1.0);
                double gap=2*pi-angles.back()+angles.front(); for (std::size_t i=1;i<angles.size();++i) gap=std::max(gap,angles[i]-angles[i-1]);
                Require(geometry.support->angularSpanDegrees && geometry.support->axialSpan &&
                    std::abs(*geometry.support->angularSpanDegrees-(2*pi-gap)*180/pi)<1e-7 &&
                    std::abs(*geometry.support->axialSpan-(high-low))<1e-7,"Formal cylinder support differs from original-point recomputation.");
                out<<",\"selectedPatchAngularSpanDegrees\":"<<(2*pi-gap)*180/pi<<",\"selectedPatchAxialSpanMm\":"<<high-low
                   <<",\"coverageNote\":\"local patch support only; axial span is not part height and extrapolated diameter is not a nominal acceptance result\"";
            }
        }
    }
    out<<",\"normalSelectionAbsXMaximum\":0.3,\"maxFitRmsMm\":0.0778,\"elapsedMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'}';
    std::cout<<"RING_NATIVE alignment="<<unsigned(alignment.status)<<'\n'<<std::flush; return passed;
}
bool SectionFit(MetrologyAlignmentHostFeature &feature,const Surface &surface,const Scan &scan,
                const Manifest &m,std::ostream &out,const std::filesystem::path &folder,std::vector<DataRevisionRef> &reports) {
    const SurfaceObjectRecord *object=nullptr;
    if (surface.objects) for (const auto &candidate:*surface.objects)
        if (candidate.isClosed && candidate.isManifold && !candidate.isTruncated &&
            (!object || candidate.triangleCount>object->triangleCount)) object=&candidate;
    if (!object) { out<<"{\"scanId\":"<<Json(scan.id)<<",\"attempted\":false,\"reason\":\"No closed manifold untruncated object; this input has no positive section acceptance\"}"; return true; }
    const auto started=std::chrono::steady_clock::now();
    const auto alignment=NativeFrame(feature,surface,m);
    Require(alignment.status==AlignmentStatus::FullyDetermined && alignment.diagnostics.isQualityPassed,"Section native frame assignment failed.");
    const auto archive=feature.GetArchive(alignment.result); Require(bool(archive),"Section frame unavailable.");
    const auto &vertices=surface.payload->GetVertices(); const auto &triangles=surface.payload->GetTriangles();
    const auto prefix=scan.id+"-section";
    std::ofstream points(folder/(prefix+"-points.csv")),faces(folder/(prefix+"-triangles.csv"));
    points.imbue(std::locale::classic()); faces.imbue(std::locale::classic()); points<<std::setprecision(17);
    points<<"id,x,y,z\n"; faces<<"id,a,b,c,valid\n";
    for (auto i=object->firstPoint;i<object->firstPoint+object->pointCount;++i)
        points<<i<<','<<vertices[3*i]<<','<<vertices[3*i+1]<<','<<vertices[3*i+2]<<'\n';
    for (auto i=object->firstTriangle;i<object->firstTriangle+object->triangleCount;++i)
        faces<<i<<','<<triangles[3*i]<<','<<triangles[3*i+1]<<','<<triangles[3*i+2]<<','
             <<(std::binary_search(surface.validTriangles.begin(),surface.validTriangles.end(),i) ? 1 : 0)<<'\n';
    points.close(); faces.close();
    Require(bool(points) && bool(faces),"Section source export failed.");
    InspectionPlan plan; plan.id=prefix; plan.alignmentResult=alignment.result; plan.requiresMeasurementQuality=true;
    AlignmentPoint midpoint{}; double diagonalSquared=0;
    for (std::size_t a=0;a<3;++a) { midpoint[a]=(object->boundsModel[2*a]+object->boundsModel[2*a+1])/2;
        const double width=object->boundsModel[2*a+1]-object->boundsModel[2*a]; diagonalSquared+=width*width; }
    for (std::size_t axis=0;axis<3;++axis) {
        InspectionGeometrySpec g; g.fit.id=prefix+"-axis-"+std::to_string(axis); g.fit.kind=AlignmentGeometryKind::Circle;
        // 审计记录任何可求解截线的拟合残差；此宽门限不是形状精度/合格判据。
        g.fit.maxFitRms=std::sqrt(diagonalSquared); g.section=InspectionSection{};
        auto &s=*g.section; s.pinnedMesh=surface.mesh; s.planePoint=midpoint; s.planeNormal={0,0,0}; s.planeNormal[axis]=1;
        s.sampleSpacing=scan.spacing/4; s.planeTolerance=scan.spacing*1e-6;
        s.triangles.reserve(static_cast<std::size_t>(object->triangleCount));
        for (auto i=object->firstTriangle;i<object->firstTriangle+object->triangleCount;++i) s.triangles.push_back(i);
        plan.geometries.push_back(std::move(g));
        InspectionItem item; item.id="diameter-"+std::to_string(axis); item.measured.first.geometry=axis; plan.items.push_back(item);
    }
    AlignmentRequest request; request.action=AlignmentAction::Inspect; request.inspection=std::move(plan);
    auto completion=std::make_shared<AlignmentCompletion>();
    Require(feature.SendRequest(std::move(request),[completion](AlignmentResult r) { completion->value=std::move(r); completion->done=true; }).status==AlignmentAdmissionStatus::Accepted,"Section inspection rejected.");
    Wait(feature,completion,m.deadlineMs+1000);
    const auto report=feature.GetInspection(completion->value.inspectionResult);
    if (report) reports.push_back(report->result);
    out<<"{\"scanId\":"<<Json(scan.id)<<",\"attempted\":true,\"selection\":\"largest closed manifold untruncated object by triangle count; all three midpoint planes retained\",\"objectIndex\":"<<object->objectIndex
       <<",\"status\":"<<unsigned(completion->value.status)<<",\"sourcePoints\":"<<Json(prefix+"-points.csv")<<",\"sourceTriangles\":"<<Json(prefix+"-triangles.csv")
       <<",\"scope\":\"numerical geometry comparison on real measured surface; no independent physical nominal or tolerance\",\"sourceToTarget\":[";
    for (std::size_t i=0;i<16;++i) { if (i) out<<','; out<<archive->sourceToTarget[i]; } out<<"],\"sections\":[";
    std::size_t succeeded=0;
    if (report) for (std::size_t axis=0;axis<report->report->geometries.size();++axis) {
        if (axis) out<<','; const auto &g=report->report->geometries[axis]; const auto &s=*report->plan->geometries[axis].section;
        out<<"{\"axis\":"<<axis<<",\"status\":"<<unsigned(g.status)<<",\"message\":"<<Json(g.message)<<",\"planePoint\":[";
        for (std::size_t a=0;a<3;++a) { if (a) out<<','; out<<s.planePoint[a]; }
        out<<"],\"sampleSpacing\":"<<s.sampleSpacing<<",\"planeTolerance\":"<<s.planeTolerance;
        if (g.status==AlignmentStatus::FullyDetermined && g.section) {
            WriteSupport(out,g.fitted);
            ++succeeded; const auto file=prefix+"-"+std::to_string(axis)+"-samples.csv";
            std::ofstream samples(folder/file); samples.imbue(std::locale::classic()); samples<<std::setprecision(17)<<"triangle,w0,w1,w2,planeCorrection\n";
            for (const auto &sample:g.section->samples) samples<<sample.triangle<<','<<sample.barycentric[0]<<','<<sample.barycentric[1]<<','<<sample.barycentric[2]<<','<<sample.planeCorrection<<'\n';
            samples.close();
            Require(bool(samples),"Section provenance export failed.");
            out<<",\"samples\":"<<Json(file)<<",\"sampleCount\":"<<g.fitted.sampleCount<<",\"radius\":"<<g.fitted.radius
               <<",\"fitRms\":"<<g.fitted.fitRms<<",\"perimeter\":"<<g.section->perimeter<<",\"sourceCenter\":[";
            for (std::size_t a=0;a<3;++a) { if (a) out<<','; out<<g.fitted.sourceCenter[a]; }
            out<<"],\"sourcePlanePoint\":[";
            for (std::size_t a=0;a<3;++a) { if (a) out<<','; out<<g.section->sourcePlanePoint[a]; }
            out<<"],\"sourcePlaneNormal\":[";
            for (std::size_t a=0;a<3;++a) { if (a) out<<','; out<<g.section->sourcePlaneNormal[a]; } out<<']';
            Require(report->report->items[axis].verdict==AlignmentInspectionVerdict::NotEvaluated,"Unspecified section nominal must not imply pass.");
        }
        out<<'}';
    }
    out<<"],\"successfulSections\":"<<succeeded<<",\"elapsedMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'}';
    std::cout<<"SECTIONS "<<scan.id<<" success="<<succeeded<<'\n'<<std::flush;
    return report && report->isCurrent && succeeded>0;
}
void CaptureReport(MetrologyAlignmentHostFeature &feature,TestDataPort &data,const std::vector<Surface> &surfaces,
                   const Manifest &manifest,const std::vector<DataRevisionRef> &inspections,const std::vector<DataRevisionRef> &deviations,const std::vector<DataRevisionRef> &volumes,
                   std::ostream &out,const std::filesystem::path &folder) {
    const auto started=std::chrono::steady_clock::now();
    const auto scalarOwners=[&](const DataRevisionRef &ref) {
        const auto node=data.GetDataGraph().view->GetData(ref);
        const auto *image=dynamic_cast<const ImageGrid3DPayload *>(node->payload.get());
        Require(image && image->GetValues(),"Report source image unavailable."); return image->GetValues().use_count();
    };
    std::vector<long> before; for (const auto &surface:surfaces) before.push_back(scalarOwners(surface.source));
    const auto commit=data.GetDataGraph().commitId;
    MetrologyReportRequest request; request.id="real-ct-inspection"; request.title="Frozen real ROI measurement contexts"; request.inspections=inspections;
    request.deviations=deviations;request.voxelVolumes=volumes;
    const auto captured=feature.GetReport(request);
    Require(captured.status==AlignmentStatus::FullyDetermined && captured.report && captured.report->capturedCommit==commit &&
        captured.report->wereInputsCurrentAtCapture && captured.report->inspections.size()==inspections.size() && captured.report->voxelVolumes.size()==volumes.size(),"Real report capture failed.");
    const auto &report=*captured.report;
    out<<"{\"status\":"<<unsigned(captured.status)<<",\"capturedCommit\":"<<report.capturedCommit
       <<",\"alignments\":"<<report.alignments.size()<<",\"inspections\":"<<report.inspections.size()
       <<",\"inputNodes\":"<<report.inputs.size()<<",\"estimatedMetadataBytes\":"<<report.estimatedMetadataBytes<<",\"sourceVolumes\":[";
    for (std::size_t i=0;i<surfaces.size();++i) {
        const auto source=std::find_if(report.inputs.begin(),report.inputs.end(),[&](const auto &node) { return node.revision==surfaces[i].source; });
        Require(source!=report.inputs.end() && source->image && source->provenance &&
            source->image->geometry.spacing==std::array<double,3>{manifest.scans[i].spacing,manifest.scans[i].spacing,manifest.scans[i].spacing} &&
            source->provenance->canonicalParameters.find(manifest.scans[i].parentSha)!=std::string::npos &&
            scalarOwners(surfaces[i].source)==before[i],"Report must preserve source spacing/provenance without retaining scalar storage.");
        if (i) out<<','; out<<"{\"id\":"<<Json(manifest.scans[i].id)<<",\"revision\":"<<Json(Id(source->revision))
            <<",\"spacingMm\":"<<source->image->geometry.spacing[0]<<",\"valueBytes\":"<<source->image->valueBytes<<'}';
    }
    for (const auto &inspection:report.inspections) {
        const auto original=feature.GetInspection(inspection.result);
        const auto owner=std::find_if(report.alignments.begin(),report.alignments.end(),[&](const auto &a) { return a.result==inspection.alignmentResult; });
        Require(original && original->plan==inspection.plan && original->report==inspection.report && owner!=report.alignments.end() &&
            owner->sourceToTarget==inspection.sourceToTarget,"Captured evaluation lost sharing or coordinate owner.");
    }
    out<<"],\"sourceScalarOwnershipUnchanged\":true,\"sharedInspectionStorageChecked\":true,\"elapsedMs\":"
       <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<",\"streamExports\":[";
    for(const auto format:{MetrologyReportFormat::Json,MetrologyReportFormat::Csv}) {
        if(format==MetrologyReportFormat::Csv)out<<',';
        const auto file=format==MetrologyReportFormat::Json ? "inspection-report.json" : "inspection-report.csv";
        Require(!std::filesystem::exists(folder/file),"Report export destination already exists.");
        MetrologyReportWriteOptions options;options.format=format;
        options.maxBytes=manifest.workingLimit;options.includeDeviationSamples=format==MetrologyReportFormat::Json;
        const auto begin=std::chrono::steady_clock::now();const auto written=WriteMetrologyReportFile(report,(folder/file).u8string(),options);
        Require(written.write.status==MetrologyReportWriteStatus::Succeeded && written.isCommitted && written.remainingTemporaryPath.empty() &&
            std::filesystem::file_size(folder/file)==written.write.bytesWritten,"Report file commit failed.");
        out<<"{\"file\":"<<Json(file)<<",\"bytes\":"<<written.write.bytesWritten<<",\"isCommitted\":true,\"elapsedMs\":"
           <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<'}';
    }
    out<<"]}";
}
void VoxelVolumes(MetrologyAlignmentHostFeature &feature,TestDataPort &data,const std::vector<Surface> &surfaces,const Manifest &m,std::ostream &out,std::vector<DataRevisionRef> &volumes) {
    out<<'[';
    for(std::size_t i=0;i<surfaces.size();++i) {
        if(i)out<<',';const auto &surface=surfaces[i];Require(surface.iso.has_value(),"Voxel experiment needs explicit C3 global initial ISO.");
        const auto source=data.GetDataGraph().view->GetData(surface.source);
        const auto image=std::dynamic_pointer_cast<const ImageGrid3DPayload>(source->payload);Require(bool(image),"Voxel source image unavailable.");
        const auto *storage=image->GetValues().get();const auto owners=image->GetValues().use_count();
        AlignmentRequest request;request.action=AlignmentAction::EvaluateVoxelVolume;request.voxelVolume=VoxelVolumeRequest{};
        auto &p=*request.voxelVolume;p.id=m.scans[i].id+"-voxel-range";p.source=surface.source;
        p.storedValueRange=std::array<double,2>{*surface.iso,std::numeric_limits<double>::max()};
        const auto started=std::chrono::steady_clock::now();auto completed=std::make_shared<AlignmentCompletion>();
        Require(feature.SendRequest(request,[completed](AlignmentResult r){completed->value=std::move(r);completed->done=true;}).status==AlignmentAdmissionStatus::Accepted,"Voxel volume request rejected.");
        Wait(feature,completed,m.deadlineMs+1000);const auto volume=feature.GetVoxelVolume(completed->value.voxelVolumeResult);
        Require(completed->value.status==AlignmentStatus::FullyDetermined && volume && volume->isCurrent &&
            image->GetValues().get()==storage && image->GetValues().use_count()==owners,"Voxel volume must preserve source storage and publish current statistics.");
        volumes.push_back(volume->result);
        const auto &s=volume->statistics;
        out<<"{\"id\":"<<Json(m.scans[i].id)<<",\"result\":"<<Json(Id(volume->result))<<",\"source\":"<<Json(Id(volume->source))
           <<",\"scope\":\"selected full cells within native ROI; global stored-value threshold, not C3 continuous surface volume or whole-part volume\",\"method\":\"StoredValueRange\""
           <<",\"lower\":"<<(*volume->parameters->storedValueRange)[0]<<",\"upper\":"<<(*volume->parameters->storedValueRange)[1]
           <<",\"total\":"<<s.total<<",\"selected\":"<<s.selected<<",\"unselected\":"<<s.unselected<<",\"masked\":"<<s.masked
           <<",\"noData\":"<<s.noData<<",\"nonFinite\":"<<s.nonFinite<<",\"invalidLabels\":"<<s.invalidLabels
           <<",\"cellVolumeMm3\":"<<s.cellVolumeMm3<<",\"observedVolumeMm3\":"<<s.observedVolumeMm3<<",\"volumeMm3\":";
        if(s.volumeMm3)out<<*s.volumeMm3;else out<<"null";
        out<<",\"validity\":"<<unsigned(s.validity)<<",\"touchesRegionBoundary\":"<<(s.touchesRegionBoundary ? "true" : "false")
           <<",\"sourceStorageUnchanged\":true,\"elapsedMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'}';
    }
    out<<']';
}
void InspectionScenes(MetrologyAlignmentHostFeature &feature,TestDataPort &data,InspectionViews &views,const std::vector<DataRevisionRef> &reports,
    std::ostream &out,const std::filesystem::path &folder) {
    out<<'[';
    for(std::size_t index=0;index<reports.size();++index) {
        const auto report=feature.GetInspection(reports[index]);Require(report && report->isCurrent,"Scene inspection is unavailable.");
        const auto found=std::find_if(report->report->items.begin(),report->report->items.end(),[](const auto &item){return item.measured.has_value();});
        Require(found!=report->report->items.end(),"Scene audit requires a measured item.");
        const auto original=data.GetSurfaceMesh(data.GetDataGraph(),report->sourceMesh);Require(original && original->mesh,"Scene native mesh unavailable.");
        const auto *points=original->mesh->GetPoints();const auto *triangles=original->mesh->GetPolys();const auto modified=original->mesh->GetMTime();
        const auto commit=data.GetDataGraph().commitId;
        AlignmentRequest request;request.action=AlignmentAction::SetSceneDisplay;request.sceneDisplay=AlignmentSceneDisplay{};
        auto &display=*request.sceneDisplay;display.anchorAlignment=report->alignmentResult;display.roiMarkerLimit=256;
        display.parts={{"measured",report->alignmentResult,true,false,report->result,found->id}};
        AlignmentResult shown;Require(feature.SendRequest(request,[&](AlignmentResult value){shown=std::move(value);}).status==AlignmentAdmissionStatus::Accepted && shown.isDisplayReady,"Real inspection scene did not attach.");
        const auto item=feature.GetSceneItem("measured",found->id);
        Require(item && item->isCurrent && item->isSelected && item->plan==report->plan && item->report==report->report,"Scene selection lost frozen report sharing.");
        auto *camera=views.port->renderer->GetActiveCamera();camera->ParallelProjectionOn();camera->SetPosition(1,-1,1);camera->SetFocalPoint(0,0,0);views.port->renderer->ResetCamera();
        const auto begin=std::chrono::steady_clock::now();views.window->Render();views.window->WaitForCompletion();
        const auto coldMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        const auto warmBegin=std::chrono::steady_clock::now();for(int frame=0;frame<3;++frame){views.window->Render();views.window->WaitForCompletion();}
        const auto warmMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warmBegin).count()/3;
        auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New();capture->SetInput(views.window);capture->ReadFrontBufferOff();capture->Update();
        auto *pixels=vtkUnsignedCharArray::SafeDownCast(capture->GetOutput()->GetPointData()->GetScalars());Require(pixels,"Scene pixels unavailable.");std::size_t selectedPixels=0;
        for(vtkIdType i=0;i<pixels->GetNumberOfTuples();++i){const auto *p=pixels->GetPointer(i*pixels->GetNumberOfComponents());if(p[0]>220 && p[1]>160 && p[2]<80)++selectedPixels;}
        Require(selectedPixels>0 && original->mesh->GetPoints()==points && original->mesh->GetPolys()==triangles && original->mesh->GetMTime()==modified && data.GetDataGraph().commitId==commit,
            "Scene must render selected glyphs without modifying native geometry or data graph.");
        const auto file="inspection-scene-"+std::to_string(index)+".png";auto writer=vtkSmartPointer<vtkPNGWriter>::New();writer->SetFileName((folder/file).u8string().c_str());writer->SetInputConnection(capture->GetOutputPort());writer->Write();
        Require(std::filesystem::exists(folder/file) && std::filesystem::file_size(folder/file)>0,"Scene image export failed.");
        if(index)out<<',';out<<"{\"inspection\":"<<Json(Id(report->result))<<",\"item\":"<<Json(found->id)<<",\"image\":"<<Json(file)
            <<",\"selectedPixels\":"<<selectedPixels<<",\"coldRenderMs\":"<<coldMs<<",\"warmRenderMs\":"<<warmMs
            <<",\"workingBytesEstimate\":"<<feature.GetState().sceneWorkingBytes<<",\"nativeStorageUnchanged\":true,\"sharedReport\":true,\"targetToModel\":[";
        for(std::size_t i=0;i<16;++i){if(i)out<<',';out<<item->targetToModel[i];}out<<"],\"geometries\":[";
        for(std::size_t i=0;i<item->geometries.size();++i){if(i)out<<',';out<<item->geometries[i];}out<<"],\"regions\":[";
        for(std::size_t i=0;i<item->displayedRegions.size();++i){if(i)out<<',';const auto &region=item->displayedRegions[i];out<<"{\"geometry\":"<<region.geometry<<",\"kind\":"<<unsigned(region.kind)<<",\"sourceCount\":";
            if(region.sourceCount)out<<*region.sourceCount;else out<<"null";out<<",\"drawnMarkers\":"<<region.drawnMarkers<<'}';}out<<"]}";
        display.isVisible=false;Require(feature.SendRequest(request).status==AlignmentAdmissionStatus::Accepted && views.port->overlays.empty(),"Scene overlay resources did not detach.");
    }
    out<<']';
}
void Align(MetrologyAlignmentHostFeature &feature,const std::vector<Surface> &surfaces,const Manifest &m,std::ostream &out) {
    if (surfaces.size()<2 || !surfaces[0].payload || !surfaces[1].payload || surfaces[0].validTriangles.empty() || surfaces[1].validTriangles.empty()) {
        out<<"{\"attempted\":false,\"reason\":\"Two measurable array surfaces are required\"}"; return;
    }
    const auto started=std::chrono::steady_clock::now();
    const auto &source=surfaces[0]; const auto &reference=surfaces[1];
    AlignmentRecipe recipe; recipe.id="array-acquisition-comparison"; recipe.targetFrameId="array-b"; recipe.unit=AlignmentUnit::Millimeter;
    recipe.method=AlignmentMethod::ConstrainedBestFit; recipe.datumCount=0; recipe.nominalData=reference.mesh; recipe.exactMesh=source.mesh;
    recipe.requiresMeasurementQuality=true; recipe.minSupportRatio=.5; recipe.maxLocalizationSigma=.5; recipe.maxSurfaceFitResidual=1;
    recipe.surfaceFit=AlignmentSurfaceFit{}; recipe.surfaceFit->sourceRegion=AlignmentRegion{source.mesh,source.validVertices,{}};
    recipe.surfaceFit->referenceTriangles=reference.validTriangles; recipe.surfaceFit->scales={{.55,4,40},{.275,2,40}};
    recipe.maxPairDistance=2; recipe.maxAlignmentRms=.5; recipe.minPairCoverage=.3;
    recipe.prealignment=AlignmentPrealignParams{}; auto &p=*recipe.prealignment;
    p.sampleSpacing=.5; p.normalRadius=1.2; p.featureRadius=2.4; p.maxPairDistance=2; p.minCoverage=.25;
    p.iterationLimit=10000; p.randomSeed=20261009; p.sourceTriangles=source.validTriangles; p.referenceTriangles=reference.validTriangles;
    AlignmentRequest save; save.action=AlignmentAction::SaveRecipe; save.recipe=recipe; DataRevisionRef recipeRef;
    Require(feature.SendRequest(std::move(save),[&](AlignmentResult r) { recipeRef=r.recipe; }).status==AlignmentAdmissionStatus::Accepted && GetDataRevisionRefValid(recipeRef),"Alignment recipe rejected.");
    AlignmentRequest run; run.recipeRef=recipeRef;
    run.input=AlignmentInput{source.source,source.mesh,source.source,"array-a","RAS","array-a",AlignmentUnit::Millimeter};
    auto completion=std::make_shared<AlignmentCompletion>();
    Require(feature.SendRequest(run,[completion](AlignmentResult r) { completion->value=std::move(r); completion->done=true; }).status==AlignmentAdmissionStatus::Accepted,"Alignment was not admitted.");
    Wait(feature,completion,m.deadlineMs+1000);
    const auto &r=completion->value;
    out<<"{\"attempted\":true,\"status\":"<<unsigned(r.status)<<",\"message\":"<<Json(r.message)<<",\"qualityPassed\":"<<(r.diagnostics.isQualityPassed ? "true" : "false")
       <<",\"rms\":"<<r.diagnostics.rms<<",\"maximum\":"<<r.diagnostics.maximum<<",\"remainingDof\":"<<r.diagnostics.remaining;
    if (r.diagnostics.prealignment) out<<",\"coarseCandidates\":"<<r.diagnostics.prealignment->candidates<<",\"coarseCoverage\":"<<r.diagnostics.prealignment->bestCoverage;
    if (const auto archive=feature.GetArchive(r.result)) {
        out<<",\"transform\":["; for (std::size_t i=0;i<16;++i) { if (i) out<<','; out<<archive->sourceToTarget[i]; } out<<']';
    }
    if (r.status==AlignmentStatus::FullyDetermined && r.diagnostics.isQualityPassed) {
        AlignmentRequest evaluate; evaluate.action=AlignmentAction::EvaluateSurface; evaluate.deviation=AlignmentDeviationRequest{};
        evaluate.deviation->alignmentResult=r.result; evaluate.deviation->maxDistance=2; evaluate.deviation->requiresMeasurementQuality=true;
        auto evaluated=std::make_shared<AlignmentCompletion>();
        Require(feature.SendRequest(evaluate,[evaluated](AlignmentResult v) { evaluated->value=std::move(v); evaluated->done=true; }).status==AlignmentAdmissionStatus::Accepted,"Deviation was not admitted.");
        Wait(feature,evaluated,m.deadlineMs+1000);
        out<<",\"deviationStatus\":"<<unsigned(evaluated->value.status);
        if (const auto field=feature.GetDeviation(evaluated->value.deviationResult)) out<<",\"deviationRms\":"<<field->forward->statistics.rms<<",\"evaluatedArea\":"<<field->forward->statistics.evaluatedArea
            <<",\"selectedArea\":"<<field->forward->statistics.selectedArea<<",\"verdict\":"<<unsigned(field->verdict);
    }
    out<<",\"sampleSpacingMm\":"<<p.sampleSpacing<<",\"normalRadiusMm\":"<<p.normalRadius<<",\"featureRadiusMm\":"<<p.featureRadius
       <<",\"ransacSeed\":"<<p.randomSeed<<",\"ransacIterations\":"<<p.iterationLimit<<",\"maxPairDistanceMm\":"<<recipe.maxPairDistance
       <<",\"maxAlignmentRmsMm\":"<<recipe.maxAlignmentRms<<",\"minimumPairCoverage\":"<<recipe.minPairCoverage
       <<",\"elapsedMs\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<<'}';
    std::cout<<"ALIGNMENT status="<<unsigned(r.status)<<" rms="<<r.diagnostics.rms<<'\n';
}
} // namespace
int wmain(int argc,wchar_t **argv) {
    try {
        Require(argc>=3 && argc<=5,"Usage: MetrologyTimepointAudit manifest output-directory [--controls-only] [--new-run]");
        bool controlsOnly=false,newRun=false;
        for (int i=3;i<argc;++i) { const std::wstring option(argv[i]); if (option==L"--controls-only") controlsOnly=true; else if (option==L"--new-run") newRun=true; else throw std::runtime_error("Unknown audit option."); }
        const auto manifest=ReadManifest(std::filesystem::path(argv[1])); std::filesystem::path folder(argv[2]);
        if (newRun) folder/="run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(folder);
        Require(!std::filesystem::exists(folder/"observations.json"),"Use a new output directory to retain previous audit evidence.");
        std::ofstream out(folder/"observations.json",std::ios::binary); out.imbue(std::locale::classic()); out<<std::setprecision(17);
        out<<"{\"schema\":\"ct-timepoint-audit-1\",\"scope\":\"public C3 to metrology experiment; measured timepoint comparison, no independent nominal truth\",\"inputLimitBytes\":"<<manifest.inputLimit
           <<",\"workingLimitBytes\":"<<manifest.workingLimit<<",\"deadlineMs\":"<<manifest.deadlineMs<<",\"scans\":[";
        auto data=std::make_shared<TestDataPort>(); HostFeatureContext context; context.data=data; context.views=std::make_shared<Views>(); context.host=std::make_shared<Control>();
        SurfaceDeterminationConfig surfaceConfig; surfaceConfig.maxWorkingBytes=manifest.workingLimit; surfaceConfig.isOverlayVisible=false;
        SurfaceDeterminationHostFeature surfaceFeature(surfaceConfig); Require(surfaceFeature.AttachHost(context),"Surface attach failed.");
        AlignmentConfig alignmentConfig; alignmentConfig.workingBytes=manifest.workingLimit; alignmentConfig.deadlineMs=manifest.deadlineMs; alignmentConfig.isOverlayVisible=false;
        auto inspectionViews=std::make_shared<InspectionViews>();auto alignmentContext=context;alignmentContext.views=inspectionViews;
        MetrologyAlignmentHostFeature alignmentFeature(alignmentConfig); Require(alignmentFeature.AttachHost(alignmentContext),"Metrology attach failed.");
        std::vector<Surface> surfaces;
        for (std::size_t i=0;i<manifest.scans.size();++i) { if (i) out<<','; surfaces.push_back(Determine(*data,surfaceFeature,manifest.scans[i],manifest,out,folder)); }
        out<<"],\"arrayAlignment\":"; if (!controlsOnly) Align(alignmentFeature,surfaces,manifest,out); else out<<"{\"attempted\":false,\"reason\":\"controls-only run\"}";
        std::vector<DataRevisionRef> deviations;
        out<<",\"rigidControl\":"; const bool controlPassed=IdentityControl(alignmentFeature,surfaces.front(),manifest,out,deviations);
        std::vector<DataRevisionRef> reports;
        out<<",\"ringInspection\":"; bool ringPassed=true; if (surfaces.size()>2) ringPassed=RingFit(alignmentFeature,surfaces[2],manifest,out,reports); else out<<"null";
        out<<",\"sectionInspections\":["; bool sectionsPassed=true;
        for (std::size_t i=0;i<surfaces.size();++i) { if (i) out<<','; sectionsPassed=SectionFit(alignmentFeature,surfaces[i],manifest.scans[i],manifest,out,folder,reports) && sectionsPassed; }
        out<<']';
        std::vector<DataRevisionRef> volumes;
        out<<",\"voxelVolumes\":";VoxelVolumes(alignmentFeature,*data,surfaces,manifest,out,volumes);
        out<<",\"reportCapture\":"; CaptureReport(alignmentFeature,*data,surfaces,manifest,reports,deviations,volumes,out,folder);
        out<<",\"inspectionScenes\":";InspectionScenes(alignmentFeature,*data,*inspectionViews,reports,out,folder);
        const bool pipelinePassed=controlPassed && ringPassed && sectionsPassed && std::all_of(surfaces.begin(),surfaces.end(),[](const auto &surface) { return surface.payload && !surface.validVertices.empty(); });
        const auto memory=ReadMemory(); out<<",\"pipelineChecksPassed\":"<<(pipelinePassed ? "true" : "false")<<",\"processPeakWorkingSetBytes\":"<<memory.peak<<",\"processPeakCommitBytes\":"<<memory.peakCommit<<"}\n";
        out.close();
        Require(bool(out),"Observation report write failed.");
        Require(alignmentFeature.DetachHost() && surfaceFeature.DetachHost(),"Audit feature detach failed.");
        std::cout<<"OBSERVATIONS "<<(folder/"observations.json").u8string()<<'\n'; return pipelinePassed ? 0 : 2;
    } catch (const std::exception &e) { std::cerr<<"AUDIT_ERROR: "<<e.what()<<'\n'; return 1; }
}
