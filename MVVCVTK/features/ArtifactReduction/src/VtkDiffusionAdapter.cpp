#include "VtkDiffusionAdapter.h"

#include <vtkAlgorithm.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImageAnisotropicDiffusion3D.h>
#include <vtkImageData.h>
#include <vtkNew.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace ArtifactReduction {
ArtifactError BuildDiffusion(const GridGeometry3D& grid,
    const ArtifactDiffusionParams& params, std::vector<float>& values, TaskControl& control)
{
    if (params.factor == 0.0) return control.GetError();
    const auto& dims = grid.dimensions;
    const auto plane = static_cast<std::size_t>(dims[0]) * dims[1];
    std::vector<float> output(values.size());
    for (int slab = 0; slab < dims[2];) {
        if (const auto error = control.GetError(); error != ArtifactError::None) return error;
        const int end = slab + std::min(params.slabDepth, dims[2] - slab);
        const int beginHalo = std::max(0, slab - params.iterations);
        const int endHalo = end + std::min(params.iterations, dims[2] - end);
        // 临时网格用相对索引；此滤波器仅使用邻接和spacing，平移extent不影响
        // 数值。原始extent/origin/direction保留在最终payload，逐块不改物理几何。
        // 每个块拥有完整N层halo；输出只消费中心区，避免人为块边界扩散。
        vtkNew<vtkImageData> input;
        input->SetExtent(0, dims[0] - 1, 0, dims[1] - 1, beginHalo, endHalo - 1);
        input->SetSpacing(grid.spacing.data());
        input->AllocateScalars(VTK_FLOAT, 1);
        auto* inputValues = static_cast<float*>(input->GetScalarPointer());
        if (!inputValues) return ArtifactError::TooLarge;
        std::memcpy(inputValues, values.data() + plane * beginHalo, plane * (endHalo - beginHalo) * sizeof(float));
        vtkNew<vtkImageAnisotropicDiffusion3D> filter;
        filter->SetEnableSMP(false);
        filter->SetNumberOfThreads(1);
        filter->SetInputData(input);
        filter->SetNumberOfIterations(params.iterations);
        filter->SetDiffusionThreshold(params.threshold);
        filter->SetDiffusionFactor(params.factor);
        filter->FacesOn(); filter->EdgesOn(); filter->CornersOn();
        filter->GradientMagnitudeThresholdOff();
        struct ObserverState final { TaskControl* control; bool hasError = false; } observerState{ &control };
        vtkNew<vtkCallbackCommand> observer;
        observer->SetClientData(&observerState);
        observer->SetCallback([](vtkObject* sender, unsigned long event, void* client, void*) {
            auto& state = *static_cast<ObserverState*>(client);
            if (event == vtkCommand::ErrorEvent) state.hasError = true;
            if (state.control->GetError() != ArtifactError::None) {
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
        } observerGuard{ filter, progressTag, errorTag };
        filter->Update();
        if (const auto error = control.GetError(); error != ArtifactError::None) return error;
        if (observerState.hasError || filter->GetErrorCode() != 0) return ArtifactError::KernelFailed;
        const auto* result = static_cast<const float*>(filter->GetOutput()->GetScalarPointer(0, 0, slab));
        if (!result) return ArtifactError::KernelFailed;
        std::memcpy(output.data() + plane * slab, result, plane * (end - slab) * sizeof(float));
        control.progress.store(40 + static_cast<unsigned int>(40.0 * end / dims[2]), std::memory_order_relaxed);
        slab = end;
    }
    values = std::move(output);
    return ArtifactError::None;
}
} // namespace ArtifactReduction
