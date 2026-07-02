// =====================================================================
// Path: MVVCVTK/features/GapAnalysis/tests/GapAnalysisAlgorithmTests.cpp
// 分类: Tests / GapAnalysis
// 说明: 合成封闭孔洞体数据的算法与服务快照回归测试。
// =====================================================================

#include "Algorithms/VoidDetector.h"
#include "Services/GapAnalysisService.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::array<int, 3> TestDims = { 7, 7, 7 };
constexpr double NumericTolerance = 1e-9;

std::size_t GetLinearIndex(int x, int y, int z, const std::array<int, 3>& dims)
{
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(y) * static_cast<std::size_t>(dims[0])
        + static_cast<std::size_t>(z) * static_cast<std::size_t>(dims[0]) * static_cast<std::size_t>(dims[1]);
}

bool GetVoxelIsInsideSyntheticVoid(int x, int y, int z)
{
    return x >= 2 && x <= 4
        && y >= 2 && y <= 4
        && z >= 2 && z <= 4;
}

void Expect(bool condition, const std::string& message, int& failureCount)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void ExpectNear(double actual, double expected, const std::string& message, int& failureCount)
{
    Expect(std::abs(actual - expected) <= NumericTolerance, message, failureCount);
}

VoidDetectionParams BuildVoidDetectionParams()
{
    VoidDetectionParams params;
    params.grayMin = -0.1f;
    params.grayMax = 0.1f;
    params.minVolumeMM3 = 0.0;
    params.angleThresholdDeg = 30.0f;
    params.tensorWindowSize = 1;
    params.erosionIterations = 0;
    return params;
}

std::vector<float> BuildSyntheticVoxelData()
{
    const auto total = static_cast<std::size_t>(TestDims[0])
        * static_cast<std::size_t>(TestDims[1])
        * static_cast<std::size_t>(TestDims[2]);
    std::vector<float> voxels(total, 1.0f);

    for (int z = 0; z < TestDims[2]; ++z) {
        for (int y = 0; y < TestDims[1]; ++y) {
            for (int x = 0; x < TestDims[0]; ++x) {
                if (GetVoxelIsInsideSyntheticVoid(x, y, z)) {
                    voxels[GetLinearIndex(x, y, z, TestDims)] = 0.0f;
                }
            }
        }
    }

    return voxels;
}

VolumeBuffer BuildSyntheticVolumeBuffer()
{
    VolumeBuffer volume;
    volume.dims = TestDims;
    volume.spacing = { 1.0, 1.0, 1.0 };
    volume.origin = { 0.0, 0.0, 0.0 };
    volume.minVal = 0.0f;
    volume.maxVal = 1.0f;
    volume.SetOwnedVoxels(BuildSyntheticVoxelData());
    return volume;
}

vtkSmartPointer<vtkImageData> BuildSyntheticImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(TestDims[0], TestDims[1], TestDims[2]);
    image->SetSpacing(1.0, 1.0, 1.0);
    image->SetOrigin(0.0, 0.0, 0.0);
    image->AllocateScalars(VTK_FLOAT, 1);

    auto* scalars = static_cast<float*>(image->GetScalarPointer());
    const auto voxels = BuildSyntheticVoxelData();
    std::copy(voxels.begin(), voxels.end(), scalars);
    image->Modified();
    return image;
}

void MutateImageToSolid(vtkImageData* image)
{
    auto* scalars = image ? static_cast<float*>(image->GetScalarPointer()) : nullptr;
    if (!scalars) {
        return;
    }

    const auto total = static_cast<std::size_t>(image->GetNumberOfPoints());
    std::fill(scalars, scalars + total, 1.0f);
    image->Modified();
}

std::size_t CountMaskValues(const std::vector<uint8_t>& mask)
{
    std::size_t count = 0;
    for (const auto value : mask) {
        if (value != 0) {
            ++count;
        }
    }
    return count;
}

void VerifySingleSyntheticVoidRegion(const VoidRegion& region, int& failureCount)
{
    Expect(region.id == 1, "synthetic void id should start from 1.", failureCount);
    Expect(region.voxelCount == 27, "synthetic void should contain 27 voxels.", failureCount);
    ExpectNear(region.volumeMM3, 27.0, "synthetic void volume should equal 27 mm3.", failureCount);
    ExpectNear(region.centroidMM[0], 3.0, "synthetic void centroid x should be 3 mm.", failureCount);
    ExpectNear(region.centroidMM[1], 3.0, "synthetic void centroid y should be 3 mm.", failureCount);
    ExpectNear(region.centroidMM[2], 3.0, "synthetic void centroid z should be 3 mm.", failureCount);

    const std::array<int, 6> expectedBbox = { 2, 4, 2, 4, 2, 4 };
    Expect(region.bbox == expectedBbox, "synthetic void bbox should match the carved 3x3x3 cube.", failureCount);

    ExpectNear(region.minGray, 0.0, "synthetic void min gray should be 0.", failureCount);
    ExpectNear(region.maxGray, 0.0, "synthetic void max gray should be 0.", failureCount);
    ExpectNear(region.meanGray, 0.0, "synthetic void mean gray should be 0.", failureCount);
}

