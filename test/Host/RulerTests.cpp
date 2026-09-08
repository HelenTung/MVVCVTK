#include "Render/Internal/RulerMetrics.h"
#include "Render/Internal/RulerOverlay.h"
#include "Host/VtkAppHostSession.h"
#include "Host/Types/HostRequestTypes.h"

#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkPNGWriter.h>
#include <vtkMatrix4x4.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <vtkWindowToImageFilter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
bool Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    return value;
}
bool Near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance * std::max({ 1.0, std::abs(a), std::abs(b) });
}
RulerInput GetInput() {
    RulerInput input;
    input.hasData = input.isVisible = true;
    input.geometry.extent = { -20,180, 30,130, -10,30 };
    input.geometry.dimensions = { 201,101,41 };
    input.geometry.spacing = { 0.02,0.02,0.05 };
    input.geometry.origin = { 11, -8, 24 };
    input.geometry.direction = { 0,-1,0, 1,0,0, 0,0,1 };
    input.dataRevision.entityId.bytes[0] = 1;
    input.dataRevision.generation = 1;
    input.bindingRevision = 7;
    return input;
}
void TestMetrics() {
    auto input = GetInput();
    Check(RulerMetrics::GetGeometryValid(input.geometry), "anisotropic rotated nonzero extent geometry");
    std::array<double,9> inverse{};
    Check(RulerMetrics::GetInverse(input.modelToWorld, inverse), "identity inverse");
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(input.geometry.extent.data());
    image->SetSpacing(input.geometry.spacing.data());
    image->SetOrigin(input.geometry.origin.data());
    image->SetDirectionMatrix(input.geometry.direction.data());
    const std::array<std::array<double,3>,3> indices{ {{100,0,0},{0,0,20},{30,40,0}} };
    const std::array<double,3> expected{ 2,1,1 };
    double start[3]{};
    const double zero[3]{};
    image->TransformContinuousIndexToPhysicalPoint(zero,start);
    for (std::size_t index = 0; index < indices.size(); ++index) {
        double point[3]{};
        image->TransformContinuousIndexToPhysicalPoint(indices[index].data(),point);
        Check(Near(RulerMetrics::GetLength(inverse, {point[0]-start[0],point[1]-start[1],point[2]-start[2]}),expected[index]), "physical delta without repeated spacing");
    }
    auto matrix = input.modelToWorld;
    matrix = { 0,-3,0,20, 2,1,0,-10, 0,0,4,8, 0,0,0,1 };
    Check(RulerMetrics::GetInverse(matrix,inverse), "rotation shear nonuniform affine inverse");
    Check(Near(RulerMetrics::GetLength(inverse, {-12,10,0}),5.0), "affine display transform preserves 3-4-5 length");
    matrix[8] = matrix[9] = matrix[10] = 0;
    Check(!RulerMetrics::GetInverse(matrix,inverse), "singular rejected");
    matrix = { 1,1,0,0, 0,1e-14,0,0, 0,0,1,0, 0,0,0,1 };
    Check(!RulerMetrics::GetInverse(matrix,inverse), "near collinear transform rejected");
    matrix = input.modelToWorld; matrix[12] = 0.01;
    Check(!RulerMetrics::GetInverse(matrix,inverse), "projective model rejected");
    RulerParams params;
    params.targetPixels = 200;
    params.unit = RulerUnit::Millimeter;
    auto state = RulerMetrics::BuildState(0.05,500,params,0);
    Check(state.status == RulerStatus::Visible && Near(state.lengthMm,10)
        && Near(state.lengthPixels,200) && state.label == "10 mm", "parallel numeric oracle");
    state = RulerMetrics::BuildState(0.051,500,params,10);
    Check(Near(state.lengthMm,10) && Near(state.lengthPixels,10/0.051), "hysteresis never freezes line length");
    params.unit = RulerUnit::Micrometer;
    state = RulerMetrics::BuildState(0.001,500,params,0);
    Check(state.label == "200 um" && Near(state.lengthMm,0.2), "display unit only");
    Check(RulerMetrics::BuildState(0,500,params,0).status == RulerStatus::InvalidScale, "zero scale rejected");
    Check(RulerMetrics::BuildState(std::numeric_limits<double>::quiet_NaN(),500,params,0).status == RulerStatus::InvalidScale, "NaN rejected");
    Check(RulerMetrics::BuildState(0.01,20,params,0).status == RulerStatus::ViewportTooSmall, "tiny viewport hidden");
    for (int exponent = -10; exponent <= 9; ++exponent) {
        auto value = RulerMetrics::BuildState(std::pow(10.,exponent),500,params,0);
        Check(value.status == RulerStatus::Visible && Near(value.lengthPixels * std::pow(10.,exponent),value.lengthMm), "scale decades consistent");
    }
    params.color[1] = -0.1;
    Check(!RulerMetrics::GetParamsValid(params), "invalid color rejected");
}

