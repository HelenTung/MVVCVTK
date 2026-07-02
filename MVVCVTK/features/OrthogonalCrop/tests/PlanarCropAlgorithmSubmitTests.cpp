// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/tests/PlanarCropAlgorithmSubmitTests.cpp
// 分类: Tests / OrthogonalCrop
// 说明: 平面 image submit 的 mask + background 单遍构建回归测试。
// =====================================================================

#include "Routing/OrthogonalCropBackendRouterService.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double TupleTolerance = 1e-9;

struct TestPlane {
    CropVectorDouble3Array normal = { 1.25, -0.5, 0.75 };
    CropVectorDouble3Array center = { 0.0, 0.0, 0.0 };
};

vtkSmartPointer<vtkImageData> BuildObliqueImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(2, 5, -1, 1, 3, 4);
    image->SetOrigin(10.0, -4.0, 2.5);
    image->SetSpacing(0.5, 1.25, 2.0);

    auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
    direction->SetElement(0, 0, 0.0);
    direction->SetElement(0, 1, -1.0);
    direction->SetElement(0, 2, 0.0);
    direction->SetElement(1, 0, 1.0);
    direction->SetElement(1, 1, 0.0);
    direction->SetElement(1, 2, 0.0);
    direction->SetElement(2, 0, 0.0);
    direction->SetElement(2, 1, 0.0);
    direction->SetElement(2, 2, 1.0);
    image->SetDirectionMatrix(direction);

    image->AllocateScalars(VTK_DOUBLE, 2);

    auto* scalars = image->GetPointData()->GetScalars();
    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    for (vtkIdType tupleId = 0; tupleId < tupleCount; ++tupleId) {
        const double tuple[2] = {
            1000.0 + static_cast<double>(tupleId),
            2000.0 + static_cast<double>(tupleId)
        };
        scalars->SetTuple(tupleId, tuple);
    }

    return image;
}

vtkSmartPointer<vtkImageData> BuildDeepRowBoundaryImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, 700, 0, 1, 0, 1);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->SetSpacing(0.1, 0.7, 0.9);
    image->AllocateScalars(VTK_DOUBLE, 1);

    auto* scalars = image->GetPointData()->GetScalars();
    const vtkIdType tupleCount = scalars->GetNumberOfTuples();
    for (vtkIdType tupleId = 0; tupleId < tupleCount; ++tupleId) {
        const double tuple[1] = { 10.0 + static_cast<double>(tupleId) };
        scalars->SetTuple(tupleId, tuple);
    }

    return image;
}

vtkSmartPointer<vtkImageData> BuildPerformanceImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(0, 159, 0, 159, 0, 63);
    image->SetOrigin(-8.0, 3.0, 1.0);
    image->SetSpacing(0.35, 0.4, 0.8);
    image->AllocateScalars(VTK_UNSIGNED_SHORT, 1);

    auto* scalars = static_cast<unsigned short*>(image->GetScalarPointer(0, 0, 0));
    const vtkIdType tupleCount = image->GetNumberOfPoints();
    for (vtkIdType tupleId = 0; tupleId < tupleCount; ++tupleId) {
        scalars[tupleId] = static_cast<unsigned short>((tupleId % 4096) + 1);
    }

    return image;
}

