#include "VtkDiffusionAdapter.h"

#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkSMPTools.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <vector>

namespace ArtifactReduction {
namespace {
ArtifactError BuildSlab(const GridGeometry3D& grid, const ArtifactDiffusionParams& params,
    const std::vector<float>& values, std::vector<float>& output, const int slab,
    TaskControl& control, const std::atomic<bool>& stopped)
{
    const auto& dims = grid.dimensions;
    const auto plane = static_cast<std::size_t>(dims[0]) * dims[1];
    const int end = slab + std::min(params.slabDepth, dims[2] - slab);
    const int beginHalo = std::max(0, slab - params.iterations);
    const int endHalo = end + std::min(params.iterations, dims[2] - end);
    // 所有块从同一份阶段输入读取完整迭代 halo，只写各自中心区。
    // 保持原 VTK 滤波参数与同步迭代顺序，不依赖相邻块的完成顺序。
    vtkNew<vtkImageData> input;
    input->SetExtent(0, dims[0] - 1, 0, dims[1] - 1, beginHalo, endHalo - 1);
    input->SetSpacing(grid.spacing.data());
    input->AllocateScalars(VTK_FLOAT, 1);
    auto* inputValues = static_cast<float*>(input->GetScalarPointer());
    if (!inputValues) return ArtifactError::TooLarge;
    std::memcpy(inputValues, values.data() + plane * beginHalo, plane * (endHalo - beginHalo) * sizeof(float));
    vtkNew<vtkImageAnisotropicDiffusion3D> filter;
    // 并行发生在独立滤波器之间；避免 VTK 内部再次开线程及共享 observer 状态。
    filter->SetEnableSMP(false);
    filter->SetNumberOfThreads(1);
    filter->SetInputData(input);
    filter->SetNumberOfIterations(params.iterations);
    filter->SetDiffusionThreshold(params.threshold);
    filter->SetDiffusionFactor(params.factor);
    filter->FacesOn(); filter->EdgesOn(); filter->CornersOn();
    filter->GradientMagnitudeThresholdOff();
    struct ObserverState final {
        TaskControl* control;
        const std::atomic<bool>* stopped;
        bool hasError = false;
    } observerState{&control, &stopped};
    vtkNew<vtkCallbackCommand> observer;
    observer->SetClientData(&observerState);
    observer->SetCallback([](vtkObject* sender, unsigned long event, void* client, void*) {
        auto& state = *static_cast<ObserverState*>(client);
        if (event == vtkCommand::ErrorEvent) state.hasError = true;
        if (state.hasError || state.stopped->load(std::memory_order_relaxed)
            || state.control->GetError() != ArtifactError::None) {
            if (auto* algorithm = vtkAlgorithm::SafeDownCast(sender)) algorithm->SetAbortExecute(1);
        }
    });
    const auto progressTag = filter->AddObserver(vtkCommand::ProgressEvent, observer);
    const auto errorTag = filter->AddObserver(vtkCommand::ErrorEvent, observer);
    struct ObserverGuard final {
        vtkImageAnisotropicDiffusion3D* filter;
        unsigned long progressTag;
        unsigned long errorTag;
        ~ObserverGuard() { filter->RemoveObserver(progressTag); filter->RemoveObserver(errorTag); }
    } observerGuard{filter, progressTag, errorTag};
    filter->Update();
    if (const auto error = control.GetError(); error != ArtifactError::None) return error;
    if (observerState.hasError || filter->GetErrorCode() != 0) return ArtifactError::KernelFailed;
    if (stopped.load(std::memory_order_relaxed)) return ArtifactError::Cancelled;
    const auto* result = static_cast<const float*>(filter->GetOutput()->GetScalarPointer(0, 0, slab));
    if (!result) return ArtifactError::KernelFailed;
    std::memcpy(output.data() + plane * slab, result, plane * (end - slab) * sizeof(float));
    return ArtifactError::None;
}
} // namespace

ArtifactError BuildDiffusion(const GridGeometry3D& grid,
    const ArtifactDiffusionParams& params, std::vector<float>& values, TaskControl& control, int workerCount)
{
    if (params.factor == 0.0) return control.GetError();
    if (workerCount < 1 || workerCount > 8) return ArtifactError::InvalidRequest;
    const auto& dims = grid.dimensions;
    const int slabCount = 1 + (dims[2] - 1) / params.slabDepth;
    workerCount = std::min(workerCount, slabCount);
    std::vector<float> output(values.size());
    std::vector<ArtifactError> errors(static_cast<std::size_t>(workerCount), ArtifactError::None);
    std::atomic<bool> stopped{false};
    std::atomic<int> completedSlices{0};
    vtkSMPTools::For(0, workerCount, 1, [&](vtkIdType first, vtkIdType last) {
        for (auto worker = first; worker < last; ++worker) {
            auto& error = errors[static_cast<std::size_t>(worker)];
            try {
                for (int index = static_cast<int>(worker); index < slabCount; index += workerCount) {
                    if (stopped.load(std::memory_order_relaxed)) break;
                    error = control.GetError();
                    if (error != ArtifactError::None) break;
                    const int slab = index * params.slabDepth;
                    error = BuildSlab(grid, params, values, output, slab, control, stopped);
                    if (error != ArtifactError::None) break;
                    const int slices = std::min(params.slabDepth, dims[2] - slab);
                    const int done = completedSlices.fetch_add(slices, std::memory_order_relaxed) + slices;
                    const auto progress = 40 + static_cast<unsigned int>(35.0 * done / dims[2]);
                    auto prior = control.progress.load(std::memory_order_relaxed);
                    while (prior < progress && !control.progress.compare_exchange_weak(prior, progress, std::memory_order_relaxed)) {}
                }
            }
            catch (const std::bad_alloc&) { error = ArtifactError::TooLarge; }
            catch (...) { error = ArtifactError::KernelFailed; }
            if (error != ArtifactError::None) stopped.store(true, std::memory_order_relaxed);
        }
    });
    // 优先保留真正失败原因，其他 worker 因联动停止返回的 Cancelled 不覆盖它。
    for (const auto error : errors) {
        if (error != ArtifactError::None && error != ArtifactError::Cancelled) return error;
    }
    if (const auto error = control.GetError(); error != ArtifactError::None) return error;
    if (stopped.load(std::memory_order_relaxed)) return ArtifactError::Cancelled;
    values = std::move(output);
    return ArtifactError::None;
}
} // namespace ArtifactReduction
