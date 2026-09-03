#include "GapKernelLabelView.h"

#include <vtkDataArray.h>
#include <vtkSetGet.h>
#include <vtkType.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace {

bool GetLabelsValid(
    const std::int32_t* source,
    const std::size_t labelCount)
{
    if (!source || labelCount == 0) {
        return false;
    }
    return std::all_of(
        source,
        source + labelCount,
        [](const std::int32_t value) { return value >= 0; });
}

template <typename TValue>
bool SetConvertedLabels(
    const TValue* source,
    const std::size_t labelCount,
    std::vector<std::int32_t>& labels)
{
    if (!source || labelCount == 0) {
        return false;
    }
    labels.resize(labelCount);
    constexpr long double maxLabel =
        static_cast<long double>(
            (std::numeric_limits<std::int32_t>::max)());
    for (std::size_t index = 0; index < labelCount; ++index) {
        const long double value = static_cast<long double>(source[index]);
        if (!std::isfinite(value)
            || value < 0.0L
            || value > maxLabel
            || std::trunc(value) != value) {
            labels.clear();
            return false;
        }
        labels[index] = static_cast<std::int32_t>(value);
    }
    return true;
}

}

const std::int32_t* GapKernelLabelView::GetData() const noexcept
{
    if (m_borrowed) {
        return m_borrowed;
    }
    return m_converted.empty() ? nullptr : m_converted.data();
}

std::size_t GapKernelLabelView::GetCount() const noexcept
{
    return m_count;
}

bool GapKernelLabelView::GetIsBorrowed() const noexcept
{
    return m_borrowed != nullptr;
}

bool BuildGapKernelLabelView(
    vtkDataArray* scalars,
    const std::size_t labelCount,
    GapKernelLabelView& out)
{
    out = {};
    if (!scalars
        || labelCount == 0
        || labelCount > static_cast<std::size_t>(
            (std::numeric_limits<vtkIdType>::max)())
        || scalars->GetNumberOfComponents() != 1
        || scalars->GetNumberOfTuples()
            != static_cast<vtkIdType>(labelCount)) {
        return false;
    }

    static_assert(std::is_same_v<int, std::int32_t>);
    if (scalars->GetDataType() == VTK_INT
        && scalars->HasStandardMemoryLayout()) {
        const auto* source = static_cast<const std::int32_t*>(
            scalars->GetVoidPointer(0));
        if (!GetLabelsValid(source, labelCount)) {
            return false;
        }
        out.m_borrowed = source;
        out.m_count = labelCount;
        return true;
    }

    bool isBuilt = false;
    switch (scalars->GetDataType()) {
        vtkTemplateMacro(
            isBuilt = SetConvertedLabels(
                static_cast<const VTK_TT*>(
                    scalars->GetVoidPointer(0)),
                labelCount,
                out.m_converted));
    default:
        break;
    }
    if (!isBuilt) {
        out.m_converted.clear();
        return false;
    }
    out.m_count = labelCount;
    return true;
}
