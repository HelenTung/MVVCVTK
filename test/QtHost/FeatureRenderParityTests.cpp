// 测试用途：核验色带方向、稀疏标签、正式数据只读性和变换后的轮廓。
#include "Render/GapDisplayData.h"
#include "Render/Strategies/GapOverlayStrategies.h"
#include "Render/CropGeometryOverlay.h"
#include "SurfaceOverlayStrategy.h"
#include <vtkCellData.h>
#include <vtkCubeSource.h>
#include <vtkPNGWriter.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkWindowToImageFilter.h>
#include <cstring>
#include <iostream>

namespace {
int failures=0;
void Check(bool value,const char* text) {
    std::cout<<(value?"PASS: ":"FAIL: ")<<text<<'\n'; if(!value)++failures;
}
void Capture(vtkRenderer* renderer,const char* path) {
    vtkNew<vtkRenderWindow> window;window->SetOffScreenRendering(1);window->SetSize(500,500);
    window->AddRenderer(renderer);renderer->ResetCamera();window->Render();
    vtkNew<vtkWindowToImageFilter> pixels;pixels->SetInput(window);pixels->ReadFrontBufferOff();pixels->Update();
    vtkNew<vtkPNGWriter> writer;writer->SetFileName(path);writer->SetInputConnection(pixels->GetOutputPort());writer->Write();
}
}
int main()
{
    auto thickness=AnalysisColorStyle::BuildRamp({0,5},true);
    double low[3],high[3];thickness->GetColor(0,low);thickness->GetColor(5,high);
    Check(low[0]>.9 && low[2]<.1 && high[2]>.9 && high[0]<.1,"thickness thin red and thick blue");
    vtkNew<vtkImageData> image;image->SetExtent(-3,4,5,12,2,9);image->SetSpacing(.5,1,1.5);
    image->SetOrigin(11,22,33);const double direction[9]{0,-1,0,1,0,0,0,0,1};
    image->SetDirectionMatrix(direction);image->AllocateScalars(VTK_INT,1);
    auto* original=static_cast<int*>(image->GetScalarPointer());
    std::fill_n(original,image->GetNumberOfPoints(),0);
    *static_cast<int*>(image->GetScalarPointer(-3,5,2))=60001;
    *static_cast<int*>(image->GetScalarPointer(3,10,8))=33554433;
    const std::vector<int> before(original,original+image->GetNumberOfPoints());
    VoidRegion a;a.id=60001;a.voxelCount=1;a.volumeMM3=.5f;a.bbox={-3,-3,5,5,2,2};
    VoidRegion c;c.id=33554433;c.voxelCount=1;c.volumeMM3=3.f;c.bbox={3,3,10,10,8,8};
    std::atomic<bool> stopping{false};
    const auto display=GapDisplayData::Build(image,{a,c},stopping);
    Check(display && display->labels->GetNumberOfTableValues()==3,"sparse IDs allocate by region count");
    if (display) {
        const auto* background=display->labels->MapValue(0);
        const auto* first=display->labels->MapValue(60001);
        const auto* second=display->labels->MapValue(33554433);
        Check(background[3]==0 && first[2]>200 && second[0]>200,"background transparent and exact large IDs use volume colours");
        const auto* ids=vtkIntArray::SafeDownCast(display->mesh->GetCellData()->GetScalars());
        bool hasA=false,hasC=false;
        if(ids)for(vtkIdType i=0;i<ids->GetNumberOfTuples();++i){hasA|=ids->GetValue(i)==a.id;hasC|=ids->GetValue(i)==c.id;}
        Check(hasA&&hasC,"display cells retain integer IDs beyond float32 integer precision");
        Check(std::equal(before.begin(),before.end(),original),"display construction leaves formal labels byte-identical");
        double bounds[6];display->mesh->GetBounds(bounds);
        Check(bounds[0]<6 && bounds[1]>6 && bounds[2]<20.5 && bounds[3]>20.5,
            "nonzero extent and direction preserve the boundary defect physical position");
        vtkNew<vtkRenderer> renderer;
        auto mesh=std::make_shared<GapMeshOverlayStrategy>(display);mesh->SetInputData(display->mesh);
        mesh->AttachRenderer(renderer);Capture(renderer,"RenderParity-Gap3D.png");
        mesh->DetachRenderer(renderer);Check(renderer->GetViewProps()->GetNumberOfItems()==0,"gap legend and mesh clean up together");
        auto slice=std::make_shared<GapSliceOverlayStrategy>(Orientation::Top_down,display);
        slice->SetInputData(image);slice->SetOverlayState({{6,20.5,36}});
        slice->AttachRenderer(renderer);Capture(renderer,"RenderParity-GapSlice.png");
        slice->DetachRenderer(renderer);Check(renderer->GetViewProps()->GetNumberOfItems()==0,"gap slice colour, outline and legend clean up together");
    }
    stopping=true;Check(!GapDisplayData::Build(image,{a,c},stopping),"cancelled display has no partial candidate");
    stopping=false;auto bad=a;bad.bbox[0]=-4;
    Check(!GapDisplayData::Build(image,{bad},stopping),"out-of-source bbox is rejected before allocation");
    vtkNew<vtkCubeSource> cube;cube->Update();vtkNew<vtkRenderer> renderer;
    auto surface=std::make_shared<SurfaceSliceOverlayStrategy>(std::array<double,3>{0,0,1});
    surface->SetInputData(cube->GetOutput());FeatureOverlayState state;
    state.cursor={10,20,30};state.modelToWorld={0,-2,0,10,3,0,0,20,0,0,4,30,0,0,0,1};
    surface->SetOverlayState(state);surface->AttachRenderer(renderer);
    renderer->GetViewProps()->InitTraversal();
    auto* actor=vtkActor::SafeDownCast(renderer->GetViewProps()->GetNextProp());
    auto* mapper=actor?vtkPolyDataMapper::SafeDownCast(actor->GetMapper()):nullptr;
    if(mapper)mapper->Update();bool onPlane=mapper&&mapper->GetInput()->GetNumberOfPoints()>0;
    if(onPlane)for(vtkIdType i=0;i<mapper->GetInput()->GetNumberOfPoints();++i) {
        double p[4]{0,0,0,1},world[4];mapper->GetInput()->GetPoint(i,p);
        actor->GetUserMatrix()->MultiplyPoint(p,world);onPlane&=std::abs(world[2]-30)<1e-10;
    }
    Check(onPlane,"transformed surface contour intersects the requested world plane exactly once");
    surface->DetachRenderer(renderer);
    auto crop=std::make_shared<CropGeometryOverlay>(HostRenderViewRole::TopDownSlice);
    CropOpItem operation;operation.geometryType=CropShape::Cylinder;operation.radius=2;operation.height=6;
    crop->SetOperation(operation,{-5,5,-5,5,-5,5});crop->SetOverlayState({{0,0,0}});
    crop->AttachRenderer(renderer);Capture(renderer,"RenderParity-CylinderSlice.png");
    Check(renderer->GetViewProps()->GetNumberOfItems()==2,"crop owns its slice section and dimension label");
    crop->DetachRenderer(renderer);Check(renderer->GetViewProps()->GetNumberOfItems()==0,"crop annotations leave no residue");
    return failures?1:0;
}
