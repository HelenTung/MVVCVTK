// 测试用途：验证类型专门化、跨层边去重，以及动态并行的确定性/停止/异常。
#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceSeedBuilder.h"
#include "SurfaceEdgeTopology.h"
#include "SurfaceGpuFinalizer.h"
#include "SurfaceBackendCost.h"
#include "SurfaceMeasurementSerialization.h"
#include <sstream>
#include "SurfaceDeterminationTestSupport.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <future>
#include <cfenv>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <atomic>

namespace {
using namespace SurfaceTest;

class EnvironmentSwitch final {
public:
    explicit EnvironmentSwitch(const char *name) : m_name(name)
    {
        const auto size = GetEnvironmentVariableA(m_name, nullptr, 0);
        if (size) {
            m_previous.resize(size);
            GetEnvironmentVariableA(m_name, m_previous.data(), size);
            m_previous.resize(size - 1);
            m_present = true;
        }
    }
    ~EnvironmentSwitch()
    {
        SetEnvironmentVariableA(m_name, m_present ? m_previous.c_str() : nullptr);
    }
    void Set(bool enabled) { SetEnvironmentVariableA(m_name, enabled ? "1" : "0"); }
private:
    const char *m_name;
    std::string m_previous;
    bool m_present = false;
};

template <class T>
void TestTypedSeed(Checks &checks, int type)
{
    std::vector<T> values(60);
    std::vector<unsigned char> mask(60, 1);
    for (int z = 0; z < 4; ++z) for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x)
        values[(z * 3 + y) * 5 + x] = static_cast<T>(x);
    SurfaceSeedGrid grid;
    grid.extent = {-2, 2, 5, 7, -4, -1};
    grid.dimensions = {5, 3, 4};
    grid.indexToModel = grid.modelToIndex = {1,0,0,0,1,0,0,0,1};
    grid.values = values.data();
    grid.readScalar = [](const void *data, std::size_t i) { return double(static_cast<const T *>(data)[i]); };
    grid.validity = mask.data();
    const std::array<double, 2> range{1, 3};
    PagedArray<Point3> referencePoints;
    PagedArray<SurfaceSeedTriangle> referenceTriangles;
    const auto verify = [&](std::uint32_t depth, double iso = 2, double expectedX = 0) {
        PagedArray<Point3> points;
        PagedArray<SurfaceSeedTriangle> triangles;
        SurfaceExecutionStats stats;
        const auto status = SurfaceSeedBuilder::BuildMesh(grid, iso, {}, 0, depth, 1024U * 1024U,
                                                          [] { return false; }, points, triangles, stats);
        checks.Get(status == SurfaceSeedStatus::Succeeded && points.size() == 12 && triangles.size() == 12,
                   "typed exact-threshold plane shares 12 vertices across z blocks: " + std::to_string(type));
        checks.Get(std::all_of(points.begin(), points.end(), [expectedX](const auto &p) { return p[0] == expectedX; }),
                   "typed reader preserves analytic crossing");
        if (iso == 2 && !grid.scalarValues) { referencePoints=points; referenceTriangles=triangles; }
        if (iso == 2 && grid.scalarValues)
            checks.Get(points==referencePoints && triangles.size()==referenceTriangles.size() &&
                std::equal(triangles.begin(),triangles.end(),referenceTriangles.begin(),
                           [](const auto &a,const auto &b){return a.vertices==b.vertices;}),
                "typed and callback readers preserve point/triangle order");
    };
    verify(1);
    grid.scalarValues = values.data();
    grid.scalarType = type;
    grid.materialRange = &range;
    verify(1); verify(2); verify(99);
    for (bool hasRange : {false,true}) for (bool hasMask : {false,true}) {
        grid.materialRange=hasRange?&range:nullptr;
        grid.validity=hasMask?mask.data():nullptr;
        verify(1);
    }
    grid.materialRange=&range;grid.validity=mask.data();
    mask[0] = 0;
    if constexpr (std::is_floating_point_v<T>) values[0] = std::numeric_limits<T>::quiet_NaN();
    verify(1);
    // 饱和前 x=-0.625，节点饱和后 x=-0.75；能识别遗漏或错误顺序的饱和。
    std::fill(mask.begin(),mask.end(),1);
    for (int z=0;z<4;++z) for(int y=0;y<3;++y) for(int x=0;x<5;++x)
        values[(z*3+y)*5+x]=static_cast<T>(x<2?0:4);
    verify(1,1.5,-0.75);
    const auto verifyMissing = [&] {
        PagedArray<Point3> points; PagedArray<SurfaceSeedTriangle> triangles; SurfaceExecutionStats stats;
        checks.Get(SurfaceSeedBuilder::BuildMesh(grid,1.5,{},0,1,1024U*1024U,{},points,triangles,stats)
                       ==SurfaceSeedStatus::NoSurface && points.empty() && triangles.empty(),
                   "missing nodes on every crossing cell cannot create a typed surface");
    };
    for(int z=0;z<4;++z) for(int y=0;y<3;++y) mask[(z*3+y)*5+2]=0;
    verifyMissing();
    if constexpr (std::is_floating_point_v<T>) {
        std::fill(mask.begin(),mask.end(),1);
        for(int z=0;z<4;++z) for(int y=0;y<3;++y) values[(z*3+y)*5+2]=std::numeric_limits<T>::quiet_NaN();
        verifyMissing();
        for(int z=0;z<4;++z) for(int y=0;y<3;++y) values[(z*3+y)*5+2]=std::numeric_limits<T>::infinity();
        verifyMissing();
    }
    grid.dimensions[0] = 4;
    PagedArray<Point3> points;
    PagedArray<SurfaceSeedTriangle> triangles;
    SurfaceExecutionStats stats;
    checks.Get(SurfaceSeedBuilder::BuildMesh(grid, 2, {}, 0, 1, 1024U * 1024U, {}, points, triangles, stats)
                   == SurfaceSeedStatus::InvalidInput, "inconsistent extent cannot enter compact key encoding");
}

void TestEdgeAxes(Checks &checks)
{
    std::vector<double> values(64);
    SurfaceSeedGrid grid;
    grid.extent = {0,3,0,3,0,3}; grid.dimensions = {4,4,4};
    grid.indexToModel = grid.modelToIndex = {1,0,0,0,1,0,0,0,1};
    grid.scalarValues = values.data(); grid.scalarType = VTK_DOUBLE;
    for (int axis = 0; axis < 4; ++axis) {
        for (int z = 0; z < 4; ++z) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            const int coordinates[]{x,y,z};
            values[(z*4+y)*4+x] = axis < 3 ? coordinates[axis] : x+y+z;
        }
        PagedArray<Point3> points; PagedArray<SurfaceSeedTriangle> triangles; SurfaceExecutionStats stats;
        const double iso = axis < 3 ? 1.0 : 3.0;
        const auto status = SurfaceSeedBuilder::BuildMesh(grid, iso, {}, 0, 1, 1024U*1024U, {}, points, triangles, stats);
        checks.Get(status == SurfaceSeedStatus::Succeeded && points.size() == (axis < 3 ? 16U : 10U)
                       && triangles.size() == (axis < 3 ? 18U : 9U),
                   "axis edges and threshold-corner keys preserve analytic plane triangulation");
        for (const auto &p : points)
            checks.Get(axis < 3 ? p[axis] == 1 : p[0]+p[1]+p[2] == 3,
                       "all generated vertices lie on the independent analytic plane");
    }
}

