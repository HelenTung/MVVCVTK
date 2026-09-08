#include "Data/DataGraphStore.h"
#include "Data/DataPayloads.h"
#include "Data/VtkDataBridge.h"

#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkUnsignedLongLongArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkUnsignedIntArray.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

bool Check(const bool condition, const char* message)
{
    if (condition) return true;
    std::cerr << "[VtkDataBridge] " << message << '\n';
    return false;
}

DataSnapshot SetPayload(
    DataGraphStore& store,
    std::shared_ptr<const IDataPayload> payload)
{
    const auto entity = store.CreateDataEntityId();
    DataTransaction transaction;
    transaction.outputs.push_back(DataRevisionDraft{
        entity,
        0,
        payload ? payload->GetDataType() : DataTypeId{},
        {},
        std::move(payload),
        std::nullopt });
    const auto result = store.SetDataCommit(std::move(transaction));
    return result.published.empty() ? DataSnapshot{} : result.published.front();
}

bool GetImageRoundTripValid()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 1);
    image->SetSpacing(0.5, 0.75, 2.0);
    image->SetOrigin(3.0, 4.0, 5.0);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* values = static_cast<float*>(image->GetScalarPointer());
    values[0] = 1.0f;
    values[1] = 2.0f;
    values[2] = 3.0f;
    values[3] = 4.0f;

    auto mask = vtkSmartPointer<vtkImageData>::New();
    mask->CopyStructure(image);
    mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* maskValues = static_cast<unsigned char*>(mask->GetScalarPointer());
    maskValues[0] = 1;
    maskValues[1] = 0;
    maskValues[2] = 1;
    maskValues[3] = 1;

    VtkDataBridge bridge;
    auto invalidMask = vtkSmartPointer<vtkImageData>::New();
    invalidMask->SetDimensions(1, 1, 1);
    invalidMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    const auto rejectedMask = bridge.CreateImagePayload(image, invalidMask);
    const auto payload = bridge.CreateImagePayload(image, mask);
    values[0] = 99.0f;
    maskValues[0] = 0;
    DataGraphStore store;
    const auto snapshot = SetPayload(store, payload);
    const auto first = bridge.GetImageGrid(snapshot);
    const auto second = bridge.GetImageGrid(snapshot);
    const auto* resultValues = first
        ? static_cast<const float*>(first->image->GetScalarPointer()) : nullptr;
    const auto* resultMask = first && first->validityMask
        ? static_cast<const unsigned char*>(
            first->validityMask->GetScalarPointer()) : nullptr;
    return Check(
        !rejectedMask && payload && snapshot
            && first && second && first == second
            && resultValues && resultValues[0] == 1.0f
            && resultValues[3] == 4.0f
            && resultMask && resultMask[0] == 1
            && first->image->GetSpacing()[1] == 0.75,
        "image/mask isolation, geometry, or cache failed");
}

bool GetLabelRoundTripValid()
{
    auto labels = vtkSmartPointer<vtkImageData>::New();
    labels->SetDimensions(2, 1, 1);
    labels->AllocateScalars(VTK_INT, 1);
    auto* values = static_cast<int*>(labels->GetScalarPointer());
    values[0] = -7;
    values[1] = 42;
    VtkDataBridge bridge;
    const auto payload = bridge.CreateLabelPayload(labels);
    const auto definedPayload = payload
        ? std::make_shared<const LabelMap3DPayload>(
            payload->GetGeometry(),
            payload->GetValues(),
            std::vector<LabelDefinition>{
                LabelDefinition{
                    42, "part", { 0.1, 0.2, 0.3, 0.4 } } })
        : std::shared_ptr<const LabelMap3DPayload>{};
    DataGraphStore store;
    const auto snapshot = SetPayload(store, definedPayload);
    const auto view = bridge.GetLabelMap(snapshot);
    const auto* stored = snapshot
        ? dynamic_cast<const LabelMap3DPayload*>(snapshot->payload.get())
        : nullptr;
    const auto* output = view
        ? static_cast<const std::int32_t*>(view->labels->GetScalarPointer())
        : nullptr;
    return Check(
        payload && definedPayload && stored
            && stored->GetGeometry().dimensions
                == std::array<int, 3>{ 2, 1, 1 }
            && stored->GetDefinitions().size() == 1
            && stored->GetDefinitions().front().value == 42
            && stored->GetDefinitions().front().name == "part"
            && payload->GetValueType() == ImageValueType::Int32
            && static_cast<const std::int32_t*>(payload->GetValueData())[1] == 42
            && view && output && output[0] == -7 && output[1] == 42,
        "label geometry, definitions, or round trip failed");
}