TestPlane BuildPlaneWithBoundaryIndex(
    vtkImageData* image,
    const int boundaryIndex[3],
    const CropVectorDouble3Array& normal)
{
    TestPlane plane;
    plane.normal = normal;
    double boundaryPoint[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(boundaryIndex, boundaryPoint);
    plane.center = { boundaryPoint[0], boundaryPoint[1], boundaryPoint[2] };
    return plane;
}

bool GetPointIsInsidePlane(
    vtkImageData* image,
    const int index[3],
    const TestPlane& plane)
{
    double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(index, inputModelPoint);

    std::array<double, 3> normal = plane.normal;
    vtkMath::Normalize(normal.data());
    const double offset[3] = {
        inputModelPoint[0] - plane.center[0],
        inputModelPoint[1] - plane.center[1],
        inputModelPoint[2] - plane.center[2]
    };
    return vtkMath::Dot(offset, normal.data()) > 0.0;
}

OrthogonalCropRequest BuildImagePlaneRequest(
    const TestPlane& plane,
    CropRemovalMode removalMode,
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource)
{
    OrthogonalCropRequest request;
    request.SetGeometryType(OrthogonalCropGeometryType::Plane);
    request.SetOperation(operation);
    request.SetDataSource(dataSource);
    request.SetRemovalMode(removalMode);
    request.SetPlaneNormalInInputModel(plane.normal);
    request.SetPlaneCenterInInputModel(plane.center);
    request.SetPlaneHalfExtentsInInputModel({ 2.0, 2.0 });
    return request;
}

OrthogonalCropResult GetImagePlaneResult(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource)
{
    OrthogonalCropBackendRouterService router;
    router.SetInputImage(image);

    const auto request = BuildImagePlaneRequest(plane, removalMode, operation, dataSource);
    return router.GetResult(request);
}

void Expect(bool condition, const std::string& message, int& failureCount)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void VerifySubmitResultContract(
    const OrthogonalCropResult& result,
    CropRemovalMode removalMode,
    int& failureCount)
{
    const auto& statistics = result.GetStatistics();

    Expect(result.GetSucceeded(), "planar submit should succeed.", failureCount);
    Expect(
        result.GetResolvedOperation() == OrthogonalCropOperation::Submit,
        "planar submit result must resolve to Submit operation.",
        failureCount);
    Expect(
        result.GetResolvedDataSource() == OrthogonalCropDataSource::ImageData,
        "planar submit result must resolve to ImageData source.",
        failureCount);
    Expect(
        result.GetResolvedGeometryType() == OrthogonalCropGeometryType::Plane,
        "planar submit result must resolve to Plane geometry.",
        failureCount);
    Expect(
        result.GetResolvedRemovalMode() == removalMode,
        "planar submit result must preserve requested removal mode.",
        failureCount);
    Expect(
        result.GetFailureReason() == OrthogonalCropFailureReason::None,
        "planar submit result must not report failure.",
        failureCount);
    Expect(
        statistics.GetResolvedOperation() == OrthogonalCropOperation::Submit,
        "planar submit statistics must resolve to Submit operation.",
        failureCount);
    Expect(
        statistics.GetResolvedDataSource() == OrthogonalCropDataSource::ImageData,
        "planar submit statistics must resolve to ImageData source.",
        failureCount);
    Expect(
        statistics.GetResolvedGeometryType() == OrthogonalCropGeometryType::Plane,
        "planar submit statistics must resolve to Plane geometry.",
        failureCount);
    Expect(
        statistics.GetResolvedRemovalMode() == removalMode,
        "planar submit statistics must preserve requested removal mode.",
        failureCount);
    Expect(
        statistics.GetFailureReason() == OrthogonalCropFailureReason::None,
        "planar submit statistics must not report failure.",
        failureCount);
    Expect(result.GetSubmitImage() != nullptr, "planar submit image must exist.", failureCount);
    Expect(result.GetMaskImage() != nullptr, "planar submit mask must exist.", failureCount);
    Expect(result.GetOutlinePolyData() == nullptr, "planar submit must not return preview plane outline.", failureCount);
    Expect(result.GetClipPolyData() == nullptr, "planar submit must not return preview clip polydata.", failureCount);
}

void VerifyPreviewResultDoesNotSatisfySubmitContract(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    int& failureCount)
{
    const auto result = GetImagePlaneResult(
        image,
        plane,
        removalMode,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::VolumeData);

    Expect(result.GetSucceeded(), "planar preview contrast case should succeed.", failureCount);
    Expect(
        result.GetResolvedOperation() == OrthogonalCropOperation::Preview,
        "planar preview contrast case must resolve to Preview operation.",
        failureCount);
    Expect(result.GetSubmitImage() == nullptr, "planar preview must not return submit image.", failureCount);
    Expect(result.GetMaskImage() == nullptr, "planar preview must not return submit mask.", failureCount);
    Expect(result.GetOutlinePolyData() != nullptr, "planar preview should return preview plane outline.", failureCount);
}

void VerifySubmitImages(
    vtkImageData* sourceImage,
    vtkImageData* submitImage,
    vtkImageData* maskImage,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    int& failureCount)
{
    auto* sourceScalars = sourceImage->GetPointData()->GetScalars();
    auto* submitScalars = submitImage->GetPointData()->GetScalars();
    double scalarRange[2] = { 0.0, 0.0 };
    sourceScalars->GetRange(scalarRange);
    const double backgroundValue = scalarRange[0];

    int extent[6] = { 0, -1, 0, -1, 0, -1 };
    sourceImage->GetExtent(extent);
    const int dims[3] = {
        extent[1] - extent[0] + 1,
        extent[3] - extent[2] + 1,
        extent[5] - extent[4] + 1
    };
    const vtkIdType rowStride = static_cast<vtkIdType>(dims[0]);
    const vtkIdType sliceStride = static_cast<vtkIdType>(dims[0]) * dims[1];
    auto* maskPtr = static_cast<unsigned char*>(
        maskImage->GetScalarPointer(extent[0], extent[2], extent[4]));

    for (int kOffset = 0; kOffset < dims[2]; ++kOffset) {
        const int k = extent[4] + kOffset;
        const vtkIdType sliceStart = static_cast<vtkIdType>(kOffset) * sliceStride;
        for (int jOffset = 0; jOffset < dims[1]; ++jOffset) {
            const int j = extent[2] + jOffset;
            const vtkIdType rowStart = sliceStart + static_cast<vtkIdType>(jOffset) * rowStride;
            for (int iOffset = 0; iOffset < dims[0]; ++iOffset) {
                const int i = extent[0] + iOffset;
                int index[3] = { i, j, k };
                const vtkIdType linearIndex = rowStart + iOffset;
                const vtkIdType vtkPointId = sourceImage->ComputePointId(index);
                Expect(
                    linearIndex == vtkPointId,
                    "linear index must match VTK point id for non-zero extent image.",
                    failureCount);

                const bool isInside = GetPointIsInsidePlane(sourceImage, index, plane);
                const bool keepVoxel = removalMode == CropRemovalMode::KeepInside
                    ? isInside
                    : !isInside;
                const unsigned char expectedMask = keepVoxel ? 255 : 0;
                Expect(
                    maskPtr[linearIndex] == expectedMask,
                    "mask value does not match half-space classification.",
                    failureCount);

                const int componentCount = sourceScalars->GetNumberOfComponents();
                std::vector<double> sourceTuple(static_cast<std::size_t>(componentCount), 0.0);
                std::vector<double> submitTuple(static_cast<std::size_t>(componentCount), 0.0);
                sourceScalars->GetTuple(linearIndex, sourceTuple.data());
                submitScalars->GetTuple(linearIndex, submitTuple.data());

                for (int component = 0; component < componentCount; ++component) {
                    const double expectedValue = keepVoxel
                        ? sourceTuple[static_cast<std::size_t>(component)]
                        : backgroundValue;
                    Expect(
                        std::abs(submitTuple[static_cast<std::size_t>(component)] - expectedValue)
                            <= TupleTolerance,
                        "submit tuple does not match mask removal decision.",
                        failureCount);
                }
            }
        }
    }
}

void VerifyBoundaryEquality(
    vtkImageData* maskImage,
    const int boundaryIndex[3],
    CropRemovalMode removalMode,
    int& failureCount)
{
    int mutableBoundaryIndex[3] = { boundaryIndex[0], boundaryIndex[1], boundaryIndex[2] };
    const vtkIdType boundaryPointId = maskImage->ComputePointId(mutableBoundaryIndex);
    int extent[6] = { 0, -1, 0, -1, 0, -1 };
    maskImage->GetExtent(extent);
    auto* maskPtr = static_cast<unsigned char*>(
        maskImage->GetScalarPointer(extent[0], extent[2], extent[4]));
    const unsigned char expectedMask = removalMode == CropRemovalMode::KeepInside ? 0 : 255;
    Expect(
        maskPtr[boundaryPointId] == expectedMask,
        "plane boundary must keep strict dot > 0 inside semantics.",
        failureCount);
}

void RunSubmitCase(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    const int boundaryIndex[3],
    CropRemovalMode removalMode,
    int& failureCount)
{
    const auto result = GetImagePlaneResult(
        image,
        plane,
        removalMode,
        OrthogonalCropOperation::Submit,
        OrthogonalCropDataSource::ImageData);

    VerifySubmitResultContract(result, removalMode, failureCount);
    if (!result.GetSucceeded() || !result.GetSubmitImage() || !result.GetMaskImage()) {
        return;
    }

    VerifySubmitImages(
        image,
        result.GetSubmitImage(),
        result.GetMaskImage(),
        plane,
        removalMode,
        failureCount);
    VerifyBoundaryEquality(result.GetMaskImage(), boundaryIndex, removalMode, failureCount);
}

void RunObliqueSubmitCases(int& failureCount)
{
    auto image = BuildObliqueImage();
    const int boundaryIndex[3] = { 3, 0, 3 };
    const auto plane = BuildPlaneWithBoundaryIndex(image, boundaryIndex, { 1.25, -0.5, 0.75 });

    VerifyPreviewResultDoesNotSatisfySubmitContract(
        image,
        plane,
        CropRemovalMode::KeepInside,
        failureCount);
    RunSubmitCase(image, plane, boundaryIndex, CropRemovalMode::KeepInside, failureCount);
    RunSubmitCase(image, plane, boundaryIndex, CropRemovalMode::RemoveInside, failureCount);
}

void RunDeepBoundarySubmitCases(int& failureCount)
{
    auto image = BuildDeepRowBoundaryImage();
    const int boundaryIndex[3] = { 500, 0, 0 };
    const auto plane = BuildPlaneWithBoundaryIndex(image, boundaryIndex, { 1.0, 0.0, 0.0 });

    RunSubmitCase(image, plane, boundaryIndex, CropRemovalMode::KeepInside, failureCount);
    RunSubmitCase(image, plane, boundaryIndex, CropRemovalMode::RemoveInside, failureCount);
}

double MeasureSubmitAverageMilliseconds(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    int iterationCount,
    int& failureCount)
{
    const auto warmup = GetImagePlaneResult(
        image,
        plane,
        removalMode,
        OrthogonalCropOperation::Submit,
        OrthogonalCropDataSource::ImageData);
    Expect(warmup.GetSucceeded(), "planar submit warmup should succeed.", failureCount);

    double totalMilliseconds = 0.0;
    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = GetImagePlaneResult(
            image,
            plane,
            removalMode,
            OrthogonalCropOperation::Submit,
            OrthogonalCropDataSource::ImageData);
        const auto finish = std::chrono::steady_clock::now();
        Expect(result.GetSucceeded(), "planar submit benchmark should succeed.", failureCount);
        Expect(
            result.GetResolvedOperation() == OrthogonalCropOperation::Submit,
            "planar submit benchmark must resolve to Submit operation.",
            failureCount);

        const auto elapsed = std::chrono::duration<double, std::milli>(finish - start);
        totalMilliseconds += elapsed.count();
    }

    return totalMilliseconds / static_cast<double>(iterationCount);
}

