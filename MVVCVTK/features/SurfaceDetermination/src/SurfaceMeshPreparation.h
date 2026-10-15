#pragma once
#include "SurfaceDeterminationAlgorithm.h"
#include "FeatureSupport/SpatialMeshSource.h"

// 由现有计算 worker 准备发布载荷；owner 只校验修订并提交已完成候选。
struct SurfacePreparedMeshes final {
    std::shared_ptr<const SurfaceMeshPayload> measurement;
    std::shared_ptr<const SurfaceMeshPayload> selection;
    std::shared_ptr<const FeatureInternal::SpatialMeshSource> display;
    std::string displayError;
};
SurfacePreparedMeshes PrepareSurfaceMeshes(const SurfaceAlgorithmResult& result,
    const SurfaceDeterminationStartParams& params,const std::string& frame,
    bool hasSelection,WorkLimit workingLimit,const SurfaceCancelCheck& cancelled);
std::shared_ptr<const FeatureInternal::SpatialMeshSource> PrepareSurfaceDisplay(
    const SurfaceAlgorithmResult& result,const SurfacePreparedMeshes& meshes,
    const SurfaceAlgorithmInputs& inputs,const std::string& frame,
    WorkLimit workingLimit,const SurfaceCancelCheck& cancelled);