bool GetMeshRoundTripValid()
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->InsertNextPoint(0.0, 0.0, 0.0);
    points->InsertNextPoint(1.0, 0.0, 0.0);
    points->InsertNextPoint(0.0, 1.0, 0.0);
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    const vtkIdType ids[3] = { 0, 1, 2 };
    polys->InsertNextCell(3, ids);
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    auto normals=vtkSmartPointer<vtkDoubleArray>::New();normals->SetName("Normals");
    normals->SetNumberOfComponents(3);normals->SetNumberOfTuples(3);
    normals->FillComponent(0,0);normals->FillComponent(1,0);normals->FillComponent(2,1);
    mesh->GetPointData()->SetNormals(normals);
    auto temperatures=vtkSmartPointer<vtkFloatArray>::New();temperatures->SetName("Temperature");
    temperatures->InsertNextValue(0.5f);temperatures->InsertNextValue(1.5f);temperatures->InsertNextValue(2.5f);
    mesh->GetPointData()->SetScalars(temperatures);
    auto materials=vtkSmartPointer<vtkUnsignedIntArray>::New();materials->SetName("Material");materials->InsertNextValue(17);
    mesh->GetCellData()->SetScalars(materials);

    VtkDataBridge bridge;
    const auto payload = bridge.CreateMeshPayload(mesh);
    points->SetPoint(0, 9.0, 9.0, 9.0);
    normals->SetComponent(0,2,-1);temperatures->SetValue(2,100);materials->SetValue(0,99);
    DataGraphStore store;
    const auto snapshot = SetPayload(store, payload);
    const auto view = bridge.GetSurfaceMesh(snapshot);
    auto emptyMesh = vtkSmartPointer<vtkPolyData>::New();
    const auto emptyPayload = bridge.CreateMeshPayload(emptyMesh);
    const auto emptySnapshot = SetPayload(store, emptyPayload);
    const auto emptyView = bridge.GetSurfaceMesh(emptySnapshot);
    double first[3] = {};
    if (view && view->mesh && view->mesh->GetPoints()) {
        view->mesh->GetPoint(0, first);
    }
    auto wide=vtkSmartPointer<vtkUnsignedLongLongArray>::New();wide->SetName("WideId");wide->SetNumberOfTuples(3);
    for(vtkIdType i=0;i<3;++i)wide->SetValue(i,9007199254740993ULL);
    mesh->GetPointData()->AddArray(wide);
    const bool rejectsLoss=!bridge.CreateMeshPayload(mesh);mesh->GetPointData()->RemoveArray("WideId");
    auto duplicate=payload?payload->GetPointAttributes():std::vector<MeshAttribute>{};
    if(!duplicate.empty())duplicate.push_back(duplicate.front());
    const SurfaceMeshPayload invalidAttributes(payload->GetVertices(),payload->GetTriangles(),std::move(duplicate));
    return Check(rejectsLoss&&!invalidAttributes.GetValid(),"mesh integer precision loss or ambiguous attributes were accepted")
        &&Check(payload&&payload->GetPointAttributes().size()==2&&payload->GetCellAttributes().size()==1
            &&view&&view->mesh->GetPointData()->GetNormals()&&view->mesh->GetPointData()->GetScalars()
            &&view->mesh->GetCellData()->GetScalars()
            &&view->mesh->GetPointData()->GetNormals()->GetComponent(0,2)==1
            &&view->mesh->GetPointData()->GetScalars()->GetComponent(2,0)==2.5
            &&view->mesh->GetCellData()->GetScalars()->GetComponent(0,0)==17,
            "mesh point/cell values or active normal/scalar roles were not isolated and restored")
        && Check(
        payload && view && view->mesh->GetNumberOfPolys() == 1
            && first[0] == 0.0 && first[1] == 0.0 && first[2] == 0.0,
        "surface mesh isolation or round trip failed")
        && Check(
            emptyPayload && emptySnapshot && emptyView
                && emptyView->mesh
                && emptyView->mesh->GetNumberOfPoints() == 0
                && emptyView->mesh->GetNumberOfCells() == 0,
            "empty formal surface mesh did not round trip");
}

