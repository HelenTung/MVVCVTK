#include "Algorithms/CropAlgorithm.h"
#include "PlanarTestSuites.h"
#include "Routing/CropRouter.h"

#include <vtkCubeSource.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <vector>

namespace {
int inputIdentity = 0;

bool SetExpect(const bool isExpected, const char* message)
{
    if (!isExpected) {
        std::cerr << message << '\n';
    }
    return isExpected;
}

CropOpItem BuildBox(const std::uint64_t operationIndex)
{
    CropOpItem operation;
    operation.operationIndex = operationIndex;
    operation.geometryType = CropShape::Box;
    return operation;
}

CropOpItem BuildPlane(
    const std::uint64_t operationIndex,
    const CropRemovalMode removalMode = CropRemovalMode::KeepInside)
{
    CropOpItem operation;
    operation.operationIndex = operationIndex;
    operation.geometryType = CropShape::Plane;
    operation.removalMode = removalMode;
    return operation;
}

CropShaderPayload BuildPayload(
    const std::vector<CropOpItem>& operations,
    const std::size_t nodeCount,
    const std::uint64_t revision = 1,
    const std::uint64_t inputVersion = 7)
{
    const auto tableResult = CropAlgorithm::BuildPredicateTable(
        operations,
        operations.size());
    CropShaderPayload payload;
    payload.revision = revision;
    payload.sourceStamp = { &inputIdentity, inputVersion };
    payload.nodeCount = nodeCount;
    payload.predicateTable = tableResult.predicateTable;
    return payload;
}

bool StartLayoutCase()
{
    auto box = BuildBox(1);
    box.removalMode = CropRemovalMode::RemoveInside;
    box.boxToInputModelMatrix = {
        2.0, 0.0, 0.0, 4.0,
        0.0, 4.0, 0.0, 8.0,
        0.0, 0.0, 5.0, 10.0,
        0.0, 0.0, 0.0, 1.0
    };
    auto plane = BuildPlane(2);
    plane.planeCenterInInputModel = { 1.0, 2.0, 3.0 };
    plane.planeNormalInInputModel = { 0.0, 0.0, 2.0 };

    const auto result = CropAlgorithm::BuildPredicateTable({ box, plane }, 2);
    bool isPassed = SetExpect(
        result.isSucceeded && result.predicateTable
            && result.predicateTable->rgbaValues.size() == 40,
        "Two crop operations should compile to exactly ten RGBA texels.");
    if (!result.isSucceeded || !result.predicateTable
        || result.predicateTable->rgbaValues.size() != 40) {
        return false;
    }

    const auto& values = result.predicateTable->rgbaValues;
    isPassed = SetExpect(
        values[0] == 0.0f && values[1] == 1.0f,
        "Box shape and RemoveInside mode should use exact float enum values.") && isPassed;
    isPassed = SetExpect(
        values[4] == 0.5f && values[7] == -2.0f
            && values[9] == 0.25f && values[11] == -2.0f
            && values[14] == 0.2f && values[15] == -2.0f
            && values[19] == 1.0f,
        "Box texels should store the row-major input-to-box affine inverse.") && isPassed;
    isPassed = SetExpect(
        values[20] == 1.0f && values[21] == 0.0f
            && values[24] == 1.0f && values[25] == 2.0f && values[26] == 3.0f
            && values[28] == 0.0f && values[29] == 0.0f && values[30] == 1.0f,
        "Plane texels should store center and a stable normalized normal.") && isPassed;
    return isPassed;
}

bool StartBadInputCase()
{
    bool isPassed = true;
    auto operation = BuildBox(1);
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ operation }, 2).isSucceeded,
        "nodeCount beyond operation count should be rejected.") && isPassed;

    operation.operationIndex = 0;
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ operation }, 1).isSucceeded,
        "Zero operation index should be rejected.") && isPassed;

    auto duplicate = BuildBox(2);
    duplicate.operationIndex = 1;
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ BuildBox(1), duplicate }, 2).isSucceeded,
        "Duplicate operation indices should be rejected.") && isPassed;

    operation = BuildBox(3);
    operation.geometryType = CropShape::Cylinder;
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ operation }, 1).isSucceeded,
        "Unsupported crop shapes should be rejected.") && isPassed;

    operation = BuildBox(4);
    operation.boxToInputModelMatrix[0] = 0.0;
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ operation }, 1).isSucceeded,
        "Singular box matrices should be rejected.") && isPassed;

    operation = BuildBox(5);
    operation.boxToInputModelMatrix[0] = std::numeric_limits<double>::quiet_NaN();
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ operation }, 1).isSucceeded,
        "Non-finite box matrices should be rejected.") && isPassed;

    operation = BuildPlane(6);
    operation.planeNormalInInputModel = { 0.0, 0.0, 0.0 };
    isPassed = SetExpect(
        !CropAlgorithm::BuildPredicateTable({ operation }, 1).isSucceeded,
        "Zero plane normals should be rejected.") && isPassed;
    return isPassed;
}

