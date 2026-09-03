#include "PartSegmentationTestCases.h"

#include "Render/Strategies/PartOverlayStrategies.h"

#include <vtkImageData.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkMatrix3x3.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPointData.h>
#include <vtkPropCollection.h>
#include <vtkRenderer.h>
#include <vtkUnsignedIntArray.h>

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

} // namespace

int GetPartDisplayFailCount()
{
    int failureCount = 0;
    const auto labels = BuildLabelImage();

    vtkNew<vtkRenderer> surfaceRenderer;
    auto surface = std::make_shared<PartSurfaceOverlayStrategy>();
    surface->SetInputData(labels);
    surface->AttachRenderer(surfaceRenderer);
    failureCount += GetCaseResult(
        surfaceRenderer->GetViewProps()->GetNumberOfItems() == 1,
        "Surface overlay uses one aggregate prop") ? 0 : 1;
    surface->DetachRenderer(surfaceRenderer);
    failureCount += GetCaseResult(
        surfaceRenderer->GetViewProps()->GetNumberOfItems() == 0,
        "Surface overlay detaches its aggregate prop") ? 0 : 1;

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
