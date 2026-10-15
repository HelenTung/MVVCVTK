#pragma once
#include "Data/DataPayloads.h"
#include <vtkDoubleArray.h>
#include <vtkIdTypeArray.h>
#include <vtkTypeInt64Array.h>
#include <vtkIntArray.h>
#include <vtkBuffer.h>
#include <vtkSmartPointer.h>

// 仓内可信只读投影：移交正式载荷后，生产者及显示代码不得原地改写共享元素。
// 外部可变 VTK 输入仍经 VtkDataBridge 防御复制；浅拷贝的各 view 保留同一个 owner。
template<class Value, class Base> class BackedVtkArray final : public Base {
    using VtkValue=typename Base::ValueType;
    using StorageBuffer=vtkBuffer<VtkValue>;
    // VTK 的数组级 ShallowCopy 共用 Buffer；owner 必须跟 Buffer，而非原数组外壳。
    class OwnerBuffer final : public StorageBuffer {
    public:
        static OwnerBuffer* New(){auto* buffer=new OwnerBuffer;buffer->InitializeObjectBase();return buffer;}
        vtkTypeMacro(OwnerBuffer,StorageBuffer);
        std::shared_ptr<const DataArray<Value>> values;
    protected:
        ~OwnerBuffer()override {this->SetBuffer(nullptr,0);}
    };
public:
    static BackedVtkArray* New() {
        auto* array = new BackedVtkArray;
        array->InitializeObjectBase();
        return array;
    }
    vtkTypeMacro(BackedVtkArray, Base);
    void SetValues(DataArray<Value>&& values, int components = 1) {
        SetValues(std::make_shared<const DataArray<Value>>(std::move(values)),components);
    }
    void SetValues(std::shared_ptr<const DataArray<Value>> values,int components=1) {
        if(!values||components<=0||values->size()%components
            ||values->size()>static_cast<std::size_t>(std::numeric_limits<vtkIdType>::max()))
            throw std::invalid_argument("Invalid backed VTK array shape.");
        auto buffer=vtkSmartPointer<OwnerBuffer>::New();buffer->values=std::move(values);
        buffer->SetFreeFunction(true);
        buffer->SetBuffer(reinterpret_cast<VtkValue*>(const_cast<Value*>(buffer->values->data())),
            static_cast<vtkIdType>(buffer->values->size()));
        this->Buffer->Delete();this->Buffer=buffer;this->Buffer->Register(nullptr);
        this->SetNumberOfComponents(components);
        static_assert(sizeof(VtkValue) == sizeof(Value));
        this->SetArray(this->Buffer->GetBuffer(),this->Buffer->GetSize(),1);
    }
    std::shared_ptr<const DataArray<Value>> GetValues() const noexcept {
        const auto* buffer=dynamic_cast<OwnerBuffer*>(this->Buffer);
        if(!buffer||reinterpret_cast<const void*>(buffer->GetBuffer())!=buffer->values->data()
            ||buffer->GetSize()!=static_cast<vtkIdType>(buffer->values->size()))return {};
        return buffer->values;
    }
protected:
    BackedVtkArray() = default;
    ~BackedVtkArray() override = default;
};
