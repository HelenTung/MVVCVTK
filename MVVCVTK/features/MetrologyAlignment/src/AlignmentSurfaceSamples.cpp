#include "AlignmentSurfaceSamples.h"
#include "AlignmentMath.h"
#include "AlignmentAllocation.h"
#include <unordered_map>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
using namespace AlignmentMemory;
void Require(bool value, const char *message) { if (!value) throw std::invalid_argument(message); }
void CheckWork(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Surface sampling cancelled.");
}
std::int8_t Direction(const Vec &n) {
    if (!Finite(n) || cv::norm(n) <= 0) return 6;
    int axis = 0;
    for (int i = 1; i < 3; ++i) if (std::abs(n[i]) > std::abs(n[axis])) axis = i;
    return static_cast<std::int8_t>(2 * axis + (n[axis] < 0 ? 1 : 0));
}
const MeshField *Attribute(const SurfaceMeshPayload &mesh, const char *name, std::size_t components) {
    for (const auto &attribute : mesh.GetPointFields()) if (attribute.name == name) {
        Require(attribute.componentCount == components && attribute.values.size() == mesh.GetPointValues().size() / 3 * components,
                "Invalid surface grouping attribute.");
        return &attribute;
    }
    return nullptr;
}
using Cell = std::array<std::int64_t, 7>;
struct CellHash final {
    std::size_t operator()(const Cell &cell) const noexcept {
        std::size_t value = 0;
        for (const auto part : cell) value ^= std::hash<std::int64_t>{}(part) + std::size_t{0x9e3779b9} + (value << 6) + (value >> 2);
        return value;
    }
};
} // namespace