bool GetCacheIdentityValid()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(1, 1, 1);
    image->AllocateScalars(VTK_FLOAT, 1);
    *static_cast<float*>(image->GetScalarPointer()) = 1.0f;

    VtkDataBridge bridge;
    DataGraphStore store;
    const auto entity = store.CreateDataEntityId();
    DataTransaction firstCommit;
    firstCommit.outputs.push_back(DataRevisionDraft{
        entity, 0, DataTypes::imageGrid3D, {},
        bridge.CreateImagePayload(image), std::nullopt });
    const auto firstResult = store.SetDataCommit(std::move(firstCommit));
    const auto firstSnapshot = firstResult.published.empty()
        ? DataSnapshot{} : firstResult.published.front();
    const auto first = bridge.GetImageGrid(firstSnapshot);
    const auto firstAgain = bridge.GetImageGrid(firstSnapshot);

    *static_cast<float*>(image->GetScalarPointer()) = 2.0f;
    DataTransaction secondCommit;
    secondCommit.outputs.push_back(DataRevisionDraft{
        entity, 1, DataTypes::imageGrid3D, {},
        bridge.CreateImagePayload(image), std::nullopt });
    const auto secondResult = store.SetDataCommit(std::move(secondCommit));
    const auto secondSnapshot = secondResult.published.empty()
        ? DataSnapshot{} : secondResult.published.front();
    const auto second = bridge.GetImageGrid(secondSnapshot);
    const auto* firstValue = first
        ? static_cast<const float*>(first->image->GetScalarPointer())
        : nullptr;
    const auto* secondValue = second
        ? static_cast<const float*>(second->image->GetScalarPointer())
        : nullptr;
    return Check(
        first && firstAgain && second
            && first == firstAgain
            && first != second
            && firstSnapshot->self.generation == 1
            && secondSnapshot->self.generation == 2
            && firstValue && *firstValue == 1.0f
            && secondValue && *secondValue == 2.0f,
        "VTK view cache did not use the complete revision identity");
}

bool GetRecordTableValidationValid()
{
    RecordTablePayload invalid(
        DataTypes::recordTable,
        "test",
        { RecordColumn{ "id", std::vector<std::uint64_t>{ 1, 2 } },
          RecordColumn{ "value", std::vector<double>{ 3.0 } } });
    RecordTablePayload valid(
        DataTypes::recordTable,
        "test",
        { RecordColumn{ "id", std::vector<std::uint64_t>{ 1, 2 } },
          RecordColumn{ "value", std::vector<double>{ 3.0, 4.0 } } });
    return Check(
        !invalid.GetValid() && valid.GetValid() && valid.GetRowCount() == 2,
        "record table did not enforce equal strongly typed columns");
}

bool GetArrayLeaseValid()
{
    DataGraphStore store;
    VtkDataBridge bridge;
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 1, 1);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* values = static_cast<unsigned char*>(image->GetScalarPointer());
    values[0] = 1; values[1] = 2;
    const auto scope = store.CreateDataEntityId();
    const auto id = store.CreateDataEntityId();
    const DataRevisionRef ref{id, 1};
    DataTransaction create;
    create.outputs.push_back({id, 0, DataTypes::imageGrid3D, {}, bridge.CreateImagePayload(image), {}, scope});
    auto commit = store.SetDataCommit(std::move(create));
    auto snapshot = commit.published.at(0);
    auto view = bridge.GetImageGrid(snapshot);
    if (!Check(view != nullptr, "scoped VTK image")) return false;
    vtkSmartPointer<vtkDataArray> heldArray = view->image->GetPointData()->GetScalars();
    view.reset();
    commit = {};
    DataTransaction retire;
    retire.retireScopes.push_back({scope, DataLifetimeStatus::Published, {ref}});
    const auto blocked = store.SetDataCommit(retire);
    if (!Check(blocked.failureReason == DataCommitFailure::ResultInUse,
        "array-only VTK owner escaped retirement check")) return false;
    retire.retireScopes.front().isResourceTransition = true;
    if (!Check(store.SetDataCommit(std::move(retire)).status == DataCommitStatus::Succeeded,
        "prepared resource transition could not retire the result")) return false;
    if (!Check(!bridge.GetImageGrid(snapshot), "old DataSnapshot recreated retired VTK resources")) return false;
    snapshot.reset();
    if (!Check(store.SetDataRelease(scope).status == DataLifetimeStatus::Releasing,
        "resource transition ignored its live VTK array")) return false;
    heldArray = nullptr;
    return Check(store.SetDataRelease(scope).status == DataLifetimeStatus::Released,
        "scoped VTK payload did not release");
}

