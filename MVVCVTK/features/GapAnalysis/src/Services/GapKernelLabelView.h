#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class vtkDataArray;

// 仅服务私有 kernel bridge：标准布局 VTK_INT 借用供应商 scalar，其他类型持有转换结果。
class GapKernelLabelView final {
public:
    const std::int32_t* GetData() const noexcept;
    std::size_t GetCount() const noexcept;
    bool GetIsBorrowed() const noexcept;

private:
    friend bool BuildGapKernelLabelView(
        vtkDataArray* scalars,
        std::size_t labelCount,
        GapKernelLabelView& out);

    const std::int32_t* m_borrowed = nullptr;
    std::vector<std::int32_t> m_converted;
    std::size_t m_count = 0;
};

bool BuildGapKernelLabelView(
    vtkDataArray* scalars,
    std::size_t labelCount,
    GapKernelLabelView& out);