AlignmentSurfaceSamples AlignmentSurfaceSampling::Build(const AlignmentWork &work,
    const AlignmentMatrix &initial, std::size_t indexBytes, bool needsDirections) {
    CheckWork(work);
    Require(work.mesh && work.mesh->GetValid() && work.recipe.surfaceFit, "Invalid surface sampling input.");
    const auto &v = work.mesh->GetPointValues();
    const auto &region = work.recipe.surfaceFit->sourceRegion;
    Require(!region || region->vertexIds.empty() || region->pinnedMesh == work.input.mesh,
            "Surface selection belongs to another mesh revision.");
    std::unique_ptr<RoiQuery> roiQuery;if(region && region->roi){const auto found=work.rois.find(*region->roi);Require(found!=work.rois.end(),"Alignment ROI read view is missing.");auto query=found->second->GetQuery();if(query.error!=RoiError::None || !query.query)throw std::bad_alloc();roiQuery=std::move(query.query);}
    const auto count = region && !region->vertexIds.empty() ? region->vertexIds.size() : v.size() / 3;
    const auto limit = std::numeric_limits<std::size_t>::max();
    const auto initialCapacity = region && region->targetBounds ? 0 : count;
    const auto *normals = needsDirections ? Attribute(*work.mesh, "measurement.normal", 3) : nullptr;
    const bool needsFallback = needsDirections && !normals;
    const auto bytes = sizeof(AlignmentSurfaceSample) + (needsFallback ? 1U : 0U);
    if (indexBytes > limit || initialCapacity > (limit - indexBytes) / bytes)
        throw std::length_error("Surface selection size accounting overflows.");
    AlignmentSurfaceSamples output;
    auto &samples = output.points;
    if (region && region->roi && region->vertexIds.empty()) {
        Require(region->pinnedMesh==work.input.mesh,"ROI surface sampling belongs to another mesh revision.");
        const auto found=work.rois.find(*region->roi);Require(found!=work.rois.end(),"Alignment ROI read view is missing.");
        const auto roi=found->second;const auto inverse=Inverse(region->meshToRoi);const auto& triangles=work.mesh->GetTriangleIndices();
        for (std::size_t i=0;i<triangles.size();i+=3) {
            if(i%768==0)CheckWork(work);std::array<AlignmentPoint,3> triangle{};
            for(int j=0;j<3;++j){const auto id=triangles[i+j];triangle[j]=Point(Transform(region->meshToRoi,{v[3*id],v[3*id+1],v[3*id+2]}));}
            const auto selected=roi->GetTriangleSelection(triangle,0,[&]{return AlignmentGeometryFit::GetWorkStatus(work)!=AlignmentStatus::FullyDetermined;});Require(selected.error==RoiError::None,"Alignment ROI triangle selection failed.");
            const bool valid=AlignmentGeometryFit::GetFaceValid(*work.mesh,i/3);
            for(const auto& piece:selected.triangles){
                const AlignmentPoint center{(piece[0][0]+piece[1][0]+piece[2][0])/3,(piece[0][1]+piece[1][1]+piece[2][1])/3,(piece[0][2]+piece[1][2]+piece[2][2])/3};
                if(!roiQuery->GetContains(center))continue;const auto sourcePoint=Point(Transform(inverse,Vector(center)));
                if(region->targetBounds){const auto p=Transform(initial,Vector(sourcePoint));bool inside=true;for(int a=0;a<3;++a)if(p[a]<(*region->targetBounds)[a*2] || p[a]>(*region->targetBounds)[a*2+1])inside=false;if(!inside)continue;}
                const auto a=Transform(inverse,Vector(piece[0])),b=Transform(inverse,Vector(piece[1])),c=Transform(inverse,Vector(piece[2]));
                const auto cross=(b-a).cross(c-a);const double area=cv::norm(cross)*.5;if(!(area>0))continue;
                const bool usable=valid && AlignmentGeometryFit::GetPointUsable(work,triangles[i]) && AlignmentGeometryFit::GetPointUsable(work,triangles[i+1]) && AlignmentGeometryFit::GetPointUsable(work,triangles[i+2]);
                AlignmentSurfaceSample sample{triangles[i],area,usable?area:0,sourcePoint};
                if(normals){Vec normal{};for(int j=0;j<3;++j){const auto id=triangles[i+j];normal+=Vec(normals->values[3*id],normals->values[3*id+1],normals->values[3*id+2]);}const double length=cv::norm(normal);if(length>0)sample.sourceNormal=Point(normal/length);}
                samples.push_back(std::move(sample));if(needsFallback)output.directions.push_back(Direction(cross));
            }
        }
        Require(!samples.empty(),"Surface alignment ROI is empty.");return output;
    }
    samples.reserve(initialCapacity);
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 256 == 0) CheckWork(work);
        const auto id = region && !region->vertexIds.empty() ? region->vertexIds[i] : i;
        Require(id < v.size() / 3, "Surface selection is out of bounds.");
        if(region && region->roi){const auto found=work.rois.find(*region->roi);Require(found!=work.rois.end(),"Alignment ROI read view is missing.");if(!roiQuery->GetContains(Point(Transform(region->meshToRoi,{v[3*id],v[3*id+1],v[3*id+2]}))))continue;}
        if (region && region->targetBounds) {
            const auto p = Transform(initial, {v[3 * id], v[3 * id + 1], v[3 * id + 2]});
            bool inside = true;
            for (std::size_t axis = 0; axis < 3; ++axis)
                inside = inside && p[static_cast<int>(axis)] >= (*region->targetBounds)[2 * axis] &&
                         p[static_cast<int>(axis)] <= (*region->targetBounds)[2 * axis + 1];
            if (!inside) continue;
        }
        if (samples.size() == samples.capacity()) {
            const auto capacity = samples.capacity() == 0 ? std::size_t{1} : samples.capacity() + std::min(samples.capacity(), count - samples.capacity());
            if (capacity > (limit - indexBytes) / bytes - samples.capacity())
                throw std::length_error("Surface region growth size accounting overflows.");
            samples.reserve(capacity);
        }
        samples.push_back({static_cast<std::uint64_t>(id), 0});
    }
    Require(!samples.empty(), "Surface alignment region is empty.");
    if (region && !region->vertexIds.empty()) {
        std::size_t comparisons = 0;
        std::sort(samples.begin(), samples.end(), [&](const auto &a, const auto &b) {
            if (++comparisons % 256 == 0) CheckWork(work); return a.vertex < b.vertex;
        });
        for (std::size_t i = 1; i < samples.size(); ++i)
            Require(samples[i - 1].vertex != samples[i].vertex, "Duplicate surface selection.");
    }
    if (needsFallback) output.directions.assign(samples.size(), -1);
    const bool direct = samples.size() == v.size() / 3;
    const auto &triangles = work.mesh->GetTriangleIndices();
    Require(!triangles.empty(), "Surface fitting requires triangle connectivity.");
    for (std::size_t i = 0; i < triangles.size(); i += 3) {
        if (i % 768 == 0) CheckWork(work);
        std::array<std::vector<AlignmentSurfaceSample>::iterator,3> selected;
        bool contributes = false;
        for (std::size_t j = 0; j < 3; ++j) {
            const auto id = triangles[i+j];
            selected[j] = direct ? samples.begin()+static_cast<std::size_t>(id)
                : std::lower_bound(samples.begin(), samples.end(), id, [](const auto& sample, auto value) { return sample.vertex < value; });
            contributes = contributes || (selected[j] != samples.end() && selected[j]->vertex == id);
        }
        if (!contributes) continue;
        const bool valid = AlignmentGeometryFit::GetFaceValid(*work.mesh, i / 3);
        const auto point = [&](std::uint64_t id) { return Vec(v[3 * id], v[3 * id + 1], v[3 * id + 2]); };
        const auto cross = (point(triangles[i + 1]) - point(triangles[i])).cross(point(triangles[i + 2]) - point(triangles[i]));
        const double area = cv::norm(cross) / 6;
        Require(std::isfinite(area) && (area > 0 || !valid), "Source contains a degenerate triangle.");
        for (std::size_t j = 0; j < 3; ++j) {
            const auto id = triangles[i + j];
            const auto found = selected[j];
            if (found != samples.end() && found->vertex == id) {
                found->area += area;
                if (valid) found->validArea += area;
                if (needsFallback && valid) {
                    auto &direction = output.directions[static_cast<std::size_t>(found - samples.begin())];
                    if (direction < 0) direction = Direction(cross);
                }
            }
        }
    }
    for (const auto &sample : samples) Require(std::isfinite(sample.area) && sample.area > 0, "Selected vertex has no surface area.");
    return output;
}

