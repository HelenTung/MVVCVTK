#include "GapKernelLabelViewTests.h"

#include "GapKernelLabelView.h"

#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkIntArray.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkShortArray.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

class NonStandardIntArray final : public vtkIntArray {
public:
    static NonStandardIntArray* New();
    vtkTypeMacro(NonStandardIntArray, vtkIntArray);

    bool HasStandardMemoryLayout() const override
    {
        return false;
    }

protected:
    NonStandardIntArray() = default;
    ~NonStandardIntArray() override = default;
};

vtkStandardNewMacro(NonStandardIntArray);

class GapKernelLabelViewSuite final {
public:
    void SetExpect(
        const bool isExpected,
        const std::string& message,
        int& failureCount) const
    {
        if (!isExpected) {
            std::cerr << message << '\n';
            ++failureCount;
        }
    }

    void SetBorrowExpect(int& failureCount) const
    {
        static_assert(std::is_same_v<int, std::int32_t>);
        vtkNew<vtkIntArray> labels;
        labels->SetNumberOfComponents(1);
        labels->SetNumberOfTuples(4);
        auto* values = labels->GetPointer(0);
        const std::array<int, 4> expected{
            0,
            1,
            7,
            (std::numeric_limits<int>::max)()
        };
        std::copy(expected.begin(), expected.end(), values);
        const auto* source = static_cast<const std::int32_t*>(
            labels->GetVoidPointer(0));

        GapKernelLabelView view;
        SetExpect(
            BuildGapKernelLabelView(labels, expected.size(), view)
                && view.GetIsBorrowed()
                && view.GetData() == source
                && view.GetCount() == expected.size()
                && std::equal(
                    expected.begin(),
                    expected.end(),
                    view.GetData()),
            "VTK_INT labels should be borrowed without a conversion buffer.",
            failureCount);

        values[1] = -1;
        SetExpect(
            !BuildGapKernelLabelView(labels, expected.size(), view)
                && !view.GetIsBorrowed()
                && view.GetData() == nullptr
                && view.GetCount() == 0,
            "Borrowed VTK_INT labels should reject negative values and clear the view.",
            failureCount);
    }

    void SetConvertExpect(int& failureCount) const
    {
        vtkNew<vtkShortArray> labels;
        labels->SetNumberOfComponents(1);
        labels->SetNumberOfTuples(3);
        auto* values = labels->GetPointer(0);
        values[0] = 0;
        values[1] = 2;
        values[2] = 9;

        GapKernelLabelView view;
        const bool isBuilt = BuildGapKernelLabelView(labels, 3, view);
        const auto* converted = view.GetData();
        SetExpect(
            isBuilt
                && !view.GetIsBorrowed()
                && converted
                && view.GetCount() == 3
                && converted[0] == 0
                && converted[1] == 2
                && converted[2] == 9,
            "Non-VTK_INT labels should retain the checked conversion fallback.",
            failureCount);

        values[1] = 5;
        SetExpect(
            converted && converted[1] == 2,
            "Converted labels should own storage independent of the source array.",
            failureCount);

        vtkNew<NonStandardIntArray> nonStandard;
        nonStandard->SetNumberOfComponents(1);
        nonStandard->SetNumberOfTuples(2);
        nonStandard->SetValue(0, 3);
        nonStandard->SetValue(1, 4);
        GapKernelLabelView nonStandardView;
        SetExpect(
            BuildGapKernelLabelView(nonStandard, 2, nonStandardView)
                && !nonStandardView.GetIsBorrowed()
                && nonStandardView.GetData()
                && nonStandardView.GetData()[0] == 3
                && nonStandardView.GetData()[1] == 4,
            "Non-standard VTK_INT storage should use the owned conversion fallback.",
            failureCount);
    }

    bool GetFloatRejected(const float value) const
    {
        vtkNew<vtkFloatArray> labels;
        labels->SetNumberOfComponents(1);
        labels->SetNumberOfTuples(2);
        auto* values = labels->GetPointer(0);
        values[0] = 0.0f;
        values[1] = value;
        GapKernelLabelView view;
        return !BuildGapKernelLabelView(labels, 2, view)
            && view.GetData() == nullptr
            && view.GetCount() == 0;
    }

    void SetInvalidExpect(int& failureCount) const
    {
        SetExpect(
            GetFloatRejected(-1.0f)
                && GetFloatRejected(1.5f)
                && GetFloatRejected(
                    (std::numeric_limits<float>::quiet_NaN)())
                && GetFloatRejected(
                    (std::numeric_limits<float>::infinity)()),
            "Converted float labels should reject negative, fractional and non-finite values.",
            failureCount);

        vtkNew<vtkDoubleArray> overflow;
        overflow->SetNumberOfComponents(1);
        overflow->SetNumberOfTuples(1);
        overflow->SetValue(
            0,
            static_cast<double>(
                (std::numeric_limits<std::int32_t>::max)()) + 1.0);
        GapKernelLabelView overflowView;
        SetExpect(
            !BuildGapKernelLabelView(overflow, 1, overflowView),
            "Converted labels should reject values above the int32 range.",
            failureCount);

        vtkNew<vtkIntArray> components;
        components->SetNumberOfComponents(2);
        components->SetNumberOfTuples(2);
        GapKernelLabelView shapeView;
        SetExpect(
            !BuildGapKernelLabelView(components, 2, shapeView),
            "Label view should reject multi-component arrays.",
            failureCount);

        vtkNew<vtkIntArray> countMismatch;
        countMismatch->SetNumberOfComponents(1);
        countMismatch->SetNumberOfTuples(2);
        SetExpect(
            !BuildGapKernelLabelView(countMismatch, 3, shapeView)
                && !BuildGapKernelLabelView(nullptr, 2, shapeView),
            "Label view should reject tuple-count mismatch and missing arrays.",
            failureCount);
    }

    int GetFailCount() const
    {
        int failureCount = 0;
        SetBorrowExpect(failureCount);
        SetConvertExpect(failureCount);
        SetInvalidExpect(failureCount);
        return failureCount;
    }
};

}

int GetGapLabelFailCount()
{
    return GapKernelLabelViewSuite{}.GetFailCount();
}
