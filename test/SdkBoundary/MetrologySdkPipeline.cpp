// 仓库与安装态共用的真实 C3 派生网格控制。恒等真值只证明数值链，不证明实物精度。
#include "MVVCVTK/API/Host/VtkAppHostSession.h"
#include "MVVCVTK/API/Features/MetrologyAlignment/Host/MetrologyAlignmentHostFeature.h"
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkXMLPolyDataReader.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <thread>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <Windows.h>
#include <bcrypt.h>

namespace {
void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::string ReadFileSha256(const char *filePath) {
    struct HashState final {
        BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; std::vector<unsigned char> object;
        ~HashState() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    } state;
    Require(BCryptOpenAlgorithmProvider(&state.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0, "SHA256 provider must open.");
    ULONG bytes=0,written=0;
    Require(BCryptGetProperty(state.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&bytes),sizeof(bytes),&written,0)>=0,
            "SHA256 object size must be available.");
    state.object.resize(bytes);
    Require(BCryptCreateHash(state.algorithm,&state.hash,state.object.data(),bytes,nullptr,0,0)>=0, "SHA256 state must initialize.");
    std::ifstream input(std::filesystem::u8path(filePath),std::ios::binary); Require(bool(input), "Frozen mesh must open for hashing.");
    std::array<char,65536> buffer{};
    while (input) {
        input.read(buffer.data(),buffer.size()); const auto count=input.gcount();
        if (count) Require(BCryptHashData(state.hash,reinterpret_cast<PUCHAR>(buffer.data()),static_cast<ULONG>(count),0)>=0,
                           "Frozen mesh SHA256 update must succeed.");
    }
    Require(input.eof() && !input.bad(), "Frozen mesh must read completely.");
    std::array<unsigned char,32> digest{};
    Require(BCryptFinishHash(state.hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0, "SHA256 must finish.");
    std::ostringstream output; output<<std::hex<<std::setfill('0');
    for (auto byte:digest) output<<std::setw(2)<<static_cast<unsigned>(byte);
    return output.str();
}
class InputFeature final : public HostFeature {
public:
    std::string_view GetFeatureId() const noexcept override { return "sdk-metrology-input"; }
    bool AttachHost(const HostFeatureContext &value) override { data = value.data; return bool(data); }
    bool DetachHost() override { data.reset(); return true; }
    bool OnHostTick() override { return true; }
    std::shared_ptr<TrustedDataPort> data;
};
std::vector<MeshAttribute> Attributes(vtkFieldData *data) {
    std::vector<MeshAttribute> output;
    for (int i=0; i<data->GetNumberOfArrays(); ++i) {
        auto *array = data->GetArray(i); if (!array || !array->GetName()) continue;
        MeshAttribute attribute; attribute.name = array->GetName(); attribute.componentCount = array->GetNumberOfComponents();
        attribute.values.reserve(static_cast<std::size_t>(array->GetNumberOfValues()));
        for (vtkIdType row=0; row<array->GetNumberOfTuples(); ++row)
            for (int component=0; component<array->GetNumberOfComponents(); ++component)
                attribute.values.push_back(array->GetComponent(row,component));
        output.push_back(std::move(attribute));
    }
    return output;
}
void Run(const char *filePath, const char *expectedSha256) {
    const auto sha256=ReadFileSha256(filePath);
    Require(sha256==expectedSha256, "Real C3 mesh must match its frozen SHA256 before loading.");
    auto reader = vtkSmartPointer<vtkXMLPolyDataReader>::New(); reader->SetFileName(filePath); reader->Update();
    auto *poly = reader->GetOutput(); Require(poly && poly->GetNumberOfPoints()>0, "Real C3 mesh must load.");
    std::vector<double> vertices; vertices.reserve(static_cast<std::size_t>(poly->GetNumberOfPoints())*3);
    for (vtkIdType i=0; i<poly->GetNumberOfPoints(); ++i) { double point[3]; poly->GetPoint(i,point); vertices.insert(vertices.end(),point,point+3); }
    std::vector<std::uint64_t> triangles; vtkIdType count=0; const vtkIdType *ids=nullptr;
    poly->GetPolys()->InitTraversal();
    while (poly->GetPolys()->GetNextCell(count,ids)) {
        Require(count==3, "C3 input must preserve triangle topology.");
        for (int i=0;i<3;++i) triangles.push_back(static_cast<std::uint64_t>(ids[i]));
    }
    auto mesh = std::make_shared<const SurfaceMeshPayload>(std::move(vertices),std::move(triangles),
        Attributes(poly->GetPointData()),"RAS",Attributes(poly->GetCellData()));
    Require(mesh->GetValid(), "Real C3 payload must retain valid layout.");
    auto *pointValidity=poly->GetPointData()->GetArray("measurement.valid");
    auto *faceValidity=poly->GetCellData()->GetArray("measurement.valid");
    Require(pointValidity && faceValidity, "Real C3 point and cell quality must be present.");
    const auto &faces=mesh->GetTriangleIndices();
    std::vector<bool> selected(mesh->GetPointValues().size()/3,false), faceUsable(faces.size()/3,false);
    std::vector<std::uint64_t> validFaces;
    for (std::size_t i=0;i<faces.size()/3;++i) {
        bool valid=faceValidity->GetComponent(static_cast<vtkIdType>(i),0)==1;
        for (int j=0;j<3;++j) valid=valid && pointValidity->GetComponent(static_cast<vtkIdType>(faces[3*i+j]),0)==1;
        faceUsable[i]=valid;
        if (valid) { validFaces.push_back(i); for (int j=0;j<3;++j) selected[faces[3*i+j]]=true; }
    }
    for (std::size_t i=0;i<faceUsable.size();++i) if (!faceUsable[i])
        for (int j=0;j<3;++j) selected[faces[3*i+j]]=false;
    Require(!validFaces.empty(), "Real C3 requires valid surface support.");
    HostSessionConfig config; config.driveMode=HostDriveMode::HostDriven;
    auto window=vtkSmartPointer<vtkRenderWindow>::New(); window->SetOffScreenRendering(1);
    HostRenderViewConfig view; view.id="sdk-metrology"; view.role=HostRenderViewRole::Primary3D;
    view.renderWindow=window; view.window.width=64; view.window.height=64;
    view.isEventLoopEnabled=true; view.inputMode=HostInputMode::HostInjected; config.renderViews.push_back(view);
    VtkAppHostSession session(std::move(config)); Require(session.BuildSession(), "Actual Host session must build.");
    struct StopSession final { VtkAppHostSession &session; ~StopSession() { session.Stop(); } } stop{session};
    auto inputFeature=std::make_shared<InputFeature>(); AlignmentConfig featureConfig; featureConfig.isOverlayVisible=false;
    auto feature=std::make_shared<MetrologyAlignmentHostFeature>(featureConfig);
    Require(session.AttachFeature(inputFeature) && session.AttachFeature(feature), "SDK features must attach to the running HostDriven session.");
    auto &data=*inputFeature->data; const DataRevisionRef source{data.CreateDataEntityId(),1};
    DataTransaction transaction; transaction.outputs.push_back({source.entityId,0,DataTypes::surfaceMesh,{},mesh,
        DataProvenance{"sdk-metrology-control","load-real-c3","1","sha256="+sha256}});
    Require(data.SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded, "Actual Host must publish the real mesh.");
    const auto send=[&](AlignmentRequest request) {
        struct Completion final { bool isDone=false; AlignmentResult result; };
        auto complete=std::make_shared<Completion>();
        const auto admission=feature->SendRequest(std::move(request),[complete](AlignmentResult value) {
            complete->result=std::move(value); complete->isDone=true;
        });
        Require(admission.status==AlignmentAdmissionStatus::Accepted, "SDK request must be admitted.");
        // 默认等待完整业务完成；非零环境参数仅用于调用方显式选择的测试进程保护。
        std::array<char,64> setting{};
        const auto length=GetEnvironmentVariableA("MVVCVTK_SDK_AUDIT_TIMEOUT_MS",setting.data(),static_cast<DWORD>(setting.size()));
        Require(length<setting.size(),"Caller-selected audit deadline is invalid.");
        const auto timeout=length?std::stoull(setting.data()):0;
        const auto started=std::chrono::steady_clock::now();
        while (!complete->isDone && (!timeout || std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()<double(timeout))) { session.SendUpdates(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        Require(complete->isDone, "SDK request did not finish within the caller-selected audit deadline.");
        const auto &result=complete->result;
        if (result.status!=AlignmentStatus::FullyDetermined) std::cerr<<result.message<<'\n';
        Require(result.status==AlignmentStatus::FullyDetermined, "SDK business result must complete.");
        return result;
    };
    AlignmentRecipe recipe; recipe.id="sdk-real-c3"; recipe.targetFrameId="nominal"; recipe.unit=AlignmentUnit::Millimeter;
    recipe.method=AlignmentMethod::ConstrainedBestFit; recipe.datumCount=0; recipe.nominalData=source;
    recipe.requiresMeasurementQuality=true; recipe.surfaceFit=AlignmentSurfaceFit{}; recipe.surfaceFit->referenceTriangles=validFaces;
    AlignmentRegion region; region.pinnedMesh=source;
    for (std::size_t i=0;i<selected.size();++i) if (selected[i] && pointValidity->GetComponent(static_cast<vtkIdType>(i),0)==1) region.vertexIds.push_back(i);
    Require(region.vertexIds.size()>=3, "Complete valid C3 vertex support must exist.");
    recipe.surfaceFit->sourceRegion=region; recipe.surfaceFit->scales={{.55,4,60},{.275,.5,60}};
    recipe.maxPairDistance=.5; recipe.maxAlignmentRms=1e-5; recipe.minPairCoverage=1;
    AlignmentRequest save; save.action=AlignmentAction::SaveRecipe; save.recipe=recipe; const auto saved=send(save);
    AlignmentRequest solve; solve.recipeRef=saved.recipe;
    solve.input=AlignmentInput{source,source,source,"scan","RAS","sdk-real-c3",AlignmentUnit::Millimeter};
    auto pose=alignmentIdentity; pose[3]=.15; pose[7]=-.12; pose[11]=.08; solve.initialPoses={pose};
    const auto solved=send(solve); const auto archive=feature->GetArchive(solved.result);
    Require(archive && solved.diagnostics.isQualityPassed && solved.diagnostics.remaining==0, "SDK alignment must be current, fully constrained and quality checked.");
    for (std::size_t i=0;i<16;++i) Require(std::abs(archive->sourceToTarget[i]-alignmentIdentity[i])<1e-6, "Identity control must recover its exact numerical truth.");
    AlignmentRequest evaluate; evaluate.action=AlignmentAction::EvaluateSurface; evaluate.deviation=AlignmentDeviationRequest{};
    auto &parameters=*evaluate.deviation; parameters.alignmentResult=solved.result; parameters.sourceTriangles=validFaces;
    parameters.referenceTriangles=validFaces; parameters.requiresMeasurementQuality=true; parameters.maxDistance=1e-6;
    parameters.tolerance=std::array<double,2>{0,1e-6}; const auto evaluated=send(evaluate);
    const auto field=feature->GetDeviation(evaluated.deviationResult);
    Require(field && field->isCurrent && field->verdict==AlignmentInspectionVerdict::Pass && field->forward->statistics.maximum<=1e-6 &&
        field->forward->statistics.evaluatedArea==field->forward->statistics.selectedArea, "SDK deviation must evaluate the selected valid area without hiding coverage.");
    MetrologyReportRequest request; request.id="sdk-real-c3"; request.alignments={solved.result}; request.deviations={evaluated.deviationResult};
    const auto report=feature->GetReport(request); Require(report.report && report.status==AlignmentStatus::FullyDetermined, "SDK must freeze the real control report.");
    Require(report.report->alignments.size()==1 && report.report->alignments[0].residuals->GetRowCount()>0, "SDK report must retain residual rows.");
    std::ostringstream json; const auto written=WriteMetrologyReport(*report.report,json);
    Require(written.status==MetrologyReportWriteStatus::Succeeded && written.bytesWritten==json.str().size(), "SDK report writer must preserve complete output.");
    Require(session.DetachFeature(*feature) && session.DetachFeature(*inputFeature) && session.Stop(), "SDK features and Host must stop cleanly.");
    std::cout<<"RealC3SdkPipeline points="<<mesh->GetPointValues().size()/3<<" triangles="<<faces.size()/3
             <<" validTriangles="<<validFaces.size()<<" rms="<<solved.diagnostics.rms<<" evaluatedArea="<<field->forward->statistics.evaluatedArea
             <<" sha256="<<sha256<<'\n';
}
}
int wmain(int argc,wchar_t **argv) {
    try {
        Require(argc==3,"Usage: MetrologySdkPipeline real-c3.vtp expected-sha256");
        const auto utf8=[](const wchar_t *value) {
            const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,nullptr,0,nullptr,nullptr);
            Require(count>0,"Input arguments must be valid Unicode.");
            std::string output(static_cast<std::size_t>(count),'\0');
            Require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,output.data(),count,nullptr,nullptr)==count,
                    "Input arguments must convert to UTF-8.");
            output.pop_back(); return output;
        };
        const auto filePath=utf8(argv[1]), sha256=utf8(argv[2]); Run(filePath.c_str(),sha256.c_str());
    }
    catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
    return 0;
}