auto GetPointFields(const SurfacePointRecord &p)
{
    return std::tie(p.seedPositionModel, p.seedNormalModel, p.positionModel, p.normalModel,
                    p.localThreshold, p.contrast, p.gradientMagnitude, p.fitResidual, p.offsetFromSeed,
                    p.validSupportRatio, p.estimatedLocalizationSigma, p.crossingCount, p.objectIndex,
                    p.interfaceIndex, p.overrideIndex, p.transitionWidthModel, p.pairedSeparationModel, p.flags);
}
auto GetObjectFields(const SurfaceObjectRecord &o)
{
    return std::tie(o.objectIndex, o.interfaceIndex, o.firstPoint, o.pointCount, o.firstTriangle,
                    o.triangleCount, o.boundsModel, o.validAreaRatio, o.isClosed, o.isManifold,
                    o.isTruncated, o.isOrientationValid, o.areaValidity, o.volumeValidity,
                    o.areaModelUnit2, o.volumeModelUnit3);
}
bool GetEqualMeasurements(const SurfaceAlgorithmResult &a, const SurfaceAlgorithmResult &b)
{
    if (std::tie(a.status,a.failureReason,a.sourceRevision,a.parameterFingerprint,a.method,a.initialIsoValue,
                 a.acceptedPointCount,a.lowContrastPointCount,a.rejectedPointCount,a.truncatedPointCount,
                 a.nonManifoldObjectCount,a.triangleIndices,a.triangleValidity) !=
        std::tie(b.status,b.failureReason,b.sourceRevision,b.parameterFingerprint,b.method,b.initialIsoValue,
                 b.acceptedPointCount,b.lowContrastPointCount,b.rejectedPointCount,b.truncatedPointCount,
                 b.nonManifoldObjectCount,b.triangleIndices,b.triangleValidity) ||
        a.points.size()!=b.points.size() || a.objects.size()!=b.objects.size()) return false;
    for (std::size_t i=0;i<a.points.size();++i)
        if (GetPointFields(a.points[i])!=GetPointFields(b.points[i])) return false;
    for (std::size_t i=0;i<a.objects.size();++i)
        if (GetObjectFields(a.objects[i])!=GetObjectFields(b.objects[i])) return false;
    return true;
}

void TestParallelSampling(Checks &checks)
{
    const auto source = BuildSnapshot({8,256,256},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},
                                      VTK_FLOAT, [](const Point3 &p) { return p[0]; });
    auto params = GetParams(); params.initialIsoValue = 3.5; params.minimumObjectVoxels = 0;
    const auto build = [&] { return SurfaceDeterminationAlgorithm::BuildSurface(source, params,
                                           256U*1024U*1024U, [] { return false; }, {}); };
    EnvironmentSwitch setting("MVVCVTK_SURFACE_CPU_PARALLEL");
    EnvironmentSwitch profiling("MVVCVTK_SURFACE_GPU_PROFILE");profiling.Set(false);
    setting.Set(false); const auto serial = build();
    setting.Set(true); const auto parallel = build();
    checks.Get(serial.status == SurfaceResultStatus::Succeeded && serial.points.size()==65536 &&
               serial.triangleIndices.size()/3==130050, "large single-component plane matches analytic mesh counts");
    checks.Get(GetEqualMeasurements(serial,parallel), "all point/object measurement fields are thread independent");
    profiling.Set(true);const auto profiled=build();profiling.Set(false);
    checks.Get(GetEqualMeasurements(parallel,profiled) && profiled.cudaProfilingRequested &&
               profiled.cpuSamplingMs>0 && profiled.cudaPointCount==0 && !profiled.cudaProfile.complete,
               "optional phase clocks preserve all CPU measurement fields without requiring a CUDA device");
    checks.Get(std::all_of(parallel.points.begin(),parallel.points.end(),[](const auto &p) { return p.positionModel[0]==3.5; }),
               "parallel plane has zero analytic position error");
    checks.Get(parallel.cpuThreadCount <= parallel.plannedCpuThreads &&
               (parallel.plannedCpuThreads==1 ||
                (parallel.plannedCpuThreads<=parallel.availablePhysicalCores/2 &&
                 parallel.plannedCpuThreads<=parallel.availableLogicalThreads/2)),
               "dynamic plan leaves physical and logical CPU headroom");
    const int previousRounding = std::fegetround();
    std::fesetround(FE_UPWARD);
    const auto rounded = build();
    std::fesetround(previousRounding);
    checks.Get(rounded.status==SurfaceResultStatus::Succeeded && rounded.cpuThreadCount==1 && rounded.plannedCpuThreads==1,
               "non-default caller floating-point control keeps the original serial path");
    auto first = std::async(std::launch::async,build);
    auto second = std::async(std::launch::async,build);
    checks.Get(GetEqualMeasurements(serial,first.get()) && GetEqualMeasurements(serial,second.get()),
               "concurrent requests preserve independent deterministic results");
    if (parallel.cpuThreadCount > 1) {
        const auto owner = std::this_thread::get_id();
        const auto cancelled = SurfaceDeterminationAlgorithm::BuildSurface(source,params,256U*1024U*1024U,
            [owner] { return std::this_thread::get_id()!=owner; }, {});
        checks.Get(cancelled.status==SurfaceResultStatus::Cancelled && cancelled.points.empty() && cancelled.objects.empty(),
                   "worker cancellation joins all threads and publishes no partial mesh");
        const auto failed = SurfaceDeterminationAlgorithm::BuildSurface(source,params,256U*1024U*1024U,
            [owner] { if(std::this_thread::get_id()!=owner) throw std::runtime_error("worker failure probe"); return false; }, {});
        checks.Get(failed.failureReason==SurfaceFailureReason::InternalError && failed.points.empty(),
                   "worker exceptions are joined and returned through the existing failure boundary");
        checks.Get(GetEqualMeasurements(serial,build()), "failed requests release the shared sampling quota");
    }
    std::cout << "Surface CPU available cores=" << parallel.availablePhysicalCores
              << " logical=" << parallel.availableLogicalThreads << " planned=" << parallel.plannedCpuThreads
              << " actual=" << parallel.cpuThreadCount << '\n';
}


class RoundingSetting final {
public:
    RoundingSetting() : m_previous(std::fegetround()) {}
    ~RoundingSetting() { std::fesetround(m_previous); }
    void Set(int mode) { std::fesetround(mode); }
private:
    int m_previous;
};

void TestPointSupportRules(Checks &checks)
{
    const Point3 target{5,5,5}, missingNode{4,6,6};
    RoundingSetting rounding;
    for (int mode : {FE_TONEAREST, FE_UPWARD}) {
        rounding.Set(mode);
        for (unsigned missing = 0; missing < 3; ++missing) {
            const auto source = BuildSnapshot({8,8,8}, {2,1,1}, {4,-2,7},
                {1,0,0,0,1,0,0,0,1}, VTK_DOUBLE,
                [=](const Point3 &position) {
                    return missing == 2 && position == missingNode
                        ? std::numeric_limits<double>::quiet_NaN() : position[0];
                },
                missing == 1 ? ValidityField([=](const Point3 &position) { return position != missingNode; })
                             : ValidityField{}, 1, {-2,5,-4});
            auto params = GetParams();
            params.initialIsoValue = 5;
            params.minimumObjectVoxels = 0;
            params.componentSelection = SurfaceComponentSelection::All;
            const auto result = SurfaceDeterminationAlgorithm::BuildSurface(source, params,
                256U*1024U*1024U, [] { return false; }, {});
            const auto point = std::find_if(result.points.begin(), result.points.end(),
                [&](const auto &record) { return record.positionModel == target; });
            checks.Get(result.status == SurfaceResultStatus::Succeeded && point != result.points.end(),
                       "offset anisotropic plane retains the independently located crossing");
            if (point == result.points.end()) continue;
            checks.Get(point->seedPositionModel == target && point->normalModel[0] == -1 &&
                       point->normalModel[1] == 0 && point->normalModel[2] == 0 && point->localThreshold == 5,
                       "point position and normal match the analytic x=5 plane");
            checks.Get(point->gradientMagnitude == (missing == 0 ? 1.0F : 0.0F),
                       "linear gradient is one; missing neighbor keeps the existing zero-magnitude fallback");
            checks.Get(point->validSupportRatio == (missing == 1 ? 0.0F : 1.0F) &&
                       GetSurfaceFlag(point->flags, SurfacePointFlags::InvalidSupport) == (missing == 1),
                       "zero-weight mask corner invalidates support; scalar NaN alone preserves its status");
            if (mode == FE_UPWARD)
                checks.Get(result.cpuThreadCount == 1 && result.plannedCpuThreads == 1,
                           "non-default rounding preserves analytic results through the serial path");
        }
    }
}

