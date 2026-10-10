#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceDeterminationTestSupport.h"
#include <Windows.h>
#include <Psapi.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <fcntl.h>
#include <io.h>

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
    if (argc < 7) {
        std::cerr << "SurfaceAccelerationAudit raw nx ny nz iso output-prefix [--metrics-only|--binary-stdout] "
                     "[--roi-box xmin xmax ymin ymax zmin zmax]\n";
        return 2;
    }
    try {
        bool streamBinary=false,metricsOnly=false;
        std::optional<std::array<double,6>> roiBounds;
        for(int i=7;i<argc;++i) {
            const std::string_view option(argv[i]);
            if(option=="--binary-stdout" || option=="--metrics-only") {
                if(streamBinary || metricsOnly)return 2;
                streamBinary=option=="--binary-stdout";metricsOnly=!streamBinary;
            } else if(option=="--roi-box") {
                if(roiBounds || argc-i<7)return 2;
                roiBounds.emplace();
                for(auto &value:*roiBounds)value=std::stod(argv[++i]);
                for(unsigned axis=0;axis<3;++axis)
                    if(!std::isfinite((*roiBounds)[2*axis]) || !std::isfinite((*roiBounds)[2*axis+1]) ||
                        (*roiBounds)[2*axis]>=(*roiBounds)[2*axis+1])return 2;
            } else return 2;
        }
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
        SurfaceAlgorithmInputs inputs;
        if(roiBounds){
            inputs.roi=SurfaceTest::BuildRoi(source,*roiBounds);
            if(!inputs.roi)return 2;
            params.analysisRoi=inputs.roi->GetRevision();
        }
        PROCESS_MEMORY_COUNTERS_EX before{};
        before.cb = sizeof(before);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&before),
                             sizeof(before));
        const auto start = std::chrono::steady_clock::now();
        auto result = SurfaceDeterminationAlgorithm::BuildSurface(
            source, params, std::numeric_limits<std::size_t>::max(), [] { return false; }, {},inputs);
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        PROCESS_MEMORY_COUNTERS_EX after{};
        after.cb = sizeof(after);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&after),
                             sizeof(after));
        if (streamBinary) {
            // 管道流式校验完整结果，避免大体量回归重复写入数 GiB 文件。
            if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
            WriteResult(std::cout, result);
            std::cout.flush();
            if (!std::cout) return 2;
        } else if(!metricsOnly) {
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
             << ",\"seedClassificationCacheBytes\":" << result.seedClassificationCacheBytes
             << ",\"seedClassifiedNodeCount\":" << result.seedClassifiedNodeCount
             << ",\"seedUniformCellCount\":" << result.seedUniformCellCount
             << ",\"seedCacheBudgetFallbackCount\":" << result.seedCacheBudgetFallbackCount
             << ",\"cudaPointCount\":" << result.cudaPointCount
             << ",\"cudaDeviceBytes\":" << result.cudaDeviceBytes
             << ",\"cudaHostBufferBytes\":" << result.cudaHostBufferBytes
             << ",\"cudaBatchCount\":" << result.cudaBatchCount
             << ",\"cudaProfilingRequested\":" << result.cudaProfilingRequested
             << ",\"cudaStartMs\":" << result.cudaStartMs
             << ",\"cudaIndexMs\":" << result.cudaIndexMs
             << ",\"cudaSubmitMs\":" << result.cudaSubmitMs
             << ",\"cudaCollectMs\":" << result.cudaCollectMs
             << ",\"cudaFinalizeMs\":" << result.cudaFinalizeMs
             << ",\"cudaCloseMs\":" << result.cudaCloseMs
             << ",\"cpuSamplingMs\":" << result.cpuSamplingMs
             << ",\"cudaProfileEventsAvailable\":" << result.cudaProfile.eventsAvailable
             << ",\"cudaProfileComplete\":" << result.cudaProfile.complete
             << ",\"cudaProfileFailed\":" << result.cudaProfile.failed
             << ",\"cudaProfileSubmittedBatches\":" << result.cudaProfile.submittedBatchCount
             << ",\"cudaProfileMeasuredBatches\":" << result.cudaProfile.measuredBatchCount
             << ",\"cudaProfileMeasuredPoints\":" << result.cudaProfile.measuredPointCount
             << ",\"cudaSlabUploadCount\":" << result.cudaProfile.slabUploadCount
             << ",\"cudaSlabUploadBytes\":" << result.cudaProfile.slabUploadBytes
             << ",\"cudaEventSetupMs\":" << result.cudaProfile.eventSetupMs
             << ",\"cudaSlabWaitMs\":" << result.cudaProfile.slabWaitMs
             << ",\"cudaSlabUploadHostMs\":" << result.cudaProfile.slabUploadHostMs
             << ",\"cudaPointUploadMs\":" << result.cudaProfile.pointUploadMs
             << ",\"cudaKernelMs\":" << result.cudaProfile.kernelMs
             << ",\"cudaSampleDownloadMs\":" << result.cudaProfile.sampleDownloadMs
             << ",\"cpuThreadCount\":" << result.cpuThreadCount
             << ",\"availablePhysicalCores\":" << result.availablePhysicalCores
             << ",\"availableLogicalThreads\":" << result.availableLogicalThreads
             << ",\"plannedCpuThreads\":" << result.plannedCpuThreads
             << ",\"componentCpuThreads\":" << result.componentCpuThreads
             << ",\"componentCpuPointCount\":" << result.componentCpuPointCount
             << ",\"componentWorkspaceBound\":" << result.componentWorkspaceBound
             << ",\"componentBudgetFallbackCount\":" << result.componentBudgetFallbackCount
             << ",\"componentParallelMs\":" << result.componentParallelMs
             << ",\"beforePrivateBytes\":" << before.PrivateUsage
             << ",\"peakPrivateBytes\":" << after.PeakPagefileUsage
             << ",\"retainedPrivateBytes\":" << after.PrivateUsage
             << ",\"peakWorkingBytes\":" << after.PeakWorkingSetSize << "}\n";
        if (!streamBinary) std::cout << "points=" << result.points.size() << " triangles=" << result.triangleIndices.size() / 3
                  << " totalMs=" << elapsed << " peakPrivate=" << after.PeakPagefileUsage << '\n';
        if (result.status != SurfaceResultStatus::Succeeded)
            std::cerr << result.message << '\n';
        return result.status == SurfaceResultStatus::Succeeded ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
