#include "Data/DataGraphStore.h"
#include "Data/DataPayloads.h"
#include "Data/VtkDataBridge.h"

#include <vtkCellArray.h>
#include <vtkImageData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

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

    VtkDataBridge bridge;
    const auto payload = bridge.CreateMeshPayload(mesh);
    points->SetPoint(0, 9.0, 9.0, 9.0);
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
    return Check(
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

} // namespace

int main()
{
    return GetImageRoundTripValid()
        && GetLabelRoundTripValid()
        && GetMeshRoundTripValid()
        && GetCacheIdentityValid()
        && GetRecordTableValidationValid()
        ? 0 : 1;
}
