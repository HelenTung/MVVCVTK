#pragma once

#include "Render/Internal/RenderResourceCoordinator.h"

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

struct IsoSurfaceKey final {
    RenderInputStamp inputStamp;
    const void* maskIdentity = nullptr;
    vtkMTimeType inputMTime = 0;
    vtkMTimeType inputScalarMTime = 0;
    vtkMTimeType maskMTime = 0;
    vtkMTimeType maskScalarMTime = 0;
    std::array<int, 3> outputDimensions{};
    double isoValue = 0.0;
};

struct IsoSurfaceProduct final {
    std::uint64_t requestRevision = 0;
    RenderInputStamp inputStamp;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    std::array<int, 3> inputDimensions{};
    double isoValue = 0.0;
    vtkSmartPointer<vtkPolyData> surface;
    std::uint64_t actualBytes = 0;
    bool isPreview = false;
};

struct IsoSurfaceBuildRequest final {
    IsoSurfaceKey key;
    std::uint64_t requestRevision = 0;
    VolumeQuality requestedQuality = VolumeQuality::Auto;
    vtkSmartPointer<vtkImageData> input;
    vtkSmartPointer<vtkImageData> mask;
    bool isPreview = false;
};

struct IsoSurfaceBuildResult final {
    RenderProductFailure failureReason = RenderProductFailure::None;
    std::shared_ptr<const IsoSurfaceProduct> product;
    std::string message;
};

class IsoSurfaceProductBuilder final {
public:
    IsoSurfaceBuildResult BuildProduct(
        const IsoSurfaceBuildRequest& request,
        const RenderTaskToken& stopToken) const;
};
