// 这个测试独立在根 test/ 下验证裁切算法 submit 契约，不参与主应用链路：
// 1. 用合成 vtkImageData 覆盖非零 extent、方向矩阵和多 component 标量。
// 2. 直接调用 backend router，避免窗口/交互器影响算法结果判断。
// 3. 明确校验 mask 和 submit image 的一致性，防止后续优化只改一条输出链。

#include "Algorithms/OrthogonalCropAlgorithm.h"
#include "Algorithms/PlanarCropAlgorithm.h"
#include "Routing/CropRouter.h"
#include "DataManager.h"
#include "InteractionComputeService.h"
#include "CropBridgeTests.h"
#include "PlanarTestSuites.h"

#include <vtkCamera.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

class DataManagerTest final : public RawVolumeDataManager {
public:
    using BaseDataManager::GetImageSnapshot;
};

class PlanarCropSuite final {
public:

inline static constexpr double TupleTolerance = 1e-9;
inline static constexpr double MatrixTolerance = 1e-9;

struct TestPlane {
    // normal/center 均在 input model physical 坐标系中，和 OrthogonalCropRequest 字段保持同一语义。
    CropVectorDouble3Array normal = { 1.25, -0.5, 0.75 };
    CropVectorDouble3Array center = { 0.0, 0.0, 0.0 };
};

vtkSmartPointer<vtkImageData> BuildOblique()
{
    // 方向矩阵把 image index 轴旋转到 input model physical 坐标：
    // physicalPoint = origin + direction * (index - extentMin) * spacing。
    // 这个场景专门防止 submit 逻辑偷用裸 index 坐标而绕过 VTK 的物理坐标变换。
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

vtkSmartPointer<vtkImageData> BuildDeepImage()
{
    // X 方向超过 512 个点是为了覆盖曾经容易出错的深行索引边界，
    // 同时使用非各向同性 spacing，确保线性下标和物理半空间判断相互独立。
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

vtkSmartPointer<vtkImageData> BuildPerfImage()
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

vtkSmartPointer<vtkImageData> BuildExport()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(3, 4, 2);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->SetSpacing(1.0, 1.0, 1.0);
    image->AllocateScalars(VTK_FLOAT, 1);

    auto* scalars = static_cast<float*>(image->GetScalarPointer());
    const vtkIdType tupleCount = image->GetNumberOfPoints();
    for (vtkIdType tupleId = 0; tupleId < tupleCount; ++tupleId) {
        scalars[tupleId] = static_cast<float>(tupleId + 1);
    }
    image->Modified();
    return image;
}

std::array<double, 16> GetIdentityData()
{
    return {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
}

std::filesystem::path BuildOutputRoot()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("MVVCVTK_export_tests_" + std::to_string(ticks));
}

int GetFileCount(
    const std::filesystem::path& directory,
    const std::string& extension)
{
    int count = 0;
    if (!std::filesystem::exists(directory)) {
        return count;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            ++count;
        }
    }
    return count;
}

bool GetFileBytes(
    const std::filesystem::path& directory,
    const std::string& extension)
{
    if (!std::filesystem::exists(directory)) {
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != extension) {
            continue;
        }
        if (std::filesystem::file_size(entry.path()) > 0) {
            return true;
        }
    }
    return false;
}

TestPlane BuildIndexPlane(
    vtkImageData* image,
    const int boundaryIndex[3],
    const CropVectorDouble3Array& normal)
{
    TestPlane plane;
    plane.normal = normal;
    double boundaryPoint[3] = { 0.0, 0.0, 0.0 };
    // 边界点通过 VTK index -> physical point 变换得到，使测试平面和真实算法使用同一坐标系。
    image->TransformIndexToPhysicalPoint(boundaryIndex, boundaryPoint);
    plane.center = { boundaryPoint[0], boundaryPoint[1], boundaryPoint[2] };
    return plane;
}

bool GetPointInPlane(
    vtkImageData* image,
    const int index[3],
    const TestPlane& plane)
{
    double inputModelPoint[3] = { 0.0, 0.0, 0.0 };
    image->TransformIndexToPhysicalPoint(index, inputModelPoint);

    std::array<double, 3> normal = plane.normal;
    vtkMath::Normalize(normal.data());
    // 半空间公式：signedDistance = dot(inputModelPoint - plane.center, normalize(plane.normal))。
    // 算法约定 signedDistance > 0 才属于 inside，等于 0 的边界点不属于 inside。
    const double offset[3] = {
        inputModelPoint[0] - plane.center[0],
        inputModelPoint[1] - plane.center[1],
        inputModelPoint[2] - plane.center[2]
    };
    return vtkMath::Dot(offset, normal.data()) > 0.0;
}

