#pragma once

#include "Data/DataPayloads.h"
#include "Host/TrustedDataPort.h"

#include <memory>

class vtkImageData;
class vtkPolyData;

// VTK 仅作为受信 view/build adapter；DataRevision 与通用 Payload 不含 VTK identity。
class VtkDataBridge final {
public:
    VtkDataBridge();
    ~VtkDataBridge();

    VtkDataBridge(const VtkDataBridge&) = delete;
    VtkDataBridge& operator=(const VtkDataBridge&) = delete;
    VtkDataBridge(VtkDataBridge&&) = delete;
    VtkDataBridge& operator=(VtkDataBridge&&) = delete;

    std::shared_ptr<const ImageGrid3DPayload> CreateImagePayload(
        vtkImageData* image,
        vtkImageData* validityMask = nullptr,
        ImageMetadata metadata = {}) const;
    std::shared_ptr<const LabelMap3DPayload> CreateLabelPayload(
        vtkImageData* labels) const;
    std::shared_ptr<const SurfaceMeshPayload> CreateMeshPayload(
        vtkPolyData* mesh) const;

    VtkImageGridSnapshot GetImageGrid(DataSnapshot data) const;
    VtkLabelMapSnapshot GetLabelMap(DataSnapshot data) const;
    VtkSurfaceMeshSnapshot GetSurfaceMesh(DataSnapshot data) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
