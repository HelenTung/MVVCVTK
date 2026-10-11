#pragma once
#include "AlignmentGeometry.h"
#include "Host/VoxelVolume.h"

struct VoxelVolumeWork final {
    std::shared_ptr<const ImageGrid3DPayload> image,validityImage;
    std::shared_ptr<const LabelMap3DPayload> labels;
};
struct VoxelVolumeData final {
    AlignmentStatus status=AlignmentStatus::InvalidInput;
    VoxelVolumeStatistics statistics;
    std::string message;
};
namespace AlignmentVoxelVolume {
bool GetRequestValid(const VoxelVolumeRequest &request,const AlignmentConfig &config);
bool GetGeometrySame(const GridGeometry3D &a,const GridGeometry3D &b);
double GetCellVolumeMm3(const GridGeometry3D &geometry);
VoxelVolumeData Build(const AlignmentWork &work,const VoxelVolumeWork &source,const VoxelVolumeRequest &request);
}