OrthogonalCropRequest BuildPlaneRequest(
    const TestPlane& plane,
    CropRemovalMode removalMode,
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource)
{
    // 请求只表达 image submit 所需的稳定输入：数据源、几何、保留语义和 input model 坐标中的平面。
    // 这里不设置窗口或交互状态，确保测试覆盖算法边界而不是 host 绑定边界。
    OrthogonalCropRequest request;
    request.geometryType = CropShape::Plane;
    request.operation = operation;
    request.dataSource = dataSource;
    request.removalMode = removalMode;
    request.planeNormalInInputModel = plane.normal;
    request.planeCenterInInputModel = plane.center;
    request.planeHalf = { 2.0, 2.0 };
    return request;
}

OrthogonalCropResult GetPlaneResult(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    OrthogonalCropOperation operation,
    OrthogonalCropDataSource dataSource)
{
    CropRouter router;
    router.SetInputImage(image);

    const auto request = BuildPlaneRequest(plane, removalMode, operation, dataSource);
    return router.GetResult(request);
}

static void SetExpect(bool isExpected, const std::string& message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void StartCropFailure(int& failureCount)
{
    OrthogonalCropRequest request;
    request.dataSource = OrthogonalCropDataSource::PolyData;
    request.operation = OrthogonalCropOperation::Preview;
    request.geometryType = CropShape::Plane;
    request.removalMode = CropRemovalMode::RemoveInside;

    CropRouter router;
    const auto result = router.GetResult(request);
    SetExpect(!result.isSucceeded, "missing polydata route should fail.", failureCount);
    SetExpect(
        result.resolvedDataSource == OrthogonalCropDataSource::PolyData,
        "failure result must preserve the PolyData source.",
        failureCount);
    SetExpect(
        result.resolvedOperation == OrthogonalCropOperation::Preview,
        "failure result must preserve the Preview operation.",
        failureCount);
    SetExpect(
        result.resolvedGeometryType == CropShape::Plane,
        "failure result must preserve the Plane geometry.",
        failureCount);
    SetExpect(
        result.resolvedRemovalMode == CropRemovalMode::RemoveInside,
        "failure result must preserve the RemoveInside mode.",
        failureCount);
    SetExpect(
        result.failureReason == CropFailure::NoPolyData,
        "failure result must report NoPolyData.",
        failureCount);
    SetExpect(!result.message.empty(), "failure result message must not be empty.", failureCount);
}

void StartResultView(int& failureCount)
{
    const auto resultExpect = [&failureCount](
        const OrthogonalCropResult& result,
        const OrthogonalCropRequest& request,
        CropFailure failureReason,
        const std::string& label) {
        SetExpect(
            result.resolvedDataSource == request.dataSource,
            label + " result data source must match the request.",
            failureCount);
        SetExpect(
            result.resolvedOperation == request.operation,
            label + " result operation must match the request.",
            failureCount);
        SetExpect(
            result.resolvedGeometryType == request.geometryType,
            label + " result geometry must match the request.",
            failureCount);
        SetExpect(
            result.resolvedRemovalMode == request.removalMode,
            label + " result removal mode must match the request.",
            failureCount);
        SetExpect(
            result.failureReason == failureReason,
            label + " result failure reason must match the expected value.",
            failureCount);
        if (failureReason != CropFailure::None) {
            SetExpect(
                !result.message.empty(),
                label + " failure message must not be empty.",
                failureCount);
        }
    };
    const auto requestFactory = [](
        CropShape geometryType,
        OrthogonalCropOperation operation,
        OrthogonalCropDataSource dataSource) {
        OrthogonalCropRequest request;
        request.geometryType = geometryType;
        request.operation = operation;
        request.dataSource = dataSource;
        request.removalMode = CropRemovalMode::KeepInside;
        return request;
    };

    auto factoryRequest = requestFactory(
        CropShape::Plane,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::PolyData);
    factoryRequest.removalMode = CropRemovalMode::RemoveInside;
    OrthogonalCropResult factoryResult;
    factoryResult.resolvedDataSource = factoryRequest.dataSource;
    factoryResult.resolvedOperation = factoryRequest.operation;
    factoryResult.resolvedGeometryType = factoryRequest.geometryType;
    factoryResult.resolvedRemovalMode = factoryRequest.removalMode;
    factoryResult.failureReason = CropFailure::NoPolyData;
    factoryResult.message = "Synthetic missing polydata.";
    resultExpect(factoryResult, factoryRequest, CropFailure::NoPolyData, "failure factory");

    OrthogonalCropResult setterResult;
    setterResult.resolvedDataSource = factoryRequest.dataSource;
    setterResult.resolvedOperation = factoryRequest.operation;
    setterResult.resolvedGeometryType = factoryRequest.geometryType;
    setterResult.resolvedRemovalMode = factoryRequest.removalMode;
    setterResult.failureReason = CropFailure::NoPolyData;
    setterResult.message = "Synthetic setter failure.";
    resultExpect(setterResult, factoryRequest, CropFailure::NoPolyData, "result setters");

    auto image = BuildExport();
    double imageInputBounds[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }; // image input model 坐标 bounds
    image->GetBounds(imageInputBounds);
    const CropBoundsDouble6Array boxInputBounds = {
        imageInputBounds[0], imageInputBounds[1],
        imageInputBounds[2], imageInputBounds[3],
        imageInputBounds[4], imageInputBounds[5]
    };

    // 1. 先验证 Box 三条合法公开算法路径，防止护栏误拒绝全部请求。
    // 2. 再按 geometry / operation / dataSource 单轴构造非法 image route。
    // 3. 最后覆盖 Box / Plane 两个 polydata overload 的非法 route。
    auto boxPreviewRequest = requestFactory(
        CropShape::Box,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::VolumeData);
    boxPreviewRequest.boxToInputModelMatrix = CropGeometryAlgorithm::GetBoxMatrix(boxInputBounds);
    const auto boxPreviewResult = OrthogonalCropAlgorithm::GetResult(image, boxPreviewRequest);
    SetExpect(boxPreviewResult.isSucceeded, "valid box preview route should succeed.", failureCount);
    resultExpect(boxPreviewResult, boxPreviewRequest, CropFailure::None, "valid box preview");

    auto boxSubmitRequest = requestFactory(
        CropShape::Box,
        OrthogonalCropOperation::Submit,
        OrthogonalCropDataSource::ImageData);
    boxSubmitRequest.boxToInputModelMatrix = CropGeometryAlgorithm::GetBoxMatrix(boxInputBounds);
    boxSubmitRequest.removalMode = CropRemovalMode::RemoveInside;
    const auto boxSubmitResult = OrthogonalCropAlgorithm::GetResult(image, boxSubmitRequest);
    SetExpect(boxSubmitResult.isSucceeded, "valid box submit route should succeed.", failureCount);
    SetExpect(boxSubmitResult.submitImage != nullptr, "valid box submit must return an image.", failureCount);
    SetExpect(boxSubmitResult.maskImage != nullptr, "valid box submit must return a mask.", failureCount);
    resultExpect(boxSubmitResult, boxSubmitRequest, CropFailure::None, "valid box submit");

    auto boxPolyRequest = requestFactory(
        CropShape::Box,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::PolyData);
    boxPolyRequest.boxToInputModelMatrix = CropGeometryAlgorithm::GetBoxMatrix(boxInputBounds);
    const auto boxPolyResult = OrthogonalCropAlgorithm::GetResult(
        boxPreviewResult.outlinePolyData,
        boxPolyRequest);
    SetExpect(boxPolyResult.isSucceeded, "valid box polydata route should succeed.", failureCount);
    resultExpect(boxPolyResult, boxPolyRequest, CropFailure::None, "valid box polydata preview");

    struct DirectRouteCase {
        const char* label = "";
        bool isPlaneAlgo = false;
        bool isPolyInput = false;
        OrthogonalCropRequest request;
    };
    const std::array<DirectRouteCase, 8> routeCases = {{
        { "box geometry mismatch", false, false, requestFactory(CropShape::Plane, OrthogonalCropOperation::Preview, OrthogonalCropDataSource::VolumeData) },
        { "box operation mismatch", false, false, requestFactory(CropShape::Box, OrthogonalCropOperation::None, OrthogonalCropDataSource::VolumeData) },
        { "box source mismatch", false, false, requestFactory(CropShape::Box, OrthogonalCropOperation::Submit, OrthogonalCropDataSource::VolumeData) },
        { "box polydata mismatch", false, true, requestFactory(CropShape::Box, OrthogonalCropOperation::Submit, OrthogonalCropDataSource::PolyData) },
        { "plane geometry mismatch", true, false, requestFactory(CropShape::Box, OrthogonalCropOperation::Preview, OrthogonalCropDataSource::VolumeData) },
        { "plane operation mismatch", true, false, requestFactory(CropShape::Plane, OrthogonalCropOperation::None, OrthogonalCropDataSource::VolumeData) },
        { "plane source mismatch", true, false, requestFactory(CropShape::Plane, OrthogonalCropOperation::Submit, OrthogonalCropDataSource::VolumeData) },
        { "plane polydata mismatch", true, true, requestFactory(CropShape::Plane, OrthogonalCropOperation::Submit, OrthogonalCropDataSource::PolyData) }
    }};

    for (const auto& routeCase : routeCases) {
        OrthogonalCropResult routeResult;
        if (routeCase.isPlaneAlgo) {
            routeResult = routeCase.isPolyInput
                ? PlanarCropAlgorithm::GetResult(static_cast<vtkPolyData*>(nullptr), routeCase.request)
                : PlanarCropAlgorithm::GetResult(image, routeCase.request);
        }
        else {
            routeResult = routeCase.isPolyInput
                ? OrthogonalCropAlgorithm::GetResult(static_cast<vtkPolyData*>(nullptr), routeCase.request)
                : OrthogonalCropAlgorithm::GetResult(image, routeCase.request);
        }

        const std::string label = routeCase.label;
        SetExpect(!routeResult.isSucceeded, label + " should fail.", failureCount);
        resultExpect(routeResult, routeCase.request, CropFailure::NoBackend, label);
    }
}

void SetResultExpect(
    const OrthogonalCropResult& result,
    CropRemovalMode removalMode,
    int& failureCount)
{
    SetExpect(result.isSucceeded, "planar submit should succeed.", failureCount);
    SetExpect(
        result.resolvedOperation == OrthogonalCropOperation::Submit,
        "planar submit result must resolve to Submit operation.",
        failureCount);
    SetExpect(
        result.resolvedDataSource == OrthogonalCropDataSource::ImageData,
        "planar submit result must resolve to ImageData source.",
        failureCount);
    SetExpect(
        result.resolvedGeometryType == CropShape::Plane,
        "planar submit result must resolve to Plane geometry.",
        failureCount);
    SetExpect(
        result.resolvedRemovalMode == removalMode,
        "planar submit result must preserve requested removal mode.",
        failureCount);
    SetExpect(
        result.failureReason == CropFailure::None,
        "planar submit result must not report failure.",
        failureCount);
    SetExpect(result.submitImage != nullptr, "planar submit image must exist.", failureCount);
    SetExpect(result.maskImage != nullptr, "planar submit mask must exist.", failureCount);
    SetExpect(result.outlinePolyData == nullptr, "planar submit must not return preview plane outline.", failureCount);
    SetExpect(result.clipPolyData == nullptr, "planar submit must not return preview clip polydata.", failureCount);
}

void SetPreviewTest(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    int& failureCount)
{
    const auto result = GetPlaneResult(
        image,
        plane,
        removalMode,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::VolumeData);

    SetExpect(result.isSucceeded, "planar preview contrast case should succeed.", failureCount);
    SetExpect(
        result.resolvedOperation == OrthogonalCropOperation::Preview,
        "planar preview contrast case must resolve to Preview operation.",
        failureCount);
    SetExpect(result.submitImage == nullptr, "planar preview must not return submit image.", failureCount);
    SetExpect(result.maskImage == nullptr, "planar preview must not return submit mask.", failureCount);
    SetExpect(result.outlinePolyData == nullptr, "planar preview must not return a misleading finite plane outline.", failureCount);
}

void SetSubmitExpect(
    vtkImageData* sourceImage,
    vtkImageData* submitImage,
    vtkImageData* maskImage,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    int& failureCount)
{
    // submit image 和 mask 必须逐点一致：
    // A. isVoxelKept=true 时，submit 保留原始 tuple，mask=255。
    // B. isVoxelKept=false 时，submit 写入背景值，mask=0。
    // C. 背景值取输入标量最小值，避免测试依赖某个硬编码灰度。
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
                // mask buffer 从 extent 起点取指针，所以本地 linearIndex 必须使用 offset 计算；
                // 再与 VTK ComputePointId 对照，确保非零 extent 场景没有整体平移错误。
                const vtkIdType linearIndex = rowStart + iOffset;
                const vtkIdType vtkPointId = sourceImage->ComputePointId(index);
                SetExpect(
                    linearIndex == vtkPointId,
                    "linear index must match VTK point id for non-zero extent image.",
                    failureCount);

                const bool isInside = GetPointInPlane(sourceImage, index, plane);
                const bool isVoxelKept = removalMode == CropRemovalMode::KeepInside
                    ? isInside
                    : !isInside;
                const unsigned char expectedMask = isVoxelKept ? 255 : 0;
                SetExpect(
                    maskPtr[linearIndex] == expectedMask,
                    "mask value does not match half-space classification.",
                    failureCount);

                const int componentCount = sourceScalars->GetNumberOfComponents();
                std::vector<double> sourceTuple(static_cast<std::size_t>(componentCount), 0.0);
                std::vector<double> submitTuple(static_cast<std::size_t>(componentCount), 0.0);
                sourceScalars->GetTuple(linearIndex, sourceTuple.data());
                submitScalars->GetTuple(linearIndex, submitTuple.data());

                for (int component = 0; component < componentCount; ++component) {
                    const double expectedValue = isVoxelKept
                        ? sourceTuple[static_cast<std::size_t>(component)]
                        : backgroundValue;
                    SetExpect(
                        std::abs(submitTuple[static_cast<std::size_t>(component)] - expectedValue)
                            <= TupleTolerance,
                        "submit tuple does not match mask removal decision.",
                        failureCount);
                }
            }
        }
    }
}

