// 这个测试放在仓库根 test/ 下，刻意不放回 feature 目录：
// 1. 主项目只负责编译应用链路，测试工程独立验证算法/服务契约。
// 2. 合成体数据让回归测试不依赖本地 RAW 文件或窗口初始化。
// 3. 同时测纯算法和 GapAnalysisService 快照，防止 UI/host 改动污染孔隙分析核心边界。

#include "Algorithms/VoidDetector.h"
#include "Algorithms/VolumeBuffer.h"
#include "Services/GapAnalysisService.h"
#include "GapDisplayTests.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class GapAlgorithmSuite final {
public:

inline static constexpr std::array<int, 3> TestDims = { 7, 7, 7 };
inline static constexpr double NumericTolerance = 1e-9;

std::size_t GetLinearIndex(int x, int y, int z, const std::array<int, 3>& dims)
{
    // VTK image 标量按 x 最快、y 次之、z 最慢展开；这里显式写公式，
    // 是为了让 label image 校验和合成 voxel buffer 使用同一线性布局。
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(y) * static_cast<std::size_t>(dims[0])
        + static_cast<std::size_t>(z) * static_cast<std::size_t>(dims[0]) * static_cast<std::size_t>(dims[1]);
}

bool GetVoxelInVoid(int x, int y, int z)
{
    // 合成 3x3x3 封闭低灰度块，外层一圈高灰度体素作为“封闭边界”，
    // 这样检测到的区域数量、体素数、bbox 和质心都有确定答案。
    return x >= 2 && x <= 4
        && y >= 2 && y <= 4
        && z >= 2 && z <= 4;
}

void SetExpect(bool isExpected, const std::string& message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void SetExpectNear(double actual, double expected, const std::string& message, int& failureCount)
{
    SetExpect(std::abs(actual - expected) <= NumericTolerance, message, failureCount);
}

GapVoidParams BuildVoidParams()
{
    // 参数刻意压低 minVolume 并关闭腐蚀，使测试只验证“封闭孔洞识别”本身，
    // 不把经验阈值和后处理策略混进这个单元回归。
    GapVoidParams params;
    params.grayMin = -0.1f;
    params.grayMax = 0.1f;
    params.minVolumeMM3 = 0.0;
    params.angleThresholdDeg = 30.0f;
    params.tensorWindowSize = 1;
    params.erosionIterations = 0;
    return params;
}

std::vector<float> BuildTestVoxels()
{
    const auto total = static_cast<std::size_t>(TestDims[0])
        * static_cast<std::size_t>(TestDims[1])
        * static_cast<std::size_t>(TestDims[2]);
    std::vector<float> voxels(total, 1.0f);

    for (int z = 0; z < TestDims[2]; ++z) {
        for (int y = 0; y < TestDims[1]; ++y) {
            for (int x = 0; x < TestDims[0]; ++x) {
                if (GetVoxelInVoid(x, y, z)) {
                    voxels[GetLinearIndex(x, y, z, TestDims)] = 0.0f;
                }
            }
        }
    }

    return voxels;
}

GapVolumeBuffer BuildTestVolume()
{
    GapVolumeBuffer volume;
    volume.dims = TestDims;
    volume.spacing = { 1.0, 1.0, 1.0 };
    volume.origin = { 0.0, 0.0, 0.0 };
    volume.minVal = 0.0f;
    volume.maxVal = 1.0f;
    volume.SetOwnedVoxels(BuildTestVoxels());
    return volume;
}

vtkSmartPointer<vtkImageData> BuildTestImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(TestDims[0], TestDims[1], TestDims[2]);
    image->SetSpacing(1.0, 1.0, 1.0);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->AllocateScalars(VTK_FLOAT, 1);

    auto* scalars = static_cast<float*>(image->GetScalarPointer());
    const auto voxels = BuildTestVoxels();
    std::copy(voxels.begin(), voxels.end(), scalars);
    image->Modified();
    return image;
}

