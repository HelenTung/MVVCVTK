#include "AlignmentDeviationOverlay.h"
#include "AlignmentDeviationColors.h"
#include "AlignmentReportDisplay.h"
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
#include <vtkScalarBarActor.h>
#include <vtkRenderWindow.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowToImageFilter.h>
#include <iostream>

namespace {
int failures=0;
void Check(bool condition,const char *message) { if (!condition) { ++failures; std::cerr<<"FAILED: "<<message<<'\n'; } }
DataRevisionRef Ref(unsigned char id) { DataRevisionRef r; r.entityId.bytes[0]=id; r.generation=1; return r; }
void Colors() {
    AlignmentDeviationDisplay display;display.rangeMode=AlignmentRangeMode::Manual;display.range={0,1};display.palette=AlignmentColorMode::HueLoop;
    const AlignmentDeviationColors::Ramp hue(display,{});
    Check(hue.ColorAt(0)==AlignmentDeviationColors::Color{214,75,75} && hue.ColorAt(1.0/3)==AlignmentDeviationColors::Color{75,214,75}
        && hue.ColorAt(2.0/3)==AlignmentDeviationColors::Color{75,75,214} && hue.ColorAt(1)==hue.ColorAt(0),"HueLoop exact anchors");
    display.palette=AlignmentColorMode::Diverging;
    const AlignmentDeviationColors::Ramp diverging(display,{});
    Check(diverging.ColorAt(0)==AlignmentDeviationColors::Color{75,75,214} && diverging.ColorAt(.5)==AlignmentDeviationColors::Color{224,224,224}
        && diverging.ColorAt(1)==AlignmentDeviationColors::Color{214,75,75},"diverging signed-deviation preset has a neutral midpoint");
    display.useToleranceColors=true;
    const AlignmentDeviationColors::Ramp narrow(display,std::array<double,2>{.50001,.50002});
    Check(narrow.ColorAt(.50001)==AlignmentDeviationColors::Color{60,180,95} && narrow.ColorAt(.50002)==AlignmentDeviationColors::Color{60,180,95}
        && narrow.ColorAt(.5)==AlignmentDeviationColors::Color{163,74,199},"inclusive tolerance and narrow range use exact classifications");
    Check(narrow.ColorAt(-1)==AlignmentDeviationColors::Color{163,74,199} && narrow.ColorAt(2)==AlignmentDeviationColors::Color{214,74,166},"tolerance exterior legend swatches match model classes");
    for(double v:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()})
        Check(narrow.ColorAt(v)==AlignmentDeviationColors::invalidColor,"all non-finite data values are invalid");
    display.useToleranceColors=false;display.segments={{0,.50001,AlignmentColorMode::Constant,{1,0,0}},
        {.50001,.50002,AlignmentColorMode::Constant,{0,1,0}},{.50002,1,AlignmentColorMode::Constant,{0,0,1}}};
    const AlignmentDeviationColors::Ramp exact(display,{});
    Check(exact.ColorAt(.50001)==AlignmentDeviationColors::Color{0,255,0} && exact.ColorAt(.50002)==AlignmentDeviationColors::Color{0,0,255}
        && exact.ColorAt(1)==AlignmentDeviationColors::Color{0,0,255},"ordinary segments are left-inclusive and last endpoint-inclusive");
    display.segments={{std::nullopt,.5,AlignmentColorMode::Constant,{1,0,0}},
        {.5,.8,AlignmentColorMode::Gradient,{0,1,0},{0,0,1}},
        {.8,std::nullopt,AlignmentColorMode::Constant,{1,1,0}}};
    const AlignmentDeviationColors::Ramp complete(display,{});
    Check(complete.ColorAt(-100)==AlignmentDeviationColors::Color{255,0,0}
        && complete.ColorAt(.5)==AlignmentDeviationColors::Color{0,255,0}
        && complete.ColorAt(.8)==AlignmentDeviationColors::Color{0,0,255}
        && complete.ColorAt(100)==AlignmentDeviationColors::Color{255,255,0},"complete ramp owns both constant unbounded tails and finite endpoints");
    Check(complete.GetParameters().belowColor==std::array<double,3>{1,0,0}
        && complete.GetParameters().aboveColor==std::array<double,3>{1,1,0},"tail fields resolve to the same configuration as explicit unbounded segments");
    const auto shifted=AnalysisColorStyle::BuildRamp({2,3},complete.GetParameters());
    Check(AnalysisColorStyle::GetMappedColor(*shifted,complete.GetParameters(),1)[0]==1
        && AnalysisColorStyle::GetMappedColor(*shifted,complete.GetParameters(),4)[1]==1,"changing finite automatic bounds retains the complete tails");

}
void EffectiveRange() {
    auto field=std::make_shared<AlignmentDeviationField>();
    field->samples.resize(4);field->samples[0].state=field->samples[1].state=AlignmentDeviationState::Valid;
    field->samples[0].value=-.002;field->samples[1].value=.006;
    field->samples[2].state=AlignmentDeviationState::TooFar;field->samples[2].value=-100;
    field->samples[3].state=AlignmentDeviationState::NotSelected;field->samples[3].value=std::numeric_limits<double>::quiet_NaN();
    AlignmentDeviationSnapshot snapshot;snapshot.result=Ref(9);snapshot.forward=field;
    auto params=std::make_shared<AlignmentDeviationRequest>();params->metric=AlignmentDeviationMetric::NominalNormalProjection;
    params->tolerance=std::array<double,2>{-.05,.05};snapshot.parameters=params;
    AlignmentDeviationDisplay automatic;automatic.result=snapshot.result;
    const auto resolved=AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot);
    Check(resolved && resolved->range==std::array<double,2>{-.002,.006} && !resolved->useToleranceColors,
          "default range uses only valid samples and retains continuous HueLoop within tolerance");
    const auto frozen=AlignmentReportDisplay::BuildDeviation(automatic,snapshot,{});
    Check(resolved && frozen.mappingRange==std::optional<std::array<double,2>>(resolved->range) && frozen.legendRgb==
          AlignmentDeviationColors::GetLegend(*resolved,params->tolerance),"frozen report stores resolved finite range and matching legend");
    if(resolved) {
        const AlignmentDeviationColors::Ramp ramp(*resolved,params->tolerance);
        Check(ramp.ColorAt(-.002)!=ramp.ColorAt(.002) && ramp.ColorAt(.002)!=AlignmentDeviationColors::Color{60,180,95},
              "effective range maps variation even when the whole field lies in the inspection tolerance");
    }
    auto fixed=automatic;fixed.rangeMode=AlignmentRangeMode::Manual;fixed.range={-1,1};
    Check(AlignmentDeviationColors::GetResolvedDisplay(fixed,snapshot)->range==fixed.range,"explicit clipping range is preserved");
    automatic.useToleranceColors=true;
    const auto tolerance=AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot);
    Check(tolerance && tolerance->range==std::array<double,2>{-.05,.05},"explicit tolerance colors include the full tolerance range");
    automatic.useToleranceColors=false;field->samples[1].value=field->samples[0].value;
    const auto constant=AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot);
    Check(constant && constant->range==std::array<double,2>{-.002,-.002},"constant field gets finite numerical LUT padding");
    field->samples[0].value=field->samples[1].value=0;params->metric=AlignmentDeviationMetric::UnsignedDistance;
    const auto zero=AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot);
    Check(zero && zero->range==std::array<double,2>{0,0},"constant unsigned zero keeps a nonnegative range");
    field->samples[1].value=std::numeric_limits<double>::infinity();
    Check(AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot)->range==std::array<double,2>{0,0},"non-finite data excluded from actual range");
    field->samples[0].state=field->samples[1].state=AlignmentDeviationState::InvalidSource;
    Check(bool(AlignmentDeviationColors::GetResolvedDisplay(automatic,snapshot)),"all-invalid field remains displayable with no numerical legend");
    fixed.range={0,1};Check(bool(AlignmentDeviationColors::GetResolvedDisplay(fixed,snapshot)),"explicit range can display an all-gray field");
    fixed.range[1]=std::numeric_limits<double>::quiet_NaN();
    Check(!AlignmentDeviationColors::GetResolvedDisplay(fixed,snapshot),"partial automatic range sentinel is rejected");
}
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
    field->originalVertexCount=12;
    auto poly=vtkSmartPointer<vtkPolyData>::New(); poly->SetPoints(points); poly->SetPolys(triangles);
    auto originalScalars=vtkSmartPointer<vtkDoubleArray>::New(); originalScalars->SetNumberOfTuples(12); originalScalars->FillValue(42);
    poly->GetPointData()->SetScalars(originalScalars);
    auto node=std::make_shared<DataRevision>(); node->self=Ref(1); node->type=DataTypes::surfaceMesh;
    auto source=std::make_shared<const VtkSurfaceMeshView>(VtkSurfaceMeshView{node,poly});
    AlignmentDeviationSnapshot result; result.result=Ref(2); result.sourceMesh=Ref(1); result.forward=field;
    auto parameters=std::make_shared<AlignmentDeviationRequest>(); parameters->metric=AlignmentDeviationMetric::NominalNormalProjection;
    parameters->tolerance=std::array<double,2>{-.1,.1}; result.parameters=parameters;
    AlignmentDeviationDisplay automatic;automatic.result=result.result;
    const auto automaticProduct=AlignmentDeviationOverlay::Build(source,result,automatic);
    Check(automaticProduct->lookup->GetRange()[0]==-2 && automaticProduct->lookup->GetRange()[1]==2,
          "default live overlay resolves actual valid range and excludes the TooFar patch");
    AlignmentDeviationDisplay display;display.rangeMode=AlignmentRangeMode::Manual;display.palette=AlignmentColorMode::HueLoop; display.result=result.result; display.range={-1,1}; display.selectedVertex=4;display.useToleranceColors=true;
    auto product=AlignmentDeviationOverlay::Build(source,result,display);
    Check(product->colored!=poly && product->colored->GetPoints()==points && product->colored->GetPolys()==triangles,
          "display shell shares exact original positions and topology");
    Check(poly->GetPointData()->GetScalars()==originalScalars && originalScalars->GetValue(0)==42,
          "display never modifies source point data");
    auto *colors=vtkUnsignedCharArray::SafeDownCast(product->colored->GetPointData()->GetScalars());
    const std::array<std::array<unsigned char,3>,4> expected{{{163,74,199},{60,180,95},{214,74,166},{128,128,128}}};
    for (std::size_t i=0;i<4;++i) {
        unsigned char color[3]{}; colors->GetTypedTuple(static_cast<vtkIdType>(3*i),color);
        Check(std::equal(expected[i].begin(),expected[i].end(),color),"shared ramp classifies below-range, tolerance, above-range and invalid");
    }
    auto mixed=std::make_shared<AlignmentDeviationField>(*field);mixed->samples[0].value=-.5;mixed->samples[1].value=0;mixed->samples[2].value=.5;
    auto mixedResult=result;mixedResult.forward=mixed;const auto mixedDisplay=AlignmentDeviationOverlay::Build(source,mixedResult,display);
    auto* categorical=vtkUnsignedCharArray::SafeDownCast(mixedDisplay->surface->GetCellData()->GetScalars());
    Check(mixedDisplay->usesCellColors && categorical && categorical->GetPointer(0)[0]==AlignmentDeviationColors::mixedToleranceColor[0]
        && mixedDisplay->samplePoints->GetPoints()==points,"categorical colors use existing samples on unchanged geometry");
    Check(mixedDisplay->samplePoints->GetPointData()->GetScalars()->GetTuple3(1)[1]==180,"actual tolerance sample is green without RGB category interpolation");
    auto classified=std::make_shared<AlignmentDeviationField>(*field);
    const auto checkClass=[&](std::array<double,3> values,unsigned char category,AlignmentDeviationColors::Color expected) {
        for(int i=0;i<3;++i){classified->samples[i].value=values[i];classified->samples[i].state=AlignmentDeviationState::Valid;}
        auto snapshot=result;snapshot.forward=classified;
        const auto data=AlignmentDeviationOverlay::Build(source,snapshot,display);
        auto* types=vtkUnsignedCharArray::SafeDownCast(data->surface->GetCellData()->GetArray("metrology.deviation.cell.class"));
        auto* rgb=vtkUnsignedCharArray::SafeDownCast(data->surface->GetCellData()->GetScalars());unsigned char color[3];rgb->GetTypedTuple(0,color);
        Check(types&&types->GetValue(0)==category&&std::equal(expected.begin(),expected.end(),color),"triangle tolerance classification preserves both extrema");
    };
    const AlignmentDeviationColors::Ramp ramp(display,parameters->tolerance);
    checkClass({.01,.01,.20},2,ramp.ColorAt(.20));
    checkClass({-.20,.01,.01},1,ramp.ColorAt(-.20));
    checkClass({-.20,.01,.20},3,AlignmentDeviationColors::mixedToleranceColor);
    checkClass({-.1,0,.1},0,ramp.ColorAt(0));
    auto continuousField=std::make_shared<AlignmentDeviationField>(*field);
    for(auto& sample:continuousField->samples)sample.state=AlignmentDeviationState::Valid;
    auto continuousResult=result;continuousResult.forward=continuousField;
    const auto continuous=AlignmentDeviationOverlay::Build(source,continuousResult,automatic);
    Check(!continuous->usesCellColors&&!continuous->samplePoints&&!continuous->colored->GetPointData()->GetScalars(),
        "continuous display holds no hidden sample topology or RGB array");
    const auto categoricalFromContinuous=AlignmentDeviationOverlay::Build(source,continuousResult,display,continuous.get());
    const auto continuousAgain=AlignmentDeviationOverlay::Build(source,continuousResult,automatic,categoricalFromContinuous.get());
    Check(categoricalFromContinuous->samplePoints&&categoricalFromContinuous->samplePoints->GetNumberOfVerts()==12&&!continuousAgain->samplePoints,
        "mode switches create categorical sample connectivity only when required");
    const auto recast=AlignmentDeviationOverlay::Build(source,mixedResult,display,mixedDisplay.get());
    Check(recast->samplePoints->GetVerts()==mixedDisplay->samplePoints->GetVerts() && recast->surface->GetPolys()==triangles,
        "recolor reuses both original triangles and sample connectivity");
    Check(field->samples[6].value==2 && field->samples[9].value==4,"display clipping leaves numerical deviations unchanged");
    Check(product->lookup->GetRange()[0]==-1 && product->lookup->GetRange()[1]==1,"legend uses requested fixed range");
    Check(product->marker && product->marker->GetNumberOfPoints()==1 &&
          product->marker->GetPoint(0)[0]==poly->GetPoint(4)[0],"selection marker uses original vertex position");
    auto renderer=vtkSmartPointer<vtkRenderer>::New();
    auto overlay=std::make_shared<AlignmentDeviationOverlay>(product);
    overlay->AttachRenderer(renderer); overlay->AttachRenderer(renderer);
    auto *props=renderer->GetViewProps(); props->InitTraversal();
    while(auto *prop=props->GetNextProp()) if(auto *legend=vtkScalarBarActor::SafeDownCast(prop))
        Check(legend->GetLookupTable()==product->lookup && legend->GetUseCustomLabels(),"scalar bar reuses surface mapping and shared label mechanism");
    Check(renderer->GetViewProps()->GetNumberOfItems()==4,"repeat attachment does not duplicate surface, sample colors, legend or marker");
    vtkActor *surface=nullptr;
    auto *actors=renderer->GetActors(); actors->InitTraversal();
    while (auto *actor=actors->GetNextActor()) {
        const auto mapper=vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
        if (mapper && mapper->GetInput()==product->surface) surface=actor;
    }
    FeatureOverlayState state; state.modelToWorld[3]=1; state.modelToWorld[7]=2; state.modelToWorld[11]=3;
    overlay->SetOverlayState(state);
    Check(surface && surface->GetUserMatrix() && surface->GetUserMatrix()->GetElement(2,3)==3,
          "surface follows host model transform without moving source vertices");
    auto other=vtkSmartPointer<vtkRenderer>::New(); overlay->AttachRenderer(other);
    Check(renderer->GetViewProps()->GetNumberOfItems()==0 && other->GetViewProps()->GetNumberOfItems()==4,
          "renderer transfer removes all props from previous renderer");
    overlay->DetachRenderer(renderer);
    Check(other->GetViewProps()->GetNumberOfItems()==4,"detaching wrong renderer leaves active props intact");
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
        for(std::size_t j=0;j<expected.size();++j)
            if(std::equal(expected[j].begin(),expected[j].end(),p)) ++seen[j];
    }
    for (const auto count:seen) Check(count>30,"offscreen pixels contain each intended surface classification");
    if (imagePath) { auto writer=vtkSmartPointer<vtkPNGWriter>::New(); writer->SetFileName(imagePath); writer->SetInputConnection(capture->GetOutputPort()); writer->Write(); }
    auto emptyField=std::make_shared<AlignmentDeviationField>(*field);
    for(auto& sample:emptyField->samples)sample.state=AlignmentDeviationState::InvalidSource;
    auto emptyResult=result;emptyResult.forward=emptyField;
    auto emptyDisplay=automatic;emptyDisplay.belowColor={0,1,1};emptyDisplay.aboveColor={1,1,0};
    const auto emptyProduct=AlignmentDeviationOverlay::Build(source,emptyResult,emptyDisplay);
    Check(!emptyProduct->actualRange && !emptyProduct->hasValues,"all-invalid display does not fabricate an actual range");
    overlay->SetProduct(emptyProduct);
    for(int height:{300,96}) {
        window->SetSize(800,height);window->Render();window->WaitForCompletion();capture->Modified();capture->Update();
        auto* emptyPixels=vtkUnsignedCharArray::SafeDownCast(capture->GetOutput()->GetPointData()->GetScalars());
        std::size_t belowPixels=0,abovePixels=0;
        if(emptyPixels)for(vtkIdType i=0;i<emptyPixels->GetNumberOfTuples();++i) {
            const auto* p=emptyPixels->GetPointer(i*emptyPixels->GetNumberOfComponents());
            if(p[0]==0 && p[1]==255 && p[2]==255)++belowPixels;
            if(p[0]==255 && p[1]==255 && p[2]==0)++abovePixels;
        }
        Check(belowPixels>10 && abovePixels>10,"all-invalid full and compact legends retain both configured unbounded tails");
        if(imagePath) {
            const auto path=std::string(imagePath)+".empty-"+std::to_string(height)+".png";
            auto writer=vtkSmartPointer<vtkPNGWriter>::New();writer->SetFileName(path.c_str());writer->SetInputConnection(capture->GetOutputPort());writer->Write();
        }
    }
    overlay->DetachRenderer(renderer);
    Check(renderer->GetViewProps()->GetNumberOfItems()==0,"detach releases visible surface, legend and marker");
    AlignmentDeviationDisplay huge=display;huge.range={0,1e308};huge.useToleranceColors=false;
    Check(AlignmentDeviationOverlay::GetDisplayValid(huge,result),"finite wide colorbar is valid");
    const auto hugeLegend=AlignmentDeviationColors::GetLegend(huge,{});
    Check(hugeLegend[128]==AlignmentDeviationColors::Color{75,214,214},"wide range retains finite HueLoop midpoint");
    display.range={0,0}; Check(!AlignmentDeviationOverlay::GetDisplayValid(display,result),"zero-width color range rejected");
    display.range={-.01,.01}; Check(!AlignmentDeviationOverlay::GetDisplayValid(display,result),"tolerance axis must contain both finite classification boundaries");
}
} // namespace
int main(int argc,char **argv) {
    try {
        if(argc>1 && std::string(argv[1])=="--colors-only") { Colors();EffectiveRange(); }
        else Display(argc>1 ? argv[1] : nullptr);
    }
    catch (const std::exception &e) { ++failures; std::cerr<<e.what()<<'\n'; }
    return failures ? 1 : 0;
}