vtkSmartPointer<vtkImageData> Capture(vtkRenderWindow* window,int scale,const char* name) {
    auto capture = vtkSmartPointer<vtkWindowToImageFilter>::New();
    capture->SetInput(window);
    capture->SetScale(scale);
    capture->ReadFrontBufferOff();
    capture->Update();
    auto output = vtkSmartPointer<vtkImageData>::New();
    output->DeepCopy(capture->GetOutput());
    if (name) {
        auto writer = vtkSmartPointer<vtkPNGWriter>::New();
        writer->SetFileName(name);
        writer->SetInputData(output);
        writer->Write();
    }
    return output;
}

void CheckLine(vtkImageData* image, int y, double length) {
    const int width = image->GetDimensions()[0];
    int run = 0, maxRun = 0, longRuns = 0;
    for (int x = 0; x <= width; ++x) {
        const bool white = x < width && image->GetScalarComponentAsDouble(x,y,0,0)>200;
        if (white) { ++run; maxRun = std::max(maxRun,run); }
        else { if (run > 20) ++longRuns; run = 0; }
    }
    Check(longRuns == 1, "screenshot contains one unbroken ruler");
    if (!Check(std::abs(maxRun-length)<=2.0, "raster line matches metric within endpoint pixels"))
        std::cerr << "  pixels=" << maxRun << " expected=" << length << " y=" << y << '\n';
}

