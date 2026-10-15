#pragma once
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceContracts.h"
#include "PagedArray.h"
#include "MeshVtkStorage.h"

struct SurfacePreparedGeometry final {
    MeshReadArray<SurfacePointRecord> points;
    MeshReadArray<std::uint32_t> triangles;
    MeshReadArray<std::uint8_t> validity;
    MeshReadArray<SurfaceObjectRecord> objects;
    MeshReadArray<SurfaceSelectionPiece> selection;
    std::shared_ptr<const SurfaceMeshPayload> mesh,selectionMesh;
    vtkSmartPointer<vtkPolyData> display;
};

inline std::shared_ptr<const SurfacePreparedGeometry> PrepareSurfaceGeometry(
    SurfaceAlgorithmResult& result,const std::string& frame,bool completeBoundary,const SurfaceCancelCheck& cancelled,
    const std::shared_ptr<const SurfacePreparedGeometry>& parent={})
{
    if(cancelled && cancelled())return {};
    auto stored=std::make_shared<SurfacePreparedGeometry>();
    stored->points=result.points.GetReadSnapshot();stored->triangles=result.triangleIndices.GetReadSnapshot();
    stored->validity=result.triangleValidity.GetReadSnapshot();stored->objects=result.objects.GetReadSnapshot();
    stored->selection=result.selection.GetReadSnapshot();
    if(parent){stored->mesh=parent->mesh;stored->display=parent->display;}
    else {
    const auto points=ConvertMeshArray<double>(stored->points,3,[](const auto& point,std::size_t c){return point.positionModel[c];});
    const auto triangles=ConvertMeshArray<std::uint64_t>(stored->triangles,1,[](auto value,std::size_t){return std::uint64_t(value);});
    const auto method=result.method;
    const auto normalLength=[](const SurfacePointRecord& point){double length=0;for(auto v:point.normalModel)length+=static_cast<double>(v)*v;return std::sqrt(length);};
    const auto quality=[normalLength](const SurfacePointRecord& point){
        const auto length=normalLength(point);
        return std::isfinite(point.fitResidual) && std::isfinite(point.validSupportRatio)
            && std::isfinite(point.estimatedLocalizationSigma) && std::isfinite(length) && length>1e-12
            && point.fitResidual>=0 && point.validSupportRatio>0 && point.validSupportRatio<=1 && point.estimatedLocalizationSigma>=0;
    };
    std::vector<MeshField> fields{
        {"measurement.valid",1,ConvertMeshArray<double>(stored->points,1,[method](const auto& p,std::size_t){return SurfaceContract::GetPointValid(p,method)?1.0:0.0;})},
        {"measurement.fit-residual",1,ConvertMeshArray<double>(stored->points,1,[quality](const auto& p,std::size_t){return quality(p)?double(p.fitResidual):0.0;})},
        {"measurement.support-ratio",1,ConvertMeshArray<double>(stored->points,1,[quality](const auto& p,std::size_t){return quality(p)?double(p.validSupportRatio):0.0;})},
        {"measurement.localization-sigma",1,ConvertMeshArray<double>(stored->points,1,[quality](const auto& p,std::size_t){return quality(p)?double(p.estimatedLocalizationSigma):0.0;})},
        {"measurement.normal",3,ConvertMeshArray<double>(stored->points,3,[quality,normalLength](const auto& p,std::size_t c){return quality(p)?p.normalModel[c]/normalLength(p):0.0;})},
        {"measurement.boundary-complete",1,ConvertMeshArray<double>(stored->points,1,[completeBoundary](const auto&,std::size_t){return completeBoundary?1.0:0.0;})},
        {"measurement.flags",1,ConvertMeshArray<double>(stored->points,1,[](const auto& p,std::size_t){return double(static_cast<std::uint32_t>(p.flags));})},
        {"surface.interface-index",1,ConvertMeshArray<double>(stored->points,1,[](const auto& p,std::size_t){return double(p.interfaceIndex);})},
        {"surface.override-index",1,ConvertMeshArray<double>(stored->points,1,[](const auto& p,std::size_t){return double(p.overrideIndex);})}
    };
    const auto validity=ConvertMeshArray<double>(stored->validity,1,[](auto value,std::size_t){if(value>1)throw std::runtime_error("Invalid surface qualification.");return double(value);});
    stored->mesh=MeshPayloadBuilder::Build(points,triangles,std::move(fields),frame,{{"measurement.valid",1,validity,0}},cancelled);
    if(!stored->mesh || (cancelled && cancelled()))return {};
    stored->display=MeshVtkStorage::Build(points,triangles,{}, {},cancelled);
    if(!stored->display)return {};
    }
    if(result.resolvedParams.analysisRoi){
        const auto positions=ConvertMeshArray<double>(stored->selection,9,[](const auto& p,std::size_t c){return p.points[c/3][c%3];});
        const auto barycentric=ConvertMeshArray<double>(stored->selection,9,[](const auto& p,std::size_t c){return p.barycentric[c/3][c%3];});
        const auto parents=ConvertMeshArray<double>(stored->selection,1,[](const auto& p,std::size_t){return double(p.parentTriangle);});
        PagedArray<std::uint64_t> selectedTriangles;
        if(stored->selection.size()>selectedTriangles.max_size()/3)throw std::length_error("Selection triangle count overflows.");
        selectedTriangles.reserve(stored->selection.size()*3);
        for(std::size_t i=0;i<stored->selection.size()*3;++i){if((i&1023U)==0 && cancelled && cancelled())return {};selectedTriangles.push_back(i);}
        stored->selectionMesh=MeshPayloadBuilder::Build(positions,selectedTriangles.Finish(),{{"selection.barycentric",3,barycentric,0}},frame,{{"selection.parent-triangle",1,parents,0}},cancelled);
        if(!stored->selectionMesh)return {};
    }
    return stored;
}
