#include "Algorithms/CropAlgorithm.h"
#include "Algorithms/CropMeshExact.h"
#include <limits>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <cmath>
#include <iostream>

namespace {
vtkSmartPointer<vtkPolyData> Source(double scale=1)
{
    auto points=vtkSmartPointer<vtkPoints>::New();points->SetDataTypeToDouble();
    points->InsertNextPoint(-4*scale,-4*scale,0);points->InsertNextPoint(4*scale,-4*scale,0);points->InsertNextPoint(0,4*scale,0);
    auto cells=vtkSmartPointer<vtkCellArray>::New();const vtkIdType ids[3]={0,1,2};cells->InsertNextCell(3,ids);
    auto mesh=vtkSmartPointer<vtkPolyData>::New();mesh->SetPoints(points);mesh->SetPolys(cells);
    auto normals=vtkSmartPointer<vtkDoubleArray>::New();normals->SetName("Normals");normals->SetNumberOfComponents(3);normals->SetNumberOfTuples(3);
    normals->FillComponent(0,0);normals->FillComponent(1,0);normals->FillComponent(2,1);mesh->GetPointData()->SetNormals(normals);
    auto values=vtkSmartPointer<vtkDoubleArray>::New();values->SetName("linear");
    values->InsertNextValue(-12*scale);values->InsertNextValue(-4*scale);values->InsertNextValue(8*scale);mesh->GetPointData()->SetScalars(values);
    auto material=vtkSmartPointer<vtkDoubleArray>::New();material->SetName("material");material->InsertNextValue(42);mesh->GetCellData()->SetScalars(material);
    return mesh;
}
CropBuildParams Params(CropOpItem operation)
{
    CropBuildParams params;params.sourceRevision.entityId.bytes[0]=77;params.sourceRevision.generation=1;
    operation.operationIndex=1;params.operations={operation};params.nodeCount=1;
    params.meshTolerance=0.05;params.maxDepth=64;params.maxCells=1000000;params.availableRamBytes=256ULL*1024*1024;
    return params;
}
CropMaterializationCandidate Build(vtkPolyData* source,const CropBuildParams& params,const std::function<bool()>& stop={})
{
    const auto table=CropAlgorithm::BuildPredicateTable(params.operations,params.nodeCount);
    CropShaderPayload payload;payload.revision=1;payload.sourceStamp={params.sourceRevision};payload.nodeCount=params.nodeCount;payload.predicateTable=table.predicateTable;
    return CropAlgorithm::GetResult(source,params,payload,stop);
}
double Area(vtkPolyData* mesh)
{
    if(!mesh)return 0;double area=0;vtkNew<vtkIdList> ids;
    for(vtkIdType cell=0;cell<mesh->GetNumberOfCells();++cell) {
        mesh->GetCellPoints(cell,ids);if(ids->GetNumberOfIds()!=3)return -1;
        double p[3][3];for(int i=0;i<3;++i)mesh->GetPoint(ids->GetId(i),p[i]);
        const double u[3]={p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2]};
        const double v[3]={p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2]};
        const double current=0.5*std::hypot(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]);
        if(!(current>0))return -1;area+=current;
    }
    return area;
}
bool Attributes(vtkPolyData* mesh)
{
    if(!mesh||!mesh->GetPointData()->GetNormals()||!mesh->GetPointData()->GetScalars()||!mesh->GetCellData()->GetScalars())return false;
    for(vtkIdType i=0;i<mesh->GetNumberOfPoints();++i) {
        double p[3];mesh->GetPoint(i,p);
        if(std::abs(p[2])>1e-12||std::abs(mesh->GetPointData()->GetScalars()->GetComponent(i,0)-(p[0]+2*p[1]))>1e-8
            ||std::abs(mesh->GetPointData()->GetNormals()->GetComponent(i,2)-1)>1e-12)return false;
    }
    for(vtkIdType i=0;i<mesh->GetNumberOfCells();++i)if(mesh->GetCellData()->GetScalars()->GetComponent(i,0)!=42)return false;
    return true;
}
}