void TestMaterialTriangleQuality(Checks &checks)
{
    // 同一 x 平面改变物理面面积：低于 epsilon、正常、以及有限面向量的长度溢出。
    const std::array<std::array<double,3>,3> spacings{{
        {10,1.0e-6,0.5e-6}, {10,1.0e-6,2.0e-6}, {1.0e-300,1.0e154,1.0e154}}};
    for (std::size_t fixture=0;fixture<spacings.size();++fixture) {
        const auto spacing=spacings[fixture];
        const auto source=BuildSnapshot({8,8,8},spacing,{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_DOUBLE,
            [spacing](const Point3&p){return p[0]/spacing[0];});
        auto params=GetParams();params.initialIsoValue=3.5;params.minimumObjectVoxels=0;
        const auto result=SurfaceDeterminationAlgorithm::BuildSurface(source,params,256U*1024U*1024U,
            []{return false;},{});
        checks.Get(result.status==SurfaceResultStatus::Succeeded && result.points.size()==64 &&
            result.triangleIndices.size()==294,"anisotropic analytic plane retains all quality-test points/faces");
        if(result.status!=SurfaceResultStatus::Succeeded)continue;
        const bool flipRisk=fixture==0;
        checks.Get(std::all_of(result.points.begin(),result.points.end(),[flipRisk](const auto&p){
            return GetSurfaceFlag(p.flags,SurfacePointFlags::TriangleFlipRisk)==flipRisk &&
                !GetSurfaceFlag(p.flags,SurfacePointFlags::SharpCorner) &&
                (!flipRisk || GetSurfaceFlag(p.flags,SurfacePointFlags::SeedRetained)) &&
                p.positionModel==p.seedPositionModel && p.offsetFromSeed==0;
        }),"degenerate risk and nonfinite normalization preserve their separate geometric predicates");
        const auto point=std::find_if(result.points.begin(),result.points.end(),[spacing](const auto&p){
            return p.positionModel[1]>2*spacing[1] && p.positionModel[1]<5*spacing[1] &&
                p.positionModel[2]>2*spacing[2] && p.positionModel[2]<5*spacing[2];
        });
        checks.Get(point!=result.points.end(),"quality plane has an independent interior sample");
        if(point==result.points.end())continue;
        const std::array<float,3> normal=fixture==2?std::array<float,3>{0,0,1}:std::array<float,3>{-1,0,0};
        checks.Get(point->normalModel==normal &&
            point->seedNormalModel==Point3{normal[0],normal[1],normal[2]},
            "post-sampling recovery uses the final gradient normal; overflow retains the seed-normal fallback");
        const auto diagnostic=SurfaceDeterminationAlgorithm::GetPointDiagnostic(source,result.resolvedParams,*point);
        checks.Get(diagnostic.isAvailable && GetPointFields(diagnostic.point)==GetPointFields(*point),
                   "diagnostic retains all published geometric quality fields");
    }
}

void TestManyComponents(Checks &checks)
{
    const auto source = BuildSnapshot({34,34,34}, {1,1,1}, {0,0,0},
        {1,0,0,0,1,0,0,0,1}, VTK_FLOAT, [](const Point3 &point) {
            for (auto value : point) {
                const auto index = int(value);
                if (index < 5 || index > 29 || (index - 5) % 6 != 0) return 0.0;
            }
            return 1.0;
        });
    auto params = GetParams();
    params.initialIsoValue = .5;
    params.minimumObjectVoxels = 0;
    params.componentSelection = SurfaceComponentSelection::All;
    const auto build = [&] { return SurfaceDeterminationAlgorithm::BuildSurface(source, params,
        256U*1024U*1024U, [] { return false; }, {}); };
    const auto first = build(), second = build();
    checks.Get(first.status == SurfaceResultStatus::Succeeded && first.objects.size() == 125 &&
               first.points.size() == 750 && first.triangleIndices.size() == 3000,
               "125 independent high nodes produce 125 six-vertex eight-face components");
    checks.Get(GetEqualMeasurements(first, second), "dense component grouping preserves deterministic fields and order");
    std::size_t object = 0;
    for (int z=5;z<=29;z+=6) for (int y=5;y<=29;y+=6) for (int x=5;x<=29;x+=6) {
        if (object >= first.objects.size()) continue;
        const auto &record = first.objects[object++];
        checks.Get(record.boundsModel == std::array<double,6>{x-.5,x+.5,y-.5,y+.5,z-.5,z+.5} &&
                   record.pointCount == 6 && record.triangleCount == 8 && record.isClosed && record.isManifold,
                   "component bounds and ordering follow isolated-node geometry and first scan encounter");
    }
    params.componentSelection=SurfaceComponentSelection::Largest;
    const auto largest=build();
    checks.Get(largest.status==SurfaceResultStatus::Succeeded && largest.objects.size()==1 && !first.objects.empty() &&
        largest.objects[0].boundsModel==first.objects[0].boundsModel &&
        largest.componentCandidateCount==125 && largest.componentSelectionPointVisits==0,
        "equal-volume/equal-triangle Largest keeps the original minimum point-ID tie break without distance scans");
    params.componentSelection=SurfaceComponentSelection::Seeded;params.seedModelPoint=Point3{8,5,5};
    const auto tied=build();
    checks.Get(tied.status==SurfaceResultStatus::Succeeded && tied.objects.size()==1 && !first.objects.empty() &&
        tied.objects[0].boundsModel==first.objects[0].boundsModel &&
        tied.componentCandidateCount==125 && tied.componentSelectionPointVisits==first.points.size(),
        "Seeded equidistance keeps the original point-ID tie break and scans each candidate point once");
    params.seedModelPoint=Point3{std::numeric_limits<double>::max(),0,0};
    const auto overflowed=build();
    checks.Get(overflowed.status==SurfaceResultStatus::Succeeded && overflowed.objects.size()==1 && !first.objects.empty() &&
        overflowed.objects[0].boundsModel==first.objects[0].boundsModel,
        "overflowed squared distances keep the finite max-distance sentinel and original tie break");
    params.minimumObjectVoxels=std::numeric_limits<std::uint64_t>::max();
    const auto excluded=build();
    checks.Get(excluded.failureReason==SurfaceFailureReason::NoSurface && excluded.points.empty() &&
        excluded.componentCandidateCount==0 && excluded.componentSelectionPointVisits==0,
        "fully excluded closed components preserve the empty selection failure without distance scans");
    const auto singleSource=BuildSnapshot({9,9,9},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_FLOAT,
        [](const Point3&p){return p==Point3{4,4,4}?1.:0.;});
    params.minimumObjectVoxels=0;params.seedModelPoint=Point3{4,4,4};
    const auto single=SurfaceDeterminationAlgorithm::BuildSurface(singleSource,params,WorkLimit{},[]{return false;},{});
    checks.Get(single.status==SurfaceResultStatus::Succeeded && single.objects.size()==1 && single.points.size()==6 &&
        single.componentCandidateCount==1 && single.componentSelectionPointVisits==0,
        "single-candidate Seeded preserves the analytic octahedron and avoids an unnecessary distance scan");
    params.componentSelection=SurfaceComponentSelection::All;params.seedModelPoint.reset();
    bool grouping = false;
    unsigned checksInGrouping = 0;
    const auto cancelled = SurfaceDeterminationAlgorithm::BuildSurface(source, params, 256U*1024U*1024U,
        [&] { return grouping && ++checksInGrouping >= 3; },
        [&](SurfaceDeterminationStage stage, double) {
            if (stage == SurfaceDeterminationStage::SubvoxelRefinement) grouping = true;
        });
    checks.Get(cancelled.status == SurfaceResultStatus::Cancelled && cancelled.points.empty() &&
               cancelled.objects.empty() && checksInGrouping >= 3,
               "component construction cancellation publishes no partial grouped result");
}


void TestComponentPool(Checks &checks)
{
    const auto source=BuildSnapshot({100,100,100},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_FLOAT,
        [](const Point3&p){for(auto value:p){const auto i=int(value);if(i<5||i>95||(i-5)%6)return 0.;}return 1.;});
    auto params=GetParams();params.initialIsoValue=.5;params.minimumObjectVoxels=0;
    params.componentSelection=SurfaceComponentSelection::All;
    // 八面体面法线与顶点法线夹角约54.7°，关闭尖角拒绝后可独立判定完整面积和体积。
    params.sharpCornerAngleDeg=180;
    EnvironmentSwitch setting("MVVCVTK_SURFACE_COMPONENT_PARALLEL");
    const auto build=[&](WorkLimit budget=WorkLimit{}){
        return SurfaceDeterminationAlgorithm::BuildSurface(source,params,budget,[]{return false;},{});};
    setting.Set(false);const auto serial=build();
    setting.Set(true);const auto parallel=build();
    checks.Get(serial.status==SurfaceResultStatus::Succeeded && serial.objects.size()==4096 &&
        serial.points.size()==24576 && serial.triangleIndices.size()==4096*24,
        "4096 isolated nodes form 4096 six-point eight-face octahedra");
    checks.Get(GetEqualMeasurements(serial,parallel),"component pool preserves all measurement fields and output order");
    checks.Get(parallel.acceptedPointCount==24576 && parallel.rejectedPointCount==0 &&
        std::all_of(parallel.points.begin(),parallel.points.end(),[](const auto&p){return p.flags==SurfacePointFlags::None;}),
        "analytic octahedra retain complete support and no point rejection flags");
    checks.Get(parallel.componentCpuThreads<=parallel.plannedCpuThreads &&
        parallel.cpuThreadCount<=parallel.plannedCpuThreads,
        "component workers share the dynamically planned CPU allowance");
    std::size_t object=0;
    for(int z=5;z<=95;z+=6)for(int y=5;y<=95;y+=6)for(int x=5;x<=95;x+=6){
        if(object>=parallel.objects.size())continue;
        const auto &o=parallel.objects[object];
        checks.Get(o.objectIndex==object && o.firstPoint==object*6 && o.firstTriangle==object*8 &&
            o.boundsModel==std::array<double,6>{x-.5,x+.5,y-.5,y+.5,z-.5,z+.5} &&
            o.areaValidity==SurfaceMetricValidity::Valid && o.volumeValidity==SurfaceMetricValidity::Valid &&
            o.areaModelUnit2 && std::abs(*o.areaModelUnit2-std::sqrt(3.))<1e-12 &&
            o.volumeModelUnit3 && std::abs(*o.volumeModelUnit3-1./6)<1e-12,
            "each ordered octahedron has analytic bounds, area sqrt(3), and volume 1/6");
        ++object;
    }
    const auto tight=build(serial.requiredBytes);
    checks.Get(GetEqualMeasurements(serial,tight) && tight.componentCpuThreads==1,
               "original exact budget retains serial output without optional worker workspace");
    if(parallel.componentCpuThreads>1){
        checks.Get(parallel.componentCpuPointCount==24570 && parallel.componentWorkspaceBound>0,
                   "the largest component stays serial and the remaining point slices use real workers");
        checks.Get(tight.componentBudgetFallbackCount>0,"tight working budget disables the optional component pool");
        const auto owner=std::this_thread::get_id();bool callbackOnOwner=true;
        const auto cancelled=SurfaceDeterminationAlgorithm::BuildSurface(source,params,WorkLimit{},
            [owner]{return std::this_thread::get_id()!=owner;},
            [&](SurfaceDeterminationStage,double){callbackOnOwner=callbackOnOwner&&std::this_thread::get_id()==owner;});
        checks.Get(cancelled.status==SurfaceResultStatus::Cancelled && cancelled.points.empty() &&
            cancelled.objects.empty() && callbackOnOwner,"component cancellation joins workers; progress remains owner-only");
        const auto failed=SurfaceDeterminationAlgorithm::BuildSurface(source,params,WorkLimit{},
            [owner]{if(std::this_thread::get_id()!=owner)throw std::runtime_error("component worker failure");return false;},{});
        checks.Get(failed.failureReason==SurfaceFailureReason::InternalError && failed.points.empty(),
                   "component worker exceptions join and use the existing failure boundary");
        checks.Get(GetEqualMeasurements(serial,build()),"component failure releases the outer shared quota");
        auto first=std::async(std::launch::async,build,WorkLimit{});
        auto second=std::async(std::launch::async,build,WorkLimit{});
        checks.Get(GetEqualMeasurements(serial,first.get()) && GetEqualMeasurements(serial,second.get()),
                   "concurrent component pools preserve independent deterministic results");
    }
    const auto oldRounding=std::fegetround();std::fesetround(FE_UPWARD);
    const auto rounded=build();std::fesetround(oldRounding);
    checks.Get(rounded.status==SurfaceResultStatus::Succeeded && rounded.componentCpuThreads==1 &&
        rounded.plannedCpuThreads==1,"non-default floating control keeps component processing serial");
    std::cout<<"Surface component workers="<<parallel.componentCpuThreads<<" points="
             <<parallel.componentCpuPointCount<<" workspace-bound="<<parallel.componentWorkspaceBound<<'\n';
}

class CountingRoi final : public RoiReadView {
public:
    explicit CountingRoi(RoiReadSnapshot inner,std::optional<double> fixed={}) : m_inner(std::move(inner)),m_fixed(fixed) {}
    DataRevisionRef GetRevision() const noexcept override{return m_inner->GetRevision();}
    DataRevisionRef GetSource() const noexcept override{return m_inner->GetSource();}
    const RoiDefinition &GetDefinition() const noexcept override{return m_inner->GetDefinition();}
    const std::vector<DataRevisionRef> &GetDependencies() const noexcept override{return m_inner->GetDependencies();}
    std::array<double,6> GetBounds() const noexcept override{return m_inner->GetBounds();}
    RoiQueryResult GetQuery() const override{return m_inner->GetQuery();}
    bool GetContains(const std::array<double,3>&p) const override{return m_inner->GetContains(p);}
    RoiPointsResult GetPoints(const std::vector<std::array<double,3>>&p) const override{return m_inner->GetPoints(p);}
    RoiPlanesResult GetClipPlanes() const override{return m_inner->GetClipPlanes();}
    RoiTriangleSelection GetTriangleSelection(const std::array<std::array<double,3>,3>& p,double tolerance,std::function<bool()> cancel)const override{return m_inner->GetTriangleSelection(p,tolerance,std::move(cancel));}
    RoiTriangleSelection GetTriangleChunks(const RoiTriangle& p,std::size_t count,const RoiTriangleCallback& callback,double tolerance,std::function<bool()> cancel)const override{return m_inner->GetTriangleChunks(p,count,callback,tolerance,std::move(cancel));}
    RoiPolygonResult GetClippedTriangle(const std::array<std::array<double,3>,3>&p) const override{return m_inner->GetClippedTriangle(p);}
    double GetBoundaryDistance(const std::array<double,3>&p) const noexcept override{
        queries.fetch_add(1,std::memory_order_relaxed);return m_fixed?*m_fixed:m_inner->GetBoundaryDistance(p);
    }
    RoiMaskResult GetMaskChunk(const RoiMaskRequest&r) const override{return m_inner->GetMaskChunk(r);}
    mutable std::atomic<std::size_t> queries{0};
private:
    RoiReadSnapshot m_inner;
    std::optional<double> m_fixed;
};

void TestSingleRoiDistance(Checks &checks)
{
    const auto source=BuildSnapshot({16,16,16},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_DOUBLE,
        [](const Point3&p){return p[0];});
    const auto roi=BuildRoi(source,{2,13,3.1,11.7,4.2,12.6});
    auto params=GetParams();params.initialIsoValue=7.5;params.minimumObjectVoxels=0;
    params.analysisRoi=roi->GetRevision();
    const auto build=[&](const RoiReadSnapshot&view){return SurfaceDeterminationAlgorithm::BuildSurface(
        source,params,WorkLimit{},[]{return false;},{},SurfaceAlgorithmInputs{view});};
    const auto reference=build(roi);
    auto counted=std::make_shared<CountingRoi>(roi);const auto actual=build(counted);
    checks.Get(reference.status==SurfaceResultStatus::Succeeded && !reference.points.empty() &&
        GetEqualMeasurements(reference,actual),"counted immutable ROI preserves every published measurement field");
    // ROI 只选父表面片段；边界距离不参与材料细化、支持判定或正式父点资格。
    checks.Get(counted->queries.load()==0 && !actual.selection.empty(),"ROI selection preserves complete measurement support");
    const auto diagnostic=SurfaceDeterminationAlgorithm::GetPointDiagnostic(source,actual.resolvedParams,
        actual.points.front(),SurfaceAlgorithmInputs{counted});
    checks.Get(diagnostic.isAvailable && GetPointFields(diagnostic.point)==GetPointFields(actual.points.front()) &&
        counted->queries.load()==0,"diagnostic retains the complete parent measurement independently of ROI boundary");
    for(double distance:{0.,1e-9,1.,std::numeric_limits<double>::quiet_NaN(),
                        -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()}){
        auto fixed=std::make_shared<CountingRoi>(roi,distance);const auto result=build(fixed);
        checks.Get(result.status==SurfaceResultStatus::Succeeded && GetEqualMeasurements(reference,result) &&
            fixed->queries.load()==0 && result.selection.size()==reference.selection.size(),
            "ROI boundary distance does not create material boundaries or change parent measurements");
    }
}

void TestDiagonalRoiSeedClip(Checks &checks)
{
    const auto source=BuildSnapshot({7,7,7},{1,1,1},{0,0,0},{1,0,0,0,1,0,0,0,1},VTK_FLOAT,
        [](const Point3&p){return p[2];});
    DataGraphStore store;
    RoiNode node;node.primitive.localToSource={1,1,0,3, 1,-1,0,3, 0,0,2,3, 0,0,0,1};
    RoiDefinition definition{source->data->self,{node}};
    const DataRevisionRef ref{store.CreateDataEntityId(),1};
    DataTransaction transaction;
    transaction.outputs={{source->data->self.entityId,0,DataTypes::imageGrid3D,{},source->data->payload},
        {ref.entityId,0,DataTypes::roiGeometry,RoiEvaluator::GetInputs(definition),std::make_shared<const RoiGeometryPayload>(definition)}};
    const auto committed=store.SetDataCommit(transaction);
    const auto roi=RoiEvaluator::GetRoi(committed.graph,ref,source->data->self).roi;
    checks.Get(bool(roi),"diagonal ROI fixture evaluates a nonsingular transformed box");
    if(!roi)return;
    SurfaceSeedGrid grid;grid.extent={0,6,0,6,0,6};grid.dimensions={7,7,7};
    grid.indexToModel=grid.modelToIndex={1,0,0,0,1,0,0,0,1};
    grid.values=grid.scalarValues=source->image->GetScalarPointer();grid.scalarType=VTK_FLOAT;
    grid.readScalar=[](const void*p,std::size_t i){return double(static_cast<const float*>(p)[i]);};
    PagedArray<Point3> points;PagedArray<SurfaceSeedTriangle> triangles;SurfaceExecutionStats stats;
    const auto status=SurfaceSeedBuilder::BuildMesh(grid,3.5,roi,0,2,WorkLimit{}, {},points,triangles,stats);
    checks.Get(status==SurfaceSeedStatus::Succeeded && !triangles.empty() &&
        std::all_of(points.begin(),points.end(),[](const auto&p){return p[2]==3.5 &&
            p[0]+p[1]>=4-1e-12 && p[0]+p[1]<=8+1e-12 && std::abs(p[0]-p[1])<=2+1e-12;}),
        "diagonal clip keeps the analytic plane inside the four diamond half spaces");
    double area=0;
    for(const auto &triangle:triangles){
        const auto &a=points[triangle.vertices[0]],&b=points[triangle.vertices[1]],&c=points[triangle.vertices[2]];
        area+=.5*std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]));
    }
    // 四顶点 (1,3),(3,1),(5,3),(3,5) 的解析面积为8；裕量仅覆盖 float64 投影舍入。
    checks.Get(std::abs(area-8)<1e-12,"shared intersections in reverse edge directions retain the analytic diamond area");
}

