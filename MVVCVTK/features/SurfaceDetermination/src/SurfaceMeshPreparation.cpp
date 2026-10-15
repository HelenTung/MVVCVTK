#include "SurfaceMeshPreparation.h"
#include "SurfaceContracts.h"
#include "MappedArrayStorage.h"
#include "MeshPayloadBuilder.h"
#include <cmath>
#include <stdexcept>

SurfacePreparedMeshes PrepareSurfaceMeshes(const SurfaceAlgorithmResult& result,
    const SurfaceDeterminationStartParams& params,const std::string& frame,
    bool hasSelection,WorkLimit workingLimit,const SurfaceCancelCheck& cancelled)
{
    SurfacePreparedMeshes prepared;
    if(SurfaceContract::GetPurpose(params)!=SurfaceTaskPurpose::Determine)return prepared;
    if(frame.empty())throw std::invalid_argument("Surface coordinate frame is unavailable.");
    ResultArrayScope storage(workingLimit.GetBound(8*1024*1024));
    const auto check=[&]{if(cancelled&&cancelled())throw std::runtime_error("Surface mesh preparation cancelled.");};
    check();
    if(!result.sharedParent) {
        if(result.points.empty()||result.triangleIndices.empty()||result.triangleIndices.size()%3
            ||result.triangleValidity.size()!=result.triangleIndices.size()/3)
            throw std::invalid_argument("Surface result geometry is incomplete.");
        if(result.points.size()>SIZE_MAX/3)throw std::length_error("Surface coordinate count overflows.");
        DataArray<double> vertices;vertices.reserve(result.points.size()*3);
        std::vector<MeshAttribute> attributes{
            {"measurement.valid",1,{}},{"measurement.fit-residual",1,{}},
            {"measurement.support-ratio",1,{}},{"measurement.localization-sigma",1,{}},
            {"measurement.normal",3,{}},{"measurement.boundary-complete",1,{}},
            {"measurement.flags",1,{}},{"surface.interface-index",1,{}},{"surface.override-index",1,{}}};
        for(auto& attribute:attributes)attribute.values.reserve(result.points.size()*attribute.componentCount);
        const bool complete=params.componentSelection==SurfaceComponentSelection::All
            &&!params.analysisRoi&&params.minimumObjectVoxels<=1;
        std::size_t index=0;
        for(const auto& point:result.points) {
            if(!(index++%4096))check();
            vertices.insert(vertices.end(),point.positionModel.begin(),point.positionModel.end());
            double normalLength=0.0;
            for(const auto value:point.normalModel)normalLength+=static_cast<double>(value)*value;
            normalLength=std::sqrt(normalLength);
            const bool hasQuality=std::isfinite(point.fitResidual)&&std::isfinite(point.validSupportRatio)
                &&std::isfinite(point.estimatedLocalizationSigma)&&std::isfinite(normalLength)&&normalLength>1e-12
                &&point.fitResidual>=0.0F&&point.validSupportRatio>0.0F&&point.validSupportRatio<=1.0F
                &&point.estimatedLocalizationSigma>=0.0F;
            const bool valid=SurfaceContract::GetPointValid(point,result.method);
            attributes[0].values.push_back(valid?1.0:0.0);
            attributes[1].values.push_back(hasQuality?point.fitResidual:0.0);
            attributes[2].values.push_back(hasQuality?point.validSupportRatio:0.0);
            attributes[3].values.push_back(hasQuality?point.estimatedLocalizationSigma:0.0);
            for(const auto value:point.normalModel)attributes[4].values.push_back(hasQuality?value/normalLength:0.0);
            attributes[5].values.push_back(complete?1.0:0.0);
            attributes[6].values.push_back(static_cast<std::uint32_t>(point.flags));
            attributes[7].values.push_back(point.interfaceIndex);attributes[8].values.push_back(point.overrideIndex);
        }
        DataArray<std::uint64_t> triangles;triangles.reserve(result.triangleIndices.size());
        for(std::size_t i=0;i<result.triangleIndices.size();++i){if(!(i%4096))check();triangles.push_back(result.triangleIndices[i]);}
        std::vector<MeshAttribute> cellAttributes{{"measurement.valid",1,{}}};
        auto& validity=cellAttributes[0].values;validity.reserve(result.triangleValidity.size());
        index=0;
        for(const auto value:result.triangleValidity){if(!(index++%4096))check();if(value>1)throw std::invalid_argument("Invalid surface triangle validity.");validity.push_back(static_cast<double>(value));}
        prepared.measurement=MeshPayloadBuilder::Build(std::make_shared<const DataArray<double>>(std::move(vertices)),
            std::make_shared<const DataArray<std::uint64_t>>(std::move(triangles)),std::move(attributes),frame,std::move(cellAttributes),cancelled);
        if(!prepared.measurement)throw std::invalid_argument("Invalid or cancelled prepared measurement mesh.");
    }
    if(hasSelection) {
        DataArray<double> vertices,barycentric,parents;DataArray<std::uint64_t> triangles;
        std::size_t index=0;
        for(const auto& piece:result.selection) {
            if(!(index++%4096))check();
            for(int p=0;p<3;++p){triangles.push_back(vertices.size()/3);vertices.insert(vertices.end(),piece.points[p].begin(),piece.points[p].end());barycentric.insert(barycentric.end(),piece.barycentric[p].begin(),piece.barycentric[p].end());}
            parents.push_back(static_cast<double>(piece.parentTriangle));
        }
        std::vector<MeshAttribute> pointData,cellData;
        pointData.push_back({"selection.barycentric",3,std::move(barycentric)});
        cellData.push_back({"selection.parent-triangle",1,std::move(parents)});
        prepared.selection=MeshPayloadBuilder::Build(std::make_shared<const DataArray<double>>(std::move(vertices)),
            std::make_shared<const DataArray<std::uint64_t>>(std::move(triangles)),std::move(pointData),frame,std::move(cellData),cancelled);
        if(!prepared.selection)throw std::invalid_argument("Invalid or cancelled prepared selection mesh.");
    }
    check();storage.Finish();return prepared;
}

