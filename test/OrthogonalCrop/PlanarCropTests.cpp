// 这个测试独立在根 test/ 下验证裁切算法 submit 契约，不参与主应用链路：
// 1. 用合成 vtkImageData 覆盖非零 extent、方向矩阵和多 component 标量。
// 2. 直接调用 backend router，避免窗口/交互器影响算法结果判断。
// 3. 明确校验 mask 和 submit image 的一致性，防止后续优化只改一条输出链。

#include "Algorithms/OrthogonalCropAlgorithm.h"
#include "Algorithms/PlanarCropAlgorithm.h"
#include "Routing/CropRouter.h"
#include "DataManager.h"
#include "Interaction/CropCameraState.h"
#include "InteractionComputeService.h"

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
#include <vector>

namespace {

constexpr double TupleTolerance = 1e-9;
constexpr double MatrixTolerance = 1e-9;

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
    request.SetGeometryType(CropShape::Plane);
    request.SetOperation(operation);
    request.SetDataSource(dataSource);
    request.SetRemovalMode(removalMode);
    request.SetPlaneNormal(plane.normal);
    request.SetPlaneCenter(plane.center);
    request.SetPlaneHalf({ 2.0, 2.0 });
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

void SetExpect(bool isExpected, const std::string& message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void StartCropFailure(int& failureCount)
{
    OrthogonalCropRequest request;
    request.SetDataSource(OrthogonalCropDataSource::PolyData);
    request.SetOperation(OrthogonalCropOperation::Preview);
    request.SetGeometryType(CropShape::Plane);
    request.SetRemovalMode(CropRemovalMode::RemoveInside);

    CropRouter router;
    const auto result = router.GetResult(request);
    const auto& statistics = result.GetStatistics();

    SetExpect(!result.GetSucceeded(), "missing polydata route should fail.", failureCount);
    SetExpect(
        result.GetResolvedDataSource() == OrthogonalCropDataSource::PolyData,
        "failure result must preserve the PolyData source.",
        failureCount);
    SetExpect(
        result.GetResolvedOperation() == OrthogonalCropOperation::Preview,
        "failure result must preserve the Preview operation.",
        failureCount);
    SetExpect(
        result.GetResolvedGeometryType() == CropShape::Plane,
        "failure result must preserve the Plane geometry.",
        failureCount);
    SetExpect(
        result.GetResolvedRemovalMode() == CropRemovalMode::RemoveInside,
        "failure result must preserve the RemoveInside mode.",
        failureCount);
    SetExpect(
        statistics.GetResolvedDataSource() == OrthogonalCropDataSource::PolyData,
        "failure statistics must preserve the PolyData source.",
        failureCount);
    SetExpect(
        statistics.GetResolvedOperation() == OrthogonalCropOperation::Preview,
        "failure statistics must preserve the Preview operation.",
        failureCount);
    SetExpect(
        statistics.GetResolvedGeometryType() == CropShape::Plane,
        "failure statistics must preserve the Plane geometry.",
        failureCount);
    SetExpect(
        statistics.GetResolvedRemovalMode() == CropRemovalMode::RemoveInside,
        "failure statistics must preserve the RemoveInside mode.",
        failureCount);
    SetExpect(
        result.GetFailureReason() == CropFailure::NoPolyData,
        "failure result must report NoPolyData.",
        failureCount);
    SetExpect(
        statistics.GetFailureReason() == CropFailure::NoPolyData,
        "failure statistics must report NoPolyData.",
        failureCount);
    SetExpect(
        result.GetFailureReason() == statistics.GetFailureReason(),
        "failure result and statistics reasons must stay synchronized.",
        failureCount);
    SetExpect(!result.GetMessage().empty(), "failure result message must not be empty.", failureCount);
    SetExpect(
        result.GetMessage() == statistics.GetValidationMessage(),
        "failure result and statistics messages must stay synchronized.",
        failureCount);
}

void StartResultSync(int& failureCount)
{
    const auto resultExpect = [&failureCount](
        const OrthogonalCropResult& result,
        const OrthogonalCropRequest& request,
        CropFailure failureReason,
        const std::string& label) {
        const auto& resultStatistics = result.GetStatistics();
        SetExpect(
            result.GetResolvedDataSource() == request.GetDataSource(),
            label + " result data source must match the request.",
            failureCount);
        SetExpect(
            result.GetResolvedOperation() == request.GetOperation(),
            label + " result operation must match the request.",
            failureCount);
        SetExpect(
            result.GetResolvedGeometryType() == request.GetGeometryType(),
            label + " result geometry must match the request.",
            failureCount);
        SetExpect(
            result.GetResolvedRemovalMode() == request.GetRemovalMode(),
            label + " result removal mode must match the request.",
            failureCount);
        SetExpect(
            resultStatistics.GetResolvedDataSource() == request.GetDataSource(),
            label + " statistics data source must match the request.",
            failureCount);
        SetExpect(
            resultStatistics.GetResolvedOperation() == request.GetOperation(),
            label + " statistics operation must match the request.",
            failureCount);
        SetExpect(
            resultStatistics.GetResolvedGeometryType() == request.GetGeometryType(),
            label + " statistics geometry must match the request.",
            failureCount);
        SetExpect(
            resultStatistics.GetResolvedRemovalMode() == request.GetRemovalMode(),
            label + " statistics removal mode must match the request.",
            failureCount);
        SetExpect(
            result.GetFailureReason() == failureReason,
            label + " result failure reason must match the expected value.",
            failureCount);
        SetExpect(
            resultStatistics.GetFailureReason() == failureReason,
            label + " statistics failure reason must match the expected value.",
            failureCount);
        SetExpect(
            result.GetMessage() == resultStatistics.GetValidationMessage(),
            label + " result and statistics messages must stay synchronized.",
            failureCount);
        if (failureReason != CropFailure::None) {
            SetExpect(
                !result.GetMessage().empty(),
                label + " failure message must not be empty.",
                failureCount);
        }
    };
    const auto requestFactory = [](
        CropShape geometryType,
        OrthogonalCropOperation operation,
        OrthogonalCropDataSource dataSource) {
        OrthogonalCropRequest request;
        request.SetGeometryType(geometryType);
        request.SetOperation(operation);
        request.SetDataSource(dataSource);
        request.SetRemovalMode(CropRemovalMode::KeepInside);
        return request;
    };

    auto statistics = OrthogonalCropStatistics::GetResolved(
        OrthogonalCropDataSource::PolyData,
        OrthogonalCropOperation::Preview,
        CropShape::Plane,
        CropRemovalMode::RemoveInside);
    statistics.SetFailureReason(CropFailure::NoPolyData);
    statistics.SetValidationMessage("Synthetic missing polydata.");

    const auto factoryResult = OrthogonalCropResult::GetResolved(statistics);
    auto factoryRequest = requestFactory(
        CropShape::Plane,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::PolyData);
    factoryRequest.SetRemovalMode(CropRemovalMode::RemoveInside);
    resultExpect(factoryResult, factoryRequest, CropFailure::NoPolyData, "resolved factory");

    OrthogonalCropResult setterResult;
    setterResult.SetResolvedDataSource(factoryRequest.GetDataSource());
    setterResult.SetResolvedOperation(factoryRequest.GetOperation());
    setterResult.SetResolvedGeometryType(factoryRequest.GetGeometryType());
    setterResult.SetResolvedRemovalMode(factoryRequest.GetRemovalMode());
    setterResult.SetFailureReason(CropFailure::NoPolyData);
    setterResult.SetMessage("Synthetic setter failure.");
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
    boxPreviewRequest.SetBoxBounds(boxInputBounds);
    const auto boxPreviewResult = OrthogonalCropAlgorithm::GetResult(image, boxPreviewRequest);
    SetExpect(boxPreviewResult.GetSucceeded(), "valid box preview route should succeed.", failureCount);
    resultExpect(boxPreviewResult, boxPreviewRequest, CropFailure::None, "valid box preview");

    auto boxSubmitRequest = requestFactory(
        CropShape::Box,
        OrthogonalCropOperation::Submit,
        OrthogonalCropDataSource::ImageData);
    boxSubmitRequest.SetBoxBounds(boxInputBounds);
    boxSubmitRequest.SetRemovalMode(CropRemovalMode::RemoveInside);
    const auto boxSubmitResult = OrthogonalCropAlgorithm::GetResult(image, boxSubmitRequest);
    SetExpect(boxSubmitResult.GetSucceeded(), "valid box submit route should succeed.", failureCount);
    SetExpect(boxSubmitResult.GetSubmitImage() != nullptr, "valid box submit must return an image.", failureCount);
    SetExpect(boxSubmitResult.GetMaskImage() != nullptr, "valid box submit must return a mask.", failureCount);
    resultExpect(boxSubmitResult, boxSubmitRequest, CropFailure::None, "valid box submit");

    auto boxPolyRequest = requestFactory(
        CropShape::Box,
        OrthogonalCropOperation::Preview,
        OrthogonalCropDataSource::PolyData);
    boxPolyRequest.SetBoxBounds(boxInputBounds);
    const auto boxPolyResult = OrthogonalCropAlgorithm::GetResult(
        boxPreviewResult.GetOutlinePolyData(),
        boxPolyRequest);
    SetExpect(boxPolyResult.GetSucceeded(), "valid box polydata route should succeed.", failureCount);
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
        SetExpect(!routeResult.GetSucceeded(), label + " should fail.", failureCount);
        resultExpect(routeResult, routeCase.request, CropFailure::NoBackend, label);
    }
}

void StartCameraState(int& failureCount)
{
    constexpr double savedNear = 0.125;
    constexpr double savedFar = 987.5;
    constexpr double changedNear = 12.0;
    constexpr double changedFar = 24.0;

    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    auto camera = vtkSmartPointer<vtkCamera>::New();
    renderer->SetActiveCamera(camera);
    camera->SetClippingRange(savedNear, savedFar);

    CropCameraState cameraState;
    cameraState.SetCameraState(renderer);
    SetExpect(cameraState.GetSaved(), "camera state should report a saved snapshot.", failureCount);

    camera->SetClippingRange(4.0, 8.0);
    cameraState.ResetCamera(renderer);

    double restoredRange[2] = { 0.0, 0.0 };
    camera->GetClippingRange(restoredRange);
    SetExpect(
        restoredRange[0] == savedNear && restoredRange[1] == savedFar,
        "camera reset must restore the exact clipping range.",
        failureCount);
    SetExpect(!cameraState.GetSaved(), "camera reset must consume the saved snapshot.", failureCount);

    camera->SetClippingRange(changedNear, changedFar);
    cameraState.ResetCamera(renderer);
    camera->GetClippingRange(restoredRange);
    SetExpect(
        restoredRange[0] == changedNear && restoredRange[1] == changedFar,
        "camera reset without a saved snapshot must leave the clipping range unchanged.",
        failureCount);
}

void SetResultExpect(
    const OrthogonalCropResult& result,
    CropRemovalMode removalMode,
    int& failureCount)
{
    const auto& statistics = result.GetStatistics();

    SetExpect(result.GetSucceeded(), "planar submit should succeed.", failureCount);
    SetExpect(
        result.GetResolvedOperation() == OrthogonalCropOperation::Submit,
        "planar submit result must resolve to Submit operation.",
        failureCount);
    SetExpect(
        result.GetResolvedDataSource() == OrthogonalCropDataSource::ImageData,
        "planar submit result must resolve to ImageData source.",
        failureCount);
    SetExpect(
        result.GetResolvedGeometryType() == CropShape::Plane,
        "planar submit result must resolve to Plane geometry.",
        failureCount);
    SetExpect(
        result.GetResolvedRemovalMode() == removalMode,
        "planar submit result must preserve requested removal mode.",
        failureCount);
    SetExpect(
        result.GetFailureReason() == CropFailure::None,
        "planar submit result must not report failure.",
        failureCount);
    SetExpect(
        statistics.GetResolvedOperation() == OrthogonalCropOperation::Submit,
        "planar submit statistics must resolve to Submit operation.",
        failureCount);
    SetExpect(
        statistics.GetResolvedDataSource() == OrthogonalCropDataSource::ImageData,
        "planar submit statistics must resolve to ImageData source.",
        failureCount);
    SetExpect(
        statistics.GetResolvedGeometryType() == CropShape::Plane,
        "planar submit statistics must resolve to Plane geometry.",
        failureCount);
    SetExpect(
        statistics.GetResolvedRemovalMode() == removalMode,
        "planar submit statistics must preserve requested removal mode.",
        failureCount);
    SetExpect(
        statistics.GetFailureReason() == CropFailure::None,
        "planar submit statistics must not report failure.",
        failureCount);
    SetExpect(result.GetSubmitImage() != nullptr, "planar submit image must exist.", failureCount);
    SetExpect(result.GetMaskImage() != nullptr, "planar submit mask must exist.", failureCount);
    SetExpect(result.GetOutlinePolyData() == nullptr, "planar submit must not return preview plane outline.", failureCount);
    SetExpect(result.GetClipPolyData() == nullptr, "planar submit must not return preview clip polydata.", failureCount);
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

    SetExpect(result.GetSucceeded(), "planar preview contrast case should succeed.", failureCount);
    SetExpect(
        result.GetResolvedOperation() == OrthogonalCropOperation::Preview,
        "planar preview contrast case must resolve to Preview operation.",
        failureCount);
    SetExpect(result.GetSubmitImage() == nullptr, "planar preview must not return submit image.", failureCount);
    SetExpect(result.GetMaskImage() == nullptr, "planar preview must not return submit mask.", failureCount);
    SetExpect(result.GetOutlinePolyData() == nullptr, "planar preview must not return a misleading finite plane outline.", failureCount);
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
    if (!result.GetSucceeded() || !result.GetSubmitImage() || !result.GetMaskImage()) {
        return;
    }

    SetSubmitExpect(
        image,
        result.GetSubmitImage(),
        result.GetMaskImage(),
        plane,
        removalMode,
        failureCount);
    SetBoundExpect(result.GetMaskImage(), boundaryIndex, removalMode, failureCount);
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
    SetExpect(warmup.GetSucceeded(), "planar submit warmup should succeed.", failureCount);

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
        SetExpect(result.GetSucceeded(), "planar submit benchmark should succeed.", failureCount);
        SetExpect(
            result.GetResolvedOperation() == OrthogonalCropOperation::Submit,
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
    RawVolumeDataManager dataManager;
    auto image = BuildExport();
    const auto identity = GetIdentityData();
    const auto outputRoot = BuildOutputRoot();
    std::error_code createError;
    std::filesystem::create_directories(outputRoot, createError);
    SetExpect(!createError, "data export test should create its temporary output directory.", failureCount);

    const auto initialVersion = dataManager.GetDataVersion();
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

    std::error_code removeError;
    std::filesystem::remove_all(outputRoot, removeError);
    SetExpect(
        !removeError,
        "test cleanup should remove temporary export files.",
        failureCount);
}

} // namespace

int main()
{
    int failureCount = 0;
    StartCropFailure(failureCount);
    StartResultSync(failureCount);
    StartCameraState(failureCount);
    StartOblique(failureCount);
    StartDeepCases(failureCount);
    StartBench(failureCount);
    StartSliceView(failureCount);
    StartDataExport(failureCount);

    if (failureCount != 0) {
        std::cerr << "PlanarCropTests failed: " << failureCount << '\n';
        return 1;
    }

    std::cout << "PlanarCropTests passed.\n";
    return 0;
}