void TestSparseTopology(Checks &checks)
{
    const std::array<double,3> spacing{2,3,4},origin{-17,29,5};
    const auto source=BuildSnapshot({32,25,27},spacing,origin,{1,0,0,0,1,0,0,0,1},VTK_FLOAT,
        [=](const Point3&p){
            const int x=int((p[0]-origin[0])/spacing[0]),y=int((p[1]-origin[1])/spacing[1]),z=int((p[2]-origin[2])/spacing[2]);
            return (x==-1&&y==6&&z==-1)||((x==4||x==5)&&y==11&&z==3)||(x==17&&y==20&&z==13)?1.:0.;
        },{},1,{-3,2,-5});
    auto params=GetParams();params.initialIsoValue=.5;params.minimumObjectVoxels=0;
    params.sharpCornerAngleDeg=180;params.componentSelection=SurfaceComponentSelection::All;
    const auto result=SurfaceDeterminationAlgorithm::BuildSurface(source,params,WorkLimit{},[]{return false;},{});
    checks.Get(result.status==SurfaceResultStatus::Succeeded && result.objects.size()==3 &&
        result.points.size()==22 && result.triangleIndices.size()==96,"sparse anisotropic islands retain two octahedra and one adjacent-node component");
    if(result.objects.size()!=3)return;
    for(std::size_t i=0;i<3;++i){
        const auto &o=result.objects[i];const bool pair=i==0;
        const double area=std::sqrt(244.)+(pair?20:0),volume=pair?16:4;
        // 本例坐标均为可精确表示的半整数；最多16面只在sqrt、/6和顺序累加时舍入。
        // 以下为保守float64舍入裕量：绝对面积1e-12 model-unit²、体积1e-10 model-unit³。
        checks.Get(o.pointCount==(pair?10:6) && o.triangleCount==(pair?16:8) && o.isClosed && o.isManifold &&
            o.volumeValidity==SurfaceMetricValidity::Valid && o.areaValidity==SurfaceMetricValidity::Valid &&
            o.areaModelUnit2 && std::abs(*o.areaModelUnit2-area)<1e-12 &&
            o.volumeModelUnit3 && std::abs(*o.volumeModelUnit3-volume)<1e-10,
            "local sparse point IDs preserve independent anisotropic area and volume");
    }
    params.componentSelection=SurfaceComponentSelection::Largest;
    const auto largest=SurfaceDeterminationAlgorithm::BuildSurface(source,params,WorkLimit{},[]{return false;},{});
    checks.Get(largest.status==SurfaceResultStatus::Succeeded && largest.objects.size()==1 &&
        largest.objects[0].pointCount==10 && largest.objects[0].volumeModelUnit3 &&
        std::abs(*largest.objects[0].volumeModelUnit3-16)<1e-10,
        "early local topology estimates still select the independently largest component");
}

