#include "Host/MetrologyReport.h"
#include <type_traits>
static_assert(std::is_same_v<decltype(MetrologyReportRequest::voxelVolumes),std::vector<DataRevisionRef>>);
static_assert(std::is_same_v<decltype(MetrologyReport::voxelVolumes),std::vector<VoxelVolumeSnapshot>>);