bool StartTruthCase()
{
    constexpr float epsilon = 1.0e-6f;
    const auto boxTable = CropAlgorithm::BuildPredicateTable({ BuildBox(1) }, 1);
    bool isPassed = SetExpect(boxTable.isSucceeded, "Identity box should compile.");
    isPassed = SetExpect(
        CropAlgorithm::GetPointKept(*boxTable.predicateTable, 1, { 1.0f - epsilon, 0.0f, 0.0f })
            && CropAlgorithm::GetPointKept(*boxTable.predicateTable, 1, { 1.0f, 0.0f, 0.0f })
            && CropAlgorithm::GetPointKept(*boxTable.predicateTable, 1, { 1.0f + epsilon, 0.0f, 0.0f })
            && !CropAlgorithm::GetPointKept(*boxTable.predicateTable, 1, { 1.0f + 2.0f * epsilon, 0.0f, 0.0f }),
        "Box truth should use abs(q)<=1+1e-6 in float32.") && isPassed;

    const auto keepPlane = CropAlgorithm::BuildPredicateTable({ BuildPlane(2) }, 1);
    isPassed = SetExpect(
        !CropAlgorithm::GetPointKept(*keepPlane.predicateTable, 1, { 0.0f, 0.0f, -1.0f })
            && !CropAlgorithm::GetPointKept(*keepPlane.predicateTable, 1, { 0.0f, 0.0f, 0.0f })
            && CropAlgorithm::GetPointKept(*keepPlane.predicateTable, 1, { 0.0f, 0.0f, 1.0f }),
        "Plane KeepInside should use strict positive dot truth.") && isPassed;

    const auto removePlane = CropAlgorithm::BuildPredicateTable(
        { BuildPlane(3, CropRemovalMode::RemoveInside) },
        1);
    isPassed = SetExpect(
        CropAlgorithm::GetPointKept(*removePlane.predicateTable, 1, { 0.0f, 0.0f, -1.0f })
            && CropAlgorithm::GetPointKept(*removePlane.predicateTable, 1, { 0.0f, 0.0f, 0.0f })
            && !CropAlgorithm::GetPointKept(*removePlane.predicateTable, 1, { 0.0f, 0.0f, 1.0f }),
        "Plane RemoveInside should invert the strict inside result.") && isPassed;
    return isPassed;
}

bool StartPrefixCase()
{
    std::vector<CropOpItem> operations;
    operations.reserve(513);
    for (std::uint64_t index = 1; index <= 513; ++index) {
        auto operation = BuildPlane(index);
        operation.planeCenterInInputModel = { 0.0, 0.0, index == 513 ? 1.0 : -1.0 };
        operations.push_back(operation);
    }

    const auto result = CropAlgorithm::BuildPredicateTable(operations, operations.size());
    return SetExpect(
        result.isSucceeded
            && result.predicateTable
            && result.predicateTable->rgbaValues.size() == 513 * 5 * 4
            && CropAlgorithm::GetPointKept(*result.predicateTable, 512, { 0.0f, 0.0f, 0.0f })
            && !CropAlgorithm::GetPointKept(*result.predicateTable, 513, { 0.0f, 0.0f, 0.0f }),
        "The 513th operation should remain dynamically addressable by nodeCount.");
}