void TestBackendCostChoice(Checks &checks)
{
    SurfaceBackendProbe probe{100,100,1000,1000,100,100,10,5,40};
    auto choice=GetSurfaceBackendChoice(probe);
    checks.Get(choice.hasEstimate && choice.keepGpu && choice.cpuTotalMs==1150 && choice.gpuTotalMs==245,
        "backend cost includes completed prefix/startup/pilot and separates first-window upload from point compute");
    probe.cpuMs=1;choice=GetSurfaceBackendChoice(probe);
    checks.Get(choice.hasEstimate && !choice.keepGpu && choice.cpuTotalMs==61 && choice.gpuTotalMs==146,
        "fast CPU chooses a CPU tail even when GPU computation is correct");
    probe.cpuMs=11.875;choice=GetSurfaceBackendChoice(probe);
    checks.Get(choice.hasEstimate && !choice.keepGpu,"exact twenty-percent margin remains conservative");
    probe.cpuMs=11.876;choice=GetSurfaceBackendChoice(probe);
    checks.Get(choice.keepGpu,"measured benefit beyond the margin can retain GPU without a fixed thread-count rule");
    for(const double invalid:{0.,-1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){
        probe.cpuMs=invalid;
        checks.Get(!GetSurfaceBackendChoice(probe).hasEstimate,"invalid CPU timing disables cost-based GPU continuation");
    }
    probe.cpuMs=100;probe.uploadedBytes=0;
    checks.Get(!GetSurfaceBackendChoice(probe).hasEstimate,"missing optional traffic snapshot falls back without requiring GPU timing events");
    probe.uploadedBytes=100;probe.gpuMs=4;
    checks.Get(!GetSurfaceBackendChoice(probe).hasEstimate,"inconsistent elapsed/upload timings never produce a negative compute estimate");
}

void TestBackendCalibrationCache(Checks &checks)
{
    using Cache=SurfaceBackendCalibrationCache;
    Cache cache;
    const Cache::Clock::time_point now{};
    const SurfaceBackendCalibration calibration{.05,.05,40};
    Cache::Key key{};key[0]=17;
    const auto fast=GetSurfaceBackendAdmission(calibration,100,2,1000,1000);
    const auto slow=GetSurfaceBackendAdmission(calibration,100,100,1000,1000);
    checks.Get(fast.hasEstimate && !fast.keepGpu && slow.hasEstimate && slow.keepGpu,
        "prior cost can only skip startup for a clearly faster measured CPU; slow CPU still probes actual GPU");
    cache.Store(key,calibration,now);
    auto different=key;++different[8];
    checks.Get(!cache.Take(different,now),"changed actual CPU plan cannot reuse another calibration class");
    checks.Get(bool(cache.Take(key,now)) && !cache.Take(key,now),"prior cost is consumed once to force periodic actual GPU reevaluation");
    cache.Store(key,calibration,now);
    checks.Get(!cache.Take(key,now+std::chrono::minutes(2)),"expired prior cost cannot skip GPU initialization indefinitely");
    cache.Store(key,calibration,now);
    checks.Get(!cache.Take(key,now-std::chrono::seconds(1)),"clock reversal invalidates a cost record");
    for(std::uint64_t value=0;value<5;++value){key[0]=value;cache.Store(key,calibration,now);}
    key[0]=0;checks.Get(!cache.Take(key,now),"fifth calibration evicts a bounded slot instead of growing storage");
    key[0]=99;cache.Store(key,calibration,now);
    std::atomic<unsigned> hits{0};
    auto first=std::async(std::launch::async,[&]{if(cache.Take(key,now))++hits;});
    auto second=std::async(std::launch::async,[&]{if(cache.Take(key,now))++hits;});
    first.get();second.get();
    checks.Get(hits.load()==1,"concurrent requests consume the same cost record at most once");
    std::cout<<"Surface calibration cache bytes="<<sizeof(Cache)<<'\n';
}

void TestCompleteMeasurementSerialization(Checks &checks)
{
    SurfaceAlgorithmResult original;original.status=SurfaceResultStatus::Succeeded;
    original.points.resize(1);original.objects.resize(1);original.interfaces.resize(1);
    original.isoEstimate=SurfaceIsoEstimate{};
    const auto serialize=[](const auto &result,bool full){std::ostringstream out(std::ios::binary);
        SurfaceAudit::WriteResult(out,result,full);return out.str();};
    const auto legacy=serialize(original,false),full=serialize(original,true);
    checks.Get(full.size()>legacy.size() && full.substr(0,legacy.size())==legacy,
        "complete measurement schema preserves the unchanged legacy hash prefix");
    const auto changed=[&](auto mutate,const char *name){auto result=original;mutate(result);
        checks.Get(serialize(result,true)!=full,name);};
    for(auto field:{&SurfacePointRecord::contrast,&SurfacePointRecord::fitResidual,&SurfacePointRecord::offsetFromSeed,
                   &SurfacePointRecord::estimatedLocalizationSigma,&SurfacePointRecord::transitionWidthModel,
                   &SurfacePointRecord::pairedSeparationModel})
        changed([field](auto &r){r.points[0].*field=1;},"complete schema detects every additional point quality quantity");
    changed([](auto&r){r.points[0].crossingCount=1;},"complete schema detects crossing count");
    changed([](auto&r){r.points[0].overrideIndex=1;},"complete schema detects override identity");
    for(auto field:{&SurfaceObjectRecord::isClosed,&SurfaceObjectRecord::isManifold,&SurfaceObjectRecord::isTruncated,
                   &SurfaceObjectRecord::isOrientationValid})
        changed([field](auto&r){r.objects[0].*field=true;},"complete schema detects every object topology/coverage state");
    changed([](auto&r){r.objects[0].boundsModel[2]=1;},"complete schema detects object model bounds");
    changed([](auto&r){r.objects[0].validAreaRatio=.5;},"complete schema detects valid area ratio");
    changed([](auto&r){r.objects[0].firstPoint=1;},"complete schema detects point slice");
    changed([](auto&r){r.objects[0].firstTriangle=1;},"complete schema detects triangle slice");
    changed([](auto&r){r.sourceRevision.generation=1;},"complete schema detects source revision");
    changed([](auto&r){r.acceptedPointCount=1;},"complete schema detects global acceptance counters");
    changed([](auto&r){r.isoEstimate->separationRatio=.5;},"complete schema detects iso-estimation quality");
    changed([](auto&r){r.interfaces[0].materials.materialA=1;},"complete schema detects interface material pair");
    changed([](auto&r){r.interfaces[0].canonicalId="gray";},"complete schema detects interface canonical identity");
    changed([](auto&r){r.execution.processedExtent[0]=1;},"complete schema detects processed input extent");
}

void TestGpuFinalizerOwnership(Checks &checks)
{
    std::vector<SurfaceCudaSample> samples(surfaceCudaBatchCapacity);
    for (auto &sample:samples) { sample.support=1;sample.gradientValid=1;sample.magnitude=17; }
    std::promise<void> entered,release;
    auto released=release.get_future().share();
    std::atomic<bool> copied{false};
    const auto callback=[&](std::size_t begin,std::size_t count,const SurfaceCudaSample *values) {
        if(begin==99)throw std::runtime_error("finalizer exception probe");
        if(begin==0){entered.set_value();released.wait();}
        SurfaceGpuFinalizedBatch result;
        if(!values){result.accepted=count;return result;}
        copied.store(std::all_of(values,values+count,[](const auto &sample){
            return sample.support==1 && sample.gradientValid==1 && sample.magnitude==17;
        }));
        result.accepted=count;
        return result;
    };
    SurfaceGpuFinalizer<decltype(callback)> pool(1,callback);
    checks.Get(pool.GetThreadCount()==1,"finalizer ownership fixture starts a bounded persistent worker");
    if(!pool.GetThreadCount())return;
    pool.Submit(0,0,samples.size(),samples.data());
    entered.get_future().wait();
    // 模拟 owner 重用 pinned 样本；worker 必须继续读取自己的普通 CPU 副本。
    for(auto &sample:samples)sample.magnitude=-23;
    release.set_value();
    const auto first=pool.Finish(0);
    checks.Get(copied.load() && first.accepted==samples.size(),"finalizer does not borrow mutable CUDA samples");
    pool.Submit(0,1,31,nullptr);
    checks.Get(pool.Finish(0).accepted==31 && pool.Finish(0).accepted==0,
        "completed slot is reusable and merged once; missing GPU data reaches CPU callback");
    pool.Submit(1,99,1,nullptr);
    bool threw=false;
    try{pool.Finish(1);}catch(const std::runtime_error&){threw=true;}
    pool.Stop();
    checks.Get(threw,"finalizer joins workers and propagates exceptions through owner");
    SurfaceGpuFinalizer<decltype(callback)> recovery(1,callback);
    if(recovery.GetThreadCount()){
        recovery.Submit(0,2,7,nullptr);
        checks.Get(recovery.Finish(0).accepted==7,"failed finalizer leaves no persistent task state");
    }
}

struct SeedReadCounter final {
    const std::vector<float> *values;
    std::size_t reads = 0;
    const std::array<double,2> *range = nullptr;
};
std::size_t cacheAllocationCount = 0, failCacheAllocation = 0;
void CheckCacheAllocation(std::size_t)
{
    if (++cacheAllocationCount == failCacheAllocation) throw std::bad_alloc{};
}

template<class T>
void TestSeedCacheTails(Checks &checks, int type)
{
    // 覆盖不足八格及八格快速段后的每种尾长，并跨精确阈值端点生成共享顶点。
    for (int width = 9; width <= 25; ++width) {
        std::vector<T> values(std::size_t(width) * 3 * 4);
        for (int z = 0; z < 4; ++z) for (int y = 0; y < 3; ++y) for (int x = 0; x < width; ++x)
            values[(z * 3 + y) * width + x] = static_cast<T>(x < 8 ? 0 : 4);
        SurfaceSeedGrid grid;
        grid.extent = {-5,width-6,-9,-7,11,14}; grid.dimensions = {width,3,4};
        grid.indexToModel = grid.modelToIndex = {1,0,0,0,1,0,0,0,1};
        grid.values = values.data(); grid.scalarType = type;
        grid.readScalar = [](const void *data, std::size_t index) {
            return double(static_cast<const T *>(data)[index]);
        };
        for (double iso : {1.5,4.0}) {
            PagedArray<Point3> baselinePoints, cachedPoints;
            PagedArray<SurfaceSeedTriangle> baselineTriangles, cachedTriangles;
            SurfaceExecutionStats baselineStats, cachedStats;
            grid.scalarValues = nullptr;
            const auto baseline = SurfaceSeedBuilder::BuildMesh(grid,iso,{},0,2,WorkLimit{},
                {},baselinePoints,baselineTriangles,baselineStats);
            grid.scalarValues = values.data();
            const auto cached = SurfaceSeedBuilder::BuildMesh(grid,iso,{},0,2,WorkLimit{},
                {},cachedPoints,cachedTriangles,cachedStats);
            checks.Get(baseline==cached && cached==SurfaceSeedStatus::Succeeded &&
                cachedPoints==baselinePoints && cachedTriangles.size()==baselineTriangles.size() &&
                std::equal(cachedTriangles.begin(),cachedTriangles.end(),baselineTriangles.begin(),
                    [](const auto&a,const auto&b){return a.vertices==b.vertices;}) &&
                cachedStats.scannedCellCount==baselineStats.scannedCellCount &&
                cachedStats.skippedCellCount==baselineStats.skippedCellCount &&
                cachedStats.blockCount==baselineStats.blockCount &&
                cachedStats.processedExtent==baselineStats.processedExtent,
                "typed cache run/tail and exact-threshold order: "+std::to_string(type));
            const double expectedX = iso==4 ? 3.0 : 2.375;
            checks.Get(cachedPoints.size()==12 && cachedTriangles.size()==12 &&
                std::all_of(cachedPoints.begin(),cachedPoints.end(),
                    [expectedX](const auto&p){return p[0]==expectedX;}),
                "run/tail retains the independent analytic plane");
        }
    }
}

void TestSeedClassificationCache(Checks &checks)
{
    constexpr std::size_t width=74,height=7,depth=5;
    std::vector<float> values(width*height*depth);
    std::vector<unsigned char> mask(values.size(),1);
    for(std::size_t z=0;z<depth;++z)for(std::size_t y=0;y<height;++y)for(std::size_t x=0;x<width;++x)
        values[(z*height+y)*width+x]=x<36?0.F:4.F;
    SurfaceSeedGrid grid;
    grid.extent={-13,60,4,10,-7,-3};grid.dimensions={74,7,5};
    grid.indexToModel=grid.modelToIndex={1,0,0,0,1,0,0,0,1};
    grid.scalarType=VTK_FLOAT;
    SeedReadCounter reader{&values};
    grid.values=&reader;
    grid.readScalar=[](const void *context,std::size_t index){
        auto &reader=*const_cast<SeedReadCounter*>(static_cast<const SeedReadCounter*>(context));
        ++reader.reads;const auto value=double((*reader.values)[index]);
        return reader.range&&std::isfinite(value)?std::clamp(value,(*reader.range)[0],(*reader.range)[1]):value;
    };
    struct Mesh final {
        SurfaceSeedStatus status;
        PagedArray<Point3> points;
        PagedArray<SurfaceSeedTriangle> triangles;
        SurfaceExecutionStats stats;
        SurfaceSeedAudit audit;
        std::size_t callbackReads;
    };
    const auto build=[&](bool typed,std::size_t cacheLimit,WorkLimit budget,std::uint32_t block,
                         std::uint64_t cancelCell=UINT64_MAX){
        Mesh mesh;reader.reads=0;cacheAllocationCount=0;
        grid.scalarValues=typed?values.data():nullptr;grid.classificationCacheLimit=cacheLimit;grid.audit=&mesh.audit;
        mesh.status=SurfaceSeedBuilder::BuildMesh(grid,1.5,{},0,block,budget,
            [&]{return mesh.stats.scannedCellCount>=cancelCell;},mesh.points,mesh.triangles,mesh.stats);
        mesh.callbackReads=reader.reads;return mesh;
    };
    const auto equal=[&](const Mesh&a,const Mesh&b){
        return a.status==b.status&&a.points==b.points&&a.triangles.size()==b.triangles.size()&&
            std::equal(a.triangles.begin(),a.triangles.end(),b.triangles.begin(),
                       [](const auto&a,const auto&b){return a.vertices==b.vertices;})&&
            a.stats.scannedCellCount==b.stats.scannedCellCount&&a.stats.skippedCellCount==b.stats.skippedCellCount&&
            a.stats.blockCount==b.stats.blockCount&&a.stats.processedExtent==b.stats.processedExtent;
    };
    const auto reference=build(false,SIZE_MAX,WorkLimit{},2);
    const auto cached=build(true,SIZE_MAX,WorkLimit{},2);
    checks.Get(equal(reference,cached)&&cached.callbackReads==0&&reference.callbackReads==7104,
               "rolling tags preserve geometry/statistics and callback scan read count");
    checks.Get(reference.points.size()==35&&reference.triangles.size()==48&&
               std::all_of(cached.points.begin(),cached.points.end(),[](const auto&p){return p[0]==22.375;}),
               "uniform-run transitions retain the independent interpolated plane");
    const bool cacheEnabled=cached.audit.classificationCacheBytes!=0;
    if(cacheEnabled)checks.Get(cached.audit.classificationCacheBytes==2*width*height&&
        cached.audit.classifiedNodeCount==values.size()&&cached.audit.uniformCellCount>0,
        "two bounded planes classify each node once and skip valid same-side cells");
    checks.Get(equal(reference,build(true,0,WorkLimit{},2))&&
               equal(reference,build(true,1,WorkLimit{},2)),
               "disabled and insufficient cache limits preserve baseline output");
    const auto fallback=build(true,SIZE_MAX,reference.stats.estimatedWorkingBytes,2);
    checks.Get(equal(reference,fallback),"cache release keeps a mesh admitted at its original exact estimate");
    if(cacheEnabled)checks.Get(fallback.audit.budgetFallbackCount==1,
                              "exact mesh budget triggers one optional cache release");
    if(cacheEnabled){
        grid.cacheAllocationCheckpoint=CheckCacheAllocation;
        for(std::size_t allocation=1;allocation<=2;++allocation){
            failCacheAllocation=allocation;
            const auto recovered=build(true,SIZE_MAX,WorkLimit{},2);
            checks.Get(equal(reference,recovered)&&cacheAllocationCount==allocation&&
                recovered.audit.classificationCacheBytes==0,"optional first/second cache allocation failure falls back");
        }
        failCacheAllocation=0;grid.cacheAllocationCheckpoint=nullptr;

        // estimatedWorkingBytes 是历史峰值；第二次失败前实际存在过第一层，不能抹掉该开销。
        std::vector<float> uniform(64*64*3,0.F);
        SurfaceSeedGrid uniformGrid;
        uniformGrid.extent={0,63,0,63,0,2};uniformGrid.dimensions={64,64,3};
        uniformGrid.indexToModel=uniformGrid.modelToIndex={1,0,0,0,1,0,0,0,1};
        uniformGrid.scalarValues=uniform.data();uniformGrid.scalarType=VTK_FLOAT;
        uniformGrid.cacheAllocationCheckpoint=CheckCacheAllocation;
        for(std::size_t allocation=1;allocation<=2;++allocation){
            failCacheAllocation=allocation;cacheAllocationCount=0;
            PagedArray<Point3> points;PagedArray<SurfaceSeedTriangle> triangles;
            SurfaceExecutionStats stats;SurfaceSeedAudit audit;uniformGrid.audit=&audit;
            const auto budget=std::size_t(64*1024+2*64*64);
            const auto status=SurfaceSeedBuilder::BuildMesh(uniformGrid,1.5,{},0,2,budget,
                {},points,triangles,stats);
            checks.Get(status==SurfaceSeedStatus::NoSurface && audit.classificationCacheBytes==0 &&
                stats.estimatedWorkingBytes==64*1024+(allocation==2?64*64:0) &&
                stats.estimatedWorkingBytes<=budget,
                "failed optional allocation retains only the actual historical workspace peak");
        }
        failCacheAllocation=0;
    }
    for(unsigned missing=0;missing<3;++missing){
        const auto index=(1*height+3)*width+8;mask[index]=missing==0?0:1;grid.validity=mask.data();
        const auto old=values[index];
        if(missing==1)values[index]=std::numeric_limits<float>::quiet_NaN();
        if(missing==2)values[index]=std::numeric_limits<float>::infinity();
        checks.Get(equal(build(false,SIZE_MAX,WorkLimit{},3),build(true,SIZE_MAX,WorkLimit{},3)),
                   "invalid/nonfinite tags cannot skip invalid cells or contaminate windows across block boundaries");
        values[index]=old;mask[index]=1;
    }
    grid.validity=nullptr;
    const std::array<double,2> range{1,3};grid.materialRange=&range;reader.range=&range;
    const auto clamped=build(true,SIZE_MAX,WorkLimit{},1);
    checks.Get(equal(build(false,SIZE_MAX,WorkLimit{},1),clamped)&&
               std::all_of(clamped.points.begin(),clamped.points.end(),[](const auto&p){return p[0]==22.25;}),
               "cache applies finite node clamp before classification and preserves interpolation");
    grid.materialRange=nullptr;reader.range=nullptr;
    const auto cancelled=build(true,SIZE_MAX,WorkLimit{},2,1025);
    const auto oldCancelled=build(false,SIZE_MAX,WorkLimit{},2,1025);
    checks.Get(equal(oldCancelled,cancelled)&&cancelled.status==SurfaceSeedStatus::Cancelled&&
               cancelled.stats.scannedCellCount==1025,"uniform skipping preserves the exact virtual-cell checkpoint");
    if(cacheEnabled){
        Mesh stopped;grid.scalarValues=values.data();grid.classificationCacheLimit=SIZE_MAX;grid.audit=&stopped.audit;
        unsigned calls=0;
        stopped.status=SurfaceSeedBuilder::BuildMesh(grid,1.5,{},0,2,WorkLimit{},
            [&]{return ++calls==2;},stopped.points,stopped.triangles,stopped.stats);
        checks.Get(stopped.status==SurfaceSeedStatus::Cancelled&&stopped.stats.scannedCellCount==0&&
                   stopped.points.empty(),"classification cancellation returns before any cell or geometry");
    }
}

void TestSortedTopology(Checks &checks)
{
    const auto verify=[&](std::initializer_list<std::array<std::uint32_t,3>> triangles,
                         std::uint64_t boundary,std::uint64_t nonManifold,bool oriented) {
        DataArray<std::uint64_t> keys;
        for(const auto &tri:triangles) for(unsigned e=0;e<3;++e)
            keys.push_back(GetSurfaceDirectedEdgeKey(tri[e],tri[(e+1)%3]));
        const auto result=GetSortedSurfaceEdgeTopology(keys,{});
        checks.Get(result.boundaryEdgeCount==boundary&&result.nonManifoldEdgeCount==nonManifold&&
                   result.isOrientationValid==oriented,"integer edge topology matches independent mesh incidence");
    };
    verify({{0,2,1},{0,1,3},{0,3,2},{1,2,3}},0,0,true);
    verify({{0,2,1},{0,3,1},{0,3,2},{1,2,3}},0,0,false);
    verify({{0,1,2}},3,0,true);
    verify({{0,1,2},{1,0,3},{0,1,4}},6,1,false);
    verify({{0,0,1}},1,0,true);
    verify({{0,0,1},{0,0,1}},0,1,false);
    const auto forward=GetSurfaceDirectedEdgeKey(0,0x7fffffffU);
    const auto reverse=GetSurfaceDirectedEdgeKey(0x7fffffffU,0);
    checks.Get((forward&~std::uint64_t(1))==(reverse&~std::uint64_t(1))&&forward!=reverse&&
               forward!=GetSurfaceDirectedEdgeKey(0,0x7ffffffeU),"31-bit boundary keys retain identity and direction");
    DataArray<std::uint64_t> keys(10000);
    std::iota(keys.rbegin(),keys.rend(),0U);
    bool interrupted=false;
    try { GetSortedSurfaceEdgeTopology(keys,[]{throw std::runtime_error("sort cancellation probe");}); }
    catch(const std::runtime_error&){interrupted=true;}
    checks.Get(interrupted,"edge sort propagates cancellation before returning partial statistics");
}
} // namespace

