// 测试用途：验证裁剪布局、参数校验、合成真值、历史快照、结果构建与取消。
#include "Algorithms/CropAlgorithm.h"
#include "PlanarTestSuites.h"
#include "Routing/CropRouter.h"
#include "../TestDataPort.h"

#include <vtkCubeSource.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {

DataRevisionRef GetSourceRevision(
    const std::uint64_t generation = 7,
    const std::uint8_t entityKey = 1)
{
    DataEntityId entity;
    entity.bytes[0] = entityKey;
    return { entity, generation };
}

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
    const DataRevisionRef sourceRevision = GetSourceRevision())
{
    const auto tableResult = CropAlgorithm::BuildPredicateTable(
        operations,
        operations.size());
    CropShaderPayload payload;
    payload.revision = revision;
    payload.sourceStamp = { sourceRevision };
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
    operation.geometryType = static_cast<CropShape>(255);
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

    const auto boxTable =
        CropAlgorithm::BuildPredicateTable(
            { BuildBox(7) }, 1);
    auto malformedBox = *boxTable.predicateTable;
    malformedBox.rgbaValues[16] = 1.0f;
    isPassed = SetExpect(
        !CropAlgorithm::GetPointKept(
            malformedBox, 1, { 0.0f, 0.0f, 0.0f }),
        "A non-affine compiled box predicate should be rejected.") && isPassed;

    const auto planeTable =
        CropAlgorithm::BuildPredicateTable(
            { BuildPlane(8) }, 1);
    auto malformedPlane = *planeTable.predicateTable;
    malformedPlane.rgbaValues[8] = 0.0f;
    malformedPlane.rgbaValues[9] = 0.0f;
    malformedPlane.rgbaValues[10] = 0.0f;
    isPassed = SetExpect(
        !CropAlgorithm::GetPointKept(
            malformedPlane, 1, { 0.0f, 0.0f, 1.0f }),
        "A compiled plane predicate with a zero normal should be rejected.") && isPassed;
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

    TestDataPort data;
    const auto imageView = data.SetPrimaryImage(image);
    CropInputSnapshot input;
    input.graph = imageView->graph;
    input.binding = imageView->binding;
    input.data = imageView->data;
    input.inputModelBounds = { 0.0, 1.0, 0.0, 1.0, 0.0, 1.0 };
    input.image = imageView;

    bool isPassed = SetExpect(
        CropAlgorithm::GetInputValid(input),
        "A graph image snapshot with finite bounds should be valid.");
    const auto same = input;
    isPassed = SetExpect(
        CropAlgorithm::GetInputSame(input, same),
        "An unchanged revision and bounds snapshot should compare equal.") && isPassed;

    const auto changedView = data.SetPrimaryImage(image);
    auto changed = input;
    changed.graph = changedView->graph;
    changed.binding = changedView->binding;
    changed.data = changedView->data;
    changed.image = changedView;
    isPassed = SetExpect(
        !CropAlgorithm::GetInputSame(input, changed),
        "A different source revision should invalidate snapshot identity.") && isPassed;
    changed = input;
    changed.inputModelBounds[1] = 2.0;
    isPassed = SetExpect(
        !CropAlgorithm::GetInputSame(input, changed),
        "Input bounds changes should invalidate snapshot identity.") && isPassed;
    changed = input;
    changed.data.reset();
    changed.image.reset();
    isPassed = SetExpect(
        !CropAlgorithm::GetInputValid(changed),
        "A missing source revision should be rejected.") && isPassed;
    changed = input;
    changed.mesh = std::make_shared<const VtkSurfaceMeshView>(
        VtkSurfaceMeshView{
            input.data, vtkSmartPointer<vtkPolyData>::New() });
    isPassed = SetExpect(
        !CropAlgorithm::GetInputValid(changed),
        "Image snapshots should reject a simultaneous PolyData payload.") && isPassed;
    return isPassed;
}

