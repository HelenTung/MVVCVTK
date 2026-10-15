#include "SurfaceRead.h"
#include "JsonInput.h"
#include "MappedArrayStorage.h"
#include <QSaveFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <cmath>
#include <stdexcept>
namespace Manual {
namespace {
void Check(const ReportCheckpoint& checkpoint){if(checkpoint)checkpoint();}
void Finish(QSaveFile& file,QTextStream& text,const ReportCheckpoint& checkpoint,const SurfaceFileCommit& commit) {
    text.flush();
    if(text.status()!=QTextStream::Ok || file.error()!=QFile::NoError)throw std::runtime_error("表面导出写入失败");
    Check(checkpoint); // 取消只能撤销未提交的临时文件，原目标保持完整。
    const auto publish=[&]{if(!file.commit())throw std::runtime_error("表面导出提交失败");};
    if(commit)commit(publish);else publish();
}
}
std::shared_ptr<const SurfaceMeasurementSelection> ReadSurfaceMeasurement(
    const SurfaceGenerationSnapshot& snapshot,SurfaceMeasurementCriteria criteria,const ReportCheckpoint& checkpoint)
{
    if(!snapshot.points||!snapshot.triangleIndices||!snapshot.triangleValidity)
        throw std::invalid_argument("当前修订没有完整测量几何");
    if(!std::isfinite(criteria.support)||!std::isfinite(criteria.sigma)||!std::isfinite(criteria.residual)
        ||criteria.support<0||criteria.support>1||criteria.sigma<0||criteria.residual<0)
        throw std::invalid_argument("测量质量范围无效");
    const auto& points=*snapshot.points;const auto& triangles=*snapshot.triangleIndices;
    if(triangles.size()%3 || snapshot.triangleValidity->size()!=triangles.size()/3)
        throw std::invalid_argument("三角形质量信息与拓扑不一致");
    ResultArrayScope storage;auto result=std::make_shared<SurfaceMeasurementSelection>();
    result->generation=snapshot.dataRevision;result->source=snapshot.sourceRevision;result->mesh=snapshot.meshRevision;
    result->totalVertices=points.size();result->totalTriangles=triangles.size()/3;
    DataArray<std::uint8_t> used(points.size(),0);
    const auto valid=[&](std::uint32_t id){const auto& point=points[id];return static_cast<int>(point.flags)==0
        &&point.validSupportRatio>=criteria.support&&point.estimatedLocalizationSigma<=criteria.sigma&&point.fitResidual<=criteria.residual;};
    for(std::size_t i=0;i<result->totalTriangles;++i) {
        if(i%1024==0)Check(checkpoint);
        const auto a=triangles[3*i],b=triangles[3*i+1],c=triangles[3*i+2];
        if(a>=points.size()||b>=points.size()||c>=points.size())throw std::invalid_argument("测量网格索引越界");
        const auto& x=points[a].positionModel;const auto& y=points[b].positionModel;const auto& z=points[c].positionModel;
        const std::array<double,3> u{y[0]-x[0],y[1]-x[1],y[2]-x[2]},v{z[0]-x[0],z[1]-x[1],z[2]-x[2]};
        const std::array<double,3> cross{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
        const auto areaSquared=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2];
        if(!std::isfinite(areaSquared)||areaSquared<=0){++result->degenerate;continue;}
        if(!valid(a)||!valid(b)||!valid(c)||!(*snapshot.triangleValidity)[i]){++result->rejectedQuality;continue;}
        result->triangles.push_back(i);used[a]=used[b]=used[c]=1;
    }
    for(std::size_t i=0;i<used.size();++i){if(i%4096==0)Check(checkpoint);if(used[i])result->vertices.push_back(i);}
    Check(checkpoint);storage.Finish();Check(checkpoint);return result;
}
void ExportSurfaceComparison(const QString& path,const SurfaceGenerationSnapshot& snapshot,const ReportCheckpoint& checkpoint,const SurfaceFileCommit& commit)
{
    if(!snapshot.points)throw std::invalid_argument("表面点不可用");
    Check(checkpoint);QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly|QIODevice::Text))throw std::runtime_error("无法打开表面导出文件");
    QTextStream out(&file);out.setLocale(QLocale::c());out.setRealNumberPrecision(17);
    out<<"object_id,point_id";
    if(GetDataRevisionRefValid(snapshot.dataRevision))out<<" [mvvcvtk-result="<<GetRefText(snapshot.dataRevision)<<']';
    out<<",x_mm,y_mm,z_mm,nx,ny,nz,flags,support_ratio,fit_residual,localization_sigma_mm\n";
    std::uint64_t i=0;
    for(const auto& v:*snapshot.points) {
        if(i%1024==0)Check(checkpoint);
        out<<qulonglong(v.objectIndex)<<','<<qulonglong(i++);
        for(auto x:v.positionModel)out<<','<<x;for(auto x:v.normalModel)out<<','<<x;
        out<<','<<int(v.flags)<<','<<v.validSupportRatio<<','<<v.fitResidual<<','<<v.estimatedLocalizationSigma<<'\n';
    }
    Finish(file,out,checkpoint,commit);
}
std::uint64_t ExportSurfaceFixture(const QString& path,const SurfaceGenerationSnapshot& snapshot,
    const std::array<double,16>& transform,bool selectionOnly,const ReportCheckpoint& checkpoint,const SurfaceFileCommit& commit)
{
    if(!snapshot.points||!snapshot.triangleIndices)throw std::invalid_argument("正式测量表面不可用");
    const auto& triangles=*snapshot.triangleIndices;
    if(triangles.size()%3)throw std::runtime_error("正式表面三角形索引不完整");
    Check(checkpoint);
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))throw std::runtime_error("无法创建参考网格目录");
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Text))throw std::runtime_error("无法保存参考网格");
    QTextStream text(&file);text.setLocale(QLocale::c());text.setRealNumberPrecision(17);
    // 只供已知刚体变换回归，不作为外部名义 CAD 或计量精度参考。
    text<<"solid measured_surface_regression\n";std::uint64_t triangleCount=0;
    const auto facet=[&](const std::array<std::array<double,3>,3>& points){
        if(triangleCount%1024==0)Check(checkpoint);
        text<<"facet normal 0 0 0\nouter loop\n";
        for(const auto& point:points){text<<"vertex";for(int row=0;row<3;++row)
            text<<' '<<transform[row*4]*point[0]+transform[row*4+1]*point[1]+transform[row*4+2]*point[2]+transform[row*4+3];text<<'\n';}
        text<<"endloop\nendfacet\n";++triangleCount;
    };
    if(selectionOnly&&snapshot.selection){for(const auto& piece:*snapshot.selection)facet(piece.points);}
    else for(std::size_t face=0;face<triangles.size();face+=3){
        std::array<std::array<double,3>,3> points;
        for(std::size_t corner=0;corner<3;++corner){const auto index=triangles[face+corner];
            if(index>=snapshot.points->size())throw std::runtime_error("正式表面三角形索引越界");points[corner]=(*snapshot.points)[index].positionModel;}
        facet(points);
    }
    if(!triangleCount)throw std::runtime_error("参考表面选区为空");
    text<<"endsolid measured_surface_regression\n";Finish(file,text,checkpoint,commit);return triangleCount;
}
}