void RunPerformanceBenchmark(int& failureCount)
{
    auto image = BuildPerformanceImage();
    const int boundaryIndex[3] = { 80, 80, 32 };
    const auto plane = BuildPlaneWithBoundaryIndex(image, boundaryIndex, { 0.35, -0.2, 1.0 });
    const int iterationCount = 5;
    const auto voxelCount = image->GetNumberOfPoints();

    const double keepInsideMilliseconds = MeasureSubmitAverageMilliseconds(
        image,
        plane,
        CropRemovalMode::KeepInside,
        iterationCount,
        failureCount);
    const double removeInsideMilliseconds = MeasureSubmitAverageMilliseconds(
        image,
        plane,
        CropRemovalMode::RemoveInside,
        iterationCount,
        failureCount);

    std::cout << "Planar submit benchmark voxels=" << voxelCount
              << " iterations=" << iterationCount
              << " keepInsideAvgMs=" << keepInsideMilliseconds
              << " removeInsideAvgMs=" << removeInsideMilliseconds << '\n';
}

} // namespace

int main()
{
    int failureCount = 0;
    RunObliqueSubmitCases(failureCount);
    RunDeepBoundarySubmitCases(failureCount);
    RunPerformanceBenchmark(failureCount);

    if (failureCount != 0) {
        std::cerr << "PlanarCropAlgorithmSubmitTests failed: " << failureCount << '\n';
        return 1;
    }

    std::cout << "PlanarCropAlgorithmSubmitTests passed.\n";
    return 0;
}