CropBuildParams BuildParams(const CropOpItem& operation)
{
    CropBuildParams params;
    params.sourceRevision = GetSourceRevision();
    params.operations = { operation };
    params.nodeCount = 1;
    params.availableRamBytes = 64ULL * 1024ULL * 1024ULL;
    return params;
}

bool StartImageBuildCase()
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
    const auto params = BuildParams(operation);
    const auto result = CropAlgorithm::GetResult(
        image,
        nullptr,
        params,
        BuildPayload(params.operations, params.nodeCount));

    bool isPassed = SetExpect(
        result.isSucceeded
            && result.failureReason == CropFailure::None
            && result.sourceRevision == params.sourceRevision
            && result.nodeCount == 1
            && result.operations.size() == 1
            && result.imageData
            && result.maskImage,
        "Image build should return the captured prefix, image copy, and aligned mask.");
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
        "Image build should preserve and share the immutable scalar storage.") && isPassed;
    isPassed = SetExpect(
        maskValues[0] == 0 && maskValues[1] == 255 && maskValues[2] == 255
            && std::equal(std::begin(inputExtent), std::end(inputExtent), std::begin(outputExtent))
            && std::equal(std::begin(inputExtent), std::end(inputExtent), std::begin(maskExtent))
            && result.maskImage->GetOrigin()[0] == image->GetOrigin()[0]
            && result.maskImage->GetSpacing()[1] == image->GetSpacing()[1]
            && result.maskImage->GetDirectionMatrix()->GetElement(0, 1) == 1.0
            && result.maskImage->GetScalarType() == VTK_UNSIGNED_CHAR
            && result.maskImage->GetNumberOfScalarComponents() == 1,
        "Image build mask should align non-zero extent/origin/spacing/direction metadata and strict 0/255 truth.") && isPassed;

    auto invalidTransformImage =
        vtkSmartPointer<vtkImageData>::New();
    invalidTransformImage->ShallowCopy(image);
    invalidTransformImage->SetOrigin(
        std::numeric_limits<double>::infinity(),
        -2.0,
        3.0);
    const auto invalidTransformResult =
        CropAlgorithm::GetResult(
            invalidTransformImage,
            nullptr,
            params,
            BuildPayload(
                params.operations,
                params.nodeCount));
    isPassed = SetExpect(
        !invalidTransformResult.isSucceeded
            && invalidTransformResult.failureReason
                == CropFailure::BadInput,
        "A non-finite VTK index transform should be rejected before materialization.") && isPassed;

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
            params,
            BuildPayload(
                params.operations,
                params.nodeCount));
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

    auto upperPlane = BuildPlane(2);
    upperPlane.planeCenterInInputModel = { 2.0, 0.0, 0.0 };
    upperPlane.planeNormalInInputModel = { -1.0, 0.0, 0.0 };
    auto fusedParams = params;
    fusedParams.operations = { operation, upperPlane };
    fusedParams.nodeCount = fusedParams.operations.size();
    const auto fusedResult =
        CropAlgorithm::GetResult(
            image,
            nullptr,
            fusedParams,
            BuildPayload(
                fusedParams.operations,
                fusedParams.nodeCount));
    const auto* fusedValues =
        fusedResult.maskImage
        ? static_cast<const unsigned char*>(
            fusedResult.maskImage->GetScalarPointer())
        : nullptr;
    isPassed = SetExpect(
        fusedResult.isSucceeded
            && fusedValues
            && fusedValues[0] == 0
            && fusedValues[1] == 255
            && fusedValues[2] == 0,
        "A fused predicate prefix should scan the image once and publish only the final mask.") && isPassed;

    auto slabImage =
        vtkSmartPointer<vtkImageData>::New();
    slabImage->SetExtent(-1, 2, 4, 5, 7, 14);
    slabImage->SetOrigin(0.0, 0.0, 0.0);
    slabImage->SetSpacing(1.0, 1.0, 1.0);
    slabImage->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    auto slabMask =
        vtkSmartPointer<vtkImageData>::New();
    slabMask->CopyStructure(slabImage);
    slabMask->AllocateScalars(
        VTK_UNSIGNED_CHAR, 1);
    for (int k = 7; k <= 14; ++k) {
        for (int j = 4; j <= 5; ++j) {
            for (int i = -1; i <= 2; ++i) {
                auto* maskValue =
                    static_cast<unsigned char*>(
                        slabMask->GetScalarPointer(
                            i, j, k));
                *maskValue =
                    (k - 7) % 2 == 0
                    ? 255 : 0;
            }
        }
    }
    auto lowerSlabPlane = BuildPlane(3);
    lowerSlabPlane.planeNormalInInputModel =
        { 1.0, 0.0, 0.0 };
    auto upperSlabPlane = BuildPlane(4);
    upperSlabPlane.planeCenterInInputModel =
        { 1.5, 0.0, 0.0 };
    upperSlabPlane.planeNormalInInputModel =
        { -1.0, 0.0, 0.0 };
    auto slabParams = params;
    slabParams.operations = {
        lowerSlabPlane, upperSlabPlane
    };
    slabParams.nodeCount =
        slabParams.operations.size();
    const auto slabResult =
        CropAlgorithm::GetResult(
            slabImage,
            slabMask,
            slabParams,
            BuildPayload(
                slabParams.operations,
                slabParams.nodeCount));
    std::size_t slabKeptCount = 0;
    bool hasExactSlabMask =
        slabResult.isSucceeded
        && slabResult.maskImage;
    for (int k = 7;
        hasExactSlabMask && k <= 14;
        ++k) {
        for (int j = 4; j <= 5; ++j) {
            for (int i = -1; i <= 2; ++i) {
                const auto* maskValue =
                    static_cast<const unsigned char*>(
                        slabResult.maskImage
                            ->GetScalarPointer(
                                i, j, k));
                const bool isExpected =
                    i == 1
                    && (k - 7) % 2 == 0;
                hasExactSlabMask =
                    maskValue
                    && *maskValue
                        == (isExpected ? 255 : 0);
                slabKeptCount +=
                    maskValue && *maskValue != 0
                    ? 1 : 0;
            }
        }
    }
    isPassed = SetExpect(
        hasExactSlabMask
            && slabKeptCount == 8,
        "Z-slice fusion should keep deterministic counts for negative extents and an input mask.") && isPassed;

    auto removeAll = operation;
    removeAll.planeCenterInInputModel = { 10.0, 0.0, 0.0 };
    const auto emptyResult = CropAlgorithm::GetResult(
        image,
        nullptr,
        BuildParams(removeAll),
        BuildPayload({ removeAll }, 1));
    isPassed = SetExpect(
        !emptyResult.isSucceeded
            && emptyResult.failureReason == CropFailure::EmptyResult
            && !emptyResult.imageData
            && !emptyResult.maskImage,
        "An all-removed image build should publish EmptyResult without half payloads.") && isPassed;
    return isPassed;
}