std::vector<AlignmentSurfaceSample> AlignmentSurfaceSampling::Reduce(const AlignmentWork &work,
    const AlignmentSurfaceSamples &source, double voxelSize, std::size_t indexBytes) {
    Require(std::isfinite(voxelSize) && voxelSize > 0, "Invalid surface voxel size.");
    CheckWork(work);
    const auto &v = work.mesh->GetPointValues();
    const auto *normals = Attribute(*work.mesh, "measurement.normal", 3);
    const auto *interfaces = Attribute(*work.mesh, "surface.interface-index", 1);
    const auto *overrides = Attribute(*work.mesh, "surface.override-index", 1);
    Require(normals || source.directions.size() == source.points.size(), "Surface direction groups are missing.");
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (indexBytes > limit || source.GetWorkingBytes() > limit - indexBytes)
        throw std::length_error("Surface preprocessing size accounting overflows.");
    AllocationAccounting budget{limit - indexBytes - source.GetWorkingBytes(), 0};
    using Entry = std::pair<const Cell, std::size_t>;
    std::unordered_map<Cell, std::size_t, CellHash, std::equal_to<Cell>, AccountingAllocator<Entry>>
        groups(0, CellHash{}, std::equal_to<Cell>{}, AccountingAllocator<Entry>{&budget});
    std::vector<AlignmentSurfaceSample> result;
    const Vec origin(v[0], v[1], v[2]);
    const auto regionId = [](const MeshField *attribute, std::uint64_t id) -> std::int64_t {
        if (!attribute) return 0;
        const double value = attribute->values[static_cast<std::size_t>(id)];
        Require(value >= 0 && value <= std::numeric_limits<std::uint32_t>::max() && std::floor(value) == value,
                "Invalid interface or override identity.");
        return static_cast<std::int64_t>(value);
    };
    for (std::size_t i = 0; i < source.points.size(); ++i) {
        if (i % 256 == 0) CheckWork(work);
        const auto &sample = source.points[i];
        const auto id = sample.vertex;
        const auto position=sample.sourcePosition.value_or(AlignmentPoint{v[3*id],v[3*id+1],v[3*id+2]});
        Cell key{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double cell = std::floor((position[axis] - origin[static_cast<int>(axis)]) / voxelSize);
            // 保持坐标格编号可精确传递，避免 double 到 int64 的边界溢出。
            Require(std::isfinite(cell) && std::abs(cell) <= 9007199254740991.0, "Surface grid coordinate exceeds exact integer range.");
            key[axis] = static_cast<std::int64_t>(cell);
        }
        key[3] = sample.sourceNormal ? Direction(Vector(*sample.sourceNormal)):normals ? Direction({normals->values[3 * id], normals->values[3 * id + 1], normals->values[3 * id + 2]}) : source.directions[i];
        key[4] = sample.validArea > 0 && AlignmentGeometryFit::GetPointUsable(work, static_cast<std::size_t>(id)) ? 1 : 0;
        key[5] = regionId(interfaces, id); key[6] = regionId(overrides, id);
        const auto found = groups.find(key);
        if (found == groups.end()) {
            if (result.size() == result.capacity()) {
                const auto oldCapacity = result.capacity();
                const auto capacity = oldCapacity == 0 ? std::size_t{1}
                    : oldCapacity + std::min(oldCapacity, source.points.size() - oldCapacity);
                if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(AlignmentSurfaceSample))
                    throw std::length_error("Surface sample capacity overflows.");
                budget.Acquire(capacity * sizeof(AlignmentSurfaceSample));
                result.reserve(capacity);
                budget.Release(oldCapacity * sizeof(AlignmentSurfaceSample));
            }
            groups.emplace(key, result.size()); result.push_back(sample);
        } else {
            auto &representative = result[found->second];
            if (!representative.sourcePosition && !sample.sourcePosition) representative.vertex = std::min(representative.vertex, sample.vertex);
            representative.area += sample.area;
            representative.validArea += sample.validArea;
            Require(std::isfinite(representative.area), "Surface group area overflows.");
        }
    }
    std::size_t comparisons = 0;
    std::sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
        if (++comparisons % 256 == 0) CheckWork(work); return a.vertex < b.vertex;
    });
    return result;
}
