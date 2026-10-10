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
            std::vector<Point3> baselinePoints, cachedPoints;
            std::vector<SurfaceSeedTriangle> baselineTriangles, cachedTriangles;
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
        std::vector<Point3> points;
        std::vector<SurfaceSeedTriangle> triangles;
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
            std::vector<Point3> points;std::vector<SurfaceSeedTriangle> triangles;
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
    TestPointSupportRules(checks); TestManyComponents(checks);
    TestSeedClassificationCache(checks);
    TestSeedCacheTails<float>(checks,VTK_FLOAT); TestSeedCacheTails<double>(checks,VTK_DOUBLE);
    TestSeedCacheTails<unsigned short>(checks,VTK_UNSIGNED_SHORT);
    return checks.failureCount;
}