bool StartCancelledBuildCase()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(128, 32, 8);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    image->GetPointData()->GetScalars()->FillComponent(0, 7);
    auto operation = BuildPlane(1);
    operation.planeCenterInInputModel = {-1, 0, 0};
    operation.planeNormalInInputModel = {1, 0, 0};
    const auto params = BuildParams(operation);
    const auto payload = BuildPayload(params.operations, params.nodeCount);
    const auto before = CropAlgorithm::GetResult(
        image, nullptr, params, payload, 0, [] { return true; });
    std::atomic<int> checks{0};
    const auto during = CropAlgorithm::GetResult(image, nullptr, params, payload, 0,
        [&] { return checks.fetch_add(1) >= 8; });
    const auto after = CropAlgorithm::GetResult(image, nullptr, params, payload);
    return SetExpect(before.isCancelled && !before.isSucceeded && !before.maskImage
        && during.isCancelled && !during.isSucceeded && !during.maskImage
        && !during.imageData && checks.load() >= 9
        && after.isSucceeded && after.maskImage
        && image->GetPointData()->GetScalars()->GetComponent(0, 0) == 7,
        "Cancellation before/during SMP publishes no partial mask and leaves input reusable.");
}

bool StartFourShapeImageCase()
{
    auto image=vtkSmartPointer<vtkImageData>::New();image->SetExtent(-4,4,-4,4,-4,4);
    image->AllocateScalars(VTK_UNSIGNED_CHAR,1);image->GetPointData()->GetScalars()->FillComponent(0,7);
    for(auto shape:{CropShape::Sphere,CropShape::Cylinder})for(auto mode:{CropRemovalMode::KeepInside,CropRemovalMode::RemoveInside}) {
        CropOpItem operation;operation.operationIndex=1;operation.geometryType=shape;operation.removalMode=mode;
        operation.centerInInputModel={0.25,0,0};operation.radius=2.3;operation.height=3.2;operation.axisInInputModel={1,1,0};
        const auto params=BuildParams(operation);const auto payload=BuildPayload(params.operations,params.nodeCount);
        auto result=CropAlgorithm::GetResult(image,nullptr,params,payload);
        if(!SetExpect(result.isSucceeded&&result.maskImage,"Four-shape image candidate failed."))return false;
        const auto* mask=static_cast<const unsigned char*>(result.maskImage->GetScalarPointer());std::size_t index=0;
        for(int z=-4;z<=4;++z)for(int y=-4;y<=4;++y)for(int x=-4;x<=4;++x,++index) {
            const long double dx=static_cast<long double>(x)-0.25L,dy=y,dz=z;
            long double squared=dx*dx+dy*dy+dz*dz;bool cap=true;
            if(shape==CropShape::Cylinder) {
                const long double axial=(dx+dy)/std::sqrt(2.0L);
                squared-=axial*axial;cap=std::abs(axial)<=static_cast<long double>(operation.height)/2;
            }
            const bool inside=cap&&squared<=static_cast<long double>(operation.radius)*operation.radius;
            const bool expected=mode==CropRemovalMode::KeepInside?inside:!inside;
            if(!SetExpect((mask[index]!=0)==expected,"Full-resolution mask differs from independent long-double curved geometry."))return false;
        }
    }
    // Quantizing both coordinates and plane center to float would remove every voxel.
    image->SetExtent(0,2,0,0,0,0);image->SetOrigin(100000000.0,0,0);image->SetSpacing(0.25,1,1);
    image->AllocateScalars(VTK_UNSIGNED_CHAR,1);image->GetPointData()->GetScalars()->FillComponent(0,7);
    auto plane=BuildPlane(1);plane.planeCenterInInputModel={100000000.125,0,0};plane.planeNormalInInputModel={1,0,0};
    const auto params=BuildParams(plane);auto result=CropAlgorithm::GetResult(image,nullptr,params,BuildPayload(params.operations,1));
    const auto* mask=result.maskImage?static_cast<const unsigned char*>(result.maskImage->GetScalarPointer()):nullptr;
    return SetExpect(result.isSucceeded&&mask&&mask[0]==0&&mask[1]==255&&mask[2]==255,
        "CPU mask must retain double geometry and model coordinates at large origins.");
}

