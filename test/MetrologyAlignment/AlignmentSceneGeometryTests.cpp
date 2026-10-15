#include "AlignmentSceneGeometry.h"
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkRenderWindow.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#define CHECK(v) do{if(!(v))throw std::runtime_error(#v);}while(false)
namespace {
struct Fixture {
    AlignmentSceneProduct product;AlignmentScenePartData part;AlignmentScenePart request;
    std::shared_ptr<InspectionPlan> plan=std::make_shared<InspectionPlan>();std::shared_ptr<InspectionReport> report=std::make_shared<InspectionReport>();
    SurfaceMeshPayload mesh;std::size_t charged=0,limit=16*1024*1024;
    Fixture():mesh({0,0,1,1,0,1,0,1,1,3,0,1,4,0,1,5,0,1,6,0,1,7,0,1,8,0,1},{0,1,2}) {
        part.inspection=InspectionSnapshot{};part.inspection->plan=plan;part.inspection->report=report;part.inspection->sourceToTarget[3]=10;product.targetToModel[3]=-10;
        InspectionGeometrySpec spec;spec.fit.id="point";spec.fit.kind=AlignmentGeometryKind::Point;spec.fit.region.vertexIds={0,1,2,3,4,5,6,7,8};plan->geometries.push_back(spec);
        InspectionGeometry g;g.status=AlignmentStatus::FullyDetermined;g.fitted.kind=AlignmentGeometryKind::Point;g.alignedCenter={14,0,1};g.alignedDirection={0,0,1};report->geometries.push_back(g);
        InspectionItem item;item.id="item";plan->items.push_back(item);request.selectedItem="item";
    }
    void Build(std::size_t markers=3) {
        AlignmentSceneGeometry::Build(product,part,request,mesh,nullptr,4,markers,1,[&](std::size_t n,std::size_t width){if(n>(limit-charged)/width)throw std::length_error("test budget");charged+=n*width;});
    }
};
void Semantics() {
    InspectionPlan plan;plan.geometries.resize(8);InspectionItem item;
    item.measured.first={0,1};item.measured.second=InspectionOperand{2,3};item.nominalMeasure=InspectionMeasure{};
    item.nominalMeasure->first={4,5};item.nominalMeasure->second=InspectionOperand{6,7};plan.items.push_back(item);
    CHECK(AlignmentSceneGeometry::GetItemGeometries(plan,0)==std::vector<std::size_t>({0,1,2,3,4,5,6,7}));
    Fixture f;const auto *original=&f.mesh.GetPointValues();f.Build();CHECK(f.product.glyphs.size()==2 && f.part.regions.size()==1);
    const auto &region=f.part.regions[0];CHECK(region.kind==AlignmentSceneRegionKind::VertexCandidates && region.sourceCount==9 && region.drawnMarkers==3);
    auto marks=f.product.glyphs.back().data;CHECK(marks->GetNumberOfPoints()==3 && marks->GetPoint(0)[0]==10 && marks->GetPoint(1)[0]==13 && marks->GetPoint(2)[0]==16);
    CHECK(&f.mesh.GetPointValues()==original && f.plan->geometries[0].fit.region.vertexIds.size()==9 && f.product.glyphs[0].data->GetNumberOfPoints()==1);
    Fixture bounds;bounds.plan->geometries[0].fit.region.vertexIds.clear();bounds.plan->geometries[0].fit.region.targetBounds=std::array<double,6>{10,12,20,22,30,32};bounds.Build(0);
    CHECK(bounds.part.regions[0].kind==AlignmentSceneRegionKind::TargetBounds && !bounds.part.regions[0].sourceCount && bounds.part.regions[0].drawnMarkers==0);
    auto box=bounds.product.glyphs.back().data;CHECK(box->GetNumberOfPoints()==8 && box->GetNumberOfLines()==12 && box->GetPoint(0)[0]==10 && box->GetPoint(7)[2]==32);
    Fixture section;section.plan->geometries[0].section=InspectionSection{};auto trace=std::make_shared<InspectionSectionTrace>();trace->sourcePlaneNormal={0,0,1};
    trace->samples.assign(8,InspectionSectionSample{0,{.2,.3,.5},.25});section.report->geometries[0].section=trace;section.Build(3);
    auto samples=section.product.glyphs.back().data;CHECK(samples->GetNumberOfPoints()==3 && std::abs(samples->GetPoint(0)[0]-10.3)<1e-12 && samples->GetPoint(0)[1]==.5 && samples->GetPoint(0)[2]==.75);
    CHECK(section.part.regions[0].kind==AlignmentSceneRegionKind::SectionSamples && section.part.regions[0].sourceCount==8 && section.part.regions[0].drawnMarkers==3 && trace->samples.size()==8);
    Fixture failed;failed.report->geometries[0].status=AlignmentStatus::Degenerate;failed.Build();CHECK(failed.product.glyphs.size()==1 && failed.product.glyphs[0].data->GetNumberOfVerts()==3);
}
void Render(const char *path) {
    Fixture f;f.plan->geometries.clear();f.report->geometries.clear();
    const std::array<AlignmentGeometryKind,4> kinds{AlignmentGeometryKind::Plane,AlignmentGeometryKind::Circle,AlignmentGeometryKind::Sphere,AlignmentGeometryKind::Cylinder};
    for(std::size_t i=0;i<kinds.size();++i){InspectionGeometrySpec spec;spec.source=InspectionGeometrySource::NominalDefinition;spec.fit.kind=kinds[i];f.plan->geometries.push_back(spec);
        InspectionGeometry value;value.status=AlignmentStatus::FullyDetermined;value.source=spec.source;value.fitted.kind=kinds[i];value.fitted.radius=1;value.alignedCenter={3.0*i,0,0};value.alignedDirection={0,0,1};f.report->geometries.push_back(value);}
    f.plan->items[0].measured.first.geometry=2;f.product.targetToModel=alignmentIdentity;f.Build();CHECK(f.product.glyphs.size()==2 && f.part.regions.empty());
    auto product=std::make_shared<AlignmentSceneProduct>(std::move(f.product));auto overlay=std::make_shared<AlignmentSceneOverlay>(product);auto renderer=vtkSmartPointer<vtkRenderer>::New();overlay->AttachRenderer(renderer);
    auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);window->SetSize(800,300);window->SetMultiSamples(0);window->AddRenderer(renderer);
    renderer->SetBackground(.025,.025,.025);auto *camera=renderer->GetActiveCamera();camera->ParallelProjectionOn();camera->SetPosition(12,-14,20);camera->SetFocalPoint(4,0,0);renderer->ResetCamera();
    window->Render();window->WaitForCompletion();auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New();capture->SetInput(window);capture->ReadFrontBufferOff();capture->Update();
    auto *pixels=vtkUnsignedCharArray::SafeDownCast(capture->GetOutput()->GetPointData()->GetScalars());CHECK(pixels);std::size_t selected=0,normal=0;
    for(vtkIdType i=0;i<pixels->GetNumberOfTuples();++i){const auto *p=pixels->GetPointer(i*pixels->GetNumberOfComponents());if(p[0]>220 && p[1]>160 && p[2]<80)++selected;if(p[0]<120 && p[1]>220 && p[2]>160)++normal;}
    CHECK(selected>30 && normal>30);if(path){auto writer=vtkSmartPointer<vtkPNGWriter>::New();writer->SetFileName(path);writer->SetInputConnection(capture->GetOutputPort());writer->Write();}
    overlay->DetachRenderer(renderer);std::cout<<"INSPECTION_GLYPHS plane circle sphere cylinder; selected sphere pixels="<<selected<<" other="<<normal<<'\n';
}
}
void TestInspectionSceneGeometry(const char *path){Semantics();Render(path);}
