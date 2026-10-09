#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceDeterminationTestSupport.h"
#include <Windows.h>
#include <Psapi.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>

namespace {
template <class T> void Write(std::ostream &stream, const T &value) {
    stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
}
void WriteResult(std::ostream &stream, const SurfaceAlgorithmResult &result) {
    Write(stream, result.status);
    Write(stream, result.failureReason);
    Write(stream, result.parameterFingerprint);
    Write(stream, result.points.size());
    for (const auto &p : result.points) {
        for (auto x : p.seedPositionModel)
            Write(stream, x);
        for (auto x : p.seedNormalModel)
            Write(stream, x);
        for (auto x : p.positionModel)
            Write(stream, x);
        for (auto x : p.normalModel)
            Write(stream, x);
        Write(stream, p.localThreshold);
        Write(stream, p.gradientMagnitude);
        Write(stream, p.validSupportRatio);
        Write(stream, p.flags);
        Write(stream, p.objectIndex);
        Write(stream, p.interfaceIndex);
    }
    Write(stream, result.triangleIndices.size());
    for (auto x : result.triangleIndices)
        Write(stream, x);
    for (auto x : result.triangleValidity)
        Write(stream, x);
    Write(stream, result.objects.size());
    for (const auto &o : result.objects) {
        Write(stream, o.pointCount);
        Write(stream, o.triangleCount);
        Write(stream, o.areaValidity);
        Write(stream, o.volumeValidity);
        Write(stream, o.areaModelUnit2.has_value());
        Write(stream, o.areaModelUnit2.value_or(0));
        Write(stream, o.volumeModelUnit3.has_value());
        Write(stream, o.volumeModelUnit3.value_or(0));
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 7 && !(argc == 8 && std::string_view(argv[7]) == "--metrics-only")) {
        std::cerr << "SurfaceAccelerationAudit raw nx ny nz iso output-prefix [--metrics-only]\n";
        return 2;
    }
    try {
        const std::array<int, 3> dimensions{std::stoi(argv[2]), std::stoi(argv[3]), std::stoi(argv[4])};
        for (auto n : dimensions)
            if (n < 2)
                return 2;
        // 只向 VTK 自有数组读取，不另建整卷测试副本。
        auto image = vtkSmartPointer<vtkImageData>::New();
        image->SetDimensions(dimensions.data());
        image->SetSpacing(0.1537, 0.1537, 0.1537);
        image->AllocateScalars(VTK_FLOAT, 1);
        const auto count = std::size_t(dimensions[0]) * dimensions[1] * dimensions[2];
        std::ifstream raw(argv[1], std::ios::binary);
        if (!raw.read(static_cast<char *>(image->GetScalarPointer()), count * sizeof(float)) ||
            raw.peek() != std::char_traits<char>::eof())
            return 2;
        image->GetPointData()->GetScalars()->Modified();
        VtkDataBridge bridge;
        const auto payload = bridge.CreateImagePayload(image, {});
        const auto ref = GetTestDataRef(1);
        const auto data =
            std::make_shared<const DataRevision>(DataRevision{ref, DataTypes::imageGrid3D, {}, payload, {}});
        const auto source = std::make_shared<const VtkImageGridView>(
            VtkImageGridView{{}, DataBinding{std::string(primaryVolumeBinding), ref, 1}, data, image, {}});
        auto params = SurfaceTest::GetParams(SurfaceDeterminationMethod::GlobalAutomatic);
        params.componentSelection = SurfaceComponentSelection::All;
        params.minimumObjectVoxels = 0;
        params.initialIsoValue = std::stod(argv[5]);
        PROCESS_MEMORY_COUNTERS_EX before{};
        before.cb = sizeof(before);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&before),
                             sizeof(before));
        const auto start = std::chrono::steady_clock::now();
        auto result = SurfaceDeterminationAlgorithm::BuildSurface(
            source, params, std::numeric_limits<std::size_t>::max(), [] { return false; }, {});
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        PROCESS_MEMORY_COUNTERS_EX after{};
        after.cb = sizeof(after);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&after),
                             sizeof(after));
        if(argc==7) {
            std::ofstream binary(std::string(argv[6]) + ".bin", std::ios::binary);
            WriteResult(binary, result);
            if(!binary) return 2;
        }
        std::ofstream json(std::string(argv[6]) + ".json");
        json << std::setprecision(17) << "{\"status\":" << unsigned(result.status)
             << ",\"points\":" << result.points.size()
             << ",\"triangles\":" << result.triangleIndices.size() / 3 << ",\"totalMs\":" << elapsed
             << ",\"seedMs\":" << result.execution.seedMs
             << ",\"refinementMs\":" << result.execution.refinementMs
             << ",\"componentsMs\":" << result.componentsMs
             << ",\"meshPreparationMs\":" << result.meshPreparationMs
             << ",\"pointSamplingMs\":" << result.pointSamplingMs
             << ",\"outputMs\":" << result.outputMs
             << ",\"cudaPointCount\":" << result.cudaPointCount
             << ",\"cudaDeviceBytes\":" << result.cudaDeviceBytes
             << ",\"cudaHostBufferBytes\":" << result.cudaHostBufferBytes
             << ",\"cudaBatchCount\":" << result.cudaBatchCount
             << ",\"cpuThreadCount\":" << result.cpuThreadCount
             << ",\"availablePhysicalCores\":" << result.availablePhysicalCores
             << ",\"availableLogicalThreads\":" << result.availableLogicalThreads
             << ",\"plannedCpuThreads\":" << result.plannedCpuThreads
             << ",\"beforePrivateBytes\":" << before.PrivateUsage
             << ",\"peakPrivateBytes\":" << after.PeakPagefileUsage
             << ",\"retainedPrivateBytes\":" << after.PrivateUsage
             << ",\"peakWorkingBytes\":" << after.PeakWorkingSetSize << "}\n";
        std::cout << "points=" << result.points.size() << " triangles=" << result.triangleIndices.size() / 3
                  << " totalMs=" << elapsed << " peakPrivate=" << after.PeakPagefileUsage << '\n';
        if (result.status != SurfaceResultStatus::Succeeded)
            std::cerr << result.message << '\n';
        return result.status == SurfaceResultStatus::Succeeded ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
