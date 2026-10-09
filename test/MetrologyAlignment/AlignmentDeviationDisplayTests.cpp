#include "AlignmentDeviationOverlay.h"
#include "AlignmentDeviationColors.h"
#include "Data/DataPayloads.h"
#include <algorithm>
#include <cmath>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkDoubleArray.h>
#include <vtkImageData.h>
#include <vtkLookupTable.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <iostream>

namespace {
int failures=0;
void Check(bool condition,const char *message) { if (!condition) { ++failures; std::cerr<<"FAILED: "<<message<<'\n'; } }
DataRevisionRef Ref(unsigned char id) { DataRevisionRef r; r.entityId.bytes[0]=id; r.generation=1; return r; }
void Display(const char *imagePath) {
    auto points=vtkSmartPointer<vtkPoints>::New(); points->SetDataTypeToDouble();
    auto triangles=vtkSmartPointer<vtkCellArray>::New();
    auto field=std::make_shared<AlignmentDeviationField>();
    for (int patch=0;patch<4;++patch) {
        const double x=1.2*patch;
        points->InsertNextPoint(x,0,0); points->InsertNextPoint(x+1,0,0); points->InsertNextPoint(x,1,0);
        const vtkIdType ids[]{3*patch,3*patch+1,3*patch+2}; triangles->InsertNextCell(3,ids);
        for (int i=0;i<3;++i) {
            AlignmentDeviationSample sample; sample.value=2.0*(patch-1); sample.distance=std::abs(sample.value);
            sample.area=1.0/6; sample.state=patch==3 ? AlignmentDeviationState::TooFar : AlignmentDeviationState::Valid;
            field->samples.push_back(sample);
        }
    }
    auto poly=vtkSmartPointer<vtkPolyData>::New(); poly->SetPoints(points); poly->SetPolys(triangles);
    auto originalScalars=vtkSmartPointer<vtkDoubleArray>::New(); originalScalars->SetNumberOfTuples(12); originalScalars->FillValue(42);
    poly->GetPointData()->SetScalars(originalScalars);
    auto node=std::make_shared<DataRevision>(); node->self=Ref(1); node->type=DataTypes::surfaceMesh;
    auto source=std::make_shared<const VtkSurfaceMeshView>(VtkSurfaceMeshView{node,poly});
    AlignmentDeviationSnapshot result; result.result=Ref(2); result.sourceMesh=Ref(1); result.forward=field;
    auto parameters=std::make_shared<AlignmentDeviationRequest>(); parameters->metric=AlignmentDeviationMetric::NominalNormalProjection;
    parameters->tolerance=std::array<double,2>{-.1,.1}; result.parameters=parameters;
    AlignmentDeviationDisplay display; display.result=result.result; display.range={-1,1}; display.selectedVertex=4;
    const auto deadline=std::chrono::steady_clock::time_point::max();
    auto product=AlignmentDeviationOverlay::Build(source,result,display,1024*1024,deadline);
    Check(product->colored!=poly && product->colored->GetPoints()==points && product->colored->GetPolys()==triangles,
          "display shell shares exact original positions and topology");
    Check(poly->GetPointData()->GetScalars()==originalScalars && originalScalars->GetValue(0)==42,
          "display never modifies source point data");
    auto *colors=vtkUnsignedCharArray::SafeDownCast(product->colored->GetPointData()->GetScalars());
    const std::array<std::array<unsigned char,3>,4> expected{{{45,90,215},{60,180,95},{210,45,35},{128,128,128}}};
    for (std::size_t i=0;i<4;++i) {
        unsigned char color[3]{}; colors->GetTypedTuple(static_cast<vtkIdType>(3*i),color);
        Check(std::equal(expected[i].begin(),expected[i].end(),color),"fixed colors classify cold, in-tolerance, hot and invalid");
    }
    Check(field->samples[6].value==2 && field->samples[9].value==4,"display clipping leaves numerical deviations unchanged");
    Check(product->lookup->GetRange()[0]==-1 && product->lookup->GetRange()[1]==1,"legend uses requested fixed range");
    Check(product->marker && product->marker->GetNumberOfPoints()==1 &&
          product->marker->GetPoint(0)[0]==poly->GetPoint(4)[0],"selection marker uses original vertex position");
    auto renderer=vtkSmartPointer<vtkRenderer>::New();
    auto overlay=std::make_shared<AlignmentDeviationOverlay>(product);
    overlay->AttachRenderer(renderer); overlay->AttachRenderer(renderer);
    Check(renderer->GetViewProps()->GetNumberOfItems()==3,"repeat attachment does not duplicate surface, legend or marker");
    vtkActor *surface=nullptr;
    auto *actors=renderer->GetActors(); actors->InitTraversal();
    while (auto *actor=actors->GetNextActor()) {
        const auto mapper=vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        if (mapper && mapper->GetInput()==product->colored) surface=actor;
    }
    FeatureOverlayState state; state.modelToWorld[3]=1; state.modelToWorld[7]=2; state.modelToWorld[11]=3;
    overlay->SetOverlayState(state);
    Check(surface && surface->GetUserMatrix() && surface->GetUserMatrix()->GetElement(2,3)==3,
          "surface follows host model transform without moving source vertices");
    auto other=vtkSmartPointer<vtkRenderer>::New(); overlay->AttachRenderer(other);
    Check(renderer->GetViewProps()->GetNumberOfItems()==0 && other->GetViewProps()->GetNumberOfItems()==3,
          "renderer transfer removes all props from previous renderer");
    overlay->DetachRenderer(renderer);
    Check(other->GetViewProps()->GetNumberOfItems()==3,"detaching wrong renderer leaves active props intact");
    overlay->AttachRenderer(renderer); state.modelToWorld=alignmentIdentity; overlay->SetOverlayState(state);
    renderer->SetBackground(.04,.04,.04); renderer->GetActiveCamera()->ParallelProjectionOn(); renderer->ResetCamera();
    auto window=vtkSmartPointer<vtkRenderWindow>::New(); window->SetOffScreenRendering(1); window->SetSize(800,300);
    window->SetMultiSamples(0); window->AddRenderer(renderer); window->Render(); window->WaitForCompletion();
    const auto start=std::chrono::steady_clock::now();
    for (int i=0;i<10;++i) { window->Render(); window->WaitForCompletion(); }
    std::cout<<"DEVIATION_DISPLAY_RENDER synthetic_triangles=4 average_ms="
             <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/10<<'\n';
    auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New(); capture->SetInput(window); capture->ReadFrontBufferOff(); capture->Update();
    auto *pixels=vtkUnsignedCharArray::SafeDownCast(capture->GetOutput()->GetPointData()->GetScalars());
    std::array<std::size_t,4> seen{};
    if (pixels) for (vtkIdType i=0;i<pixels->GetNumberOfTuples();++i) {
        const auto *p=pixels->GetPointer(i*pixels->GetNumberOfComponents());
        if (p[2]>180 && p[0]<80 && p[1]<130) ++seen[0];
        if (p[1]>150 && p[0]<100 && p[2]<130) ++seen[1];
        if (p[0]>180 && p[1]<90 && p[2]<90) ++seen[2];
        if (p[0]>110 && p[0]<145 && p[0]==p[1] && p[1]==p[2]) ++seen[3];
    }
    for (const auto count:seen) Check(count>30,"offscreen pixels contain each intended surface classification");
    if (imagePath) { auto writer=vtkSmartPointer<vtkPNGWriter>::New(); writer->SetFileName(imagePath); writer->SetInputConnection(capture->GetOutputPort()); writer->Write(); }
    overlay->DetachRenderer(renderer);
    Check(renderer->GetViewProps()->GetNumberOfItems()==0,"detach releases visible surface, legend and marker");
    bool limited=false;
    try { (void)AlignmentDeviationOverlay::Build(source,result,display,1,deadline); } catch (const std::length_error &) { limited=true; }
    Check(limited,"color buffer admission enforces explicit memory budget");
    AlignmentDeviationDisplay huge=display;huge.range={0,1e308};huge.useToleranceColors=false;
    Check(AlignmentDeviationOverlay::GetDisplayValid(huge,result),"finite wide colorbar is valid");
    const auto hugeLegend=AlignmentDeviationColors::GetLegend(huge,{});
    Check(hugeLegend[128]==AlignmentDeviationColors::Color{223,140,135},"legend interpolation avoids intermediate range-times-index overflow");
    display.range={0,0}; Check(!AlignmentDeviationOverlay::GetDisplayValid(display,result),"zero-width color range rejected");
    display.range={-.01,.01}; Check(!AlignmentDeviationOverlay::GetDisplayValid(display,result),"color scale must contain tolerance interval");
}
} // namespace
int main(int argc,char **argv) {
    try { Display(argc>1 ? argv[1] : nullptr); }
    catch (const std::exception &e) { ++failures; std::cerr<<e.what()<<'\n'; }
    return failures ? 1 : 0;
}
