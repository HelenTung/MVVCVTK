// 合成几何对照只验证分块边界、标签和取消契约；真实大数据性能由独立整卷链记录。
#include "PartSegmentationTestCases.h"
#include "Render/Internal/PartSurfaceProductBuilder.h"
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkExtractVOI.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkUnsignedIntArray.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>
namespace {
using Triangle = std::array<double, 10>;
std::vector<Triangle> GetTriangleIndices(vtkPolyData& mesh)
{
    std::vector<Triangle> result;
    vtkIdType count = 0, cell = 0; const vtkIdType* ids = nullptr;
    mesh.GetPolys()->InitTraversal();
    while (mesh.GetPolys()->GetNextCell(count, ids)) {
        if (count != 3 || !mesh.GetCellData()->GetScalars()) return {};
        Triangle best{}; bool first = true;
        for (int start = 0; start < 3; ++start) {
            Triangle value{}; value[0] = mesh.GetCellData()->GetScalars()->GetTuple1(cell);
            for (int i = 0; i < 3; ++i) mesh.GetPoint(ids[(start+i)%3], &value[1+3*i]);
            if (first || value < best) { best = value; first = false; }
        }
        result.push_back(best); ++cell;
    }
    std::sort(result.begin(), result.end()); return result;
}
vtkSmartPointer<vtkPolyData> GetReference(const PartSurfaceBuildRequest& request)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(request.extent[0],request.extent[1],request.extent[2],request.extent[3],request.extent[4],request.extent[5]);
    image->SetSpacing(request.spacing.data()); image->SetOrigin(request.origin.data());
    auto direction = vtkSmartPointer<vtkMatrix3x3>::New(); direction->DeepCopy(request.direction.data()); image->SetDirectionMatrix(direction);
    auto values = vtkSmartPointer<vtkUnsignedIntArray>::New(); values->SetNumberOfComponents(1);
    values->SetArray(const_cast<unsigned int*>(request.labels->data()), static_cast<vtkIdType>(request.labels->size()), 1);
    image->GetPointData()->SetScalars(values);
    auto crop = vtkSmartPointer<vtkExtractVOI>::New(); crop->SetInputData(image);
    auto contour = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
    if (request.foregroundExtent) {
        auto extent = *request.foregroundExtent;
        for (int axis = 0; axis < 3; ++axis) {
            if (extent[2*axis] > request.extent[2*axis]) --extent[2*axis];
            if (extent[2*axis+1] < request.extent[2*axis+1]) ++extent[2*axis+1];
        }
        crop->SetVOI(extent.data()); contour->SetInputConnection(crop->GetOutputPort());
    } else contour->SetInputData(image);
    const auto count = request.partIds.empty() ? request.partCount : request.partIds.size();
    contour->SetNumberOfContours(static_cast<int>(count));
    for (std::size_t i = 0; i < count; ++i) contour->SetValue(static_cast<int>(i), request.partIds.empty() ? i+1 : request.partIds[i]);
    contour->Update(); auto output = vtkSmartPointer<vtkPolyData>::New(); output->ShallowCopy(contour->GetOutput()); return output;
}
PartSurfaceBuildRequest GetInput()
{
    PartSurfaceBuildRequest request; request.dimensions={70,69,19}; request.extent={13,82,-7,61,4,22};
    request.spacing={.1375,.21,.42}; request.origin={13.25,-37.2,114.14};
    request.direction={.6,-.8,0,.8,.6,0,0,0,1}; request.partCount=3; request.maxWorkingBytes=256U*1024U*1024U;
    auto labels = std::make_shared<std::vector<std::uint32_t>>(70*69*19,0);
    for (int z=3;z<16;++z) for(int y=5;y<62;++y) for(int x=5;x<65;++x)
        (*labels)[x+70*(y+69*z)] = x<34 ? 1 : 2;
    for(int z=6;z<14;++z) for(int y=20;y<29;++y) for(int x=30;x<38;++x) (*labels)[x+70*(y+69*z)]=3;
    request.labels=labels; return request;
}
}
int GetPartSurfaceProductFailCount()
{
    int failures=0;
    const auto check=[&](bool passed,const char* name){std::cout<<(passed?"[PASS] ":"[FAIL] ")<<name<<'\n';if(!passed)++failures;};
    for (int mode=0;mode<4;++mode) {
        auto request=GetInput();
        if(mode==1)request.partIds={3,1};
        if(mode==2)request.foregroundExtent=std::array<int,6>{17,74,2,49,8,18};
        if(mode==3)request.origin={1e8,-1e8,1e8};
        const auto expected=GetReference(request); const auto actual=PartSurfaceProductBuilder::BuildProduct(request,{},{});
        check(actual.product && actual.failureReason==PartFailureReason::None && expected->GetNumberOfCells()>0
            && expected->GetNumberOfPoints()==actual.product->surface->GetNumberOfPoints()
            && GetTriangleIndices(*expected)==GetTriangleIndices(*actual.product->surface),
            "Block extraction exactly preserves VTK labeled triangles, winding and shared vertices");
    }
    auto sparse=GetInput(); sparse.partCount=1470697;
    bool cancelled=false; const auto start=std::chrono::steady_clock::now();
    const auto result=PartSurfaceProductBuilder::BuildProduct(sparse,[&]{return cancelled;},[&](double progress){if(progress>0)cancelled=true;});
    check(cancelled && result.failureReason==PartFailureReason::Cancelled && !result.product
        && std::chrono::steady_clock::now()-start<std::chrono::seconds(2),
        "Sparse million-label display cancels within the existing stop deadline without publishing a partial mesh");
    return failures;
}
