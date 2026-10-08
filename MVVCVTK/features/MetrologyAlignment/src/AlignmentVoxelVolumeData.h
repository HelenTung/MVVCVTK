#pragma once
#include "AlignmentVoxelVolume.h"
#include "Host/TrustedDataPort.h"

namespace AlignmentVoxelVolumeData {
inline const DataTypeId resultType{"org.mvvcvtk.metrology-alignment.voxel-volume",1};
struct Context final {
    AlignmentWork work;
    VoxelVolumeWork source;
    VoxelVolumeSnapshot snapshot;
    std::vector<DataExpectation> expectations;
};
class Payload final : public IDataPayload {
public:
    Payload(VoxelVolumeSnapshot value,std::vector<DataExpectation> dependencies):snapshot(std::move(value)),expectations(std::move(dependencies)) {}
    DataTypeId GetDataType() const override { return resultType; }
    std::shared_ptr<const IDataPayload> CreateSnapshot() const override { return std::make_shared<const Payload>(*this); }
    std::vector<std::shared_ptr<const void>> GetDataResources() const override { return {snapshot.parameters}; }
    const VoxelVolumeSnapshot snapshot;
    const std::vector<DataExpectation> expectations;
};
bool SetType(TrustedDataPort &data);
std::optional<Context> Prepare(const DataGraphSnapshot &graph,VoxelVolumeRequest request,const AlignmentConfig &config);
DataTransaction BuildTransaction(TrustedDataPort &data,Context &context,const VoxelVolumeData &result,DataRevisionRef &ref);
std::optional<VoxelVolumeSnapshot> GetResult(const DataGraphSnapshot &graph,const DataRevisionRef &ref);
}
