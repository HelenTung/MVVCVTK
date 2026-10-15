#pragma once
#include "Host/SurfaceDeterminationHostTypes.h"
#include "ReportDocument.h"
#include <QString>
namespace Manual {
struct SurfaceMeasurementCriteria final { double support=.5, sigma=.5, residual=1.; };
// 只保存当前一次筛选的完整原始编号，读取页不会重扫正式几何。
struct SurfaceMeasurementSelection final {
    DataRevisionRef generation,source,mesh;
    DataArray<std::uint64_t> vertices,triangles;
    std::uint64_t totalVertices=0,totalTriangles=0,degenerate=0,rejectedQuality=0;
};
std::shared_ptr<const SurfaceMeasurementSelection> ReadSurfaceMeasurement(
    const SurfaceGenerationSnapshot&,SurfaceMeasurementCriteria,const ReportCheckpoint& = {});
using SurfaceFileCommit = std::function<void(const std::function<void()>&)>;
void ExportSurfaceComparison(const QString&,const SurfaceGenerationSnapshot&,const ReportCheckpoint& = {},const SurfaceFileCommit& = {});
std::uint64_t ExportSurfaceFixture(const QString&,const SurfaceGenerationSnapshot&,
    const std::array<double,16>&,bool selectionOnly,const ReportCheckpoint& = {},const SurfaceFileCommit& = {});
}
