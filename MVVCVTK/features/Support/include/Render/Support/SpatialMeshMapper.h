#pragma once
#include <vtkCompositePolyDataMapper.h>
#include <vtkPoints.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLBatchedPolyDataMapper.h>
#include <vtkOpenGLCompositePolyDataMapperDelegator.h>
#include <vtkOpenGLIndexBufferObject.h>
#include <vtkOpenGLCellToVTKCellMap.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <stdexcept>

namespace FeatureInternal {
// 每个 delegate 恰好一页，使用 VTK 原有单页数组缓存，避免 batched 路径
// 在样式外壳变化时无条件清空全部 VBO。绘制和正式编号仍走 composite 路径。
class SpatialPageMapper final : public vtkOpenGLBatchedPolyDataMapper {
public:
    static SpatialPageMapper* New();
    vtkTypeMacro(SpatialPageMapper,vtkOpenGLBatchedPolyDataMapper);
protected:
    void BuildBufferObjects(vtkRenderer* renderer,vtkActor* actor) override {
        if(VTKPolyDataToGLBatchElement.empty())return;
        if(VTKPolyDataToGLBatchElement.size()!=1)throw std::logic_error("Spatial delegate requires one geometry page.");
        auto& page=*VTKPolyDataToGLBatchElement.begin()->second;auto* mesh=page.Parent.PolyData;
        SetInputData(mesh);CurrentInput=mesh;
        vtkOpenGLPolyDataMapper::BuildBufferObjects(renderer,actor);
        vtkCellArray* cells[]{mesh->GetVerts(),mesh->GetLines(),mesh->GetPolys(),mesh->GetStrips()};
        page.CellCellMap->SetStartOffset(0);
        page.CellCellMap->BuildPrimitiveOffsetsIfNeeded(cells,actor->GetProperty()->GetRepresentation(),mesh->GetPoints());
        page.StartVertex=0;page.NextVertex=static_cast<unsigned int>(mesh->GetNumberOfPoints());
        for(int kind=PrimitiveStart;kind<PrimitiveEnd;++kind){page.StartIndex[kind]=0;page.NextIndex[kind]=static_cast<unsigned int>(Primitives[kind].IBO->IndexCount);}
    }
    void BuildIBO(vtkRenderer* renderer,vtkActor* actor,vtkPolyData* mesh) override {
        // 基类复用同一个临时状态对象；只让实际拓扑和绘制方式决定索引更新。
        TempState.Clear();vtkOpenGLPolyDataMapper::BuildIBO(renderer,actor,mesh);
    }
};
inline vtkStandardNewMacro(SpatialPageMapper);
class SpatialPageDelegator final : public vtkOpenGLCompositePolyDataMapperDelegator {
public:
    static SpatialPageDelegator* New();
    vtkTypeMacro(SpatialPageDelegator,vtkOpenGLCompositePolyDataMapperDelegator);
protected:
    SpatialPageDelegator(){GLDelegate=SpatialPageMapper::New();Delegate.TakeReference(GLDelegate);}
};
inline vtkStandardNewMacro(SpatialPageDelegator);
// 保持一个场景 Actor；每个空间页有独立 delegate/VBO，新增页不重打包旧页。
// 页数不是准入条件，完整可见集合可以持续增加并逐页完成上传。
class SpatialMeshMapper : public vtkCompositePolyDataMapper {
public:
    static SpatialMeshMapper* New();
    vtkTypeMacro(SpatialMeshMapper,vtkCompositePolyDataMapper);
protected:
    SpatialMeshMapper()=default;
    ~SpatialMeshMapper() override=default;
    vtkCompositePolyDataMapperDelegator* CreateADelegator() override {return SpatialPageDelegator::New();}
    MapperHashType GenerateHash(vtkPolyData* page) override {
        // 样式只替换浅拷贝外壳，坐标数组仍是同一显示页。稳定 key 保留其 VBO，
        // 不把样式变更误认成新几何；不同空间页各有独立的局部坐标数组。
        return reinterpret_cast<MapperHashType>(page->GetPoints() ? static_cast<void*>(page->GetPoints()->GetData()) : static_cast<void*>(page));
    }
};
inline vtkStandardNewMacro(SpatialMeshMapper);
}
