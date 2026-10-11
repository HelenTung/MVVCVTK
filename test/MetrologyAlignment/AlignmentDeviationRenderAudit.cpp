// 真实 C3 表面上的刚体扰动显示审计；2D 截面为仓内验证投影，不是产品 UI。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../TestDataPort.h"
#include "AlignmentDeviation.h"
#include "AlignmentDeviationColors.h"
#include "AlignmentDeviationOverlay.h"
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCutter.h>
#include <vtkDoubleArray.h>
#include <vtkPlane.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <vtkXMLPolyDataReader.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
void Require(bool valid,const char *message) { if(!valid)throw std::runtime_error(message); }
double Ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
vtkSmartPointer<vtkActor> Actor(vtkPolyData *mesh,bool colored) {
    auto mapper=vtkSmartPointer<vtkPolyDataMapper>::New();mapper->SetInputData(mesh);
    if(colored) { mapper->SetColorModeToDirectScalars();mapper->SetScalarModeToUsePointData(); }
    else mapper->ScalarVisibilityOff();
    auto actor=vtkSmartPointer<vtkActor>::New();actor->SetMapper(mapper);
    actor->GetProperty()->SetLighting(false);actor->GetProperty()->SetColor(.502,.502,.502);
    actor->GetProperty()->SetLineWidth(2);return actor;
}
void SetColors(vtkPolyData *mesh,const AlignmentDeviationColors::Ramp &ramp) {
    auto *values=mesh->GetPointData()->GetArray("audit.deviation.value");Require(values,"Cut value array missing.");
    auto rgb=vtkSmartPointer<vtkUnsignedCharArray>::New();rgb->SetName("audit.deviation.rgb");rgb->SetNumberOfComponents(3);
    rgb->SetNumberOfTuples(mesh->GetNumberOfPoints());
    for(vtkIdType i=0;i<mesh->GetNumberOfPoints();++i) {
        const auto color=ramp.ColorAt(values->GetTuple1(i));std::copy(color.begin(),color.end(),rgb->GetPointer(3*i));
    }
    mesh->GetPointData()->SetScalars(rgb);
}
// 先插值偏差标量再映射颜色，避免把 RGB 插值误当成色环的数值插值。
vtkSmartPointer<vtkPolyData> Cut(vtkPolyData *mesh,const double *center,int axis) {
    auto plane=vtkSmartPointer<vtkPlane>::New();plane->SetOrigin(center);
    double normal[3]{};normal[axis]=1;plane->SetNormal(normal);
    auto cutter=vtkSmartPointer<vtkCutter>::New();cutter->SetCutFunction(plane);cutter->SetInputData(mesh);
    cutter->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);cutter->Update();
    auto output=vtkSmartPointer<vtkPolyData>::New();output->ShallowCopy(cutter->GetOutput());return output;
}
void Save(vtkRenderer *renderer,const std::filesystem::path &file,const double *bounds,int axis,
          std::ostream &out,const std::string &name) {
    double center[3]{};double width=0;
    for(int a=0;a<3;++a) { center[a]=(bounds[2*a]+bounds[2*a+1])/2; width=std::max(width,bounds[2*a+1]-bounds[2*a]); }
    auto *camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();camera->SetFocalPoint(center);
    double position[3]{center[0],center[1],center[2]};position[axis<0 ? 2 : axis]+=2*width;
    camera->SetPosition(position);camera->SetViewUp(0,axis==2 || axis<0 ? 1 : 0,axis==2 || axis<0 ? 0 : 1);
    if(axis<0)renderer->ResetCamera(bounds);else renderer->ResetCamera();
    if(axis<0) {camera->Azimuth(30);camera->Elevation(20);}
    // 左侧预留色条区；三维与截面均保持相同的相机偏移和缩放规则。
    camera->SetWindowCenter(-.22,0);camera->Zoom(.82);
    renderer->ResetCameraClippingRange();
    auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);
    window->SetMultiSamples(0);window->SetSize(1024,768);window->AddRenderer(renderer);
    const auto start=std::chrono::steady_clock::now();window->Render();window->WaitForCompletion();const auto first=Ms(start);
    for(int i=0;i<3;++i) {window->Render();window->WaitForCompletion();}
    double total=0;
    for(int i=0;i<10;++i) {const auto frame=std::chrono::steady_clock::now();window->Render();window->WaitForCompletion();total+=Ms(frame);}
    auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New();capture->SetInput(window);capture->ReadFrontBufferOff();capture->Update();
    auto writer=vtkSmartPointer<vtkPNGWriter>::New();writer->SetInputConnection(capture->GetOutputPort());
    writer->SetFileName(file.u8string().c_str());writer->Write();Require(writer->GetErrorCode()==0,"PNG output failed.");
    out<<"{\"name\":\""<<name<<"\",\"firstGpuFrameMs\":"<<first<<",\"meanGpuFrameMs\":"<<total/10<<",\"frames\":10}";
    window->Finalize();
}
void Render(TestDataPort &data,const DataRevisionRef &meshRef,const AlignmentDeviationSnapshot &snapshot,
            const std::filesystem::path &folder,const std::string &phase,std::ostream &out) {
    const auto source=data.GetSurfaceMesh(data.GetDataGraph(),meshRef);Require(source && source->mesh,"Source view missing.");
    AlignmentDeviationDisplay automatic;automatic.result=snapshot.result;
    const auto resolved=AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot);
    Require(resolved.has_value(),"No finite effective deviation range.");
    const auto &display=*resolved;
    const AlignmentDeviationColors::Ramp ramp(display,snapshot.parameters->tolerance);
    const auto product=AlignmentDeviationOverlay::Build(source,snapshot,display);
    auto *quality=source->mesh->GetCellData()->GetArray("measurement.valid");Require(quality,"Cell quality missing.");
    auto valid=vtkSmartPointer<vtkCellArray>::New(),invalid=vtkSmartPointer<vtkCellArray>::New();
    auto *polys=source->mesh->GetPolys();polys->InitTraversal();vtkIdType count=0;const vtkIdType *ids=nullptr;vtkIdType cell=0;
    vtkIdType rejectedFacesWithValidVertices=0,mixedVertexFaces=0;
    while(polys->GetNextCell(count,ids)) {
        vtkIdType validVertices=0;
        for(vtkIdType j=0;j<count;++j)validVertices+=snapshot.forward->samples[ids[j]].state==AlignmentDeviationState::Valid;
        const bool usable=quality->GetTuple1(cell)==1 && validVertices==count;
        rejectedFacesWithValidVertices+=quality->GetTuple1(cell)!=1 && validVertices==count;
        mixedVertexFaces+=validVertices>0 && validVertices<count;
        (usable ? valid : invalid)->InsertNextCell(count,ids);++cell;
    }
    auto values=vtkSmartPointer<vtkDoubleArray>::New();values->SetName("audit.deviation.value");values->SetNumberOfTuples(source->mesh->GetNumberOfPoints());
    for(vtkIdType i=0;i<source->mesh->GetNumberOfPoints();++i)values->SetValue(i,snapshot.forward->samples[i].value);
    auto qualified=vtkSmartPointer<vtkPolyData>::New();qualified->ShallowCopy(source->mesh);qualified->SetPolys(valid);
    qualified->GetCellData()->Initialize();qualified->GetPointData()->SetScalars(values);
    auto rejected=vtkSmartPointer<vtkPolyData>::New();rejected->SetPoints(source->mesh->GetPoints());rejected->SetPolys(invalid);
    double bounds[6]{};source->mesh->GetBounds(bounds);double center[3]{};
    for(int a=0;a<3;++a)center[a]=(bounds[2*a]+bounds[2*a+1])/2;
    const auto effective=AlignmentDeviationColors::GetEffectiveRange(*snapshot.forward);
    out<<"{\"phase\":\""<<phase<<"\",\"rangeSource\":\"finite Valid samples\",\"effectiveRangeMm\":["<<(*effective)[0]<<','<<(*effective)[1]
       <<"],\"rangeMm\":["<<display.range[0]<<','<<display.range[1]<<"],\"useToleranceColors\":false,\"validFaces\":"<<valid->GetNumberOfCells()<<",\"grayFaces\":"<<invalid->GetNumberOfCells()
       <<",\"qualityRejectedFacesWithValidVertices\":"<<rejectedFacesWithValidVertices<<",\"mixedVertexFaces\":"<<mixedVertexFaces
       <<",\"sectionOriginRAS\":["<<center[0]<<','<<center[1]<<','<<center[2]<<"],\"views\":[";
    for(int view=-2;view<3;++view) {
        if(view!=-2)out<<',';
        auto renderer=vtkSmartPointer<vtkRenderer>::New();renderer->SetBackground(.035,.045,.06);
        std::shared_ptr<AlignmentDeviationOverlay> overlay;
        const std::string name=phase+(view==-2 ? "-3d-product" : view==-1 ? "-3d-quality" : "-2d-"+std::to_string(view));
        if(view==-2) {overlay=std::make_shared<AlignmentDeviationOverlay>(product);overlay->AttachRenderer(renderer);}
        else {
            auto colored=vtkSmartPointer<vtkPolyData>::New();
            if(view<0)colored->ShallowCopy(qualified);else colored=Cut(qualified,center,view);
            SetColors(colored,ramp);renderer->AddActor(Actor(colored,true));
            renderer->AddActor(Actor(view<0 ? rejected.GetPointer() : Cut(rejected,center,view).GetPointer(),false));
            auto legend=vtkSmartPointer<vtkScalarBarActor>::New();legend->SetLookupTable(ramp.GetLookup());
            AnalysisColorStyle::SetLegend(*legend,"Normal projection (mm)");legend->SetPosition(.15,.2);
            legend->SetNanAnnotation("invalid / unmatched");legend->DrawNanAnnotationOn();renderer->AddActor2D(legend);
        }
        Save(renderer,folder/(name+".png"),bounds,view<0 ? -1 : view,out,name);
        if(overlay)overlay->DetachRenderer(renderer);
    }
    out<<"]}";out.flush();
}
}
int wmain(int argc,wchar_t **argv) {
    try {
        Require(argc==3 || argc==19,"Usage: deviation-render-audit source.vtp output-directory [16 solved matrix values]");
        auto solvedPose=alignmentIdentity;
        if(argc==19)for(int i=0;i<16;++i)solvedPose[i]=std::stod(std::wstring(argv[3+i]));
        const auto path=std::filesystem::path(argv[1]),folder=std::filesystem::path(argv[2]);
        std::filesystem::create_directories(folder);Require(!std::filesystem::exists(folder/"render.json"),"Use a new output directory.");
        auto reader=vtkSmartPointer<vtkXMLPolyDataReader>::New();reader->SetFileName(path.u8string().c_str());reader->Update();
        VtkDataBridge bridge;const auto payload=bridge.CreateMeshPayload(reader->GetOutput(),"RAS");Require(payload && payload->GetValid(),"Source VTP rejected.");
        TestDataPort data;const DataRevisionRef meshRef{data.CreateDataEntityId(),1};DataTransaction tx;
        tx.outputs.push_back({meshRef.entityId,0,DataTypes::surfaceMesh,{},payload,{}});
        Require(data.SetDataCommit(std::move(tx)).status==DataCommitStatus::Succeeded,"Mesh publication failed.");
        AlignmentWork work;work.mesh=payload;work.referenceMesh=payload;work.cancelled=std::make_shared<std::atomic<bool>>(false);
        auto request=std::make_shared<AlignmentDeviationRequest>();request->alignmentResult={data.CreateDataEntityId(),1};
        request->metric=AlignmentDeviationMetric::NominalNormalProjection;request->requiresMeasurementQuality=true;
        request->reverseCoverage=false;request->maxDistance=5;request->tolerance=std::array<double,2>{-.05,.05};
        const MeshAttribute *pointQuality=nullptr,*faceQuality=nullptr;
        for(const auto &a:payload->GetPointAttributes())if(a.name=="measurement.valid")pointQuality=&a;
        for(const auto &a:payload->GetCellAttributes())if(a.name=="measurement.valid")faceQuality=&a;
        Require(pointQuality && faceQuality,"Measurement quality missing.");
        const auto &triangles=payload->GetTriangles();
        for(std::size_t i=0;i<triangles.size()/3;++i)if(faceQuality->values[i]==1 && pointQuality->values[triangles[3*i]]==1 &&
            pointQuality->values[triangles[3*i+1]]==1 && pointQuality->values[triangles[3*i+2]]==1)request->sourceTriangles.push_back(i);
        request->referenceTriangles=request->sourceTriangles;
        std::ofstream out(folder/"render.json");out<<std::setprecision(17)
            <<"{\"schema\":\"deviation-render-audit-2\",\"scope\":\"real C3 mesh; known rigid perturbation versus supplied public solve pose or exact identity; not cross-acquisition or CAD nominal acceptance\",\"afterPoseFromPublicSolve\":"<<(argc==19 ? "true" : "false")<<",\"rangeMode\":\"effective finite Valid sample range per field, shared by 2D and 3D\",\"useToleranceColors\":false,\"viewport\":[1024,768],\"points\":"
            <<payload->GetVertices().size()/3<<",\"triangles\":"<<triangles.size()/3<<",\"cases\":[";
        for(int phase=0;phase<2;++phase) {
            if(phase)out<<',';const std::string name=phase ? "aligned" : "perturbed";
            auto pose=phase ? solvedPose : alignmentIdentity;if(!phase) { const double a=.01;pose[0]=pose[5]=std::cos(a);pose[1]=-std::sin(a);pose[4]=std::sin(a);pose[3]=.15;pose[7]=-.12;pose[11]=.08; }
            request->computeBackend=AlignmentComputeBackend::Cpu;const auto start=std::chrono::steady_clock::now();
            const auto cpu=AlignmentDeviation::Build(work,*request,pose);const auto cpuMs=Ms(start);
            Require(cpu.status==AlignmentStatus::FullyDetermined && cpu.forward,"CPU evaluation failed.");
            request->computeBackend=AlignmentComputeBackend::Cuda;const auto gpuStart=std::chrono::steady_clock::now();
            const auto gpu=AlignmentDeviation::Build(work,*request,pose);const auto gpuMs=Ms(gpuStart);
            Require(gpu.status==AlignmentStatus::FullyDetermined && gpu.forward,"CUDA evaluation failed.");
            Require(cpu.forward->samples.size()==gpu.forward->samples.size(),"CPU/CUDA sample count differs.");
            std::size_t mismatches=0;double maximumDelta=0;
            for(std::size_t i=0;i<cpu.forward->samples.size();++i) {
                const auto &a=cpu.forward->samples[i],&b=gpu.forward->samples[i];maximumDelta=std::max(maximumDelta,std::abs(a.value-b.value));
                mismatches+=a.value!=b.value || a.distance!=b.distance || a.closestTriangle!=b.closestTriangle || a.state!=b.state || a.area!=b.area;
            }
            out<<"{\"phase\":\""<<name<<"\",\"sourceToTarget\":[";
            for(int i=0;i<16;++i){if(i)out<<',';out<<pose[i];}
            const auto &stats=cpu.forward->statistics;
            out<<"],\"cpuCoreMs\":"<<cpuMs<<",\"cudaCoreMs\":"<<gpuMs<<",\"cpuCudaMismatchSamples\":"<<mismatches
                <<",\"cpuCudaMaximumValueDeltaMm\":"<<maximumDelta<<",\"minimumMm\":"<<stats.minimum<<",\"maximumMm\":"<<stats.maximum
                <<",\"rmsMm\":"<<stats.rms<<",\"validSamples\":"<<stats.counts[static_cast<std::size_t>(AlignmentDeviationState::Valid)]<<",\"render\":";
            auto cpuRequest=std::make_shared<AlignmentDeviationRequest>(*request);cpuRequest->computeBackend=AlignmentComputeBackend::Cpu;
            AlignmentDeviationSnapshot snapshot;snapshot.sourceMesh=meshRef;snapshot.result={data.CreateDataEntityId(),1};
            snapshot.forward=cpu.forward;snapshot.parameters=cpuRequest;snapshot.unit=AlignmentUnit::Millimeter;snapshot.sourceToTarget=pose;
            Render(data,meshRef,snapshot,folder,name,out);out<<'}';
            Require(mismatches==0,"CPU/CUDA sample mismatch; retained report must not claim equality.");
            std::cout<<"RENDER "<<name<<" rms_mm="<<stats.rms<<" mismatch_samples="<<mismatches<<'\n'<<std::flush;
        }
        out<<"],\"status\":\"PASS\"}\n";out.close();Require(bool(out),"Render report write failed.");return 0;
    } catch(const std::exception &e) {std::cerr<<"RENDER_AUDIT_ERROR: "<<e.what()<<'\n';return 1;}
}