std::shared_ptr<const FeatureInternal::SpatialMeshSource> PrepareSurfaceDisplay(
    const SurfaceAlgorithmResult& result,const SurfacePreparedMeshes& meshes,
    const SurfaceAlgorithmInputs& inputs,const std::string& frame,
    WorkLimit workingLimit,const SurfaceCancelCheck& cancelled)
{
    if(result.sharedParent&&inputs.parentDisplay)return inputs.parentDisplay;
    auto mesh=result.sharedParent?inputs.parentMesh:meshes.measurement;
    if(!mesh) {
        // 预览未发布测量网格，只准备坐标和拓扑，不展开诊断属性。
        ResultArrayScope storage(workingLimit.GetBound(8*1024*1024));
        DataArray<double> vertices;DataArray<std::uint64_t> faces;
        const auto& points=result.sharedParent?*result.sharedParent->points:result.points;
        const auto& indices=result.sharedParent?*result.sharedParent->triangleIndices:result.triangleIndices;
        if(points.size()>SIZE_MAX/3)throw std::length_error("Surface display coordinates overflow.");
        vertices.reserve(points.size()*3);faces.reserve(indices.size());
        for(std::size_t i=0;i<points.size();++i){if(!(i%4096)&&cancelled&&cancelled())throw std::runtime_error("Surface display cancelled.");vertices.insert(vertices.end(),points[i].positionModel.begin(),points[i].positionModel.end());}
        for(std::size_t i=0;i<indices.size();++i){if(!(i%4096)&&cancelled&&cancelled())throw std::runtime_error("Surface display cancelled.");faces.push_back(indices[i]);}
        mesh=MeshPayloadBuilder::Build(std::make_shared<const DataArray<double>>(std::move(vertices)),
            std::make_shared<const DataArray<std::uint64_t>>(std::move(faces)),{},frame,{},cancelled);
        if(!mesh)throw std::runtime_error("Surface display geometry is unavailable.");
        storage.Finish();
    }
    return FeatureInternal::SpatialMeshSource::Build(std::move(mesh),cancelled);
}
