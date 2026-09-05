#include "PartSegmentationTestCases.h"

#include "Render/Strategies/PartOverlayStrategies.h"
#include "Render/Internal/PartSurfaceProductBuilder.h"

#include <vtkActor.h>
#include <vtkImageData.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkPointData.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkUnsignedIntArray.h>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

bool GetCaseResult(const bool passed, const std::string_view name)
{
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

vtkSmartPointer<vtkImageData> BuildLabelImage()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(4, 4, 4);
    image->AllocateScalars(VTK_UNSIGNED_INT, 1);
    auto* labels = static_cast<unsigned int*>(image->GetScalarPointer());
    std::fill(labels, labels + 64, 0U);
    labels[0] = 1U;
    labels[63] = 2U;
    return image;
}

std::uint64_t GetWorkingSetBytes() noexcept
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(
        GetCurrentProcess(), &counters, sizeof(counters))
        ? static_cast<std::uint64_t>(counters.WorkingSetSize)
        : 0;
}

} // namespace

int GetPartDisplayFailCount()
{
    int failureCount = 0;
    const auto labels = BuildLabelImage();
    auto surfaceLabels =
        std::make_shared<std::vector<std::uint32_t>>(64, 0U);
    for (int z = 1; z <= 2; ++z) {
        for (int y = 1; y <= 2; ++y) {
            for (int x = 1; x <= 2; ++x) {
                (*surfaceLabels)[static_cast<std::size_t>(
                    x + 4 * (y + 4 * z))] = 1U;
            }
        }
    }
    PartSurfaceBuildRequest surfaceRequest;
    surfaceRequest.extent = { 0, 3, 0, 3, 0, 3 };
    surfaceRequest.dimensions = { 4, 4, 4 };
    surfaceRequest.labels = surfaceLabels;
    surfaceRequest.partCount = 1;
    surfaceRequest.maxWorkingBytes = 1024U * 1024U;
    const std::uint64_t surfaceRssBefore = GetWorkingSetBytes();
    std::uint64_t surfaceRssPeak = surfaceRssBefore;
    const auto surfaceProduct = PartSurfaceProductBuilder::BuildProduct(
        surfaceRequest, {},
        [&surfaceRssPeak](double) {
            surfaceRssPeak = std::max(
                surfaceRssPeak, GetWorkingSetBytes());
        });
    const std::uint64_t surfaceRssAfter = GetWorkingSetBytes();
    std::cout
        << "PART_SURFACE_RSS: before=" << surfaceRssBefore
        << " peak=" << surfaceRssPeak
        << " after=" << surfaceRssAfter
        << " product="
        << (surfaceProduct.product
            ? surfaceProduct.product->actualBytes : 0)
        << '\n';

    vtkNew<vtkRenderer> surfaceRenderer;
    auto surface = std::make_shared<PartSurfaceOverlayStrategy>();
    if (surfaceProduct.product) {
        surface->SetInputData(surfaceProduct.product->surface);
    }
    surface->AttachRenderer(surfaceRenderer);
    vtkNew<vtkRenderer> secondSurfaceRenderer;
    auto secondSurface =
        std::make_shared<PartSurfaceOverlayStrategy>();
    if (surfaceProduct.product) {
        secondSurface->SetInputData(surfaceProduct.product->surface);
    }
    secondSurface->AttachRenderer(secondSurfaceRenderer);
    auto getSurfaceInput = [](vtkRenderer* renderer) {
        auto* props = renderer ? renderer->GetViewProps() : nullptr;
        if (!props) return static_cast<vtkPolyData*>(nullptr);
        props->InitTraversal();
        auto* actor = vtkActor::SafeDownCast(props->GetNextProp());
        auto* mapper = actor
            ? vtkPolyDataMapper::SafeDownCast(actor->GetMapper())
            : nullptr;
        return mapper ? mapper->GetInput() : nullptr;
    };
    failureCount += GetCaseResult(
        surfaceProduct.failureReason == PartFailureReason::None
            && surfaceProduct.product
            && surfaceProduct.product->surface
            && surfaceProduct.product->surface->GetNumberOfCells() > 0
            && surfaceProduct.product->actualBytes > 0
            && surfaceRssBefore > 0
            && surfaceRssPeak >= surfaceRssBefore
            && surfaceRssAfter > 0
            && surfaceRenderer->GetViewProps()->GetNumberOfItems() == 1
            && secondSurfaceRenderer->GetViewProps()
                ->GetNumberOfItems() == 1
            && getSurfaceInput(surfaceRenderer)
                == surfaceProduct.product->surface.GetPointer()
            && getSurfaceInput(secondSurfaceRenderer)
                == surfaceProduct.product->surface.GetPointer(),
        "Surface overlays share one materialized polydata product") ? 0 : 1;
    surface->DetachRenderer(surfaceRenderer);
    secondSurface->DetachRenderer(secondSurfaceRenderer);
    failureCount += GetCaseResult(
        surfaceRenderer->GetViewProps()->GetNumberOfItems() == 0,
        "Surface overlay detaches its aggregate prop") ? 0 : 1;

    const auto cancelledSurface =
        PartSurfaceProductBuilder::BuildProduct(
            surfaceRequest, [] { return true; }, {});
    failureCount += GetCaseResult(
        cancelledSurface.failureReason == PartFailureReason::Cancelled
            && !cancelledSurface.product,
        "Cancelled surface extraction publishes no partial product")
        ? 0 : 1;

    constexpr std::array<Orientation, 3> orientations{
        Orientation::Top_down,
        Orientation::Front_back,
        Orientation::Left_right
    };
    for (const auto orientation : orientations) {
        vtkNew<vtkRenderer> renderer;
        auto slice = std::make_shared<PartSliceOverlayStrategy>(orientation);
        slice->SetInputData(labels);
        FeatureOverlayState state;
        state.cursor = { 1.0, 2.0, 3.0 };
        slice->SetOverlayState(state);
        slice->AttachRenderer(renderer);
        slice->AttachRenderer(renderer);
        failureCount += GetCaseResult(
            renderer->GetViewProps()->GetNumberOfItems() == 1,
            "Slice overlay keeps one prop per View") ? 0 : 1;
        slice->DetachRenderer(renderer);
        failureCount += GetCaseResult(
            renderer->GetViewProps()->GetNumberOfItems() == 0,
            "Slice overlay detaches without residue") ? 0 : 1;
    }

    auto rotatedLabels = BuildLabelImage();
    auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
    direction->DeepCopy(std::array<double, 9>{
        0.0, 0.0, 1.0,
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0
    }.data());
    rotatedLabels->SetDirectionMatrix(direction);
    vtkNew<vtkRenderer> rotatedRenderer;
    auto rotatedSlice = std::make_shared<PartSliceOverlayStrategy>(
        Orientation::Top_down);
    rotatedSlice->SetInputData(rotatedLabels);
    rotatedSlice->AttachRenderer(rotatedRenderer);
    auto* props = rotatedRenderer->GetViewProps();
    props->InitTraversal();
    auto* sliceProp = vtkImageSlice::SafeDownCast(props->GetNextProp());
    auto* mapper = sliceProp
        ? vtkImageResliceMapper::SafeDownCast(sliceProp->GetMapper())
        : nullptr;
    double normal[3]{};
    if (mapper && mapper->GetSlicePlane()) {
        mapper->GetSlicePlane()->GetNormal(normal);
    }
    failureCount += GetCaseResult(
        mapper
            && std::abs(normal[0] - 1.0) < 1e-12
            && std::abs(normal[1]) < 1e-12
            && std::abs(normal[2]) < 1e-12,
        "Slice overlay follows the image direction axis") ? 0 : 1;
    rotatedSlice->DetachRenderer(rotatedRenderer);

    static_assert(std::is_same_v<std::uint32_t, unsigned int>);
    auto candidateOwner =
        std::make_shared<std::vector<std::uint32_t>>(64, 1U);
    const std::weak_ptr<std::vector<std::uint32_t>> weakOwner =
        candidateOwner;
    auto borrowedImage = vtkSmartPointer<vtkImageData>::New();
    borrowedImage->SetDimensions(4, 4, 4);
    auto borrowedScalars =
        vtkSmartPointer<vtkUnsignedIntArray>::New();
    borrowedScalars->SetNumberOfComponents(1);
    borrowedScalars->SetArray(candidateOwner->data(), 64, 1);
    borrowedImage->GetPointData()->SetScalars(borrowedScalars);
    auto activeOwner = candidateOwner;
    candidateOwner.reset();
    const bool keptAfterCandidate =
        !weakOwner.expired()
        && borrowedImage->GetScalarPointer() == activeOwner->data();
    borrowedImage = nullptr;
    borrowedScalars = nullptr;
    activeOwner.reset();
    failureCount += GetCaseResult(
        keptAfterCandidate && weakOwner.expired(),
        "Borrowed VTK labels remain owned until the active image retires")
        ? 0 : 1;
    return failureCount;
}
