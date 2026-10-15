#pragma once
#include "Data/RoiTypes.h"
// 测试只改轻量描述，不复制载荷；源读取器始终拥有原始数据。
class RoiArchiveTestReader : public RoiArchiveReader {
public:
    explicit RoiArchiveTestReader(std::shared_ptr<const RoiArchiveReader> reader):metadata(reader->GetMetadata()),source(std::move(reader)){}
    const RoiArchiveDescriptor& GetMetadata()const noexcept override{return metadata;}
    std::optional<std::size_t> GetMaskCount(std::size_t i)const noexcept override{return source->GetMaskCount(i);}
    std::optional<std::size_t> GetVoxelPayloadCount(std::size_t i)const noexcept override{return source->GetVoxelPayloadCount(i);}
    RoiArchiveMaskChunk GetMaskChunk(const RoiArchiveChunkRequest& r)const override{return source->GetMaskChunk(r);}
    RoiArchiveVoxelPayloadChunk GetVoxelPayloadChunk(const RoiArchiveChunkRequest& r)const override{return source->GetVoxelPayloadChunk(r);}
    RoiArchiveDescriptor metadata;
protected:
    std::shared_ptr<const RoiArchiveReader> source;
};
inline std::shared_ptr<const RoiVoxelSet> MakeRoiVoxelFixture(const std::vector<RoiVoxel>& values){
    RoiVoxelBuilder builder;builder.AppendVoxels(values);return builder.Finish();
}