vtkSmartPointer<vtkImageData> BuildShortImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(TestDims[0], TestDims[1], TestDims[2]);
    image->SetSpacing(1.0, 1.0, 1.0);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->AllocateScalars(VTK_SHORT, 1);

    auto* scalars = static_cast<short*>(image->GetScalarPointer());
    const auto voxels = BuildTestVoxels();
    std::transform(voxels.begin(), voxels.end(), scalars,
        [](float value) { return static_cast<short>(value); });
    image->Modified();
    return image;
}

void SetSolidImage(vtkImageData* image)
{
    auto* scalars = image ? static_cast<float*>(image->GetScalarPointer()) : nullptr;
    if (!scalars) {
        return;
    }

    std::fill_n(scalars, image->GetNumberOfPoints(), 1.0f);
    image->Modified();
}

std::size_t GetMaskCount(const std::vector<uint8_t>& mask)
{
    std::size_t count = 0;
    for (const auto value : mask) {
        if (value != 0) {
            ++count;
        }
    }
    return count;
}

void SetRegionExpect(const VoidRegion& region, int& failureCount)
{
    SetExpect(region.id == 1, "synthetic void id should start from 1.", failureCount);
    SetExpect(region.voxelCount == 27, "synthetic void should contain 27 voxels.", failureCount);
    SetExpectNear(region.volumeMM3, 27.0, "synthetic void volume should equal 27 mm3.", failureCount);
    SetExpectNear(region.centroidMM[0], 3.0, "synthetic void centroid x should be 3 mm.", failureCount);
    SetExpectNear(region.centroidMM[1], 3.0, "synthetic void centroid y should be 3 mm.", failureCount);
    SetExpectNear(region.centroidMM[2], 3.0, "synthetic void centroid z should be 3 mm.", failureCount);

    const std::array<int, 6> expectedBbox = { 2, 4, 2, 4, 2, 4 };
    SetExpect(region.bbox == expectedBbox, "synthetic void bbox should match the carved 3x3x3 cube.", failureCount);

    SetExpectNear(region.minGray, 0.0, "synthetic void min gray should be 0.", failureCount);
    SetExpectNear(region.maxGray, 0.0, "synthetic void max gray should be 0.", failureCount);
    SetExpectNear(region.meanGray, 0.0, "synthetic void mean gray should be 0.", failureCount);
}

void SetLabelExpect(vtkImageData* labelImage, int& failureCount)
{
    // label image 是后续 overlay 的输入真源；这里逐 voxel 校验，确保 mesh 成功不掩盖 label 错位。
    SetExpect(labelImage != nullptr, "gap analysis label image should exist.", failureCount);
    if (!labelImage) {
        return;
    }

    int dims[3] = { 0, 0, 0 };
    labelImage->GetDimensions(dims);
    SetExpect(
        dims[0] == TestDims[0] && dims[1] == TestDims[1] && dims[2] == TestDims[2],
        "gap analysis label image dimensions should match input.",
        failureCount);

    auto* labels = static_cast<int*>(labelImage->GetScalarPointer());
    SetExpect(labels != nullptr, "gap analysis label image should have scalar data.", failureCount);
    if (!labels) {
        return;
    }

    std::size_t labeledVoxelCount = 0;
    for (int z = 0; z < TestDims[2]; ++z) {
        for (int y = 0; y < TestDims[1]; ++y) {
            for (int x = 0; x < TestDims[0]; ++x) {
                const auto label = labels[GetLinearIndex(x, y, z, TestDims)];
                const bool isLabelExpected = GetVoxelInVoid(x, y, z);
                if (label != 0) {
                    ++labeledVoxelCount;
                }
                SetExpect(
                    isLabelExpected ? label == 1 : label == 0,
                    "gap analysis label image should mark exactly the synthetic void.",
                    failureCount);
            }
        }
    }

    SetExpect(labeledVoxelCount == 27, "gap analysis label image should contain 27 labeled voxels.", failureCount);
}