int GetSurfaceOptimizationFailCount()
{
    Checks checks;
    TestTypedSeed<char>(checks,VTK_CHAR); TestTypedSeed<signed char>(checks,VTK_SIGNED_CHAR);
    TestTypedSeed<unsigned char>(checks,VTK_UNSIGNED_CHAR); TestTypedSeed<short>(checks,VTK_SHORT);
    TestTypedSeed<unsigned short>(checks,VTK_UNSIGNED_SHORT); TestTypedSeed<int>(checks,VTK_INT);
    TestTypedSeed<unsigned int>(checks,VTK_UNSIGNED_INT); TestTypedSeed<long>(checks,VTK_LONG);
    TestTypedSeed<unsigned long>(checks,VTK_UNSIGNED_LONG); TestTypedSeed<long long>(checks,VTK_LONG_LONG);
    TestTypedSeed<unsigned long long>(checks,VTK_UNSIGNED_LONG_LONG);
    TestTypedSeed<float>(checks,VTK_FLOAT); TestTypedSeed<double>(checks,VTK_DOUBLE);
    TestEdgeAxes(checks); TestSortedTopology(checks); TestParallelSampling(checks);
    TestPointSupportRules(checks); TestManyComponents(checks); TestMaterialTriangleQuality(checks); TestComponentPool(checks);
    TestSingleRoiDistance(checks); TestDiagonalRoiSeedClip(checks); TestSparseTopology(checks);
    TestGpuFinalizerOwnership(checks);
    TestBackendCostChoice(checks);
    TestBackendCalibrationCache(checks);
    TestCompleteMeasurementSerialization(checks);
    TestSeedClassificationCache(checks);
    TestSeedCacheTails<float>(checks,VTK_FLOAT); TestSeedCacheTails<double>(checks,VTK_DOUBLE);
    TestSeedCacheTails<unsigned short>(checks,VTK_UNSIGNED_SHORT);
    return checks.failureCount;
}