bool StartSnapshotCase()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(2, 2, 2);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    CropInputSnapshot input;
    input.dataSource = OrthogonalCropDataSource::ImageData;
    input.inputVersion = 1;
    input.inputModelBounds = { 0.0, 1.0, 0.0, 1.0, 0.0, 1.0 };
    input.imageData = image;

    bool isPassed = SetExpect(
        CropAlgorithm::GetInputValid(input),
        "A versioned image snapshot with finite bounds should be valid.");
    const auto same = input;
    isPassed = SetExpect(
        CropAlgorithm::GetInputSame(input, same),
        "An unchanged source/version/pointer/bounds snapshot should compare equal.") && isPassed;

    auto changed = input;
    changed.inputVersion = 2;
    isPassed = SetExpect(
        !CropAlgorithm::GetInputSame(input, changed),
        "Input version changes should invalidate snapshot identity.") && isPassed;
    changed = input;
    changed.inputModelBounds[1] = 2.0;
    isPassed = SetExpect(
        !CropAlgorithm::GetInputSame(input, changed),
        "Input bounds changes should invalidate snapshot identity.") && isPassed;
    changed = input;
    changed.inputVersion = 0;
    isPassed = SetExpect(
        !CropAlgorithm::GetInputValid(changed),
        "Zero input versions should be rejected.") && isPassed;
    changed = input;
    changed.polyData = vtkSmartPointer<vtkPolyData>::New();
    isPassed = SetExpect(
        !CropAlgorithm::GetInputValid(changed),
        "Image snapshots should reject a simultaneous PolyData payload.") && isPassed;
    return isPassed;
}

CropExportRequest BuildExportRequest(
    const OrthogonalCropDataSource dataSource,
    const CropOpItem& operation)
{
    CropExportRequest request;
    request.dataSource = dataSource;
    request.operations = { operation };
    request.nodeCount = 1;
    request.inputVersion = 7;
    request.availableRamBytes = 64ULL * 1024ULL * 1024ULL;
    return request;
}

bool StartImageExportCase()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(2, 2, 3, 5, 4, 4);
    image->SetOrigin(-5.0, -2.0, 3.0);
    image->SetSpacing(0.5, 1.5, 2.0);
    vtkNew<vtkMatrix3x3> direction;
    direction->Zero();
    direction->SetElement(0, 1, 1.0);
    direction->SetElement(1, 0, 1.0);
    direction->SetElement(2, 2, 1.0);
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_UNSIGNED_SHORT, 1);
    auto* inputValues = static_cast<unsigned short*>(image->GetScalarPointer());
    inputValues[0] = 10;
    inputValues[1] = 20;
    inputValues[2] = 30;

    auto operation = BuildPlane(1);
    operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    const auto request = BuildExportRequest(
        OrthogonalCropDataSource::ImageData,
        operation);
    const auto result = CropAlgorithm::GetResult(
        image,
        nullptr,
        request,
        BuildPayload(request.operations, request.nodeCount));

    bool isPassed = SetExpect(
        result.isSucceeded
            && result.failureReason == CropFailure::None
            && result.inputVersion == 7
            && result.nodeCount == 1
            && result.operations.size() == 1
            && result.imageData
            && result.maskImage,
        "Image export should return the captured prefix, image copy, and aligned mask.");
    if (!result.isSucceeded || !result.imageData || !result.maskImage) {
        return false;
    }

    const auto* outputValues = static_cast<const unsigned short*>(
        result.imageData->GetScalarPointer());
    const auto* maskValues = static_cast<const unsigned char*>(
        result.maskImage->GetScalarPointer());
    int inputExtent[6] = {};
    int outputExtent[6] = {};
    int maskExtent[6] = {};
    image->GetExtent(inputExtent);
    result.imageData->GetExtent(outputExtent);
    result.maskImage->GetExtent(maskExtent);
    isPassed = SetExpect(
        outputValues[0] == 10 && outputValues[1] == 20 && outputValues[2] == 30
            && inputValues[0] == 10 && inputValues[1] == 20 && inputValues[2] == 30
            && result.imageData->GetPointData()->GetScalars()
                == image->GetPointData()->GetScalars(),
        "Image export should preserve and share the immutable scalar storage.") && isPassed;
    isPassed = SetExpect(
        maskValues[0] == 0 && maskValues[1] == 255 && maskValues[2] == 255
            && std::equal(std::begin(inputExtent), std::end(inputExtent), std::begin(outputExtent))
            && std::equal(std::begin(inputExtent), std::end(inputExtent), std::begin(maskExtent))
            && result.maskImage->GetOrigin()[0] == image->GetOrigin()[0]
            && result.maskImage->GetSpacing()[1] == image->GetSpacing()[1]
            && result.maskImage->GetDirectionMatrix()->GetElement(0, 1) == 1.0
            && result.maskImage->GetScalarType() == VTK_UNSIGNED_CHAR
            && result.maskImage->GetNumberOfScalarComponents() == 1,
        "Image export mask should align non-zero extent/origin/spacing/direction metadata and strict 0/255 truth.") && isPassed;

    auto baselineMask =
        vtkSmartPointer<vtkImageData>::New();
    baselineMask->CopyStructure(image);
    baselineMask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    auto* baselineValues =
        static_cast<unsigned char*>(
            baselineMask->GetScalarPointer());
    baselineValues[0] = 255;
    baselineValues[1] = 255;
    baselineValues[2] = 0;
    const auto mergedResult =
        CropAlgorithm::GetResult(
            image,
            baselineMask,
            request,
            BuildPayload(
                request.operations,
                request.nodeCount));
    const auto* mergedValues =
        mergedResult.maskImage
        ? static_cast<const unsigned char*>(
            mergedResult.maskImage
                ->GetScalarPointer())
        : nullptr;
    isPassed = SetExpect(
        mergedResult.isSucceeded
            && mergedValues
            && mergedValues[0] == 0
            && mergedValues[1] == 255
            && mergedValues[2] == 0,
        "Repeated CPU crop should AND the new predicate with the baseline mask.") && isPassed;

    auto removeAll = operation;
    removeAll.planeCenterInInputModel = { 10.0, 0.0, 0.0 };
    const auto emptyResult = CropAlgorithm::GetResult(
        image,
        nullptr,
        BuildExportRequest(OrthogonalCropDataSource::ImageData, removeAll),
        BuildPayload({ removeAll }, 1));
    isPassed = SetExpect(
        !emptyResult.isSucceeded
            && emptyResult.failureReason == CropFailure::EmptyResult
            && !emptyResult.imageData
            && !emptyResult.maskImage,
        "An all-removed image export should publish EmptyResult without half payloads.") && isPassed;
    return isPassed;
}