bool StartCurvedRecipeCase()
{
    auto image=vtkSmartPointer<vtkImageData>::New();image->SetDimensions(3,3,3);
    image->AllocateScalars(VTK_UNSIGNED_CHAR,1);image->GetPointData()->GetScalars()->FillComponent(0,7);
    TestDataPort data;auto view=data.SetPrimaryImage(image);
    CropInputSnapshot input;input.graph=view->graph;input.binding=view->binding;input.data=view->data;
    input.image=view;input.inputModelBounds={0,2,0,2,0,2};
    for(auto shape:{CropShape::Sphere,CropShape::Cylinder}) {
        CropOpItem operation;operation.operationIndex=1;operation.geometryType=shape;
        operation.centerInInputModel={1,1,1};operation.radius=1.2;operation.height=2;operation.axisInInputModel={2,0,0};
        auto params=BuildParams(operation);params.sourceRevision=view->data->self;
        auto payload=BuildPayload(params.operations,1,1,params.sourceRevision);
        auto task=CropRouter{}.BuildResultTask(input,params,payload);if(!task)return false;
        auto future=task->get_future();(*task)();const auto candidate=future.get();
        if(!SetExpect(candidate.isSucceeded&&candidate.recipePayload&&candidate.preparedView,"Curved worker recipe preparation failed."))return false;
        const auto& primitive=candidate.recipePayload->GetDefinition().nodes.at(1).primitive;
        if(!SetExpect(primitive.shape==(shape==CropShape::Sphere?RoiShape::Sphere:RoiShape::Cylinder)
            &&primitive.origin==operation.centerInInputModel&&primitive.radius==operation.radius
            &&primitive.boundaryPolicy==RoiBoundaryPolicy::CropV1
            &&(shape==CropShape::Sphere||primitive.normal==std::array<double,3>{1,0,0}),
            "Published curved recipe must carry normalized source geometry and versions."))return false;
    }
    std::vector<CropOpItem> deep(4096);
    for(std::size_t i=0;i<deep.size();++i){deep[i].operationIndex=i+1;deep[i].removalMode=CropRemovalMode::RemoveInside;}
    const auto recipe=CropRouter::CreateRecipePayload(deep,view->data->self);
    if(!SetExpect(recipe&&recipe->GetDefinition().nodes.size()==roiNodeLimit,"Maximum history path must fit the bounded unified ROI expression."))return false;
    DataTransaction transaction;transaction.outputs.push_back({data.CreateDataEntityId(),0,DataTypes::roiGeometry,
        {{"source-data",view->data->self}},recipe});
    if(!SetExpect(data.SetDataCommit(std::move(transaction)).status==DataCommitStatus::Succeeded,"Deep balanced recipe must satisfy unified ROI validation."))return false;
    return true;
}