void StartAlgoCase(int& failureCount)
{
    // 纯算法路径不经过 service 或线程，先确认 VoidDetector 的数学结果稳定。
    const auto volume = BuildTestVolume();
    const auto interior = VoidDetector::CreateInteriorMask(volume, 0.5f);
    SetExpect(GetMaskCount(interior) == 27, "interior mask should contain only the enclosed void.", failureCount);

    auto candidates = VoidDetector::BuildCandidates(volume, interior, BuildVoidParams());
    SetExpect(GetMaskCount(candidates) == 27, "candidate mask should preserve the enclosed void.", failureCount);

    std::vector<int> labels;
    const auto regions = VoidDetector::BuildRegions(
        volume,
        candidates,
        BuildVoidParams(),
        labels);

    SetExpect(regions.size() == 1, "pure gap analysis algorithm should detect one enclosed void.", failureCount);
    SetExpect(labels.size() == volume.voxels.size(), "label volume should align with input voxel count.", failureCount);
    if (regions.size() == 1) {
        SetRegionExpect(regions.front(), failureCount);
    }

    auto maskedVolume = BuildTestVolume();
    std::vector<std::uint8_t> validityMask(
        maskedVolume.voxels.size(), 255);
    validityMask[GetLinearIndex(3, 3, 3, TestDims)] = 0;
    maskedVolume.SetOwnedMask(std::move(validityMask));
    const auto maskedInterior =
        VoidDetector::CreateInteriorMask(maskedVolume, 0.5f);
    SetExpect(GetMaskCount(maskedInterior) == 0,
        "mask=0 should open the carved void to the analysis exterior.",
        failureCount);
    auto maskedCandidates = VoidDetector::BuildCandidates(
        maskedVolume, maskedInterior, BuildVoidParams());
    std::vector<int> maskedLabels;
    const auto maskedRegions = VoidDetector::BuildRegions(
        maskedVolume,
        maskedCandidates,
        BuildVoidParams(),
        maskedLabels);
    SetExpect(maskedRegions.empty(),
        "invalid-domain voxels should not enter gap statistics.",
        failureCount);
    SetExpect(std::none_of(
            maskedLabels.begin(),
            maskedLabels.end(),
            [](int label) { return label != 0; }),
        "invalid-domain voxels should remain zero in the label result.",
        failureCount);
}

void StartBufferCase(int& failureCount)
{
    // owned 路径复制后必须各自拥有 vector；移动后别名必须重绑，不能保留源对象地址。
    GapVolumeBuffer owned;
    owned.dims = { 2, 1, 1 };
    owned.SetOwnedVoxels({ 3.0f, 5.0f });
    owned.SetOwnedMask({ 255, 0 });
    GapVolumeBuffer ownedCopy(owned);
    SetExpect(ownedCopy.voxelsPtr != owned.voxelsPtr
        && ownedCopy.validMaskPtr != owned.validMaskPtr
        && ownedCopy.GetVoxelValue(1, 0, 0) == 5.0f
        && !ownedCopy.GetVoxelValid(1),
        "owned VolumeBuffer copy should bind independent voxel and mask vectors.",
        failureCount);

    GapVolumeBuffer ownedAssigned;
    ownedAssigned = owned;
    SetExpect(ownedAssigned.voxelsPtr != owned.voxelsPtr
        && ownedAssigned.GetVoxelValue(0, 0, 0) == 3.0f,
        "owned VolumeBuffer copy assignment should bind its independent vector.", failureCount);

    GapVolumeBuffer ownedMoved(std::move(ownedCopy));
    SetExpect(!ownedCopy.GetVoxelReady()
        && ownedMoved.voxelsPtr == ownedMoved.voxels.data(),
        "owned VolumeBuffer move should clear the source and rebind moved storage.", failureCount);
    SetExpect(ownedCopy.validMaskPtr == nullptr
        && ownedMoved.validMaskPtr == ownedMoved.validMask.data()
        && !ownedMoved.GetVoxelValid(1),
        "owned VolumeBuffer move should clear and rebind the validity mask.",
        failureCount);

    GapVolumeBuffer ownedMoveAssigned;
    ownedMoveAssigned = std::move(ownedAssigned);
    SetExpect(!ownedAssigned.GetVoxelReady()
        && ownedMoveAssigned.voxelsPtr == ownedMoveAssigned.voxels.data(),
        "owned VolumeBuffer move assignment should clear the source and rebind moved storage.", failureCount);

    std::weak_ptr<std::vector<float>> weakOwner;
    {
        auto sharedVoxels = std::make_shared<std::vector<float>>(
            std::initializer_list<float>{ 7.0f, 11.0f });
        auto sharedMask = std::make_shared<std::vector<std::uint8_t>>(
            std::initializer_list<std::uint8_t>{ 255, 0 });
        weakOwner = sharedVoxels;

        GapVolumeBuffer shared;
        shared.dims = { 2, 1, 1 };
        SetExpect(shared.SetSharedVoxels(sharedVoxels, sharedVoxels->data()),
            "shared VolumeBuffer should accept an owner and read-only alias.", failureCount);
        SetExpect(shared.SetSharedMask(sharedMask, sharedMask->data()),
            "shared VolumeBuffer should accept a mask owner and read-only alias.",
            failureCount);
        const auto* sharedAddress = sharedVoxels->data();
        sharedVoxels.reset();

        GapVolumeBuffer sharedCopy(shared);
        GapVolumeBuffer sharedAssigned;
        sharedAssigned = shared;
        GapVolumeBuffer sharedMoved(std::move(shared));
        GapVolumeBuffer sharedMoveAssigned;
        sharedMoveAssigned = std::move(sharedCopy);
        SetExpect(!weakOwner.expired()
            && !shared.GetVoxelReady()
            && !sharedCopy.GetVoxelReady()
            && sharedAssigned.voxelsPtr == sharedAddress
            && sharedMoved.voxelsPtr == sharedAddress
            && sharedMoveAssigned.voxelsPtr == sharedAddress
            && sharedMoved.GetVoxelValue(1, 0, 0) == 11.0f
            && !sharedMoved.GetVoxelValid(1)
            && !sharedMoveAssigned.GetVoxelValid(1),
            "shared VolumeBuffer copies and moves should retain aliased voxel and mask owners.",
            failureCount);

        sharedAssigned.SetOwnedVoxels({ 13.0f, 17.0f });
        SetExpect(sharedAssigned.voxelsPtr == sharedAssigned.voxels.data()
            && sharedAssigned.voxelsPtr != sharedAddress,
            "owned voxel replacement should detach the prior shared owner.", failureCount);
    }
    SetExpect(weakOwner.expired(),
        "shared voxel owner should release after the final VolumeBuffer owner exits.", failureCount);
}