bool GetBorrowedRenderResourceReleased()
{
    DataGraphStore store;
    GridGeometry3D geometry; geometry.dimensions={2,1,1}; geometry.extent={0,1,0,0,0,0};
    auto payload=std::make_shared<const LabelMap3DPayload>(geometry,
        LabelMapValues{std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{7,42})});
    auto backing=payload->GetLabels();
    if(!payload->GetValid()||!backing)return false;
    std::weak_ptr<const std::vector<std::uint32_t>> probe=backing;
    auto image=vtkSmartPointer<vtkImageData>::New();image->SetDimensions(2,1,1);
    auto array=vtkSmartPointer<vtkUnsignedIntArray>::New();
    array->SetArray(const_cast<std::uint32_t*>(backing->data()),2,1);
    image->GetPointData()->SetScalars(array);
    auto resource=VtkPreparedDataView::BuildResourceUse(image,nullptr,backing);
    const auto scope=store.CreateDataEntityId(),id=store.CreateDataEntityId();const DataRevisionRef ref{id,1};
    DataTransaction transaction;
    transaction.outputs.push_back({id,0,DataTypes::labelMap3D,{},payload,{},scope,{resource}});
    auto committed=store.SetDataCommit(std::move(transaction));
    if(committed.status!=DataCommitStatus::Succeeded)return false;
    DataTransaction retirement;retirement.retireScopes.push_back({scope,DataLifetimeStatus::Published,{ref},true});
    if(store.SetDataCommit(std::move(retirement)).status!=DataCommitStatus::Succeeded)return false;
    committed={};payload.reset();backing.reset();image=nullptr;resource={};
    if(!Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Releasing
        &&!probe.expired()&&array->GetValue(1)==42,"borrowed bare array lost its backing allocation or release probe"))return false;
    array=nullptr;
    return Check(probe.expired()&&store.SetDataRelease(scope).status==DataLifetimeStatus::Released,
        "borrowed array backing remained pinned after its last VTK owner ended");
}

bool GetPreparedResultReleased()
{
    DataGraphStore store;
    VtkDataBridge bridge;
    auto image=vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2,1,1);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    auto* values=static_cast<unsigned char*>(image->GetScalarPointer());values[0]=3;values[1]=8;
    const auto source=SetPayload(store,bridge.CreateImagePayload(image));
    const auto root=bridge.GetImageGrid(source);
    const auto rootPayload=std::dynamic_pointer_cast<const ImageGrid3DPayload>(source->payload);
    auto output=rootPayload->CreateMaskSnapshot(std::vector<std::uint8_t>{255,0});
    auto prepared=VtkDataBridge::BuildDataView(output,root);
    if (!Check(prepared && prepared->image
        && prepared->image->image->GetScalarPointer()==root->image->GetScalarPointer()
        && prepared->image->validityMask->GetScalarPointer()!=output->GetValidityMask()->data(),
        "prepared result copied Root scalars or exposed mutable formal mask bytes")) return false;
    const auto scope=store.CreateDataEntityId(),id=store.CreateDataEntityId();
    const DataRevisionRef ref{id,1};
    prepared=bridge.SetPreparedDataView(ref,std::move(prepared));
    if(!prepared)return false;
    DataTransaction transaction;
    transaction.outputs.push_back({id,0,DataTypes::imageGrid3D,{{"root",source->self}},output,{},scope,{prepared->resourceUse}});
    auto committed=store.SetDataCommit(std::move(transaction));
    if(!Check(committed.status==DataCommitStatus::Succeeded,"prepared result publication"))return false;
    auto view=bridge.GetImageGrid(committed.published.at(0));
    if(!Check(view&&view->data.get()==committed.published.at(0).get()
        &&view->image==prepared->image->image&&view->validityMask==prepared->image->validityMask,
        "prepared cache rebuilt the image or leaked provisional identity"))return false;
    vtkSmartPointer<vtkDataArray> heldMask=view->validityMask->GetPointData()->GetScalars();
    DataTransaction retire;
    retire.retireScopes.push_back({scope,DataLifetimeStatus::Published,{ref}});
    if(!Check(store.SetDataCommit(retire).failureReason==DataCommitFailure::ResultInUse,
        "prepared array lease was not registered at publication"))return false;
    retire.retireScopes.front().isResourceTransition=true;
    if(!Check(store.SetDataCommit(std::move(retire)).status==DataCommitStatus::Succeeded,"prepared retirement"))return false;
    if(!Check(!bridge.GetImageGrid(committed.published.at(0)),"retired prepared cache remained readable"))return false;
    view.reset();prepared.reset();output.reset();committed={};
    if(!Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Releasing,"bare prepared mask did not retain scope"))return false;
    heldMask=nullptr;
    return Check(store.SetDataRelease(scope).status==DataLifetimeStatus::Released
        &&static_cast<unsigned char*>(root->image->GetScalarPointer())[1]==8,
        "Root scalar incorrectly retained the prepared result scope");
}
} // namespace

int main()
{
    return GetBorrowedRenderResourceReleased() && GetPreparedResultReleased() && GetArrayLeaseValid()
        && GetImageRoundTripValid()
        && GetLabelRoundTripValid()
        && GetMeshRoundTripValid()
        && GetCacheIdentityValid()
        && GetRecordTableValidationValid()
        ? 0 : 1;
}
