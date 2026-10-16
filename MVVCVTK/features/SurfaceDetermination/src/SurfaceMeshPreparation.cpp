#include "SurfaceMeshPreparation.h"
#include "SurfaceGeometryStorage.h"
#include <stdexcept>

SurfacePreparedMeshes PrepareSurfaceMeshes(const SurfaceAlgorithmResult& result,
    const SurfaceDeterminationStartParams&,const std::string&,bool,WorkLimit,const SurfaceCancelCheck& cancelled)
{
    if(cancelled&&cancelled())throw std::runtime_error("Surface preparation cancelled.");
    if(!result.prepared)throw std::runtime_error("Surface prepared storage is unavailable.");
    SurfacePreparedMeshes meshes;meshes.measurement=result.prepared->mesh;meshes.selection=result.prepared->selectionMesh;
    return meshes;
}
std::shared_ptr<const FeatureInternal::SpatialMeshSource> PrepareSurfaceDisplay(
    const SurfaceAlgorithmResult& result,const SurfacePreparedMeshes& meshes,
    const SurfaceAlgorithmInputs& inputs,const std::string&,WorkLimit,const SurfaceCancelCheck& cancelled)
{
    if(inputs.parent&&inputs.parentDisplay)return inputs.parentDisplay;
    const auto mesh=meshes.measurement?meshes.measurement:result.prepared?result.prepared->mesh:nullptr;
    if(!mesh)throw std::runtime_error("Surface display geometry is unavailable.");
    return FeatureInternal::SpatialMeshSource::Build(mesh,cancelled);
}
