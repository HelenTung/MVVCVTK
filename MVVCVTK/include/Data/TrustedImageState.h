#pragma once

#include "Data/ImageReadTypes.h"

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <memory>

// 只允许固定工具链内的 DataManager/可信 Feature 共享；VTK 内部可变性不属于普通读取契约。
struct TrustedImageState final {
    vtkSmartPointer<vtkImageData> image;
    vtkSmartPointer<vtkImageData> validityMask;
    std::array<int, 3> dims = { 0, 0, 0 };
    std::array<double, 3> spacing = { 1.0, 1.0, 1.0 };
    std::array<double, 3> origin = { 0.0, 0.0, 0.0 };
    std::array<double, 9> direction = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
    std::array<double, 2> scalarRange = { 0.0, 0.0 };
    ImageMetadata metadata;
    DataVersion version = 0;
};

using TrustedImageSnapshot =
    std::shared_ptr<const TrustedImageState>;