bool StartPolyBuildCase()
{
    vtkNew<vtkCubeSource> cube;
    cube->SetBounds(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    cube->Update();

    auto operation = BuildPlane(1);
    operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    const auto params = BuildParams(operation);
    const auto result = CropAlgorithm::GetResult(
        cube->GetOutput(),
        params,
        BuildPayload(params.operations, params.nodeCount));
    const auto cancelled = CropAlgorithm::GetResult(cube->GetOutput(), params,
        BuildPayload(params.operations, params.nodeCount), [] { return true; });
    double bounds[6] = {};
    if (result.polyData) {
        result.polyData->GetBounds(bounds);
    }
    return SetExpect(
        result.isSucceeded && cancelled.isCancelled && !cancelled.polyData
            && result.polyData
            && result.polyData.GetPointer() != cube->GetOutput()
            && result.polyData->GetNumberOfPoints() > 0
            && result.polyData->GetNumberOfCells() > 0
            && bounds[0] >= -1.0e-6
            && bounds[1] <= 1.0 + 1.0e-6,
        "PolyData build should DeepCopy one clipped kept half from the temporary pipeline.");
}

bool StartCanonicalRootCase()
{
    TestDataPort data;
    auto image=vtkSmartPointer<vtkImageData>::New();image->SetDimensions(3,1,1);image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    image->GetPointData()->GetScalars()->FillComponent(0,7);
    auto mask=vtkSmartPointer<vtkImageData>::New();mask->CopyStructure(image);mask->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    auto* bytes=static_cast<unsigned char*>(mask->GetScalarPointer());bytes[0]=0;bytes[1]=255;bytes[2]=255;
    const auto view=data.SetPrimaryImage(image,mask);
    CropInputSnapshot input;input.graph=view->graph;input.binding=view->binding;input.data=view->data;input.image=view;
    input.inputModelBounds={0,2,0,0,0,0};
    auto operation=BuildPlane(1);operation.planeCenterInInputModel={0.5,0,0};operation.planeNormalInInputModel={1,0,0};
    auto params=BuildParams(operation);params.sourceRevision=view->data->self;
    auto task=CropRouter{}.BuildResultTask(input,params,BuildPayload(params.operations,1,1,params.sourceRevision));if(!task)return false;
    // A writable rendering wrapper is not authority for frozen Root geometry or M0.
    view->image->SetOrigin(1000,0,0);view->validityMask->GetPointData()->GetScalars()->FillComponent(0,0);
    auto future=task->get_future();(*task)();const auto output=future.get();
    const auto source=std::dynamic_pointer_cast<const ImageGrid3DPayload>(view->data->payload);
    const auto payload=std::dynamic_pointer_cast<const ImageGrid3DPayload>(output.outputPayload);
    if(!SetExpect(output.isSucceeded&&payload&&payload->GetValues()==source->GetValues()
        &&payload->GetGeometry().origin==std::array<double,3>{0,0,0}
        &&*payload->GetValidityMask()==std::vector<std::uint8_t>{0,255,255}
        &&output.preparedView->image->image->GetOrigin()[0]==0,
        "Materialization read mutable VTK geometry/mask instead of canonical Root."))return false;
    const auto entity=data.CreateDataEntityId();const DataRevisionRef ref{entity,1};
    auto meshPayload=std::make_shared<const SurfaceMeshPayload>(
        std::vector<double>{100000000.125,0,0,100000000.375,0,0,100000000.125,0.25,0},
        std::vector<std::uint64_t>{0,1,2});
    DataTransaction create;create.outputs.push_back({entity,0,DataTypes::surfaceMesh,{},meshPayload,{}});
    if(data.SetDataCommit(std::move(create)).status!=DataCommitStatus::Succeeded)return false;
    input={};input.graph=data.GetDataGraph();input.data=data.GetData(input.graph,ref);input.mesh=data.GetSurfaceMesh(input.graph,ref);
    input.mesh->mesh->GetBounds(input.inputModelBounds.data());double point[3]={};input.mesh->mesh->GetPoint(0,point);
    if(!SetExpect(point[0]==100000000.125,"Canonical double mesh coordinates were rounded by the VTK bridge."))return false;
    operation.planeCenterInInputModel={100000000,0,0};params=BuildParams(operation);params.sourceRevision=ref;
    task=CropRouter{}.BuildResultTask(input,params,BuildPayload(params.operations,1,1,ref));if(!task)return false;
    for(vtkIdType i=0;i<3;++i)input.mesh->mesh->GetPoints()->SetPoint(i,0,0,0);
    auto meshFuture=task->get_future();(*task)();const auto cropped=meshFuture.get();
    double bounds[6]={};if(cropped.polyData)cropped.polyData->GetBounds(bounds);
    return SetExpect(cropped.isSucceeded&&bounds[0]==100000000.125&&bounds[1]==100000000.375,
        "Materialization read mutable VTK points instead of canonical Root mesh.");
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

    TestDataPort data;
    const auto imageView = data.SetPrimaryImage(image);
    CropInputSnapshot input;
    input.graph = imageView->graph;
    input.binding = imageView->binding;
    input.data = imageView->data;
    input.inputModelBounds = { 0.0, 1.0, -0.5, 0.5, -0.5, 0.5 };
    input.image = imageView;

    auto operation = BuildPlane(1);
    operation.planeCenterInInputModel = { -1.0, 0.0, 0.0 };
    operation.planeNormalInInputModel = { 1.0, 0.0, 0.0 };
    auto params = BuildParams(operation);
    params.sourceRevision = imageView->data->self;
    CropRouter router;
    const auto payload = BuildPayload(
        params.operations, params.nodeCount, 1, params.sourceRevision);
    auto task = router.BuildResultTask(input, params, payload);
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
        result.isSucceeded
            && result.sourceRevision == imageView->data->self
            && result.nodeCount == 1,
        "Router build task should return the captured source revision and prefix.") && isPassed;

    params.sourceRevision = GetSourceRevision(8, 2);
    isPassed = SetExpect(
        !router.BuildResultTask(input, params, payload).has_value(),
        "Router should reject params/snapshot revision mismatch before worker creation.") && isPassed;
    return isPassed;
}
bool StartPublicRoiExtentCase()
{
    auto image=vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(10,12,-4,-3,7,8); image->SetSpacing(.5,2,3); image->SetOrigin(1,2,3);
    const double direction[9]={0,-1,0,1,0,0,0,0,1}; image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    auto* values=static_cast<unsigned char*>(image->GetScalarPointer());
    for (int i=0;i<12;++i) values[i]=static_cast<unsigned char>(i+1);
    auto validity=vtkSmartPointer<vtkImageData>::New(); validity->CopyStructure(image); validity->AllocateScalars(VTK_UNSIGNED_CHAR,1);
    auto* valid=static_cast<unsigned char*>(validity->GetScalarPointer()); std::fill(valid,valid+12,1); valid[0]=0;
    TestDataPort data; const auto view=data.SetPrimaryImage(image,validity);
    if (!view) return SetExpect(false,"nonzero extent fixture failed");
    RoiRequest request; request.definition.source=view->data->self; request.metadata.name="extent";
    RoiNode node; node.primitive.shape=RoiShape::HalfSpace; node.primitive.origin={8,0,0}; node.primitive.normal={1,0,0}; request.definition.nodes={node};
    const auto made=data.SetRoi(request);
    if (!made.roi) return SetExpect(false,"nonzero extent ROI failed");
    const auto roi=data.GetRoi(data.GetDataGraph(),made.roi->revision,view->data->self);
    CropInputSnapshot input; input.graph=view->graph; input.binding=view->binding; input.data=view->data; input.image=view;
    image->GetBounds(input.inputModelBounds.data());
    const auto result=CropAlgorithm::GetRoiResult(input,roi.roi,128ULL*1024*1024,{});
    if (!SetExpect(result.isSucceeded && result.maskImage && result.imageData,"nonzero extent ROI crop failed")) return false;
    const auto* mask=static_cast<const unsigned char*>(result.maskImage->GetScalarPointer());
    const auto* copied=static_cast<const unsigned char*>(result.imageData->GetScalarPointer());
    for (int i=0;i<12;++i) if ((mask[i]!=0)!=(i/3%2==0 && i!=0) || copied[i]!=values[i])
        return SetExpect(false,"nonzero extent/rotated physical mapping or existing validity changed");
    const auto cancelled=CropAlgorithm::GetRoiResult(input,roi.roi,128ULL*1024*1024,[]{return true;});
    if(!SetExpect(!cancelled.isSucceeded && cancelled.isCancelled && !cancelled.imageData,"cancelled public ROI crop leaked output"))return false;
    int polls=0;const auto midChunk=CropAlgorithm::GetRoiResult(input,roi.roi,128ULL*1024*1024,[&]{return ++polls>2;});
    return SetExpect(midChunk.isCancelled&&!midChunk.isSucceeded&&midChunk.failureReason==CropFailure::Cancelled&&!midChunk.outputPayload,
        "ROI chunk cancellation must use the public Cancelled terminal without publishing a partial mask.");
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
    failureCount += StartImageBuildCase() ? 0 : 1;
    failureCount += StartCancelledBuildCase() ? 0 : 1;
    failureCount += StartFourShapeImageCase() ? 0 : 1;
    failureCount += StartCurvedRecipeCase() ? 0 : 1;
    failureCount += StartPolyBuildCase() ? 0 : 1;
    failureCount += StartPublicRoiExtentCase() ? 0 : 1;
    failureCount += StartRouterTaskCase() ? 0 : 1;
    failureCount += StartCanonicalRootCase() ? 0 : 1;
    return failureCount;
}
