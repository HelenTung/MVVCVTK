#pragma once

#include "Data/DataGraphTypes.h"
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkDataArray.h>
#include <vtkFieldData.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// 租约跟随真实 VTK 对象；仅保留 points、cells 或数组也必须等待释放。
namespace VtkDataResourceLease {
class Observer final : public vtkCommand {
public:
    static Observer* New() { return new Observer; }
    void Execute(vtkObject*, unsigned long, void*) override {}
    std::shared_ptr<const DataResourceLease> lease;
};
inline void Attach(vtkObject* object, const std::shared_ptr<const DataResourceLease>& lease)
{
    if (!object || !lease) return;
    auto observer = vtkSmartPointer<Observer>::New();
    observer->lease = lease;
    object->AddObserver(vtkCommand::DeleteEvent, observer);
}
inline void AttachFields(vtkFieldData* data, const std::shared_ptr<const DataResourceLease>& lease)
{
    if (!data || !lease) return;
    Attach(data, lease);
    for (int index = 0; index < data->GetNumberOfArrays(); ++index) Attach(data->GetAbstractArray(index), lease);
}
inline void AttachImage(vtkImageData* image, const std::shared_ptr<const DataResourceLease>& lease)
{
    if (!image || !lease) return;
    Attach(image, lease);
    AttachFields(image->GetPointData(), lease);
    AttachFields(image->GetCellData(), lease);
    AttachFields(image->GetFieldData(), lease);
}
inline void AttachMesh(vtkPolyData* mesh, const std::shared_ptr<const DataResourceLease>& lease)
{
    if (!mesh || !lease) return;
    Attach(mesh, lease);
    if (auto* points = mesh->GetPoints()) {
        Attach(points, lease);
        Attach(points->GetData(), lease);
    }
    for (auto* cells : {mesh->GetVerts(), mesh->GetLines(), mesh->GetPolys(), mesh->GetStrips()}) {
        // vtkPolyData 的缺省空单元可能来自进程级 DummyContainer，不能把结果租约挂到它上面。
        if (!cells || cells->GetNumberOfCells() == 0) continue;
        Attach(cells, lease);
        Attach(cells->GetOffsetsArray(), lease);
        Attach(cells->GetConnectivityArray(), lease);
    }
    AttachFields(mesh->GetPointData(), lease);
    AttachFields(mesh->GetCellData(), lease);
    AttachFields(mesh->GetFieldData(), lease);
}
} // namespace VtkDataResourceLease
