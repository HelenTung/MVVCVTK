// 测试用途：验证类型专门化、跨层边去重，以及动态并行的确定性/停止/异常。
#include "SurfaceDeterminationTestCases.h"
#include "SurfaceDeterminationAlgorithm.h"
#include "SurfaceSeedBuilder.h"
#include "SurfaceEdgeTopology.h"
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

namespace {
using namespace SurfaceTest;

class ParallelSetting final {
public:
    ParallelSetting()
    {
        const auto size = GetEnvironmentVariableA("MVVCVTK_SURFACE_CPU_PARALLEL", nullptr, 0);
        if (size) {
            m_previous.resize(size);
            GetEnvironmentVariableA("MVVCVTK_SURFACE_CPU_PARALLEL", m_previous.data(), size);
            m_previous.resize(size - 1);
            m_present = true;
        }
    }
    ~ParallelSetting()
    {
        SetEnvironmentVariableA("MVVCVTK_SURFACE_CPU_PARALLEL", m_present ? m_previous.c_str() : nullptr);
    }
    void Set(bool enabled) { SetEnvironmentVariableA("MVVCVTK_SURFACE_CPU_PARALLEL", enabled ? "1" : "0"); }
private:
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
    std::vector<Point3> referencePoints;
    std::vector<SurfaceSeedTriangle> referenceTriangles;
    const auto verify = [&](std::uint32_t depth, double iso = 2, double expectedX = 0) {
        std::vector<Point3> points;
        std::vector<SurfaceSeedTriangle> triangles;
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
        std::vector<Point3> points; std::vector<SurfaceSeedTriangle> triangles; SurfaceExecutionStats stats;
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
    std::vector<Point3> points;
    std::vector<SurfaceSeedTriangle> triangles;
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
        std::vector<Point3> points; std::vector<SurfaceSeedTriangle> triangles; SurfaceExecutionStats stats;
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
    ParallelSetting setting;
    setting.Set(false); const auto serial = build();
    setting.Set(true); const auto parallel = build();
    checks.Get(serial.status == SurfaceResultStatus::Succeeded && serial.points.size()==65536 &&
               serial.triangleIndices.size()/3==130050, "large single-component plane matches analytic mesh counts");
    checks.Get(GetEqualMeasurements(serial,parallel), "all point/object measurement fields are thread independent");
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

void TestSortedTopology(Checks &checks)
{
    const auto verify=[&](std::initializer_list<std::array<std::uint32_t,3>> triangles,
                         std::uint64_t boundary,std::uint64_t nonManifold,bool oriented) {
        std::vector<std::uint64_t> keys;
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
    std::vector<std::uint64_t> keys(10000);
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
    return checks.failureCount;
}
