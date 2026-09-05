#include "PartSegmentationTestCases.h"

#include "Model/PartCatalog.h"
#include "Render/PartRenderStateTable.h"
#include "Render/Strategies/PartOverlayStrategies.h"

#include <vtkImageData.h>
#include <vtkImageProperty.h>
#include <vtkImageResliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkLookupTable.h>
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

PartCatalog BuildRenderCatalog(const bool swapLabels = false)
{
    PartCatalog catalog;
    catalog.partSetId = { 1, 2 };
    catalog.resultRevision = 1;
    catalog.catalogRevision = 1;
    catalog.partsByLabel.resize(3);
    const std::array<PartObjectId, 2> ids{ {
        { 10, 11 }, { 20, 21 }
    } };
    for (PartLabelId label = 1; label <= 2; ++label) {
        const std::size_t objectIndex = swapLabels
            ? static_cast<std::size_t>(2U - label)
            : static_cast<std::size_t>(label - 1U);
        auto& entry = catalog.partsByLabel[label];
        entry.labelId = label;
        entry.objectId = ids[objectIndex];
        entry.presentation.color = GetPartStableColor(entry.objectId);
        catalog.labelByObject.emplace(entry.objectId, label);
    }
    return catalog;
}

class PartControlStub final : public PartOverlayControl {
public:
    explicit PartControlStub(const bool isFailing = false)
        : m_isFailing(isFailing)
    {
    }

    bool SetPartStates(
        const PartRenderStateTable& states) noexcept override
    {
        ++m_setCount;
        if (m_isFailing) return false;
        m_states = states;
        return true;
    }

    const PartRenderStateTable& GetStates() const noexcept
    {
        return m_states;
    }

    int GetSetCount() const noexcept { return m_setCount; }

private:
    PartRenderStateTable m_states;
    int m_setCount = 0;
    bool m_isFailing = false;
};

class PartiallyFailingControl final : public PartOverlayControl {
public:
    bool SetPartStates(
        const PartRenderStateTable& states) noexcept override
    {
        ++m_setCount;
        m_states = states;
        if (!m_hasFailed) {
            m_hasFailed = true;
            return false;
        }
        return true;
    }

    const PartRenderStateTable& GetStates() const noexcept
    {
        return m_states;
    }

    int GetSetCount() const noexcept { return m_setCount; }

private:
    PartRenderStateTable m_states;
    int m_setCount = 0;
    bool m_hasFailed = false;
};

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

    auto firstCatalog = BuildRenderCatalog();
    auto reorderedCatalog = BuildRenderCatalog(true);
    firstCatalog.partsByLabel[1].presentation.opacity = 0.5;
    firstCatalog.partsByLabel[2].presentation.isVisible = false;
    const auto firstStates = BuildPartRenderStateTable(firstCatalog);
    const auto reorderedStates = BuildPartRenderStateTable(reorderedCatalog);
    failureCount += GetCaseResult(
        firstStates && reorderedStates
            && firstStates->statesByLabel.size() == 3
            && firstStates->statesByLabel[0].color
                == std::array<double, 4>{}
            && firstStates->statesByLabel[1].color[3]
                == firstCatalog.partsByLabel[1].presentation.color[3] * 0.5
            && firstStates->statesByLabel[2].color[3] == 0.0
            && firstStates->statesByLabel[1].color[0]
                == reorderedStates->statesByLabel[2].color[0]
            && firstStates->statesByLabel[1].color[1]
                == reorderedStates->statesByLabel[2].color[1]
            && firstStates->statesByLabel[1].color[2]
                == reorderedStates->statesByLabel[2].color[2],
        "Render table is dense, transparent, and stable across relabeling")
        ? 0 : 1;

    auto selectedCatalog = BuildRenderCatalog();
    selectedCatalog.partsByLabel[1].presentation.isSelected = true;
    const auto selectedStates = BuildPartRenderStateTable(selectedCatalog);
    failureCount += GetCaseResult(
        selectedStates
            && selectedStates->statesByLabel[1].isSelected
            && selectedStates->statesByLabel[1].color
                != reorderedStates->statesByLabel[2].color
            && selectedStates->statesByLabel[1].color[3]
                == reorderedStates->statesByLabel[2].color[3],
        "Selected state adds a temporary LUT highlight without changing alpha")
        ? 0 : 1;

    auto firstControl = std::make_shared<PartControlStub>();
    auto failingControl = std::make_shared<PartControlStub>(true);
    const PartRenderStateTable previousTable{
        { PartRenderState{ { 0.0, 0.0, 0.0, 0.0 }, false } }
    };
    (void)firstControl->SetPartStates(previousTable);
    const bool didSet = SetPartStates(
        { firstControl, failingControl },
        *selectedStates,
        previousTable);
    failureCount += GetCaseResult(
        !didSet
            && firstControl->GetStates() == previousTable
            && firstControl->GetSetCount() == 3,
        "Multi-overlay state failure restores every applied table") ? 0 : 1;

    auto rollbackControl = std::make_shared<PartControlStub>();
    auto partiallyFailing = std::make_shared<PartiallyFailingControl>();
    (void)rollbackControl->SetPartStates(previousTable);
    const bool didPartiallySet = SetPartStates(
        { rollbackControl, partiallyFailing },
        *selectedStates,
        previousTable);
    failureCount += GetCaseResult(
        !didPartiallySet
            && rollbackControl->GetStates() == previousTable
            && partiallyFailing->GetStates() == previousTable
            && partiallyFailing->GetSetCount() == 2,
        "Rollback also restores the control that reported partial failure")
        ? 0 : 1;

    vtkNew<vtkRenderer> lutRenderer;
    auto lutSlice = std::make_shared<PartSliceOverlayStrategy>(
        Orientation::Top_down);
    lutSlice->SetInputData(labels);
    const bool didSetLut = lutSlice->SetPartStates(*firstStates);
    lutSlice->AttachRenderer(lutRenderer);
    auto* lutProps = lutRenderer->GetViewProps();
    lutProps->InitTraversal();
    auto* lutProp = vtkImageSlice::SafeDownCast(lutProps->GetNextProp());
    auto* lut = lutProp
        ? vtkLookupTable::SafeDownCast(
            lutProp->GetProperty()->GetLookupTable())
        : nullptr;
    double hiddenColor[4]{};
    if (lut) lut->GetTableValue(2, hiddenColor);
    failureCount += GetCaseResult(
        didSetLut && lut && lut->GetNumberOfTableValues() == 3
            && hiddenColor[3] == 0.0,
        "Concrete aggregate overlay consumes the explicit state table")
        ? 0 : 1;
    lutSlice->DetachRenderer(lutRenderer);
    return failureCount;
}