void VerifyLabelImage(vtkImageData* labelImage, int& failureCount)
{
    Expect(labelImage != nullptr, "gap analysis label image should exist.", failureCount);
    if (!labelImage) {
        return;
    }

    int dims[3] = { 0, 0, 0 };
    labelImage->GetDimensions(dims);
    Expect(
        dims[0] == TestDims[0] && dims[1] == TestDims[1] && dims[2] == TestDims[2],
        "gap analysis label image dimensions should match input.",
        failureCount);

    auto* labels = static_cast<int*>(labelImage->GetScalarPointer());
    Expect(labels != nullptr, "gap analysis label image should have scalar data.", failureCount);
    if (!labels) {
        return;
    }

    std::size_t labeledVoxelCount = 0;
    for (int z = 0; z < TestDims[2]; ++z) {
        for (int y = 0; y < TestDims[1]; ++y) {
            for (int x = 0; x < TestDims[0]; ++x) {
                const auto label = labels[GetLinearIndex(x, y, z, TestDims)];
                const bool expectedLabeled = GetVoxelIsInsideSyntheticVoid(x, y, z);
                if (label != 0) {
                    ++labeledVoxelCount;
                }
                Expect(
                    expectedLabeled ? label == 1 : label == 0,
                    "gap analysis label image should mark exactly the synthetic void.",
                    failureCount);
            }
        }
    }

    Expect(labeledVoxelCount == 27, "gap analysis label image should contain 27 labeled voxels.", failureCount);
}

void RunPureAlgorithmCase(int& failureCount)
{
    const auto volume = BuildSyntheticVolumeBuffer();
    const auto interior = VoidDetector::CreateInteriorMask(volume, 0.5f);
    Expect(CountMaskValues(interior) == 27, "interior mask should contain only the enclosed void.", failureCount);

    auto candidates = VoidDetector::ExtractCandidates(volume, interior, BuildVoidDetectionParams());
    Expect(CountMaskValues(candidates) == 27, "candidate mask should preserve the enclosed void.", failureCount);

    std::vector<int> labels;
    const auto regions = VoidDetector::LabelAndAnalyze(
        volume,
        candidates,
        BuildVoidDetectionParams(),
        labels);

    Expect(regions.size() == 1, "pure gap analysis algorithm should detect one enclosed void.", failureCount);
    Expect(labels.size() == volume.voxels.size(), "label volume should align with input voxel count.", failureCount);
    if (regions.size() == 1) {
        VerifySingleSyntheticVoidRegion(regions.front(), failureCount);
    }
}

GapAnalysisState WaitForServiceCompletion(GapAnalysisService& service)
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

bool ConsumeCompletionCallback(GapAnalysisService& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (service.ConsumePendingCompletionCallback()) {
            service.ExecutePendingCompletionCallback();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void RunServiceSnapshotCase(int& failureCount)
{
    auto image = BuildSyntheticImage();
    GapAnalysisService service;

    Expect(service.SetInputImage(image), "gap analysis service should accept the synthetic image.", failureCount);

    // SetInputImage 必须立即复制输入。这里故意篡改原始 VTK 图像，
    // 如果后台任务仍读外部对象，就会把封闭孔洞误判为不存在。
    MutateImageToSolid(image);

    SurfaceParams surfaceParams;
    surfaceParams.isoValue = 0.5f;
    service.SetSurfaceParams(surfaceParams);
    service.SetVoidParams(BuildVoidDetectionParams());

    std::atomic<bool> callbackCalled{ false };
    std::atomic<bool> callbackSuccess{ false };
    service.RunAsync([&](bool success) {
        callbackCalled.store(true);
        callbackSuccess.store(success);
    });

    const auto finalState = WaitForServiceCompletion(service);
    Expect(finalState == GapAnalysisState::Succeeded, "gap analysis service should finish successfully.", failureCount);
    Expect(ConsumeCompletionCallback(service), "gap analysis service should expose one pending completion callback.", failureCount);
    Expect(callbackCalled.load(), "gap analysis completion callback should run on explicit consume.", failureCount);
    Expect(callbackSuccess.load(), "gap analysis completion callback should report success.", failureCount);

    const auto regions = service.GetVoidRegions();
    Expect(regions.size() == 1, "gap analysis service should detect one void from the copied snapshot.", failureCount);
    if (regions.size() == 1) {
        VerifySingleSyntheticVoidRegion(regions.front(), failureCount);
    }

    VerifyLabelImage(service.BuildLabelImage(), failureCount);

    auto voidMesh = service.BuildVoidMesh();
    Expect(voidMesh != nullptr, "gap analysis service should build a void mesh.", failureCount);
    if (voidMesh) {
        Expect(voidMesh->GetNumberOfPoints() > 0, "gap analysis void mesh should contain points.", failureCount);
        Expect(voidMesh->GetNumberOfCells() > 0, "gap analysis void mesh should contain cells.", failureCount);
    }
}

} // namespace

int main()
{
    int failureCount = 0;
    RunPureAlgorithmCase(failureCount);
    RunServiceSnapshotCase(failureCount);

    if (failureCount != 0) {
        std::cerr << "GapAnalysisAlgorithmTests failed: " << failureCount << '\n';
        return 1;
    }

    std::cout << "GapAnalysisAlgorithmTests passed.\n";
    return 0;
}