bool StartPolyExportCase()
{
    vtkNew<vtkCubeSource> cube;
    cube->SetBounds(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    cube->Update();

    auto operation = BuildPlane(1);
    operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    const auto request = BuildExportRequest(
        OrthogonalCropDataSource::PolyData,
        operation);
    const auto result = CropAlgorithm::GetResult(
        cube->GetOutput(),
        request,
        BuildPayload(request.operations, request.nodeCount));
    double bounds[6] = {};
    if (result.polyData) {
        result.polyData->GetBounds(bounds);
    }
    return SetExpect(
        result.isSucceeded
            && result.polyData
            && result.polyData.GetPointer() != cube->GetOutput()
            && result.polyData->GetNumberOfPoints() > 0
            && result.polyData->GetNumberOfCells() > 0
            && bounds[0] >= -1.0e-6
            && bounds[1] <= 1.0 + 1.0e-6,
        "PolyData export should DeepCopy one clipped kept half from the temporary pipeline.");
}

bool StartRouterTaskCase()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, 1, 0, 0, 0, 0);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* values = static_cast<unsigned char*>(image->GetScalarPointer());
    values[0] = 1;
    values[1] = 2;

    CropInputSnapshot input;
    input.dataSource = OrthogonalCropDataSource::ImageData;
    input.inputVersion = 7;
    input.inputModelBounds = { 0.0, 1.0, -0.5, 0.5, -0.5, 0.5 };
    input.imageData = image;

    auto operation = BuildPlane(1);
    operation.planeCenterInInputModel = { -1.0, 0.0, 0.0 };
    operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    auto request = BuildExportRequest(OrthogonalCropDataSource::ImageData, operation);
    CropRouter router;
    const auto payload = BuildPayload(request.operations, request.nodeCount);
    auto task = router.BuildExportTask(input, request, payload);
    bool isPassed = SetExpect(
        task.has_value(),
        "Router should build one value-capturing task for a matching snapshot.");
    if (!task) {
        return false;
    }
    auto future = task->get_future();
    (*task)();
    const auto result = future.get();
    isPassed = SetExpect(
        result.isSucceeded && result.inputVersion == 7 && result.nodeCount == 1,
        "Router export task should return the captured input version and prefix.") && isPassed;

    request.inputVersion = 8;
    isPassed = SetExpect(
        !router.BuildExportTask(input, request, payload).has_value(),
        "Router should reject request/snapshot version mismatch before worker creation.") && isPassed;
    return isPassed;
}
}

int CropAlgorithmSuite::GetFailCount() const
{
    int failureCount = 0;
    failureCount += StartLayoutCase() ? 0 : 1;
    failureCount += StartBadInputCase() ? 0 : 1;
    failureCount += StartTruthCase() ? 0 : 1;
    failureCount += StartPrefixCase() ? 0 : 1;
    failureCount += StartSnapshotCase() ? 0 : 1;
    failureCount += StartImageExportCase() ? 0 : 1;
    failureCount += StartPolyExportCase() ? 0 : 1;
    failureCount += StartRouterTaskCase() ? 0 : 1;
    return failureCount;
}