void SetBoundExpect(
    vtkImageData* maskImage,
    const int boundaryIndex[3],
    CropRemovalMode removalMode,
    int& failureCount)
{
    // 边界点 signedDistance == 0。严格 inside 使用 dot > 0，
    // 所以 KeepInside 下边界被移除，RemoveInside 下边界被保留。
    int mutableBoundaryIndex[3] = { boundaryIndex[0], boundaryIndex[1], boundaryIndex[2] };
    const vtkIdType boundaryPointId = maskImage->ComputePointId(mutableBoundaryIndex);
    int extent[6] = { 0, -1, 0, -1, 0, -1 };
    maskImage->GetExtent(extent);
    auto* maskPtr = static_cast<unsigned char*>(
        maskImage->GetScalarPointer(extent[0], extent[2], extent[4]));
    const unsigned char expectedMask = removalMode == CropRemovalMode::KeepInside ? 0 : 255;
    SetExpect(
        maskPtr[boundaryPointId] == expectedMask,
        "plane boundary must keep strict dot > 0 inside semantics.",
        failureCount);
}

void StartSubmitCase(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    const int boundaryIndex[3],
    CropRemovalMode removalMode,
    int& failureCount)
{
    const auto result = GetPlaneResult(
        image,
        plane,
        removalMode,
        OrthogonalCropOperation::Submit,
        OrthogonalCropDataSource::ImageData);

    SetResultExpect(result, removalMode, failureCount);
    if (!result.isSucceeded || !result.submitImage || !result.maskImage) {
        return;
    }

    SetSubmitExpect(
        image,
        result.submitImage,
        result.maskImage,
        plane,
        removalMode,
        failureCount);
    SetBoundExpect(result.maskImage, boundaryIndex, removalMode, failureCount);
}

