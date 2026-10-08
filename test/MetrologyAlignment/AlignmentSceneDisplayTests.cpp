#include "AlignmentSceneDisplay.h"
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkDoubleArray.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#define CHECK(v) do {if(!(v))throw std::runtime_error(#v);}while(false)
void Run(const char *file) {
    auto points=vtkSmartPointer<vtkPoints>::New();points->SetDataTypeToDouble();points->InsertNextPoint(0,0,0);points->InsertNextPoint(1,0,0);points->InsertNextPoint(0,1,0);
    auto triangles=vtkSmartPointer<vtkCellArray>::New();const vtkIdType ids[]{0,1,2};triangles->InsertNextCell(3,ids);
    auto mesh=vtkSmartPointer<vtkPolyData>::New();mesh->SetPoints(points);mesh->SetPolys(triangles);
    auto scalars=vtkSmartPointer<vtkDoubleArray>::New();scalars->SetNumberOfTuples(3);scalars->FillValue(42);mesh->GetPointData()->SetScalars(scalars);
    auto source=std::make_shared<const VtkSurfaceMeshView>(VtkSurfaceMeshView{{},mesh});const auto modified=mesh->GetMTime();
    auto product=std::make_shared<AlignmentSceneProduct>();
    for(int i=0;i<3;++i){AlignmentSceneSurface surface;surface.mesh=source;surface.localToModel[3]=2*i;surface.color[i]=1;product->surfaces.push_back(surface);}
    auto renderer=vtkSmartPointer<vtkRenderer>::New();auto overlay=std::make_shared<AlignmentSceneOverlay>(product);overlay->AttachRenderer(renderer);
    auto otherRenderer=vtkSmartPointer<vtkRenderer>::New();auto other=std::make_shared<AlignmentSceneOverlay>(product);other->AttachRenderer(otherRenderer);
    std::vector<vtkActor *> actors;auto *collection=renderer->GetActors();collection->InitTraversal();while(auto *actor=collection->GetNextActor())actors.push_back(actor);
    CHECK(actors.size()==3 && actors[0]->GetMapper()==actors[1]->GetMapper() && actors[1]->GetMapper()==actors[2]->GetMapper());
    auto *mapper=vtkPolyDataMapper::SafeDownCast(actors[0]->GetMapper());CHECK(mapper && mapper->GetInput()==mesh && !mapper->GetScalarVisibility());
    FeatureOverlayState state;state.modelToWorld={0,-1,0,10,1,0,0,2,0,0,1,0,0,0,0,1};overlay->SetOverlayState(state);
    for(int i=0;i<3;++i){CHECK(actors[i]->GetUserMatrix()->GetElement(0,3)==10);CHECK(actors[i]->GetUserMatrix()->GetElement(1,3)==2+2*i);}
    auto *otherActors=otherRenderer->GetActors();otherActors->InitTraversal();CHECK(otherActors->GetNextActor()->GetUserMatrix()->GetElement(0,3)==0);
    state.modelToWorld=alignmentIdentity;overlay->SetOverlayState(state);
    auto window=vtkSmartPointer<vtkRenderWindow>::New();window->SetOffScreenRendering(1);window->SetSize(720,220);window->SetMultiSamples(0);window->AddRenderer(renderer);
    renderer->SetBackground(.02,.02,.02);renderer->GetActiveCamera()->ParallelProjectionOn();renderer->ResetCamera();window->Render();window->WaitForCompletion();
    const auto start=std::chrono::steady_clock::now();for(int i=0;i<10;++i){window->Render();window->WaitForCompletion();}
    std::cout<<"SCENE_DISPLAY synthetic_instances=3 unique_meshes=1 triangles=1 mean_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/10<<'\n';
    auto capture=vtkSmartPointer<vtkWindowToImageFilter>::New();capture->SetInput(window);capture->ReadFrontBufferOff();capture->Update();
    auto *pixels=vtkUnsignedCharArray::SafeDownCast(capture->GetOutput()->GetPointData()->GetScalars());CHECK(pixels);std::array<std::size_t,3> seen{};
    for(vtkIdType i=0;i<pixels->GetNumberOfTuples();++i){const auto *color=pixels->GetPointer(i*pixels->GetNumberOfComponents());
        for(int channel=0;channel<3;++channel)if(color[channel]>220 && color[(channel+1)%3]<40 && color[(channel+2)%3]<40)++seen[channel];}
    for(auto count:seen)CHECK(count>30);
    CHECK(mesh->GetMTime()==modified && mesh->GetPoints()==points && mesh->GetPolys()==triangles && mesh->GetPointData()->GetScalars()==scalars && scalars->GetValue(0)==42);
    if(file){auto writer=vtkSmartPointer<vtkPNGWriter>::New();writer->SetFileName(file);writer->SetInputConnection(capture->GetOutputPort());writer->Write();}
    overlay->DetachRenderer(renderer);CHECK(renderer->GetViewProps()->GetNumberOfItems()==0);CHECK(otherRenderer->GetViewProps()->GetNumberOfItems()==3);
    other->DetachRenderer(otherRenderer);CHECK(otherRenderer->GetViewProps()->GetNumberOfItems()==0);
}
void TestInspectionSceneGeometry(const char *path);
int main(int argc,char **argv){try{Run(argc>1 ? argv[1] : nullptr);TestInspectionSceneGeometry(argc>2 ? argv[2] : nullptr);return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