void TestRender() {
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1); window->SetSize(400,300); window->SetMultiSamples(0);
    auto view = vtkSmartPointer<vtkRenderer>::New();
    view->SetBackground(0,0,0); window->AddRenderer(view);
    auto* camera = view->GetActiveCamera();
    camera->ParallelProjectionOn(); camera->SetParallelScale(25);
    const int propsBefore = view->GetViewProps()->GetNumberOfItems();
    auto input = GetInput();
    RulerParams params;
    {
        RulerOverlay ruler;
        ruler.AttachRenderer(view); ruler.AttachRenderer(view);
        Check(view->GetViewProps()->GetNumberOfItems()==propsBefore+1,"one prop per view");
        ruler.SetInput(input,params); window->Render();
        auto state = ruler.GetState();
        Check(state.status==RulerStatus::Visible && Near(state.lengthMm/state.lengthPixels,50.0/300),"actual VTK parallel projection");
        auto screenshot = Capture(window,1,"ruler-screen.png");
        CheckLine(screenshot,16,ruler.GetState().lengthPixels);
        for (int scale : {2,3}) {
            screenshot = Capture(window,scale,scale==2 ? "ruler-scale2.png":"ruler-scale3.png");
            state = ruler.GetState();
            Check(Near(state.lengthMm/state.lengthPixels,50.0/(300*scale)),"tiled capture physical pixel ratio");
            CheckLine(screenshot,16,state.lengthPixels);
            // 其余 tile 的同高度位置不能再出现第二把标尺。
            for (int tile=1;tile<scale;++tile) {
                int bright = 0;
                for(int x=0;x<400*scale;++x) bright += screenshot->GetScalarComponentAsDouble(x,16+300*tile,0,0)>200 ? 1:0;
                Check(bright==0,"tiled capture does not repeat ruler");
            }
        }
        view->SetViewport(0.25,0.25,1.0,1.0); window->Render();
        state=ruler.GetState();
        Check(Near(state.lengthMm/state.lengthPixels,50.0/225),"subviewport independent height");
        screenshot=Capture(window,1,"ruler-subviewport.png");
        CheckLine(screenshot,75+16,ruler.GetState().lengthPixels);
        screenshot=Capture(window,2,"ruler-subviewport-scale2.png");
        state=ruler.GetState();
        Check(Near(state.lengthMm/state.lengthPixels,50.0/450),"subviewport plus tile scale physical ratio");
        CheckLine(screenshot,150+16,state.lengthPixels);
        view->SetViewport(0,0,1,1);
        for (auto position : {RulerPosition::BottomLeft,RulerPosition::BottomRight,
                RulerPosition::TopLeft,RulerPosition::TopRight}) {
            params.position=position; ruler.SetInput(input,params);
            screenshot=Capture(window,3,nullptr);
            Check(ruler.GetState().status==RulerStatus::Visible,"all screenshot corners visible");
            int borderPixels=0;
            for (int x=0;x<1200;++x) for(int y=885;y<900;++y)
                borderPixels+=screenshot->GetScalarComponentAsDouble(x,y,0,0)>200?1:0;
            Check(borderPixels==0,"top label fully inside output margin");
        }
        params.position=RulerPosition::BottomRight;
        input.modelToWorld[0]=2; ruler.SetInput(input,params); window->Render();
        Check(Near(ruler.GetState().lengthMm/ruler.GetState().lengthPixels,25.0/300),"model display zoom removed");
        camera->SetParallelScale(12.5); window->Render();
        Check(Near(ruler.GetState().lengthMm/ruler.GetState().lengthPixels,12.5/300),"camera direct zoom current frame");
        input.modelToWorld={0,-3,0,20, 2,1,0,-10, 0,0,4,8, 0,0,0,1};
        ruler.SetInput(input,params); window->Render();
        Check(Near(ruler.GetState().lengthMm/ruler.GetState().lengthPixels,
            (25.0/300)*std::hypot(1.0/6,1.0/3)),"rendered shear rotation translation nonuniform scale");
        camera->ParallelProjectionOff(); window->Render();
        Check(ruler.GetState().status==RulerStatus::UnsupportedProjection && ruler.GetState().label.empty(),"perspective clears stale ruler");
        camera->ParallelProjectionOn();
        auto projection=vtkSmartPointer<vtkMatrix4x4>::New(); projection->Identity();
        projection->SetElement(3,2,-1);projection->SetElement(3,3,0);
        camera->SetExplicitProjectionTransformMatrix(projection);
        camera->UseExplicitProjectionTransformMatrixOn(); window->Render();
        Check(ruler.GetState().status==RulerStatus::UnsupportedProjection,"explicit perspective cannot masquerade as parallel projection");
        camera->UseExplicitProjectionTransformMatrixOff();
        input=GetInput(); input.modelToWorld[0]=0; ruler.SetInput(input,params); window->Render();
        Check(ruler.GetState().status==RulerStatus::InvalidTransform,"singular draw hidden");
        input=GetInput(); input.mode=VizMode::SliceTop_down; camera->SetPosition(1,1,1); ruler.SetInput(input,params); window->Render();
        Check(ruler.GetState().status==RulerStatus::UnsupportedProjection,"oblique slice hidden");
        input.isVisible=false; ruler.SetInput(input,params); window->Render();
        Check(ruler.GetState().status==RulerStatus::Hidden && ruler.GetState().lengthMm==0,"visibility clears values");
        ruler.ClearInput();
        Check(ruler.GetState().status==RulerStatus::NoData,"cleared input immediate");
        ruler.DetachRenderer(); ruler.AttachRenderer(view);
        Check(view->GetViewProps()->GetNumberOfItems()==propsBefore+1,"rebind does not grow props");
    }
    Check(view->GetViewProps()->GetNumberOfItems()==propsBefore,"destruction removes own prop");
    window->Finalize();
}