int GetCropMeshFailures()
{
    int failures=0;const auto check=[&](bool ok,const char* message,const CropMaterializationCandidate* result=nullptr) {
        if(!ok){++failures;std::cerr<<"Mesh: "<<message;if(result)std::cerr<<" failure="<<int(result->failureReason)<<" message="<<result->message;std::cerr<<'\n';}
    };
    using N=CropMeshExact::Number;
    bool exact=true;
    for(int exponent=-200;exponent<=200;exponent+=10) {
        const double big=std::ldexp(1.0,exponent),small=std::ldexp(1.0,exponent-100);
        for(int integer=1;integer<=31;++integer) {
            const auto product=(N(big)+N(small))*N(integer)-N(big)*N(integer);
            exact=exact&&product.Sign()>0&&(product-N(small)*N(integer)).Sign()==0;
            N sum;for(int i=0;i<16;++i)sum=sum+N(std::ldexp(1.0,-400+i*50));
            for(int i=15;i>0;--i)sum=sum-N(std::ldexp(1.0,-400+i*50));
            exact=exact&&sum.Sign()>0&&(sum-N(std::ldexp(1.0,-400))).Sign()==0;
        }
    }
    check(exact,"expansion signs survive mixed-magnitude products and reversed cancellation");
    constexpr double pi=3.14159265358979323846;
    auto source=Source();const auto sourceTime=source->GetMTime();
    CropOpItem sphere;sphere.geometryType=CropShape::Sphere;sphere.radius=1;
    auto params=Params(sphere);auto inside=Build(source,params);
    const double insideArea=Area(inside.polyData);
    check(inside.isSucceeded&&inside.meshTriangleCount>0&&inside.meshErrorBound<=params.meshTolerance
        &&insideArea>0&&std::abs(insideArea-pi)<=2*pi*params.meshTolerance
        &&std::abs(insideArea-pi)<=inside.meshAreaErrorBound+1e-10&&Attributes(inside.polyData),
        "sphere intersects the face interior despite three outside vertices, with certified error and attributes",&inside);
    sphere.removalMode=CropRemovalMode::RemoveInside;params=Params(sphere);auto outside=Build(source,params);
    const double outsideArea=Area(outside.polyData);
    check(outside.isSucceeded&&outsideArea>0&&std::abs(outsideArea-(32-pi))<=2*pi*params.meshTolerance
        &&outside.meshErrorBound<=params.meshTolerance&&Attributes(outside.polyData),"sphere subtraction preserves the interior hole",&outside);
    CropOpItem cylinder;cylinder.geometryType=CropShape::Cylinder;cylinder.radius=1;cylinder.height=6;cylinder.axisInInputModel={1,0,1};
    params=Params(cylinder);auto ellipse=Build(source,params);
    check(ellipse.isSucceeded&&std::abs(Area(ellipse.polyData)-pi*std::sqrt(2.0))<=10*params.meshTolerance
        &&ellipse.meshErrorBound<=params.meshTolerance&&Attributes(ellipse.polyData),"oblique cylinder face intersection matches its analytic ellipse",&ellipse);
    cylinder.axisInInputModel={1,0,0};cylinder.height=1;params=Params(cylinder);auto capped=Build(source,params);
    check(capped.isSucceeded&&std::abs(Area(capped.polyData)-2)<=6*params.meshTolerance
        &&capped.meshErrorBound<=params.meshTolerance&&Attributes(capped.polyData),"finite cylinder clips both end planes and side wall",&capped);
    sphere.removalMode=CropRemovalMode::KeepInside;sphere.centerInInputModel={0,0,1};params=Params(sphere);auto tangent=Build(source,params);
    check(!tangent.isSucceeded&&!tangent.polyData&&(tangent.failureReason==CropFailure::EmptyResult||tangent.failureReason==CropFailure::PrecisionNotMet),
        "zero-area tangent contact cannot publish a degenerate surface",&tangent);
    sphere.centerInInputModel={0,0,0};sphere.radius=0.02;params=Params(sphere);params.meshTolerance=0.01;
    auto tiny=Build(Source(100),params);
    check(tiny.isSucceeded&&Area(tiny.polyData)>0&&std::abs(Area(tiny.polyData)-pi*0.02*0.02)<=2*pi*0.02*params.meshTolerance
        &&tiny.meshErrorBound<=params.meshTolerance,"a tiny interior sphere is found inside a very large triangle",&tiny);
    sphere.centerInInputModel={0,0,std::nextafter(1.0,0.0)};sphere.radius=1;
    params=Params(sphere);params.meshTolerance=1e-6;params.maxDepth=128;
    const auto narrow=Build(source,params);
    const double narrowArea=pi*(1-sphere.centerInInputModel[2])*(1+sphere.centerInInputModel[2]);
    check(narrow.isSucceeded&&Area(narrow.polyData)>narrowArea*0.4
        &&Area(narrow.polyData)<narrowArea*1.6&&narrow.meshErrorBound<=params.meshTolerance,
        "an almost tangent positive-area disk cannot disappear between rounded edge roots",&narrow);
    sphere.centerInInputModel={0,0,0};sphere.radius=0.02;
    params=Params(sphere);params.maxCells=1;auto cells=Build(source,params);
    check(!cells.isSucceeded&&!cells.polyData&&cells.failureReason==CropFailure::ResourceLimit,"cell exhaustion publishes no partial mesh",&cells);
    params=Params(sphere);params.availableRamBytes=1024;auto memory=Build(source,params);
    check(!memory.isSucceeded&&!memory.polyData&&memory.failureReason==CropFailure::LowRam,"mesh preparation respects its memory budget",&memory);
    params=Params(sphere);params.maxDepth=1;auto depth=Build(source,params);
    check(!depth.isSucceeded&&!depth.polyData&&depth.failureReason==CropFailure::PrecisionNotMet,"unproven boundary at maxDepth fails explicitly",&depth);
    int polls=0;params=Params(sphere);auto cancelled=Build(source,params,[&]{return ++polls>8;});
    check(!cancelled.isSucceeded&&cancelled.isCancelled&&!cancelled.polyData&&source->GetMTime()==sourceTime,
        "cancellation keeps the immutable source intact and publishes no partial arrays",&cancelled);
    return failures;
}
