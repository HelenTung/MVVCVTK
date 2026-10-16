// 测试用途：通过合成体数据验证孔隙分析的快照、转换、过滤和算法结果，不代表真实 CT 验收。
// 合成体数据只通过 GapAnalysisService 进入私有 DefX bridge；
// 测试不得直接包含或调用已退役的本地孔隙算法。
#include "Services/GapAnalysisService.h"
#include "GapDisplayTests.h"
#include "GapKernelLabelViewTests.h"

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
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GapAlgorithmSuite final {
public:
    inline static constexpr std::array<int, 3> TestDims = { 7, 7, 7 };
    inline static constexpr double NumericTolerance = 1e-6;

    std::size_t GetLinearIndex(
        int x,
        int y,
        int z,
        const std::array<int, 3>& dims) const
    {
        return static_cast<std::size_t>(x)
            + static_cast<std::size_t>(y)
                * static_cast<std::size_t>(dims[0])
            + static_cast<std::size_t>(z)
                * static_cast<std::size_t>(dims[0])
                * static_cast<std::size_t>(dims[1]);
    }

    bool GetVoxelInVoid(int x, int y, int z) const
    {
        return x >= 2 && x <= 4
            && y >= 2 && y <= 4
            && z >= 2 && z <= 4;
    }

    void SetExpect(
        bool isExpected,
        const std::string& message,
        int& failureCount) const
    {
        if (!isExpected) {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void SetExpectNear(
        double actual,
        double expected,
        const std::string& message,
        int& failureCount) const
    {
        SetExpect(
            std::abs(actual - expected) <= NumericTolerance,
            message,
            failureCount);
    }

    GapVoidParams BuildVoidParams() const
    {
        GapVoidParams params;
        params.isFilterEnabled = false;
        params.minVolumeMM3 = 0.0;
        return params;
    }

    std::vector<float> BuildTestVoxels() const
    {
        const auto total = static_cast<std::size_t>(TestDims[0])
            * static_cast<std::size_t>(TestDims[1])
            * static_cast<std::size_t>(TestDims[2]);
        std::vector<float> voxels(total, 1.0f);
        for (int z = 0; z < TestDims[2]; ++z) {
            for (int y = 0; y < TestDims[1]; ++y) {
                for (int x = 0; x < TestDims[0]; ++x) {
                    if (GetVoxelInVoid(x, y, z)) {
                        voxels[GetLinearIndex(
                            x, y, z, TestDims)] = 0.0f;
                    }
                }
            }
        }
        return voxels;
    }

    vtkSmartPointer<vtkImageData> BuildTestImage() const
    {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(
            TestDims[0], TestDims[1], TestDims[2]);
        image->SetSpacing(1.0, 1.0, 1.0);
        image->SetOrigin(0.0, 0.0, 0.0);
        image->AllocateScalars(VTK_FLOAT, 1);
        auto* scalars = static_cast<float*>(
            image->GetScalarPointer());
        const auto voxels = BuildTestVoxels();
        std::copy(voxels.begin(), voxels.end(), scalars);
        image->Modified();
        return image;
    }

    vtkSmartPointer<vtkImageData> BuildShortImage() const
    {
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(
            TestDims[0], TestDims[1], TestDims[2]);
        image->SetSpacing(1.0, 1.0, 1.0);
        image->SetOrigin(0.0, 0.0, 0.0);
        image->AllocateScalars(VTK_SHORT, 1);
        auto* scalars = static_cast<short*>(
            image->GetScalarPointer());
        const auto voxels = BuildTestVoxels();
        std::transform(
            voxels.begin(),
            voxels.end(),
            scalars,
            [](float value) {
                return static_cast<short>(value);
            });
        image->Modified();
        return image;
    }

    void SetSolidImage(vtkImageData* image) const
    {
        auto* scalars = image
            ? static_cast<float*>(image->GetScalarPointer())
            : nullptr;
        if (!scalars) {
            return;
        }
        std::fill_n(
            scalars,
            image->GetNumberOfPoints(),
            1.0f);
        image->Modified();
    }

    GapAnalysisState GetServiceState(
        GapAnalysisService& service) const
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto state = service.GetAnalysisState();
            if (state != GapAnalysisState::Running) {
                return state;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
        return service.GetAnalysisState();
    }

    bool SendDoneEvent(GapAnalysisService& service) const
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (service.GetDoneEvent()) {
                service.SendCallback();
                return true;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
        }
        return false;
    }

    std::vector<std::int32_t> GetLabels(
        vtkImageData* labelImage,
        int& failureCount) const
    {
        SetExpect(
            labelImage != nullptr,
            "DefX label image should exist.",
            failureCount);
        if (!labelImage) {
            return {};
        }

        int dims[3] = {};
        labelImage->GetDimensions(dims);
        SetExpect(
            dims[0] == TestDims[0]
                && dims[1] == TestDims[1]
                && dims[2] == TestDims[2],
            "DefX label dimensions should match the frozen input.",
            failureCount);
        SetExpect(
            labelImage->GetScalarType() == VTK_INT
                && labelImage->GetNumberOfScalarComponents() == 1,
            "Feature label DTO should preserve DefX IDs as one int32 component.",
            failureCount);

        const auto pointCount = labelImage->GetNumberOfPoints();
        const auto* labels = static_cast<const int*>(
            labelImage->GetScalarPointer());
        SetExpect(
            pointCount > 0 && labels,
            "DefX label image should contain scalar data.",
            failureCount);
        if (pointCount <= 0 || !labels) {
            return {};
        }
        return {
            labels,
            labels + static_cast<std::size_t>(pointCount)
        };
    }

    void SetResultExpect(
        GapAnalysisService& service,
        bool hasDefectExpected,
        int& failureCount) const
    {
        const auto regions = service.GetVoidRegions();
        const auto labelImage = service.BuildLabelImage();
        const auto labels = GetLabels(labelImage, failureCount);
        std::unordered_set<std::int32_t> regionIds;
        std::unordered_map<std::int32_t, std::uint64_t> labelCounts;

        for (const auto& region : regions) {
            SetExpect(
                region.id > 0
                    && region.voxelCount >= 0
                    && regionIds.insert(region.id).second,
                "DefX regions should expose unique positive IDs.",
                failureCount);
            SetExpect(
                std::isfinite(region.volumeMM3)
                    && region.volumeMM3 >= 0.0f
                    && std::isfinite(region.equivalentDiameterMM)
                    && std::isfinite(region.radiusMM)
                    && std::isfinite(region.diameterMM),
                "DefX region size fields should remain finite.",
                failureCount);
        }

        for (const auto label : labels) {
            SetExpect(
                label >= 0,
                "DefX label values must not be negative.",
                failureCount);
            if (label > 0) {
                ++labelCounts[label];
                SetExpect(
                    regionIds.find(label) != regionIds.end(),
                    "Every positive DefX label should name a returned region.",
                    failureCount);
            }
        }

        std::uint64_t voidVoxelCount = 0;
        for (const auto& region : regions) {
            voidVoxelCount += static_cast<std::uint64_t>(
                region.voxelCount);
            const auto iterator = labelCounts.find(region.id);
            const auto labelCount = iterator == labelCounts.end()
                ? 0 : iterator->second;
            SetExpect(
                labelCount == static_cast<std::uint64_t>(
                    region.voxelCount),
                "DefX region voxelCount should equal its raw label count.",
                failureCount);
        }

        const auto statistics = service.GetStatistics();
        SetExpect(
            statistics.voidVoxelCount == voidVoxelCount,
            "Feature statistics should project DefX region voxel counts.",
            failureCount);
        SetExpect(
            std::isfinite(statistics.objectVolumeMM3)
                && statistics.objectVolumeMM3 >= 0.0
                && std::isfinite(statistics.voidVolumeMM3)
                && statistics.voidVolumeMM3 >= 0.0
                && std::isfinite(statistics.porosityRatio)
                && statistics.porosityRatio >= 0.0,
            "Feature statistics should project finite DefX header values.",
            failureCount);
        SetExpect(
            hasDefectExpected
                ? !regions.empty() && voidVoxelCount > 0
                : regions.empty() && voidVoxelCount == 0,
            hasDefectExpected
                ? "Synthetic input should retain the DefX defect batch."
                : "Gap minimum-volume filter should return an empty batch.",
            failureCount);

        auto voidMesh = service.BuildVoidMesh();
        SetExpect(
            voidMesh != nullptr,
            "A successful DefX batch should expose a mesh object.",
            failureCount);
        if (voidMesh) {
            SetExpect(
                hasDefectExpected
                    ? voidMesh->GetNumberOfPoints() > 0
                        && voidMesh->GetNumberOfCells() > 0
                    : voidMesh->GetNumberOfPoints() == 0
                        && voidMesh->GetNumberOfCells() == 0,
                "Mesh occupancy should follow the raw DefX labels.",
                failureCount);
        }
    }

    void StartSnapCase(int& failureCount) const
    {
        auto image = BuildTestImage();
        vtkWeakPointer<vtkImageData> weakImage;
        weakImage = image.GetPointer();
        GapAnalysisService service;
        SetExpect(
            service.SetGapInput(image),
            "Gap service should accept the synthetic image.",
            failureCount);
        SetSolidImage(image);
        image = nullptr;
        SetExpect(
            weakImage == nullptr,
            "Gap input isolation should release the caller image.",
            failureCount);

        GapSurfaceParams surface;
        surface.isoValue = 0.5f;
        surface.background = 0.0f;
        surface.material = 1.0f;
        service.SetSurface(surface);
        service.SetVoid(BuildVoidParams());

        std::atomic<bool> hasCallback{ false };
        std::atomic<bool> isCallbackOk{ false };
        SetExpect(
            service.StartAsync([&](bool isSuccess) {
                hasCallback.store(true);
                isCallbackOk.store(isSuccess);
            }),
            "DefX worker should accept the first request.",
            failureCount);
        SetExpect(
            !service.StartAsync(nullptr),
            "DefX worker should reject an overlapping request.",
            failureCount);
        SetExpect(
            GetServiceState(service) == GapAnalysisState::Succeeded,
            "DefX worker should finish successfully.",
            failureCount);
        SetExpect(
            !service.StartAsync(nullptr),
            "Pending callback should preserve the completed batch.",
            failureCount);
        SetExpect(
            SendDoneEvent(service)
                && hasCallback.load()
                && isCallbackOk.load(),
            "Explicit callback consumption should report DefX success.",
            failureCount);
        SetResultExpect(service, true, failureCount);
    }

    void StartConvertCase(int& failureCount) const
    {
        auto image = BuildShortImage();
        GapAnalysisService service;
        SetExpect(
            service.SetGapInput(image),
            "Gap service should accept a short scalar image.",
            failureCount);
        auto* source = static_cast<short*>(
            image->GetScalarPointer());
        std::fill_n(
            source,
            image->GetNumberOfPoints(),
            static_cast<short>(1));
        image->Modified();

        GapSurfaceParams surface;
        surface.isoValue = 0.5f;
        surface.background = 0.0f;
        surface.material = 1.0f;
        service.SetSurface(surface);
        service.SetVoid(BuildVoidParams());
        SetExpect(
            service.StartAsync(nullptr)
                && GetServiceState(service)
                    == GapAnalysisState::Succeeded,
            "Converted short input should complete through DefX.",
            failureCount);
        SetResultExpect(service, true, failureCount);
    }

    void StartFilterCase(int& failureCount) const
    {
        GapAnalysisService service;
        SetExpect(
            service.SetGapInput(BuildTestImage()),
            "Filter case should accept the synthetic image.",
            failureCount);
        GapSurfaceParams surface;
        surface.isoValue = 0.5f;
        surface.background = 0.0f;
        surface.material = 1.0f;
        service.SetSurface(surface);
        auto params = BuildVoidParams();
        params.minVolumeMM3 = 1000000.0;
        service.SetVoid(params);
        SetExpect(
            service.StartAsync(nullptr)
                && GetServiceState(service)
                    == GapAnalysisState::Succeeded,
            "Disabled Gap filter should accept a retained minimum-volume value.",
            failureCount);
        // filter 关闭时 Feature 不得按保留的 minVolumeMM3 在返回后二次过滤。
        SetResultExpect(service, true, failureCount);
        const auto unfiltered = service.GetStatistics();
        const auto originalLabels = GetLabels(service.BuildLabelImage(), failureCount);
        params.isFilterEnabled = true;
        params.minVolumeMM3 = 0.0;
        service.SetVoid(params);
        SetExpect(service.StartAsync(nullptr) && GetServiceState(service) == GapAnalysisState::Succeeded,
            "Enabled zero-volume filter should retain the complete batch.", failureCount);
        SetResultExpect(service, true, failureCount);
        SetExpect(service.GetLabelStorageTransferred(), "No excluded IDs should preserve borrowed label storage.", failureCount);
        SetExpect(GetLabels(service.BuildLabelImage(), failureCount) == originalLabels,
            "Zero-volume selection should retain every native label at its original voxel.", failureCount);
        params.minVolumeMM3 = 1000000.0;
        service.SetVoid(params);
        SetExpect(service.StartAsync(nullptr) && GetServiceState(service) == GapAnalysisState::Succeeded,
            "Enabled minimum-volume filter should complete through the private detection kernel.", failureCount);
        SetResultExpect(service, false, failureCount);
        SetExpectNear(service.GetStatistics().voidVolumeMM3, 0.0, "Filtered void volume should be zero.", failureCount);
        SetExpectNear(service.GetStatistics().porosityRatio, 0.0, "Filtered porosity should be zero.", failureCount);
        SetExpect(service.GetStatistics().objectVoxelCount == unfiltered.objectVoxelCount,
            "Size filtering should preserve the source object voxel count.", failureCount);
        SetExpectNear(service.GetStatistics().objectVolumeMM3, unfiltered.objectVolumeMM3,
            "Size filtering should preserve the source material volume.", failureCount);
        params.isFilterEnabled = false;
        service.SetVoid(params);
        SetExpect(service.StartAsync(nullptr) && GetServiceState(service) == GapAnalysisState::Succeeded,
            "Disabling the filter should recover the original detection batch.", failureCount);
        SetResultExpect(service, true, failureCount);
        SetExpect(GetLabels(service.BuildLabelImage(), failureCount) == originalLabels,
            "Disabling selection should restore the complete original label map.", failureCount);
        SetExpectNear(service.GetStatistics().voidVolumeMM3, unfiltered.voidVolumeMM3,
            "Disabling selection should restore the original void volume.", failureCount);
    }

    void StartPartialFilterCase(int& failureCount) const
    {
        auto image = BuildTestImage();
        SetSolidImage(image);
        auto* voxels = static_cast<float*>(image->GetScalarPointer());
        for (int z = 2; z <= 3; ++z) for (int y = 2; y <= 3; ++y) for (int x = 2; x <= 3; ++x)
            voxels[GetLinearIndex(x, y, z, TestDims)] = 0.0f;
        voxels[GetLinearIndex(5, 5, 4, TestDims)] = 0.0f;
        voxels[GetLinearIndex(5, 5, 5, TestDims)] = 0.0f;
        image->Modified();
        const std::vector<float> originalInput(voxels, voxels + image->GetNumberOfPoints());
        GapAnalysisService service;
        SetExpect(service.SetGapInput(image), "Partial filter should freeze the two-pore input.", failureCount);
        GapSurfaceParams surface; surface.isoValue = 0.5f; surface.background = 0.0f; surface.material = 1.0f;
        service.SetSurface(surface);
        auto params = BuildVoidParams(); service.SetVoid(params);
        SetExpect(service.StartAsync(nullptr) && GetServiceState(service) == GapAnalysisState::Succeeded,
            "Two-pore input should complete through DefX.", failureCount);
        const auto raw = service.GetVoidRegions();
        SetExpect(raw.size() == 2, "Partial filter fixture should detect two separate regions.", failureCount);
        if (raw.size() != 2) return;
        const auto rawLabels = GetLabels(service.BuildLabelImage(), failureCount);
        const auto kept = *std::max_element(raw.begin(), raw.end(), [](const auto& a, const auto& b) { return a.volumeMM3 < b.volumeMM3; });
        const auto excluded = *std::min_element(raw.begin(), raw.end(), [](const auto& a, const auto& b) { return a.volumeMM3 < b.volumeMM3; });
        params.isFilterEnabled = true; params.minVolumeMM3 = (kept.volumeMM3 + excluded.volumeMM3) * 0.5;
        service.SetVoid(params);
        SetExpect(service.StartAsync(nullptr) && GetServiceState(service) == GapAnalysisState::Succeeded,
            "Partial size selection should complete successfully.", failureCount);
        SetResultExpect(service, true, failureCount);
        const auto selected = service.GetVoidRegions();
        SetExpect(selected.size() == 1 && selected.front().id == kept.id && selected.front().voxelCount == kept.voxelCount,
            "Partial filtering should preserve the selected native ID and voxel count.", failureCount);
        SetExpectNear(service.GetStatistics().voidVolumeMM3, kept.volumeMM3,
            "Filtered statistics should contain only the selected region volume.", failureCount);
        const auto selectedLabels = GetLabels(service.BuildLabelImage(), failureCount);
        bool hasExpectedLabels = selectedLabels.size() == rawLabels.size();
        for (std::size_t i = 0; hasExpectedLabels && i < rawLabels.size(); ++i)
            hasExpectedLabels = selectedLabels[i] == (rawLabels[i] == kept.id ? kept.id : 0);
        SetExpect(hasExpectedLabels, "Selected IDs should preserve their exact voxel positions and excluded IDs should become background.", failureCount);
        SetExpect(!service.GetLabelStorageTransferred(), "Excluded IDs should use independent selected labels.", failureCount);
        SetExpect(std::equal(originalInput.begin(), originalInput.end(), voxels),
            "Result selection must preserve the original input voxel values.", failureCount);
    }

    int GetFailCount() const
    {
        int failureCount = 0;
        StartSnapCase(failureCount);
        StartConvertCase(failureCount);
        StartFilterCase(failureCount);
        StartPartialFilterCase(failureCount);
        return failureCount;
    }
};

int main(int argc, char** argv)
{
    if(argc==2 && std::string(argv[1])=="--mask-bridge")return GetGapMaskBridgeFailCount()?1:0;
    if (argc > 1 && std::string(argv[1]) == "--connectivity") return GetGapConnectivityFailCount(argc > 2 ? argv[2] : "", argc > 3 && std::string(argv[3]) == "write");
    if (argc == 2 && std::string(argv[1]) == "--mask-geometry") {
        const auto failures = GetGapMaskGeometryFailCount();
        return failures ? 1 : 0;
    }
    if ((argc == 3 || argc == 5) && std::string(argv[1]) == "--real-small-input") {
        return GetGapRealSmallFailCount(argv[2],argc == 5 ? argv[3] : nullptr,argc == 5 && std::string(argv[4]) == "write") ? 1 : 0;
    }
    int failureCount = GetGapLabelFailCount();
    failureCount += GapAlgorithmSuite().GetFailCount();
    failureCount += GapDisplaySuite().GetFailCount();
    if (failureCount != 0) {
        std::cerr
            << "GapAnalysisAlgorithmTests failed: "
            << failureCount << '\n';
        return 1;
    }
    std::cout << "GapAnalysisAlgorithmTests passed.\n";
    return 0;
}
