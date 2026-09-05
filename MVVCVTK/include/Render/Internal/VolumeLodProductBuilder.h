#pragma once

#include "Render/Internal/RenderResourceCoordinator.h"

#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

struct VolumeLodKey final {
    RenderInputStamp inputStamp;
    const void* inputIdentity = nullptr;
    const void* maskIdentity = nullptr;
    vtkMTimeType inputMTime = 0;
    vtkMTimeType inputScalarMTime = 0;
    vtkMTimeType maskMTime = 0;
    vtkMTimeType maskScalarMTime = 0;
    std::array<int, 3> outputDimensions{};
    double denoiseThreshold = 0.0;
    bool isDenoiseOn = false;
};

struct VolumeLodProduct final {
    std::uint64_t requestRevision = 0;
    RenderInputStamp inputStamp;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    std::array<int, 3> outputDimensions{};
    vtkSmartPointer<vtkImageData> volume;
    vtkSmartPointer<vtkImageData> mask;
    std::uint64_t actualBytes = 0;
};

struct VolumeLodBuildRequest final {
    VolumeLodKey key;
    std::uint64_t requestRevision = 0;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    vtkSmartPointer<vtkImageData> input;
    vtkSmartPointer<vtkImageData> mask;
};

struct VolumeLodBuildResult final {
    RenderProductFailure failureReason = RenderProductFailure::None;
    std::shared_ptr<const VolumeLodProduct> product;
    std::string message;
};

class VolumeLodProductBuilder final {
public:
    VolumeLodBuildResult BuildProduct(
        const VolumeLodBuildRequest& request,
        const RenderTaskToken& stopToken) const;
};