GapAnalysisState GetServiceState(GapAnalysisService& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = service.GetAnalysisState();
        if (state != GapAnalysisState::Running) {
            return state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return service.GetAnalysisState();
}

bool SendDoneEvent(GapAnalysisService& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (service.GetDoneEvent()) {
            service.SendCallback();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void StartSnapCase(int& failureCount)
{
    // 公共入口必须保持隔离语义：同步 DeepCopy 后，源图像污染与释放都不影响 worker。
    auto image = BuildTestImage();
    vtkWeakPointer<vtkImageData> weakImage;
    weakImage = image.GetPointer();
    GapAnalysisService service;

    SetExpect(service.SetGapInput(image), "gap analysis service should accept the synthetic image.", failureCount);
    SetSolidImage(image);
    image = nullptr;
    SetExpect(weakImage == nullptr,
        "public gap input should not retain the caller's mutable VTK image.", failureCount);

    GapSurfaceParams surfaceParams;
    surfaceParams.isoValue = 0.5f;
    service.SetSurface(surfaceParams);
    service.SetVoid(BuildVoidParams());

    std::atomic<bool> hasCallback{ false };
    std::atomic<bool> isCallbackOk{ false };
    SetExpect(
        service.StartAsync([&](bool isSuccess) {
            hasCallback.store(true);
            isCallbackOk.store(isSuccess);
        }),
        "gap analysis should report that the first worker request was accepted.",
        failureCount);
    SetExpect(
        !service.StartAsync(nullptr),
        "gap analysis should reject a second request while the worker is running.",
        failureCount);

    const auto finalState = GetServiceState(service);
    SetExpect(finalState == GapAnalysisState::Succeeded, "gap analysis service should finish successfully.", failureCount);
    SetExpect(
        !service.StartAsync(nullptr),
        "gap analysis should reject a new task until the pending callback is consumed.",
        failureCount);
    SetExpect(SendDoneEvent(service), "gap analysis service should expose one pending completion callback.", failureCount);
    SetExpect(hasCallback.load(), "gap analysis completion callback should run on explicit consume.", failureCount);
    SetExpect(isCallbackOk.load(), "gap analysis completion callback should report success.", failureCount);

    const auto regions = service.GetVoidRegions();
    SetExpect(regions.size() == 1, "gap analysis service should detect one void from the isolated snapshot.", failureCount);
    if (regions.size() == 1) {
        SetRegionExpect(regions.front(), failureCount);
    }

    SetLabelExpect(service.BuildLabelImage(), failureCount);

    auto voidMesh = service.BuildVoidMesh();
    SetExpect(voidMesh != nullptr, "gap analysis service should build a void mesh.", failureCount);
    if (voidMesh) {
        SetExpect(voidMesh->GetNumberOfPoints() > 0, "gap analysis void mesh should contain points.", failureCount);
        SetExpect(voidMesh->GetNumberOfCells() > 0, "gap analysis void mesh should contain cells.", failureCount);
    }
}

void StartSharedCase(int& failureCount)
{
    // 受控宿主入口共享 VTK_FLOAT scalars；调用方释放 smart pointer 后快照 owner 仍覆盖 worker 生命周期。
    auto image = BuildTestImage();
    vtkWeakPointer<vtkImageData> weakImage;
    weakImage = image.GetPointer();
    GapAnalysisService service;
    SetExpect(service.SetGapInput(image),
        "gap analysis should accept one isolated image snapshot.", failureCount);
    image = nullptr;
    SetExpect(weakImage == nullptr,
        "isolated gap snapshot should not retain the caller image owner.", failureCount);

    GapSurfaceParams surfaceParams;
    surfaceParams.isoValue = 0.5f;
    service.SetSurface(surfaceParams);
    service.SetVoid(BuildVoidParams());
    SetExpect(service.StartAsync(nullptr),
        "gap analysis should start from the controlled shared snapshot.", failureCount);
    SetExpect(GetServiceState(service) == GapAnalysisState::Succeeded,
        "gap analysis should finish the controlled shared snapshot.", failureCount);
    SetExpect(service.GetVoidRegions().size() == 1,
        "controlled shared snapshot should preserve the synthetic void.", failureCount);
}

void StartConvertCase(int& failureCount)
{
    // 非 float 输入必须在同步入口完成 float 转换；之后调用方改写 VTK scalars 不能污染算法输入。
    auto image = BuildShortImage();
    GapAnalysisService service;
    SetExpect(service.SetGapInput(image),
        "gap analysis service should accept a short scalar image.", failureCount);

    auto* source = static_cast<short*>(image->GetScalarPointer());
    std::fill_n(source, image->GetNumberOfPoints(), static_cast<short>(1));
    image->Modified();

    GapSurfaceParams surfaceParams;
    surfaceParams.isoValue = 0.5f;
    service.SetSurface(surfaceParams);
    service.SetVoid(BuildVoidParams());
    SetExpect(service.StartAsync(nullptr),
        "gap analysis should accept the converted short input.", failureCount);
    SetExpect(GetServiceState(service) == GapAnalysisState::Succeeded,
        "gap analysis should finish the converted short input.", failureCount);

    const auto regions = service.GetVoidRegions();
    SetExpect(regions.size() == 1,
        "non-float conversion should isolate the worker from later source mutations.", failureCount);
    if (regions.size() == 1) {
        SetRegionExpect(regions.front(), failureCount);
    }
}

    int GetFailCount()
    {
        int failureCount = 0;
        StartAlgoCase(failureCount);
        StartBufferCase(failureCount);
        StartSnapCase(failureCount);
        StartSharedCase(failureCount);
        StartConvertCase(failureCount);
        return failureCount;
    }
};

int main()
{
    int failureCount = GapAlgorithmSuite().GetFailCount();
    failureCount += GapDisplaySuite().GetFailCount();

    if (failureCount != 0) {
        std::cerr << "GapAnalysisAlgorithmTests failed: " << failureCount << '\n';
        return 1;
    }

    std::cout << "GapAnalysisAlgorithmTests passed.\n";
    return 0;
}
