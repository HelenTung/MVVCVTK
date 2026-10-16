// 可选真实规模显示审计；空间渐变值为模拟字段，不构成计量精度或名义比较验收。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include "../TestDataPort.h"
#include "AlignmentReference.h"
#include "AlignmentDeviationOverlay.h"
#include <vtkCamera.h>
#include <vtkPNGWriter.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkPropCollection.h>
#include <vtkWindowToImageFilter.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
struct Memory final { std::size_t working=0, privateBytes=0, peakWorking=0, peakCommit=0; };
Memory ReadMemory() {
    PROCESS_MEMORY_COUNTERS_EX value{}; value.cb=sizeof(value);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&value),sizeof(value)))
        throw std::runtime_error("Process memory counters unavailable.");
    return {value.WorkingSetSize,value.PrivateUsage,value.PeakWorkingSetSize,value.PeakPagefileUsage};
}
double Milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
void WriteMemory(std::ostream &out,const char *label,const Memory &m) {
    out<<'"'<<label<<"\":{\"workingSet\":"<<m.working<<",\"privateBytes\":"<<m.privateBytes
       <<",\"processPeakWorkingSet\":"<<m.peakWorking<<",\"processPeakCommit\":"<<m.peakCommit<<'}';
}
} // namespace
int wmain(int argc,wchar_t **argv) {
    if (argc<3) { std::cerr<<"Usage: display-audit STL output.json [image.png]\n"; return 2; }
    try {
        const auto initial=ReadMemory();
        AlignmentWork work; work.cancelled=std::make_shared<std::atomic<bool>>(false);


        AlignmentReferenceRequest input;
        input.filePath=std::filesystem::path(argv[1]).u8string(); input.frameId="render-audit"; input.coordinateFrame="RAS";
        const auto importStart=std::chrono::steady_clock::now();
        auto imported=AlignmentReferenceLoader::Load(input,work);
        if (imported.status!=AlignmentStatus::FullyDetermined || !imported.mesh) throw std::runtime_error(imported.message);
        const double importMs=Milliseconds(importStart); const auto afterImport=ReadMemory();
        const auto points=imported.mesh->GetPointValues().size()/3, triangles=imported.mesh->GetTriangleIndices().size()/3;
        TestDataPort data;
        const DataRevisionRef meshRef{data.CreateDataEntityId(),1};
        DataTransaction tx; tx.outputs.push_back({meshRef.entityId,0,DataTypes::surfaceMesh,{},imported.mesh,{}});
        if (data.SetDataCommit(std::move(tx)).status!=DataCommitStatus::Succeeded) throw std::runtime_error("Audit mesh publication failed.");
        const auto viewStart=std::chrono::steady_clock::now();
        const auto view=data.GetSurfaceMesh(data.GetDataGraph(),meshRef);
        if (!view) throw std::runtime_error("Audit render view unavailable.");
        const double viewMs=Milliseconds(viewStart); const auto afterView=ReadMemory();
        auto field=std::make_shared<AlignmentDeviationField>();field->originalVertexCount=points; field->samples.resize(points);
        const auto &coordinates=imported.mesh->GetPointValues();
        double low=coordinates[0], high=coordinates[0];
        for (std::size_t i=0;i<points;++i) { low=std::min(low,coordinates[3*i]); high=std::max(high,coordinates[3*i]); }
        for (std::size_t i=0;i<points;++i) {
            auto &sample=field->samples[i]; sample.state=AlignmentDeviationState::Valid;
            sample.value=high>low ? 4*((coordinates[3*i]-low)/(high-low)-.5) : 0;
            sample.distance=std::abs(sample.value);
        }
        AlignmentDeviationSnapshot snapshot;
        snapshot.sourceMesh=meshRef; snapshot.result={data.CreateDataEntityId(),1}; snapshot.forward=field;
        auto parameters=std::make_shared<AlignmentDeviationRequest>();
        parameters->metric=AlignmentDeviationMetric::NominalNormalProjection;
        parameters->tolerance=std::array<double,2>{-.1,.1}; snapshot.parameters=parameters;
        AlignmentDeviationDisplay display;display.rangeMode=AlignmentRangeMode::Manual; display.result=snapshot.result; display.range={-1,1};
        const auto colorStart=std::chrono::steady_clock::now();
        auto product=AlignmentDeviationOverlay::Build(view,snapshot,display);
        const double colorMs=Milliseconds(colorStart); const auto afterColor=ReadMemory();
        if (product->colored->GetPoints()!=view->mesh->GetPoints() || product->colored->GetPolys()!=view->mesh->GetPolys())
            throw std::runtime_error("Display geometry was duplicated.");
        auto renderer=vtkSmartPointer<vtkRenderer>::New(); renderer->SetBackground(.04,.04,.04);
        auto overlay=std::make_shared<AlignmentDeviationOverlay>(product); overlay->AttachRenderer(renderer);
        auto *props=renderer->GetViewProps(); props->InitTraversal();
        while (auto *prop=props->GetNextProp())
            if (auto *legend=vtkScalarBarActor::SafeDownCast(prop)) legend->SetTitle("Synthetic colors\n(render audit)");
        renderer->GetActiveCamera()->ParallelProjectionOn(); renderer->ResetCamera();
        renderer->GetActiveCamera()->Azimuth(30); renderer->GetActiveCamera()->Elevation(20); renderer->ResetCameraClippingRange();
        auto window=vtkSmartPointer<vtkRenderWindow>::New(); window->SetOffScreenRendering(1);
        window->SetSize(800,800); window->SetMultiSamples(0); window->AddRenderer(renderer);
        const auto firstStart=std::chrono::steady_clock::now(); window->Render(); window->WaitForCompletion();
        const double firstFrameMs=Milliseconds(firstStart); const auto afterFirstFrame=ReadMemory();
        std::istringstream capabilities(window->ReportCapabilities()); std::string capability;
        while (std::getline(capabilities,capability))
            if (capability.find("OpenGL vendor")!=std::string::npos || capability.find("OpenGL renderer")!=std::string::npos ||
                capability.find("OpenGL version")!=std::string::npos) std::cout<<capability<<'\n';
        for (int i=0;i<3;++i) { window->Render(); window->WaitForCompletion(); }
        std::vector<double> frames; frames.reserve(30);
        for (int i=0;i<30;++i) {
            const auto start=std::chrono::steady_clock::now();
            renderer->GetActiveCamera()->Azimuth(1); renderer->ResetCameraClippingRange(); window->Render(); window->WaitForCompletion(); frames.push_back(Milliseconds(start));
        }
        const auto afterFrames=ReadMemory();
        if (argc>3) {
            auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New(); capture->SetInput(window); capture->ReadFrontBufferOff(); capture->Update();
            auto writer=vtkSmartPointer<vtkPNGWriter>::New(); writer->SetInputConnection(capture->GetOutputPort());
            writer->SetFileName(std::filesystem::path(argv[3]).u8string().c_str()); writer->Write();
        }
        overlay->DetachRenderer(renderer); window->Finalize();
        double mean=0; for (const auto value:frames) mean+=value/frames.size();
        std::sort(frames.begin(),frames.end());
        std::ofstream out(std::filesystem::path(argv[2]),std::ios::binary);
        out<<std::setprecision(8)<<"{\n\"scope\":\"representative STL geometry rendering; synthetic spatial gradient; no CT accuracy claim\",\n"
           <<"\"status\":\"PASS\",\"sourceSha256\":\""<<imported.metadata.sha256<<"\",\n"
           <<"\"points\":"<<points<<",\"triangles\":"<<triangles<<",\"viewport\":[800,800],\"sampledFrames\":30,\n"
           <<"\"frameTiming\":\"Render plus WaitForCompletion\",\"geometryShared\":true,\"colorWorkBytes\":"<<product->workingBytes<<",\n"
           <<"\"importMs\":"<<importMs<<",\"viewMs\":"<<viewMs<<",\"colorMs\":"<<colorMs
           <<",\"firstFrameMs\":"<<firstFrameMs<<",\"meanFrameMs\":"<<mean<<",\"medianFrameMs\":"<<frames[15]
           <<",\"p95FrameMs\":"<<frames[28]<<",\n\"memoryBytes\":{";
        WriteMemory(out,"initial",initial); out<<','; WriteMemory(out,"afterImport",afterImport); out<<',';
        WriteMemory(out,"afterView",afterView); out<<','; WriteMemory(out,"afterColor",afterColor); out<<',';
        WriteMemory(out,"afterFirstFrame",afterFirstFrame); out<<','; WriteMemory(out,"afterFrames",afterFrames); out<<"}\n}\n";
        if (!out) throw std::runtime_error("Audit report write failed.");
        std::cout<<"DISPLAY_AUDIT triangles="<<triangles<<" mean_frame_ms="<<mean<<" peak_working_bytes="<<afterFrames.peakWorking<<'\n';
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