bool SendRequest(VtkAppHostSession& session,HostRequest&& request,bool expected=true) {
    struct Completion { bool done=false; bool success=false; }; auto completion=std::make_shared<Completion>();
    session.SendRequestResult(std::move(request),[completion](HostResult result){completion->done=true;completion->success=result.isSucceeded;});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (!completion->done && std::chrono::steady_clock::now()<deadline) {
        session.SendUpdates(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Check(completion->done && completion->success==expected,"Host request outcome");
}
HostReloadRequest GetReload(float spacing) {
    HostReloadRequest request;
    request.geometry.dimensions={12,16,20};
    request.geometry.spacing={spacing,spacing*2,spacing*3};
    request.geometry.origin={3,-4,5};
    request.metadata.identity.datasetId="ruler-synthetic";
    request.metadata.source.kind=ImageSourceKind::Memory;
    request.metadata.source.uri="memory://ruler-synthetic";
    request.voxels.resize(12*16*20);
    for(std::size_t index=0;index<request.voxels.size();++index)request.voxels[index]=static_cast<float>(index%17);
    return request;
}
void TestHost() {
    HostSessionConfig config;
    config.driveMode=HostDriveMode::HostDriven;
    for(int index=0;index<2;++index){
        HostRenderViewConfig view;
        view.id=index==0?"primary":"slice";
        view.role=index==0?HostRenderViewRole::Primary3D:HostRenderViewRole::Auxiliary;
        view.window.viewInit.viewMode=HostRenderMode::SliceTopDown;
        view.renderWindow=vtkSmartPointer<vtkRenderWindow>::New();
        view.renderWindow->SetOffScreenRendering(1);
        view.renderWindow->SetSize(400,300);
        config.renderViews.push_back(view);
    }
    VtkAppHostSession session(std::move(config));
    if(!Check(session.BuildSession(),"Host build"))return;
    if(!SendRequest(session,GetReload(0.02f)))return;
    session.SendUpdates(); session.SendRender({{"primary","slice"}});
    HostViewTarget target; target.viewId="primary";
    auto state=session.GetRenderViewState(target);
    if(!Check(state && state->rulerState.status==HostRulerStatus::Visible,"Host physical ruler drawn")) {session.Stop();return;}
    auto descriptor=session.GetImageDescriptor();
    Check(descriptor && descriptor->dataRevision==state->rulerState.dataRevision,"ruler committed input identity");
    const auto originalRevision=state->rulerState.dataRevision;
    HostViewSetRequest unit;
    unit.targetView=target; unit.ruler=HostRulerParams{};
    unit.ruler->unit=HostRulerUnit::Micrometer;
    unit.ruler->position=HostRulerPosition::TopLeft;
    SendRequest(session,std::move(unit)); session.SendRender({{"primary"}});
    state=session.GetRenderViewState(target);
    Check(state->rulerState.label.find(" um")!=std::string::npos,"Host micrometer display");
    Check(session.GetImageDescriptor()->dataRevision==originalRevision && session.GetImageDescriptor()->spacing==descriptor->spacing,"unit change does not recalibrate data");
    HostViewSetRequest invalid;
    invalid.targetView=target;invalid.opacity=0.25;invalid.ruler=HostRulerParams{};invalid.ruler->targetPixels=-1;
    const double oldOpacity=state->material.opacity;
    SendRequest(session,std::move(invalid),false);
    state=session.GetRenderViewState(target);
    Check(state->material.opacity==oldOpacity && state->ruler.unit==HostRulerUnit::Micrometer,"invalid config atomically rejects other fields");
    auto* endpoint=session.GetRenderViewEndpoint("primary");
    const auto before=session.GetRenderViewState(target)->rulerState.dataRevision;
    SendRequest(session,GetReload(0.1f));
    // SendUpdates 提交新主图后，未绘制的标尺不能对外保留旧的 Visible 结果。
    state=session.GetRenderViewState(target);
    Check(state->dataRevision!=before && state->rulerState.status==HostRulerStatus::Pending,"new committed input awaits draw");
    session.SendRender({{"primary","slice"}});
    state=session.GetRenderViewState(target);
    Check(state->rulerState.dataRevision==state->dataRevision,"A/B switch same draw identity");
    HostSessionSetRequest calibration;calibration.spacing=std::array<double,3>{0.2,0.3,0.4};
    SendRequest(session,std::move(calibration));
    for(int i=0;i<10;++i)session.SendUpdates();
    session.SendRender({{"primary","slice"}});
    Check(session.GetRenderViewState(target)->rulerState.dataRevision==session.GetImageDescriptor()->dataRevision,"spacing calibration follows geometry revision");
    HostViewSetRequest hide;hide.targetView=target;hide.visibility=HostVisibilityParams{};hide.visibility->isRulerVisible=false;
    SendRequest(session,std::move(hide));session.SendRender({{"primary"}});
    Check(session.GetRenderViewState(target)->rulerState.status==HostRulerStatus::Hidden,"legacy visibility controls unified ruler");
    HostViewTarget other;other.viewId="slice";
    Check(session.GetRenderViewState(other)->rulerState.status==HostRulerStatus::Visible,"other view unaffected");
    auto renderer=vtkSmartPointer<vtkRenderer>(endpoint->renderer);
    Check(session.Stop(),"Host stop");
    int rulerCount=0;
    auto* props=renderer->GetViewProps();
    props->InitTraversal();
    while(auto* prop=props->GetNextProp()) rulerCount+=prop->IsA("RulerProp")?1:0;
    Check(rulerCount==0,"Stop removes the View-owned ruler prop");
}
}
int GetRulerStateFailures();
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"all";
    if(mode!="all" && mode!="metrics" && mode!="render" && mode!="host" && mode!="state") return 2;
    if(mode=="metrics"||mode=="all")TestMetrics();
    if(mode=="render"||mode=="all")TestRender();
    if(mode=="host"||mode=="all")TestHost();
    if(mode=="state"||mode=="all")failures+=GetRulerStateFailures();
    std::cout<<"Ruler "<<mode<<" failures="<<failures<<'\n';
    return failures==0?0:1;
}
