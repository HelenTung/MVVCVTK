#pragma once
#include "Data/DataPayloads.h"

// 只允许内部已冻结 owner 的移交；公开按值构造仍隔离外部数组。
class MeshPayloadBuilder final {
public:
    static std::shared_ptr<const SurfaceMeshPayload> Build(
        std::shared_ptr<const DataArray<double>> vertices,
        std::shared_ptr<const DataArray<std::uint64_t>> triangles,
        std::vector<MeshAttribute> pointData = {}, std::string frame = "RAS",
        std::vector<MeshAttribute> cellData = {},const std::function<bool()>& cancelled={}) {
        if (!vertices || !triangles) return {};
        auto payload = std::make_shared<SurfaceMeshPayload>(DataArray<double>{}, DataArray<std::uint64_t>{});
        payload->m_coordinateFrame = std::move(frame);
        payload->m_mesh = std::make_shared<const SurfaceMeshPayload::MeshData>(
            std::move(vertices), std::move(triangles), std::move(pointData), std::move(cellData));
        payload->m_isValid = payload->GetMeshValid(cancelled);
        return payload->m_isValid ? payload : nullptr;
    }
};