void StartOblique(int& failureCount)
{
    auto image = BuildOblique();
    const int boundaryIndex[3] = { 3, 0, 3 };
    const auto plane = BuildIndexPlane(image, boundaryIndex, { 1.25, -0.5, 0.75 });

    SetPreviewTest(
        image,
        plane,
        CropRemovalMode::KeepInside,
        failureCount);
    StartSubmitCase(image, plane, boundaryIndex, CropRemovalMode::KeepInside, failureCount);
    StartSubmitCase(image, plane, boundaryIndex, CropRemovalMode::RemoveInside, failureCount);
}

void StartDeepCases(int& failureCount)
{
    auto image = BuildDeepImage();
    const int boundaryIndex[3] = { 500, 0, 0 };
    const auto plane = BuildIndexPlane(image, boundaryIndex, { 1.0, 0.0, 0.0 });

    StartSubmitCase(image, plane, boundaryIndex, CropRemovalMode::KeepInside, failureCount);
    StartSubmitCase(image, plane, boundaryIndex, CropRemovalMode::RemoveInside, failureCount);
}

double GetSubmitAvgMs(
    vtkSmartPointer<vtkImageData> image,
    const TestPlane& plane,
    CropRemovalMode removalMode,
    int iterationCount,
    int& failureCount)
{
    const auto warmup = GetPlaneResult(
        image,
        plane,
        removalMode,
        OrthogonalCropOperation::Submit,
        OrthogonalCropDataSource::ImageData);
    SetExpect(warmup.isSucceeded, "planar submit warmup should succeed.", failureCount);

    double totalMilliseconds = 0.0;
    for (int iteration = 0; iteration < iterationCount; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = GetPlaneResult(
            image,
            plane,
            removalMode,
            OrthogonalCropOperation::Submit,
            OrthogonalCropDataSource::ImageData);
        const auto finish = std::chrono::steady_clock::now();
        SetExpect(result.isSucceeded, "planar submit benchmark should succeed.", failureCount);
        SetExpect(
            result.resolvedOperation == OrthogonalCropOperation::Submit,
            "planar submit benchmark must resolve to Submit operation.",
            failureCount);

        const auto elapsed = std::chrono::duration<double, std::milli>(finish - start);
        totalMilliseconds += elapsed.count();
    }

    return totalMilliseconds / static_cast<double>(iterationCount);
}

