#pragma once
#include "PagedArray.h"
#include <vtkImplicitArray.h>
#include <vtkTypeInt64Array.h>
#include <vtkIntArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkCellArray.h>
#include <vtkPointData.h>
#include <vtkCellData.h>
#include <vtkSmartPointer.h>
#include <vtkObjectFactory.h>
#include <vtkMappedDataArray.h>

namespace MeshVtkStorage {
struct DoubleValues final {
    MeshReadArray<double> values;
    double operator()(vtkIdType index)const{return values[static_cast<std::size_t>(index)];}
};
class DoubleArray final:public vtkImplicitArray<DoubleValues> {
public:
    static DoubleArray* New(){VTK_STANDARD_NEW_BODY(DoubleArray);}
    vtkAbstractTypeMacroWithNewInstanceType(DoubleArray,vtkImplicitArray<DoubleValues>,vtkDataArray,"DoubleArray");
    vtkMappedDataArrayNewInstanceMacro(DoubleArray);
    // VTK 的连续指针适配也落入文件；禁止 implicit array 的默认整份 AOS RAM 物化。
    void* GetVoidPointer(vtkIdType index)override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const auto backend=this->GetBackend();const auto count=backend?backend->values.size():0;
        if(index<0 || std::size_t(index)>count)throw std::out_of_range("VTK mesh value offset is invalid.");
        if(!count)return nullptr;
        if(!m_flat){
            auto values=std::make_shared<PagedArray<double>>();values->reserve(count);
            for(std::size_t offset=0;offset<count;){
                auto chunk=backend->values.GetChunk(offset,8192);
                if(chunk.error!=MeshReadError::None)throw std::runtime_error("VTK mesh block read failed.");
                values->insert(values->end(),chunk.values.begin(),chunk.values.end());offset=chunk.nextOffset;
            }
            values->GetReadSnapshot();m_flat=std::move(values);
        }
        return m_flat->data()+index;
    }
    void ShallowCopy(vtkDataArray* source)override {
        if(source==this)return;
        const std::lock_guard<std::mutex> lock(m_mutex);m_flat.reset();
        if(auto* values=DoubleArray::SafeDownCast(source)){
            this->ImplicitDeepCopy(static_cast<Superclass*>(values));this->SetName(source->GetName());
        }else throw std::invalid_argument("Readonly mesh arrays require a compatible value source.");
    }
    void DeepCopy(vtkDataArray* source)override{this->ShallowCopy(source);}
protected:DoubleArray()=default;~DoubleArray()override=default;
private:std::mutex m_mutex;std::shared_ptr<PagedArray<double>> m_flat;
};
inline vtkSmartPointer<DoubleArray> GetValues(MeshReadArray<double> values,int components){
    auto array=vtkSmartPointer<DoubleArray>::New();array->ConstructBackend(DoubleValues{std::move(values)});
    array->SetNumberOfComponents(components);array->SetNumberOfTuples(static_cast<vtkIdType>(array->GetBackend()->values.size()/std::size_t(components)));
    return array;
}
inline vtkSmartPointer<vtkDataArray> CloneField(vtkDataArray* source){
    if(auto* values=DoubleArray::SafeDownCast(source)){
        auto copy=vtkSmartPointer<DoubleArray>::New();copy->ShallowCopy(values);return copy;
    }
    vtkSmartPointer<vtkDataArray> copy;copy.TakeReference(source->NewInstance());copy->ShallowCopy(source);return copy;
}
// vtkCellArray 的批准存储是 signed AOS；让其直接拥有私有文件视图，而不是再分配完整 RAM 数组。
class IdArray final:public vtkTypeInt64Array {
public:
    static IdArray* New(){VTK_STANDARD_NEW_BODY(IdArray);}
    vtkTypeMacro(IdArray,vtkTypeInt64Array);
    void SetStorage(std::shared_ptr<PagedArray<vtkTypeInt64>> values){
        m_values=std::move(values);m_values->GetReadSnapshot();
        this->SetArray(m_values->data(),static_cast<vtkIdType>(m_values->size()),1);
    }
protected:IdArray()=default;~IdArray()override=default;
private:std::shared_ptr<PagedArray<vtkTypeInt64>> m_values;
};
class IntArray final:public vtkIntArray {
public:
    static IntArray* New(){VTK_STANDARD_NEW_BODY(IntArray);}
    vtkTypeMacro(IntArray,vtkIntArray);
    void SetStorage(std::shared_ptr<PagedArray<int>> values){m_values=std::move(values);m_values->GetReadSnapshot();this->SetArray(m_values->data(),static_cast<vtkIdType>(m_values->size()),1);}
protected:IntArray()=default;~IntArray()override=default;
private:std::shared_ptr<PagedArray<int>> m_values;
};
inline vtkSmartPointer<IntArray> GetIntegerIds(vtkDataArray* source,const std::function<bool()>& cancelled={}){
    if(!source || source->GetNumberOfComponents()!=1 || source->GetNumberOfTuples()<0)return {};
    auto values=std::make_shared<PagedArray<int>>();values->reserve(static_cast<std::size_t>(source->GetNumberOfTuples()));
    for(vtkIdType i=0;i<source->GetNumberOfTuples();++i){if((i&1023)==0 && cancelled && cancelled())return {};const auto value=source->GetComponent(i,0);
        if(!std::isfinite(value) || value<INT_MIN || value>INT_MAX || std::floor(value)!=value)return {};values->push_back(static_cast<int>(value));}
    auto array=vtkSmartPointer<IntArray>::New();array->SetStorage(std::move(values));return array;
}
inline void SetFields(vtkDataSetAttributes* destination,const std::vector<MeshField>& fields){
    for(const auto& field:fields){
        auto array=GetValues(field.values,static_cast<int>(field.componentCount));array->SetName(field.name.c_str());destination->AddArray(array);
        if(field.activeRoles&MeshAttributeRoles::scalars)destination->SetActiveScalars(field.name.c_str());
        if(field.activeRoles&MeshAttributeRoles::vectors)destination->SetActiveVectors(field.name.c_str());
        if(field.activeRoles&MeshAttributeRoles::normals)destination->SetActiveNormals(field.name.c_str());
        if(field.activeRoles&MeshAttributeRoles::textureCoordinates)destination->SetActiveTCoords(field.name.c_str());
        if(field.activeRoles&MeshAttributeRoles::tensors)destination->SetActiveTensors(field.name.c_str());
    }
}
inline vtkSmartPointer<vtkPolyData> Build(MeshReadArray<double> coordinates,MeshReadArray<std::uint64_t> triangles,
    const std::vector<MeshField>& pointFields={},const std::vector<MeshField>& cellFields={},const std::function<bool()>& cancelled={}){
    if(coordinates.size()%3 || triangles.size()%3 || coordinates.size()/3>std::size_t(std::numeric_limits<vtkIdType>::max())
        || triangles.size()>std::size_t(std::numeric_limits<vtkIdType>::max()) || triangles.size()/3==SIZE_MAX)return {};
    auto connectivity=std::make_shared<PagedArray<vtkTypeInt64>>();connectivity->reserve(triangles.size());
    for(std::size_t offset=0;offset<triangles.size();){
        auto chunk=triangles.GetChunk(offset,65536,cancelled);if(chunk.error!=MeshReadError::None)return {};
        for(auto id:chunk.values){if(id>=coordinates.size()/3 || id>std::uint64_t(std::numeric_limits<vtkIdType>::max()))return {};connectivity->push_back(static_cast<vtkTypeInt64>(id));}
        offset=chunk.nextOffset;
    }
    auto offsets=std::make_shared<PagedArray<vtkTypeInt64>>();offsets->reserve(triangles.size()/3+1);
    for(std::size_t i=0;i<=triangles.size()/3;++i){if((i&1023U)==0 && cancelled && cancelled())return {};offsets->push_back(static_cast<vtkTypeInt64>(i*3));}
    auto vtkConnectivity=vtkSmartPointer<IdArray>::New(),vtkOffsets=vtkSmartPointer<IdArray>::New();
    vtkConnectivity->SetStorage(std::move(connectivity));vtkOffsets->SetStorage(std::move(offsets));
    auto cells=vtkSmartPointer<vtkCellArray>::New();cells->SetData(vtkOffsets,vtkConnectivity);if(!cells->IsValid())return {};
    auto points=vtkSmartPointer<vtkPoints>::New();points->SetData(GetValues(std::move(coordinates),3));
    auto mesh=vtkSmartPointer<vtkPolyData>::New();mesh->SetPoints(points);mesh->SetPolys(cells);
    SetFields(mesh->GetPointData(),pointFields);SetFields(mesh->GetCellData(),cellFields);return mesh;
}
inline vtkSmartPointer<vtkPolyData> Build(const SurfaceMeshPayload& mesh,const std::function<bool()>& cancelled={}){
    return mesh.GetValid()?Build(mesh.GetPointValues(),mesh.GetTriangleIndices(),mesh.GetPointFields(),mesh.GetCellFields(),cancelled):nullptr;
}
}