void StartBench(int& failureCount)
{
    auto image = BuildPerfImage();
    const int boundaryIndex[3] = { 80, 80, 32 };
    const auto plane = BuildIndexPlane(image, boundaryIndex, { 0.35, -0.2, 1.0 });
    const int iterationCount = 5;
    const auto voxelCount = image->GetNumberOfPoints();

    const double keepInsideMilliseconds = GetSubmitAvgMs(
        image,
        plane,
        CropRemovalMode::KeepInside,
        iterationCount,
        failureCount);
    const double removeInsideMilliseconds = GetSubmitAvgMs(
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

void StartSliceView(int& failureCount)
{
    const auto identity = GetIdentityData();
    const std::array<double, 3> cursorWorld = { 0.0, 0.0, 0.0 };

    const auto topDownData = InteractionComputeService::GetSliceExportData(
        identity,
        VizMode::SliceTop_down,
        cursorWorld);
    SetExpect(topDownData.has_value(), "slice export data should exist for a real slice mode.", failureCount);
    if (topDownData) {
        SetExpect(
            topDownData->orientation == Orientation::Top_down,
            "slice export data should preserve Top_down orientation.",
            failureCount);
        for (std::size_t index = 0; index < identity.size(); ++index) {
            SetExpect(
                std::abs(topDownData->matrix[index] - identity[index]) <= MatrixTolerance,
                "slice export matrix should keep the upstream model matrix.",
                failureCount);
        }
    }

    std::array<double, 16> rotatedModelToWorld = identity;
    rotatedModelToWorld[0] = 0.0;
    rotatedModelToWorld[1] = -1.0;
    rotatedModelToWorld[4] = 1.0;
    rotatedModelToWorld[5] = 0.0;
    const auto rotatedData = InteractionComputeService::GetSliceExportData(
        rotatedModelToWorld,
        VizMode::SliceTop_down,
        cursorWorld);
    SetExpect(rotatedData.has_value(), "slice export data should accept the current model matrix snapshot.", failureCount);
    if (rotatedData) {
        for (std::size_t index = 0; index < identity.size(); ++index) {
            SetExpect(
                std::abs(rotatedData->matrix[index] - rotatedModelToWorld[index]) <= MatrixTolerance,
                "slice export matrix should not apply a second angle when host angle is absent.",
                failureCount);
        }
    }

    const auto customAngleData = InteractionComputeService::GetSliceExportData(
        identity,
        VizMode::SliceTop_down,
        cursorWorld,
        90.0);
    SetExpect(customAngleData.has_value(), "slice export data should accept an optional host-provided angle.", failureCount);
    if (customAngleData) {
        bool hasMatrixChange = false;
        for (std::size_t index = 0; index < identity.size(); ++index) {
            if (std::abs(customAngleData->matrix[index] - identity[index]) > MatrixTolerance) {
                hasMatrixChange = true;
                break;
            }
        }
        SetExpect(
            hasMatrixChange,
            "optional host angle should affect the slice export matrix.",
            failureCount);
    }

    const auto invalidModeData = InteractionComputeService::GetSliceExportData(
        identity,
        VizMode::CompositeIsoSurface,
        cursorWorld);
    SetExpect(
        !invalidModeData.has_value(),
        "slice export data should reject non-slice view modes instead of falling back to Top_down.",
        failureCount);
}

void StartDataExport(int& failureCount)
{
    DataManagerTest dataManager;
    auto image = BuildExport();
    const auto identity = GetIdentityData();
    const auto outputRoot = BuildOutputRoot();
    std::error_code createError;
    std::filesystem::create_directories(outputRoot, createError);
    SetExpect(!createError, "data export test should create its temporary output directory.", failureCount);

    const auto initialVersion = dataManager.GetDataVersion();
    auto* const submittedImage = image.GetPointer();
    SetExpect(
        dataManager.SetImageSnapshot(image),
        "data export setup should accept a synthetic vtkImageData snapshot.",
        failureCount);
    SetExpect(
        dataManager.GetDataVersion() == initialVersion,
        "pending image snapshot should not change the current data version.",
        failureCount);
    SetExpect(
        dataManager.SetCurrentFromPending(),
        "data export setup should promote the pending image into the current data manager image.",
        failureCount);
    SetExpect(
        dataManager.GetDataVersion() == initialVersion + 1,
        "consuming a pending image should advance the current data version.",
        failureCount);

    const auto currentSnapshot = dataManager.GetImageState();
    const auto currentRange = dataManager.GetScalarRange();
    SetExpect(
        image.GetPointer() == submittedImage
            && currentSnapshot.image != image
            && currentSnapshot.image->GetScalarPointer() != image->GetScalarPointer()
            && currentSnapshot.dims == std::array<int, 3>{ 3, 4, 2 }
            && currentSnapshot.spacing == dataManager.GetSpacing()
            && currentSnapshot.origin == std::array<double, 3>{ 0.0, 0.0, 0.0 }
            && currentSnapshot.version == dataManager.GetDataVersion(),
        "pending image should be isolated in one metadata and version batch.",
        failureCount);

    const auto currentOwner = dataManager.GetImageSnapshot();
    const auto sharedOwner = dataManager.GetImageSnapshot();
    SetExpect(
        currentOwner
            && sharedOwner
            && currentOwner == sharedOwner
            && currentOwner->image == sharedOwner->image
            && currentOwner->image->GetScalarPointer()
                == sharedOwner->image->GetScalarPointer()
            && currentOwner->version == currentSnapshot.version,
        "internal reads of one version must share one immutable snapshot owner.",
        failureCount);

    const auto sharedSnapshot = dataManager.GetImageState();
    const auto publicImage = dataManager.GetVtkImage();
    SetExpect(
        sharedSnapshot.image != currentSnapshot.image
            && sharedSnapshot.image->GetScalarPointer() != currentSnapshot.image->GetScalarPointer()
            && publicImage != sharedSnapshot.image
            && publicImage->GetScalarPointer() != sharedSnapshot.image->GetScalarPointer()
            && publicImage != currentOwner->image
            && publicImage->GetScalarPointer() != currentOwner->image->GetScalarPointer()
            && sharedSnapshot.dims == currentSnapshot.dims
            && sharedSnapshot.spacing == currentSnapshot.spacing
            && sharedSnapshot.origin == currentSnapshot.origin
            && sharedSnapshot.scalarRange == currentSnapshot.scalarRange
            && sharedSnapshot.version == currentSnapshot.version
            && static_cast<float*>(sharedSnapshot.image->GetScalarPointer())[0] == 1.0f,
        "repeated reads of one version must return independent images with identical batch data.",
        failureCount);

    image->SetOrigin(9.0, 8.0, 7.0);
    static_cast<float*>(image->GetScalarPointer())[0] = -1234.0f;
    SetExpect(
        currentSnapshot.origin == std::array<double, 3>{ 0.0, 0.0, 0.0 }
            && static_cast<float*>(currentSnapshot.image->GetScalarPointer())[0] == 1.0f,
        "retained input aliases must not modify the DataManager batch.",
        failureCount);

    currentSnapshot.image->SetOrigin(6.0, 5.0, 4.0);
    currentSnapshot.image->SetSpacing(6.0, 5.0, 4.0);
    static_cast<float*>(currentSnapshot.image->GetScalarPointer())[0] = -5678.0f;

    const auto cleanSnapshot = dataManager.GetImageState();
    const std::array<double, 3> cleanImageSpacing = {
        cleanSnapshot.image->GetSpacing()[0],
        cleanSnapshot.image->GetSpacing()[1],
        cleanSnapshot.image->GetSpacing()[2]
    };
    SetExpect(
        cleanSnapshot.image != currentSnapshot.image
            && cleanSnapshot.image->GetScalarPointer() != currentSnapshot.image->GetScalarPointer()
            && cleanSnapshot.origin == std::array<double, 3>{ 0.0, 0.0, 0.0 }
            && cleanSnapshot.spacing == std::array<double, 3>{ 1.0, 1.0, 1.0 }
            && cleanImageSpacing == cleanSnapshot.spacing
            && static_cast<float*>(cleanSnapshot.image->GetScalarPointer())[0] == 1.0f
            && cleanSnapshot.scalarRange == currentRange
            && cleanSnapshot.version == currentSnapshot.version,
        "mutating one public snapshot must not pollute later reads of the same version.",
        failureCount);

    const std::array<double, 3> updatedSpacing = { 0.75, 1.25, 2.5 };
    SetExpect(
        dataManager.SetSpacing(updatedSpacing),
        "current spacing update should be accepted by DataManager.",
        failureCount);
    const auto spacingSnapshot = dataManager.GetImageState();
    const auto spacingOwner = dataManager.GetImageSnapshot();
    const std::array<double, 3> imageSpacing = {
        spacingSnapshot.image->GetSpacing()[0],
        spacingSnapshot.image->GetSpacing()[1],
        spacingSnapshot.image->GetSpacing()[2]
    };
    SetExpect(
        spacingSnapshot.spacing == updatedSpacing
            && imageSpacing == updatedSpacing
            && spacingSnapshot.image != currentSnapshot.image
            && spacingSnapshot.image->GetScalarPointer() != currentSnapshot.image->GetScalarPointer()
            && static_cast<float*>(spacingSnapshot.image->GetScalarPointer())[0] == 1.0f
            && spacingSnapshot.origin == std::array<double, 3>{ 0.0, 0.0, 0.0 }
            && spacingSnapshot.image->GetOrigin()[0] == 0.0
            && spacingSnapshot.image->GetOrigin()[1] == 0.0
            && spacingSnapshot.image->GetOrigin()[2] == 0.0
            && currentSnapshot.image->GetSpacing()[0] == 6.0
            && currentSnapshot.image->GetSpacing()[1] == 5.0
            && currentSnapshot.image->GetSpacing()[2] == 4.0
            && spacingSnapshot.version == currentSnapshot.version + 1,
        "spacing update must preserve internal voxels while publishing one isolated version copy.",
        failureCount);
    SetExpect(
        spacingOwner
            && spacingOwner != currentOwner
            && spacingOwner->image != currentOwner->image
            && spacingOwner->image->GetScalarPointer()
                == currentOwner->image->GetScalarPointer()
            && spacingOwner->spacing == updatedSpacing
            && spacingOwner->version == currentOwner->version + 1
            && currentOwner->spacing == std::array<double, 3>{ 1.0, 1.0, 1.0 }
            && currentOwner->image->GetSpacing()[0] == 1.0
            && currentOwner->image->GetSpacing()[1] == 1.0
            && currentOwner->image->GetSpacing()[2] == 1.0
            && static_cast<float*>(currentOwner->image->GetScalarPointer())[0] == 1.0f,
        "a new version must preserve the retired snapshot while sharing read-only voxels.",
        failureCount);
    SetExpect(
        dataManager.SetSpacing(updatedSpacing)
            && dataManager.GetDataVersion() == spacingSnapshot.version,
        "same spacing update must be a no-op without a new version.",
        failureCount);

    const auto missingPath = outputRoot / "missing_2x2x2.raw";
    SetExpect(
        !dataManager.SetDataLoaded(
            missingPath.string(),
            { 1.0f, 1.0f, 1.0f },
            { 0.0f, 0.0f, 0.0f }),
        "missing raw input should report failure.",
        failureCount);
    const auto afterFailure = dataManager.GetImageState();
    SetExpect(
        afterFailure.image != spacingSnapshot.image
            && afterFailure.image->GetScalarPointer() != spacingSnapshot.image->GetScalarPointer()
            && afterFailure.dims == spacingSnapshot.dims
            && afterFailure.spacing == spacingSnapshot.spacing
            && afterFailure.origin == spacingSnapshot.origin
            && afterFailure.version == spacingSnapshot.version
            && dataManager.GetScalarRange() == currentRange,
        "failed raw load must preserve the complete current transaction.",
        failureCount);

    const std::filesystem::path rawRequestPath = outputRoot / "volume.raw";
    SetExpect(
        dataManager.ExportData(rawRequestPath.string(), identity),
        "transformed data export should write a RAW file for synthetic input.",
        failureCount);
    SetExpect(
        GetFileCount(outputRoot, ".raw") == 1,
        "transformed data export should create exactly one RAW file in the requested folder.",
        failureCount);
    SetExpect(
        GetFileBytes(outputRoot, ".raw"),
        "transformed data export should create a non-empty RAW file.",
        failureCount);

    const std::filesystem::path sliceDir = outputRoot / "top_down_slices";
    WindowLevelParams windowLevel;
    windowLevel.windowWidth = 32.0;
    windowLevel.windowCenter = 16.0;
    SetExpect(
        !dataManager.ExportSlices("", Orientation::Top_down, windowLevel, identity),
        "slice image export should reject an empty output directory.",
        failureCount);
    SetExpect(
        dataManager.ExportSlices(sliceDir.string(), Orientation::Top_down, windowLevel, identity),
        "slice image export should write PNG slices for synthetic input.",
        failureCount);
    SetExpect(
        GetFileCount(sliceDir, ".png") == 2,
        "top-down slice export should create one PNG per z slice.",
        failureCount);
    SetExpect(
        GetFileBytes(sliceDir, ".png"),
        "slice image export should create non-empty PNG files.",
        failureCount);

    auto replacementImage = vtkSmartPointer<vtkImageData>::New();
    replacementImage->SetDimensions(2, 3, 1);
    replacementImage->SetSpacing(2.0, 3.0, 4.0);
    replacementImage->SetOrigin(5.0, 6.0, 7.0);
    replacementImage->AllocateScalars(VTK_FLOAT, 1);
    auto* replacementScalars = static_cast<float*>(replacementImage->GetScalarPointer());
    for (vtkIdType tupleId = 0; tupleId < replacementImage->GetNumberOfPoints(); ++tupleId) {
        replacementScalars[tupleId] = 77.0f + static_cast<float>(tupleId);
    }
    SetExpect(
        dataManager.SetImageSnapshot(replacementImage)
            && dataManager.SetCurrentFromPending(),
        "a second independent image should replace the complete current batch.",
        failureCount);
    const auto replacementOwner = dataManager.GetImageSnapshot();
    SetExpect(
        replacementOwner
            && replacementOwner != spacingOwner
            && replacementOwner->image != spacingOwner->image
            && replacementOwner->image->GetScalarPointer()
                != spacingOwner->image->GetScalarPointer()
            && replacementOwner->dims == std::array<int, 3>{ 2, 3, 1 }
            && replacementOwner->spacing == std::array<double, 3>{ 2.0, 3.0, 4.0 }
            && replacementOwner->origin == std::array<double, 3>{ 5.0, 6.0, 7.0 }
            && replacementOwner->version == spacingOwner->version + 1
            && static_cast<float*>(replacementOwner->image->GetScalarPointer())[0] == 77.0f
            && spacingOwner->dims == std::array<int, 3>{ 3, 4, 2 }
            && spacingOwner->spacing == updatedSpacing
            && spacingOwner->origin == std::array<double, 3>{ 0.0, 0.0, 0.0 }
            && static_cast<float*>(spacingOwner->image->GetScalarPointer())[0] == 1.0f,
        "replacing voxel storage must preserve the retired snapshot and publish one independent version.",
        failureCount);

    std::error_code removeError;
    std::filesystem::remove_all(outputRoot, removeError);
    SetExpect(
        !removeError,
        "test cleanup should remove temporary export files.",
        failureCount);
}

    int GetFailCount()
    {
        int failureCount = 0;
        StartCropFailure(failureCount);
        StartResultView(failureCount);
        StartOblique(failureCount);
        StartDeepCases(failureCount);
        StartBench(failureCount);
        StartSliceView(failureCount);
        StartDataExport(failureCount);
        return failureCount;
    }
};

int main()
{
    int failureCount = PlanarCropSuite().GetFailCount();
    failureCount += CropBridgeSuite().GetFailCount();
    failureCount += CropPreviewSuite().GetFailCount();
    failureCount += AppTaskSuite().GetFailCount();

    if (failureCount != 0) {
        std::cerr << "PlanarCropTests failed: " << failureCount << '\n';
        return 1;
    }

    std::cout << "PlanarCropTests passed.\n";
    return 0;
}
