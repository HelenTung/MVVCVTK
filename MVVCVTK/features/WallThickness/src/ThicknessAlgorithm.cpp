#include "FeatureSupport/WorkLimit.h"
#include "ThicknessAlgorithm.h"
#include "ThicknessMath.h"
#include "ThicknessGrayField.h"
#include "ThicknessProfile.h"
#include "ThicknessDisplaySampling.h"
#ifdef MVVCVTK_HAS_WALL_CUDA
#include "ThicknessCudaPath.h"
#endif
#include <exception>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace ThicknessAlgorithm
{
namespace
{
using namespace ThicknessMath;
constexpr double pi = 3.14159265358979323846;
struct Failure final
{
    ThicknessStatus status;
    const char *message;
};
void CheckWork(const Work &work)
{
    if (work.cancelled && work.cancelled->load(std::memory_order_relaxed))
        throw Failure{ThicknessStatus::Cancelled, "Thickness analysis cancelled."};
    if (std::chrono::steady_clock::now() >= work.deadline)
        throw Failure{ThicknessStatus::DeadlineExceeded, "Thickness deadline exceeded."};
}
bool Positive(double v)
{
    return std::isfinite(v) && v > 0;
}
bool Nonnegative(double v)
{
    return std::isfinite(v) && v >= 0;
}
bool GetProfileEnabled()
{
    char* value=nullptr; std::size_t length=0;
    const auto status=_dupenv_s(&value,&length,"MVVCVTK_WALL_PROFILE");
    const bool enabled=status==0 && value && std::strcmp(value,"1")==0;
    std::free(value); return enabled;
}

template <class T> double ReadScalar(const std::uint8_t *bytes, std::size_t index)
{
    T value{};
    std::memcpy(&value, bytes + index * sizeof(T), sizeof(T));
    return static_cast<double>(value);
}

class Kernel final
{
    const Work &m_work;
    const ThicknessParams &m_params;
    const GridGeometry3D &m_geometry;
    const SurfaceMeshPayload &m_mesh;
    double (*m_readScalar)(const std::uint8_t *, std::size_t) = nullptr;
    ThicknessGrayField::Field m_materialField;
    std::array<std::size_t,3> m_uniformDimensions{};
    std::vector<std::int8_t> m_uniformSigns;
    // 只细化混合 8³ 块：4³、2³、1³ 共 584 个证书，缓存上限 256 MiB。
    std::vector<std::uint32_t> m_uniformChildOffsets;
    std::vector<std::array<std::int8_t,584>> m_uniformChildren;
    bool m_hasRestrictedLabels = false;
    std::vector<double> m_areas;
    unsigned m_n = 1;
    std::size_t m_sampleCount = 0;
    double m_epsilon = 0, m_minSpacing = 0;
    struct SourceMeasurement { std::size_t triangle = 0; unsigned rule = 0; std::optional<double> value; };
#ifdef MVVCVTK_HAS_WALL_CUDA
    std::unique_ptr<ThicknessCudaPath::Client> m_cuda;
    std::vector<std::uint8_t> m_cudaSupport;
    void BuildCuda() {

        ThicknessCudaPath::Input input;
        input.extent=m_geometry.extent;input.blocks=m_uniformDimensions;input.threshold=*m_params.materialThreshold;
        switch (m_work.source->GetValueType()) {
        case ImageValueType::Int8:input.scalarType=ThicknessCudaPath::ScalarType::Int8;break;
        case ImageValueType::UInt8:input.scalarType=ThicknessCudaPath::ScalarType::UInt8;break;
        case ImageValueType::Int16:input.scalarType=ThicknessCudaPath::ScalarType::Int16;break;
        case ImageValueType::UInt16:input.scalarType=ThicknessCudaPath::ScalarType::UInt16;break;
        case ImageValueType::Int32:input.scalarType=ThicknessCudaPath::ScalarType::Int32;break;
        case ImageValueType::UInt32:input.scalarType=ThicknessCudaPath::ScalarType::UInt32;break;
        case ImageValueType::Float32:input.scalarType=ThicknessCudaPath::ScalarType::Float32;break;
        case ImageValueType::Float64:input.scalarType=ThicknessCudaPath::ScalarType::Float64;break;
        default:return;
        }
        input.values=m_work.source->GetValues()->data();input.valueBytes=m_work.source->GetValues()->size();
        if (const auto& mask=m_work.source->GetValidityMask()) {input.mask=mask->data();input.maskBytes=mask->size();}
        if (m_hasRestrictedLabels) {
            const auto count=std::visit([](const auto& values){return values->size();},m_work.labels->GetValues());
            m_cudaSupport.resize(count);
            std::visit([&](const auto& values){
                using Value=typename std::decay_t<decltype(*values)>::value_type;
                const auto& selected=m_work.archive.input.materialLabels;
                for(std::size_t i=0;i<count;++i){
                    if((i&65535U)==0)CheckWork(m_work);
                    const auto v=(*values)[i];bool valid=!input.mask||input.mask[i]!=0;
                    if constexpr(std::is_signed_v<Value>) if(v<0)valid=false;
                    const auto label=static_cast<std::uint64_t>(v);
                    if(label && !selected.empty() && std::find(selected.begin(),selected.end(),label)==selected.end())valid=false;
                    m_cudaSupport[i]=valid?1:0;
                }
            },m_work.labels->GetValues());
            input.mask=m_cudaSupport.data();input.maskBytes=m_cudaSupport.size();
        }
        if (m_materialField.uniformRegion) {
            input.signs=m_uniformSigns.data();input.signCount=m_uniformSigns.size();
            input.offsets=m_uniformChildOffsets.data();input.offsetCount=m_uniformChildOffsets.size();
            input.children=m_uniformChildren.data();input.childBytes=m_uniformChildren.size()*sizeof(m_uniformChildren[0]);
        }
        m_cuda=ThicknessCudaPath::Create(input);Profile(m_cuda?"cuda-ready":"cuda-unavailable");
    }
#endif
    WorkLimit m_nodeBudget;
    std::size_t m_sourceCapacity = 0, m_workerCount = 1;
    bool m_profile = GetProfileEnabled();
    ThicknessProfile::Data m_profileData;
    std::chrono::steady_clock::time_point m_profileBegin, m_profileLast;
    void Profile(const char* phase, std::size_t completed=0, std::size_t total=0) {
        if (!m_profile) return;
        m_profileLast=std::chrono::steady_clock::now();
        ThicknessProfile::Print(phase,std::chrono::duration<double>(m_profileLast-m_profileBegin).count(),completed,total,m_profileData);
    }

    bool GetIsSourceLocal() const
    {
        return m_params.boundaryPolicy == ThicknessBoundaryPolicy::SourceExtentLocal;
    }
    bool GetInsideExtent(const std::array<std::int64_t, 3> &index) const
    {
        for (unsigned a = 0; a < 3; ++a)
            if (index[a] < m_geometry.extent[2 * a] || index[a] > m_geometry.extent[2 * a + 1])
                return false;
        return true;
    }
    bool GetInsideSource(const ThicknessPoint &point, double margin) const
    {
        const auto index = Index(point);
        if (!Finite(index)) return false;
        for (unsigned a = 0; a < 3; ++a)
            if (index[a] < m_geometry.extent[2 * a] + margin / m_geometry.spacing[a] ||
                index[a] > m_geometry.extent[2 * a + 1] - margin / m_geometry.spacing[a])
                return false;
        return true;
    }
    ThicknessGrayField::Field GetMaterialField() const
    {
        ThicknessGrayField::Field field;
        field.extent = m_geometry.extent;
        field.threshold = *m_params.materialThreshold;
        field.check = [this] { CheckWork(m_work); };
        field.node = [this](const std::array<std::int64_t, 3> &index, double &value)
        {
            if (!GetInsideExtent(index) || (m_hasRestrictedLabels && Material(index) == -1)) return false;
            std::size_t offset = 0, stride = 1;
            for (unsigned a = 0; a < 3; ++a)
            {
                offset += std::size_t(index[a] - m_geometry.extent[2 * a]) * stride;
                stride *= std::size_t(m_geometry.dimensions[a]);
            }
            const auto &mask = m_work.source->GetValidityMask();
            if (mask && (*mask)[offset] == 0) return false;
            // 路径始终读原始源灰度，不沿用表面生成阶段的饱和值。
            value = m_readScalar(m_work.source->GetValues()->data(), offset);
            return std::isfinite(value);
        };
        return field;
    }

    void BuildUniformGrid()
    {
        if (m_uniformSigns.empty()) return;
        std::atomic<bool> failed{false};std::exception_ptr failure;
        const auto run=[&](std::size_t worker) {
            try {
                for (std::size_t block=worker;block<m_uniformSigns.size();block+=m_workerCount) {
                    if (failed.load(std::memory_order_relaxed)) return;
                    CheckWork(m_work);
                    ThicknessGrayField::Index low{},high{};
                    auto index=block;
                    for (unsigned axis=0;axis<3;++axis) {
                        low[axis]=m_geometry.extent[2*axis]+8*static_cast<std::int64_t>(index%m_uniformDimensions[axis]);
                        high[axis]=std::min(low[axis]+8,std::int64_t(m_geometry.extent[2*axis+1]));
                        index/=m_uniformDimensions[axis];
                    }
                    int sign=0;bool uniform=true;
                    double minimum=std::numeric_limits<double>::infinity(),maximum=-minimum;
                    // 8³ 单元需要全部 9³ 角点；缺失、等阈值或异号都回退细遍历。
                    for (auto z=low[2];z<=high[2] && uniform;++z)
                        for (auto y=low[1];y<=high[1] && uniform;++y)
                            for (auto x=low[0];x<=high[0];++x) {
                                double value=0;
                                if (!m_materialField.node({x,y,z},value)) {uniform=false;break;}
                                const int current=value>*m_params.materialThreshold?1:value<*m_params.materialThreshold?-1:0;
                                if (!current || (sign && sign!=current)) {uniform=false;break;}
                                sign=current;minimum=std::min(minimum,value);maximum=std::max(maximum,value);
                            }
                    if (uniform) {
                        const double scale=std::max({std::abs(minimum),std::abs(maximum),std::abs(*m_params.materialThreshold)});
                        const double margin=4096*std::numeric_limits<double>::epsilon();
                        // 射线多项式靠近阈值时不使用块跳跃，保留原数值消歧路径。
                        if (scale>0 && (sign>0 ? minimum/scale-*m_params.materialThreshold/scale>margin
                            : *m_params.materialThreshold/scale-maximum/scale>margin))
                            m_uniformSigns[block]=static_cast<std::int8_t>(sign);
                    }
                }
            } catch (...) {if (!failed.exchange(true)) failure=std::current_exception();}
        };
        std::vector<std::thread> workers;workers.reserve(m_workerCount-1);
        try {for (std::size_t i=1;i<m_workerCount;++i) workers.emplace_back(run,i);}
        catch (...) {failed.store(true);for (auto& worker:workers) worker.join();throw;}
        run(0);for (auto& worker:workers) worker.join();if (failure) std::rethrow_exception(failure);
        const auto mixed=std::count(m_uniformSigns.begin(),m_uniformSigns.end(),std::int8_t(0));
        if (mixed && m_uniformSigns.size()<(256U*1024U*1024U)/sizeof(std::uint32_t)
            && std::size_t(mixed)<(256U*1024U*1024U-m_uniformSigns.size()*sizeof(std::uint32_t))/584U) {
            try {
                m_uniformChildOffsets.resize(m_uniformSigns.size(),std::numeric_limits<std::uint32_t>::max());
                m_uniformChildren.resize(static_cast<std::size_t>(mixed));
            } catch (const std::bad_alloc&) {m_uniformChildOffsets.clear();m_uniformChildren.clear();}
        }
        if (!m_uniformChildren.empty()) {
            std::uint32_t offset=0;
            for (std::size_t block=0;block<m_uniformSigns.size();++block)
                if (!m_uniformSigns[block]) m_uniformChildOffsets[block]=offset++;
            const auto refine=[&](std::size_t worker) {
                try {
                    struct Proof {double minimum=std::numeric_limits<double>::infinity(),maximum=-std::numeric_limits<double>::infinity();bool valid=true,used=false;};
                    const auto merge=[](Proof& parent,const Proof& child) {
                        if (!child.used) return;parent.used=true;parent.valid=parent.valid && child.valid;
                        parent.minimum=std::min(parent.minimum,child.minimum);parent.maximum=std::max(parent.maximum,child.maximum);
                    };
                    const auto sign=[&](const Proof& proof) -> std::int8_t {
                        if (!proof.valid || !proof.used) return 0;
                        const auto threshold=*m_params.materialThreshold;
                        const double scale=std::max({std::abs(proof.minimum),std::abs(proof.maximum),std::abs(threshold)});
                        const double margin=4096*std::numeric_limits<double>::epsilon();
                        if (scale>0 && proof.minimum>threshold && proof.minimum/scale-threshold/scale>margin) return 1;
                        if (scale>0 && proof.maximum<threshold && threshold/scale-proof.maximum/scale>margin) return -1;
                        return 0;
                    };
                    for (std::size_t block=worker;block<m_uniformSigns.size();block+=m_workerCount) {
                        if (failed.load(std::memory_order_relaxed)) return;
                        if (m_uniformSigns[block]) continue;
                        CheckWork(m_work);
                        ThicknessGrayField::Index low{},high{};auto index=block;
                        for (unsigned axis=0;axis<3;++axis) {
                            low[axis]=m_geometry.extent[2*axis]+8*static_cast<std::int64_t>(index%m_uniformDimensions[axis]);
                            high[axis]=std::min(low[axis]+8,std::int64_t(m_geometry.extent[2*axis+1]));index/=m_uniformDimensions[axis];
                        }
                        std::array<double,729> values{};std::array<bool,729> valid{};
                        for (auto z=low[2];z<=high[2];++z) for (auto y=low[1];y<=high[1];++y) for (auto x=low[0];x<=high[0];++x) {
                            const auto node=std::size_t(x-low[0]+9*(y-low[1]+9*(z-low[2])));
                            valid[node]=m_materialField.node({x,y,z},values[node]);
                        }
                        auto& children=m_uniformChildren[m_uniformChildOffsets[block]];
                        for (unsigned octant=0;octant<8;++octant) {
                            Proof four;
                            for (unsigned sub=0;sub<8;++sub) {
                                Proof two;
                                for (unsigned leaf=0;leaf<8;++leaf) {
                                    std::array<unsigned,3> p{};
                                    for (unsigned axis=0;axis<3;++axis) p[axis]=4*((octant>>axis)&1U)+2*((sub>>axis)&1U)+((leaf>>axis)&1U);
                                    Proof one;
                                    for (unsigned axis=0;axis<3;++axis) if (low[axis]+p[axis]>=high[axis]) one.valid=false;
                                    if (one.valid) {
                                        one.used=true;
                                        for (unsigned corner=0;corner<8;++corner) {
                                            const auto n=p[0]+(corner&1U)+9*(p[1]+((corner>>1)&1U)+9*(p[2]+((corner>>2)&1U)));
                                            one.valid=one.valid && valid[n];one.minimum=std::min(one.minimum,values[n]);one.maximum=std::max(one.maximum,values[n]);
                                        }
                                    }
                                    children[72+octant*64+sub*8+leaf]=sign(one);merge(two,one);
                                }
                                children[8+octant*8+sub]=sign(two);merge(four,two);
                            }
                            children[octant]=sign(four);
                        }
                    }
                } catch (...) {if (!failed.exchange(true)) failure=std::current_exception();}
            };
            workers.clear();
            try {for (std::size_t i=1;i<m_workerCount;++i) workers.emplace_back(refine,i);}
            catch (...) {failed.store(true);for (auto& worker:workers) worker.join();throw;}
            refine(0);for (auto& worker:workers) worker.join();if (failure) std::rethrow_exception(failure);
        }
        if (m_uniformChildren.empty() && std::none_of(m_uniformSigns.begin(),m_uniformSigns.end(),[](auto sign){return sign!=0;})) return;
        m_materialField.uniformRegion=[this](const ThicknessGrayField::Index& cell) {
            ThicknessGrayField::Field::UniformRegion region;
            std::size_t offset=0,stride=1;
            for (unsigned axis=0;axis<3;++axis) {
                if (cell[axis]<m_geometry.extent[2*axis] || cell[axis]>=m_geometry.extent[2*axis+1]) return region;
                const auto block=std::size_t(cell[axis]-m_geometry.extent[2*axis])/8;
                offset+=block*stride;stride*=m_uniformDimensions[axis];
                const auto low=std::int64_t(m_geometry.extent[2*axis])+8*static_cast<std::int64_t>(block);
                region.extent[2*axis]=static_cast<int>(low);
                region.extent[2*axis+1]=static_cast<int>(std::min(low+8,std::int64_t(m_geometry.extent[2*axis+1])));
            }
            region.sign=m_uniformSigns[offset];
            if (region.sign || m_uniformChildren.empty()) return region;
            const auto child=m_uniformChildOffsets[offset];
            if (child==std::numeric_limits<std::uint32_t>::max()) return region;
            const auto& signs=m_uniformChildren[child];
            std::array<unsigned,3> keys{};
            for (unsigned level=0,size=4;level<3;++level,size/=2) {
                unsigned key=0;
                for (unsigned axis=0;axis<3;++axis) {
                    const auto half=unsigned(cell[axis]-region.extent[2*axis])/size;
                    key|=half<<axis;region.extent[2*axis]+=int(half*size);
                    region.extent[2*axis+1]=std::min(region.extent[2*axis]+int(size),region.extent[2*axis+1]);
                }
                keys[level]=key;
                const auto node=level==0 ? keys[0] : level==1 ? 8+keys[0]*8+keys[1] : 72+keys[0]*64+keys[1]*8+keys[2];
                region.sign=signs[node];if (region.sign) break;
            }
            return region;
        };
    }

    ThicknessPoint Vertex(std::uint64_t id) const
    {
        const auto *v = m_mesh.GetVertices().data() + id * 3;
        return Sub({v[0], v[1], v[2]}, m_geometry.origin);
    }
    ThicknessPoint Interpolate(std::uint64_t triangle, const ThicknessPoint &bary) const
    {
        ThicknessPoint p{};
        for (std::size_t k = 0; k < 3; ++k)
            p = Add(p, Scale(Vertex(m_mesh.GetTriangles()[triangle * 3 + k]), bary[k]));
        return p;
    }
    ThicknessPoint Index(const ThicknessPoint &p) const
    {
        ThicknessPoint r{};
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t k = 0; k < 3; ++k)
                r[a] += m_geometry.direction[k * 3 + a] * p[k] / m_geometry.spacing[a];
        return r;
    }
    ThicknessPoint Model(const ThicknessPoint &i) const
    {
        ThicknessPoint r{};
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t k = 0; k < 3; ++k)
                r[a] += m_geometry.direction[a * 3 + k] * i[k] * m_geometry.spacing[k];
        return r;
    }
    // -2缺失支持，-1其他材料/未知，0背景，1选定材料标签集合。整数不经double。
    int Material(const std::array<std::int64_t, 3> &i) const
    {
        std::size_t offset = 0, stride = 1;
        for (std::size_t a = 0; a < 3; ++a)
        {
            if (i[a] < m_geometry.extent[a * 2] || i[a] > m_geometry.extent[a * 2 + 1])
                return -2;
            offset += static_cast<std::size_t>(i[a] - m_geometry.extent[a * 2]) * stride;
            stride *= static_cast<std::size_t>(m_geometry.dimensions[a]);
        }
        const auto &mask = m_work.source->GetValidityMask();
        if (mask && (*mask)[offset] == 0)
            return -2;
        return std::visit(
            [&](const auto &values)
            {
                using Value = typename std::decay_t<decltype(*values)>::value_type;
                const auto v = (*values)[offset];
                if constexpr (std::is_signed_v<Value>)
                {
                    if (v < 0)
                        return -1;
                }
                const auto label = static_cast<std::uint64_t>(v);
                if (label == 0) return 0;
                const auto &selected = m_work.archive.input.materialLabels;
                return selected.empty() || std::find(selected.begin(), selected.end(), label) != selected.end()
                           ? 1 : -1;
            },
            m_work.labels->GetValues());
    }
    void ValidateInput()
    {
        if (!m_work.source->GetValid() || !m_work.labels->GetValid() || !m_mesh.GetValid() ||
            m_mesh.GetVertices().empty() || m_mesh.GetTriangles().empty())
            throw Failure{ThicknessStatus::InvalidInput, "Invalid input payload."};
        const auto &g = m_work.source->GetGeometry();
        if (m_work.source->GetComponentCount() != 1 ||
            std::any_of(g.dimensions.begin(), g.dimensions.end(), [](int n) { return n < 2; }))
            throw Failure{ThicknessStatus::InvalidInput, "A scalar grid with at least two nodes per axis is required."};
        switch (m_work.source->GetValueType())
        {
        case ImageValueType::Int8: m_readScalar = ReadScalar<std::int8_t>; break;
        case ImageValueType::UInt8: m_readScalar = ReadScalar<std::uint8_t>; break;
        case ImageValueType::Int16: m_readScalar = ReadScalar<std::int16_t>; break;
        case ImageValueType::UInt16: m_readScalar = ReadScalar<std::uint16_t>; break;
        case ImageValueType::Int32: m_readScalar = ReadScalar<std::int32_t>; break;
        case ImageValueType::UInt32: m_readScalar = ReadScalar<std::uint32_t>; break;
        case ImageValueType::Float32: m_readScalar = ReadScalar<float>; break;
        case ImageValueType::Float64: m_readScalar = ReadScalar<double>; break;
        default: throw Failure{ThicknessStatus::InvalidInput, "Unsupported exact scalar representation."};
        }
        if (g.extent != m_geometry.extent || g.spacing != m_geometry.spacing ||
            g.origin != m_geometry.origin || g.direction != m_geometry.direction ||
            g.coordinateFrame != m_geometry.coordinateFrame ||
            m_mesh.GetCoordinateFrame() != m_geometry.coordinateFrame)
            throw Failure{ThicknessStatus::InvalidInput, "Input geometries or frames differ."};
        for (std::size_t a = 0; a < 3; ++a)
            for (std::size_t b = 0; b < 3; ++b)
            {
                double dot = 0;
                for (std::size_t k = 0; k < 3; ++k)
                    dot += g.direction[k * 3 + a] * g.direction[k * 3 + b];
                if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 1e-8)
                    throw Failure{ThicknessStatus::InvalidInput,
                                  "Grid direction must be orthonormal."};
            }
        m_minSpacing = *std::min_element(g.spacing.begin(), g.spacing.end());
        if (m_params.maxBoundaryError > 0.5 * m_minSpacing)
            throw Failure{ThicknessStatus::InvalidInput,
                          "Endpoint error exceeds half minimum spacing."};
        m_hasRestrictedLabels = !m_work.archive.input.materialLabels.empty() || std::visit(
            [](const auto& values) { using Value=typename std::decay_t<decltype(*values)>::value_type;
                return std::is_signed_v<Value>; }, m_work.labels->GetValues());
    }
    void BuildSampling()
    {
        const auto pc=m_mesh.GetVertices().size()/3, tc=m_mesh.GetTriangles().size()/3;
        const auto& limits=m_work.archive.limits;
        auto minimum=Vertex(0), maximum=minimum;
        for (std::size_t i=0;i<pc;++i) {
            if ((i&255U)==0) CheckWork(m_work);
            const auto p=Vertex(i);
            if (!Finite(p)) throw Failure{ThicknessStatus::InvalidInput,"Coordinate overflow."};
            for (unsigned a=0;a<3;++a) {minimum[a]=std::min(minimum[a],p[a]);maximum[a]=std::max(maximum[a],p[a]);}
        }
        m_epsilon=std::max(64*std::numeric_limits<double>::epsilon()*std::max(Length(Sub(maximum,minimum)),1.),m_minSpacing*1e-8);
        for (double origin:m_geometry.origin) m_epsilon=std::max(m_epsilon,8*std::numeric_limits<double>::epsilon()*std::abs(origin));
        if (!std::isfinite(m_epsilon) || m_epsilon>=.01*m_minSpacing)
            throw Failure{ThicknessStatus::InvalidInput,"Coordinate precision cannot resolve the input spacing."};
        if (tc>WorkLimit(limits.maxWorkingBytes)/sizeof(double))
            throw Failure{ThicknessStatus::BudgetExceeded,"Sampling geometry exceeds working budget."};
        m_areas.reserve(tc);
        double maxEdge=0;
        const auto perSample=sizeof(ThicknessSample)+640U;
        const auto areaBytes=tc*sizeof(double);
        const auto sampleBudget=WorkLimit(limits.maxSamples).GetBound(
            ((WorkLimit(limits.maxWorkingBytes)-areaBytes)/perSample).GetBound(std::numeric_limits<std::size_t>::max()));
        for (std::size_t i=0;i<tc;++i) {
            if ((i&255U)==0) CheckWork(m_work);
            std::array<ThicknessPoint,3> p{};
            for (unsigned k=0;k<3;++k) p[k]=Vertex(m_mesh.GetTriangles()[i*3+k]);
            const double area=.5*Length(Cross(Sub(p[1],p[0]),Sub(p[2],p[0])));
            if (!std::isfinite(area)) throw Failure{ThicknessStatus::InvalidInput,"Triangle area overflow."};
            // 退化的种子面没有正面积来源；不使其他局部来源整体失败。
            m_areas.push_back(area>m_epsilon*m_epsilon?area:0.);
            if (area>m_epsilon*m_epsilon) for (unsigned k=0;k<3;++k) maxEdge=std::max(maxEdge,Length(Sub(p[k],p[(k+1)%3])));
            if (m_areas.back()>0) ThicknessDisplaySampling::Visit(p,m_params.sampleSpacing,[&]{CheckWork(m_work);},
                [&](const auto&) {
                    if (m_sampleCount==sampleBudget) throw Failure{ThicknessStatus::BudgetExceeded,"Local display sampling exceeds working budget."};
                    ++m_sampleCount;
                });
        }
        const double divisions=std::ceil(maxEdge/m_params.sampleSpacing);
        if (!std::isfinite(divisions) || divisions>=std::numeric_limits<unsigned>::max())
            throw Failure{ThicknessStatus::BudgetExceeded,"Surface subdivision limit exceeded."};
        m_n=static_cast<unsigned>(std::max(divisions,1.));
        if (tc>std::numeric_limits<std::size_t>::max()/9U)
            throw Failure{ThicknessStatus::InvalidInput,"Sample count overflows."};
        if (m_sampleCount>WorkLimit(limits.maxSamples) || m_sampleCount>(WorkLimit(limits.maxWorkingBytes)-areaBytes)/perSample)
            throw Failure{ThicknessStatus::BudgetExceeded,"Sampling exceeds working budget."};
        const auto remaining=WorkLimit(limits.maxWorkingBytes)-areaBytes-m_sampleCount*perSample;
        m_sourceCapacity=WorkLimit(limits.maxSamples).GetBound(tc*9U);
        m_workerCount=std::min(std::size_t(32),std::size_t(std::max(1U,std::thread::hardware_concurrency())));
        const auto batchBytes=std::min(m_sourceCapacity,std::size_t(4096U*9U))*sizeof(SourceMeasurement);
        if (remaining<batchBytes) throw Failure{ThicknessStatus::BudgetExceeded,"Source batch exceeds working budget."};
        std::size_t blocks=1;
        for (unsigned axis=0;axis<3;++axis) {
            m_uniformDimensions[axis]=(std::size_t(m_geometry.dimensions[axis])-2)/8+1;
            if (m_uniformDimensions[axis]>std::numeric_limits<std::size_t>::max()/blocks) {blocks=0;break;}
            blocks*=m_uniformDimensions[axis];
        }
        // 缓存为可选优化；显式字节上限下保留原预算准入与细遍历。
        if (blocks && !WorkLimit(limits.maxWorkingBytes).GetValue()) {
            try {m_uniformSigns.resize(blocks);}
            catch (const std::bad_alloc&) {m_uniformSigns.clear();}
        }
        m_nodeBudget=(remaining-batchBytes-m_uniformSigns.size())/192U;
    }
    struct NodeWeight { std::array<int, 3> index{}; double weight = 0; };
    std::array<NodeWeight, 8> GetNodeWeights(const ThicknessPoint &point) const
    {
        std::array<NodeWeight, 8> result{};
        const auto index = Index(point);
        if (!Finite(index)) throw Failure{ThicknessStatus::InvalidInput, "Source index overflow."};
        if (!GetInsideSource(point, 0)) return result;
        std::array<std::int64_t, 3> cell{};
        for (unsigned a = 0; a < 3; ++a) cell[a] = static_cast<std::int64_t>(std::floor(index[a]));
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            auto node = cell;
            double weight = 1;
            for (unsigned a = 0; a < 3; ++a)
            {
                node[a] += (corner >> a) & 1U;
                weight *= std::max(0.0, 1 - std::abs(index[a] - double(node[a])));
            }
            if (weight <= 0 || !GetInsideExtent(node)) continue;
            if (m_params.evaluationBounds)
            {
                // 只省略对评价区所有 Q1 查询严格零贡献的节点；每个保留节点仍取完整来源。
                const auto center = Add(Model({double(node[0]), double(node[1]), double(node[2])}), m_geometry.origin);
                bool intersects = true;
                for (unsigned a = 0; a < 3; ++a)
                {
                    double radius = m_epsilon;
                    for (unsigned k = 0; k < 3; ++k)
                        radius += std::abs(m_geometry.direction[a * 3 + k]) * m_geometry.spacing[k];
                    if (center[a] + radius < (*m_params.evaluationBounds)[2 * a] ||
                        center[a] - radius > (*m_params.evaluationBounds)[2 * a + 1]) intersects = false;
                }
                if (!intersects) continue;
            }
            result[corner] = {{int(node[0]), int(node[1]), int(node[2])}, weight};
        }
        return result;
    }
    std::shared_ptr<const std::vector<ThicknessNode>> BuildNodes()
    {
        struct Quadrature { ThicknessPoint bary{}; double weight = 0; };
        std::array<Quadrature, 9> rules{};
        const double a = std::sqrt(3.0 / 5.0);
        const std::array<double, 3> abscissas{-a, 0, a}, weights{5.0 / 9, 8.0 / 9, 5.0 / 9};
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j)
            {
                const double u = (abscissas[i] + 1) * 0.5, v = (abscissas[j] + 1) * 0.5;
                rules[3 * i + j] = {{1 - u, u * (1 - v), u * v}, 0.5 * u * weights[i] * weights[j]};
            }
        struct Accumulator { double valid=0,total=0,moment=0,validError=0,totalError=0,momentError=0; };
        struct IndexHash {
            std::size_t operator()(const std::array<int,3>& index) const noexcept {
                std::size_t hash=0;
                for (auto value:index) hash^=std::hash<int>{}(value)+std::size_t(0x9e3779b9U)+(hash<<6)+(hash>>2);
                return hash;
            }
        };
        const auto add=[](double& sum,double& error,double value) {
            const double adjusted=value-error,next=sum+adjusted;error=(next-sum)-adjusted;sum=next;
        };
        std::unordered_map<std::array<int,3>,Accumulator,IndexHash> accumulators;
        std::vector<SourceMeasurement> sources;
        sources.reserve(std::min(m_sourceCapacity,std::size_t(4096U*9U)));
        std::size_t totalSources=0;
        // 保持原三角形/九点顺序归并，只保留当前有界批次的射线结果。
        for (std::size_t begin=0;begin<m_areas.size();begin+=4096U) {
            sources.clear();
            const auto end=std::min(m_areas.size(),begin+4096U);
            for (std::size_t triangle=begin;triangle<end;++triangle) {
                if (m_areas[triangle]<=0) continue;
                for (unsigned rule=0;rule<rules.size();++rule) {
                    CheckWork(m_work);
                    const auto support=GetNodeWeights(Interpolate(triangle,rules[rule].bary));
                    if (std::none_of(support.begin(),support.end(),[](const auto& w){return w.weight>0;})) continue;
                    if (totalSources==m_sourceCapacity)
                        throw Failure{ThicknessStatus::BudgetExceeded,"Source quadrature count limit exceeded."};
                    ++totalSources;sources.push_back({triangle,rule,{}});
                }
            }
#ifdef MVVCVTK_HAS_WALL_CUDA
            if (!m_cuda) throw Failure{ThicknessStatus::InternalError,"CUDA wall engine is unavailable; no CPU measurement fallback is configured."};
            std::vector<ThicknessCudaPath::Measurement> measurements;measurements.reserve(sources.size());
            for (const auto& source:sources) measurements.push_back({Interpolate(source.triangle,rules[source.rule].bary)});
            ThicknessCudaPath::Params params;
            params.spacing=m_geometry.spacing;params.direction=m_geometry.direction;params.epsilon=m_epsilon;
            params.maxDistance=m_params.maxDistance;params.maxBoundaryError=m_params.maxBoundaryError;
            params.directionCount=m_params.directionCount;params.sourceLocal=GetIsSourceLocal();
            params.tolerance=m_work.archive.input.unit==ThicknessUnit::Millimeter?1e-9:1e-12;
            const double theta=m_params.coneAngleDegrees*pi/180;
            params.coneCos=std::cos(theta);params.coneSin=std::sin(theta);
            for (unsigned di=1;di<m_params.directionCount;++di) {
                const double phi=2*pi*(di-1)/(m_params.directionCount-1);params.phiCos[di]=std::cos(phi);params.phiSin[di]=std::sin(phi);
            }
            const auto values=m_cuda->Measure(measurements,params,[this]{CheckWork(m_work);});
            if (values.size()!=sources.size()) throw Failure{ThicknessStatus::InternalError,"Invalid CUDA measurement output size."};
            for(std::size_t i=0;i<sources.size();++i) if(values[i].valid) sources[i].value=values[i].thickness;
            if(m_profile) m_profileData.counts[ThicknessProfile::Sources]+=sources.size();
#else
            throw Failure{ThicknessStatus::InternalError,"WallThickness requires the CUDA build preset; CPU measurement is not supported."};
#endif
            for (const auto& source:sources) {
                CheckWork(m_work);const auto& rule=rules[source.rule];
                const auto support=GetNodeWeights(Interpolate(source.triangle,rule.bary));
                for (const auto& node:support) {
                    if (node.weight<=0) continue;
                    auto found=accumulators.find(node.index);
                    if (found==accumulators.end()) {
                        if (accumulators.size()>=m_nodeBudget) throw Failure{ThicknessStatus::BudgetExceeded,"Sparse node field exceeds budget."};
                        found=accumulators.emplace(node.index,Accumulator{}).first;
                    }
                    auto& sum=found->second;const double weight=m_areas[source.triangle]*rule.weight*node.weight;
                    add(sum.total,sum.totalError,weight);
                    if (source.value) {add(sum.valid,sum.validError,weight);add(sum.moment,sum.momentError,weight**source.value);}
                    if (!std::isfinite(sum.total) || !std::isfinite(sum.moment))
                        throw Failure{ThicknessStatus::InvalidInput,"Node accumulation overflow."};
                }
            }
            if (m_profile && std::chrono::steady_clock::now()-m_profileLast>std::chrono::seconds(10))
                Profile("nodes",end,m_areas.size());
        }
        auto nodes = std::make_shared<std::vector<ThicknessNode>>();
        nodes->reserve(accumulators.size());
        for (const auto &entry : accumulators)
        {
            CheckWork(m_work);
            const auto &sum = entry.second;
            nodes->push_back({entry.first, sum.valid, sum.total, sum.valid > 0 ? sum.moment / sum.valid : 0});
        }
        // 只改变查找容器；每个节点仍按原来源顺序补偿累加，发布前恢复索引排序。
        std::size_t comparisons=0;
        std::sort(nodes->begin(),nodes->end(),[&](const auto& left,const auto& right) {
            if ((comparisons++&4095U)==0) CheckWork(m_work);
            return left.index<right.index;
        });
        return nodes;
    }

  public:
    explicit Kernel(const Work &work)
        : m_work(work), m_params(work.archive.params), m_geometry(work.labels->GetGeometry()),
          m_mesh(*work.mesh)
    {
    }
    Candidate Build()
    {
        m_profileBegin=m_profileLast=std::chrono::steady_clock::now();
        CheckWork(m_work);
        ValidateInput();
        m_materialField=GetMaterialField();
        BuildSampling();
        Profile("sampling",0,m_areas.size());
        BuildUniformGrid();
#ifdef MVVCVTK_HAS_WALL_CUDA
        BuildCuda();
#endif
        Profile("uniform",std::count_if(m_uniformSigns.begin(),m_uniformSigns.end(),[](auto sign){return sign!=0;}),m_uniformSigns.size());
        Field field;
        field.geometry = m_geometry;
        field.coordinateTolerance = m_epsilon;
        field.nodes = BuildNodes();
        Profile("nodes-complete",m_areas.size(),m_areas.size());
        auto samples = std::make_shared<std::vector<ThicknessSample>>();
        auto neighbors = std::make_shared<Neighbors>();
        samples->reserve(m_sampleCount);
        neighbors->reserve(m_sampleCount);
        using VertexKey = std::array<std::pair<std::uint64_t, double>, 3>;
        std::map<VertexKey, std::size_t> vertices;
        struct EdgeHash {
            std::size_t operator()(const std::pair<std::size_t,std::size_t>& edge) const noexcept {
                return edge.first^(edge.second+std::size_t(0x9e3779b9U)+(edge.first<<6)+(edge.first>>2));
            }
        };
        std::unordered_map<std::pair<std::size_t, std::size_t>, std::pair<std::size_t, std::size_t>,EdgeHash> edges;
        auto add =
            [&](std::uint64_t triangle, const ThicknessDisplaySampling::Triangle& patch)
        {
            CheckWork(m_work);
            ThicknessSample sample;
            sample.sourceTriangle = triangle;
            ThicknessPoint bary{};
            std::array<std::size_t, 3> ids{};
            for (std::size_t k = 0; k < 3; ++k)
            {
                VertexKey key{};
                std::size_t count = 0;
                for (std::size_t a = 0; a < 3; ++a)
                {
                    sample.barycentricCorners[k][a] = patch.bary[k][a];
                    if (patch.bary[k][a])
                        key[count++] = {m_mesh.GetTriangles()[triangle * 3 + a], patch.bary[k][a]};
                }
                std::sort(key.begin(), key.begin() + count);
                ids[k] = count==1 ? static_cast<std::size_t>(key[0].first)
                    : vertices.emplace(key,m_mesh.GetVertices().size()/3+vertices.size()).first->second;
                bary = Add(bary, Scale(sample.barycentricCorners[k], 1.0 / 3));
            }
            const auto p = Interpolate(triangle, bary);
            sample.source = Add(p, m_geometry.origin);
            sample.area = m_areas[triangle]*patch.fraction;
            if (m_params.evaluationBounds)
                for (std::size_t a = 0; a < 3; ++a)
                    if (sample.source[a] < (*m_params.evaluationBounds)[a * 2] ||
                        sample.source[a] > (*m_params.evaluationBounds)[a * 2 + 1])
                        sample.validity = ThicknessValidity::OutsideEvaluation;
            const auto id = samples->size();
            neighbors->push_back({noNeighbor, noNeighbor, noNeighbor});
            for (std::size_t k = 0; k < 3; ++k)
            {
                const std::pair<std::size_t, std::size_t> edge{std::min(ids[k], ids[(k + 1) % 3]),
                                                               std::max(ids[k], ids[(k + 1) % 3])};
                const auto found = edges.find(edge);
                if (found == edges.end())
                    edges.emplace(edge, std::make_pair(id, k));
                else
                {
                    const auto old = found->second;
                    (*neighbors)[id][k] = old.first;
                    (*neighbors)[old.first][old.second] = id;
                    edges.erase(found);
                }
            }
            samples->push_back(std::move(sample));
        };
        for (std::size_t triangle=0;triangle<m_areas.size();++triangle) {
            if (m_areas[triangle]<=0) continue;
            ThicknessDisplaySampling::Corners p{};
            for (unsigned k=0;k<3;++k) p[k]=Vertex(m_mesh.GetTriangles()[triangle*3+k]);
            ThicknessDisplaySampling::Visit(p,m_params.sampleSpacing,[&]{CheckWork(m_work);},[&](const auto& patch){add(triangle,patch);});
        }
        Profile("sample-geometry",samples->size(),m_sampleCount);
        // 点查询彼此独立；三角形、邻接和结果数组顺序仍由上面的串行构造确定。
        std::atomic<std::size_t> nextSample{0};std::atomic<bool> failed{false};std::exception_ptr failure;
        const auto query=[&] {
            try {
                for (;;) {
                    const auto begin=nextSample.fetch_add(256,std::memory_order_relaxed);
                    if (begin>=samples->size() || failed.load(std::memory_order_relaxed)) break;
                    for (auto i=begin;i<std::min(begin+256,samples->size());++i) {
                        CheckWork(m_work);auto& sample=(*samples)[i];
                        if (sample.validity==ThicknessValidity::OutsideEvaluation) continue;
                        const auto value=GetValue(field,sample.source);
                        sample.validity=value ? ThicknessValidity::Valid : ThicknessValidity::NoValidSource;
                        sample.thickness=value.value_or(0);
                    }
                }
            } catch (...) {if (!failed.exchange(true)) failure=std::current_exception();}
        };
        std::vector<std::thread> workers;workers.reserve(m_workerCount-1);
        try {for (std::size_t i=1;i<m_workerCount;++i) workers.emplace_back(query);}
        catch (...) {failed.store(true);for (auto& worker:workers) worker.join();throw;}
        query();for (auto& worker:workers) worker.join();if (failure) std::rethrow_exception(failure);
        field.samples = std::move(samples);
        field.neighbors = std::move(neighbors);
        field.subdivisions = m_n;
        Profile("samples",field.samples->size(),m_sampleCount);
        auto result = BuildEvaluation(field, m_work.archive.evaluation, m_params,
                                      m_work.archive.limits, m_work.cancelled, m_work.deadline);
        result.field = std::move(field);
        Profile("evaluation");
        return result;
    }
};
} // namespace
bool GetParamsValid(const ThicknessParams &p) noexcept
{
    if (!Positive(p.maxDistance) || !Positive(p.sampleSpacing) || !p.materialThreshold ||
        !std::isfinite(*p.materialThreshold) || !Nonnegative(p.maxBoundaryError) ||
        static_cast<unsigned>(p.boundaryPolicy) > 1 ||
        !Nonnegative(p.coneAngleDegrees) || p.coneAngleDegrees > 30 ||
        (p.directionCount != 1 && p.directionCount != 9 && p.directionCount != 17) ||
        (p.directionCount > 1 && p.coneAngleDegrees == 0))
        return false;
    if (p.evaluationBounds)
        for (std::size_t a = 0; a < 3; ++a)
            if (!std::isfinite((*p.evaluationBounds)[a * 2]) ||
                !std::isfinite((*p.evaluationBounds)[a * 2 + 1]) ||
                (*p.evaluationBounds)[a * 2] >= (*p.evaluationBounds)[a * 2 + 1])
                return false;
    return true;
}
bool GetEvaluationValid(const ThicknessEvaluation &e) noexcept
{
    return Nonnegative(e.lower) && Positive(e.upper) && e.lower <= e.upper &&
           Nonnegative(e.histogramRange[0]) && Positive(e.histogramRange[1]) &&
           e.histogramRange[0] < e.histogramRange[1] && e.histogramBins > 0 &&
           e.histogramBins <= 4096 && Nonnegative(e.minRegionArea);
}
bool GetDisplayValid(const ThicknessDisplay &d) noexcept
{
    return static_cast<unsigned>(d.mode) <= 1 && Nonnegative(d.range[0]) && Positive(d.range[1]) &&
           d.range[0] < d.range[1] && Nonnegative(d.opacity) && d.opacity <= 1;
}
bool GetConfigValid(const ThicknessConfig &c) noexcept
{
    return WorkLimit(c.maxWorkingBytes) >= 4096 && WorkLimit(c.maxSamples) > 0 && (!WorkLimit(c.deadlineMilliseconds).GetValue() || WorkLimit(c.deadlineMilliseconds) > 0) &&
           (!WorkLimit(c.deadlineMilliseconds).GetValue() || WorkLimit(c.deadlineMilliseconds) <= 3600000) && c.stopTimeoutMilliseconds <= 5000;
}
std::optional<double> GetValue(const Field &field, const ThicknessPoint &modelPoint)
{
    if (!field.nodes || field.nodes->empty() || !Finite(modelPoint)) return {};
    const auto &g = field.geometry;
    ThicknessPoint index{}, fraction{};
    std::array<std::int64_t, 3> cell{};
    unsigned nearest = 0;
    for (unsigned a = 0; a < 3; ++a)
    {
        for (unsigned k = 0; k < 3; ++k)
            index[a] += g.direction[k * 3 + a] * (modelPoint[k] - g.origin[k]) / g.spacing[a];
        if (!std::isfinite(index[a]) || index[a] < g.extent[2 * a] || index[a] > g.extent[2 * a + 1])
            return {};
        cell[a] = static_cast<std::int64_t>(std::floor(index[a]));
        fraction[a] = index[a] - double(cell[a]);
        if (fraction[a] >= 0.5) nearest |= 1U << a;
    }
    std::array<std::optional<double>, 8> values{};
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        std::array<int, 3> key{};
        bool inside = true;
        for (unsigned a = 0; a < 3; ++a)
        {
            const auto node = cell[a] + ((corner >> a) & 1U);
            if (node < g.extent[2 * a] || node > g.extent[2 * a + 1]) { inside = false; break; }
            key[a] = static_cast<int>(node);
        }
        if (!inside) continue;
        const auto found = std::lower_bound(field.nodes->begin(), field.nodes->end(), key,
            [](const ThicknessNode &node, const auto &target) { return node.index < target; });
        if (found != field.nodes->end() && found->index == key && found->validWeight > 0 &&
            Positive(found->thickness)) values[corner] = found->thickness;
    }
    if (!values[nearest]) return {};
    // 顺序是合同的一部分；缺失角点不作全局权重归一化。
    std::size_t count = 8;
    for (unsigned a = 0; a < 3; ++a)
    {
        for (std::size_t i = 0; i < count / 2; ++i)
        {
            const auto left = values[2 * i], right = values[2 * i + 1];
            values[i] = !left ? right : !right ? left
                        : std::optional<double>((1 - fraction[a]) * *left + fraction[a] * *right);
        }
        count /= 2;
    }
    return values[0];
}
Candidate BuildField(const Work &w) noexcept
{
    try
    {
        if (!w.source || !w.labels || !w.mesh || !GetParamsValid(w.archive.params) ||
            !GetConfigValid(w.archive.limits) || !GetEvaluationValid(w.archive.evaluation) ||
            std::find(w.archive.input.materialLabels.begin(), w.archive.input.materialLabels.end(), 0) != w.archive.input.materialLabels.end() || static_cast<unsigned>(w.archive.input.unit) < 1 ||
            static_cast<unsigned>(w.archive.input.unit) > 2)
            return {ThicknessStatus::InvalidInput, {}, {}, {}, "Invalid thickness work input."};
        return Kernel(w).Build();
    }
    catch (const Failure &f)
    {
        return {f.status, {}, {}, {}, f.message};
    }
    catch (const std::bad_alloc &)
    {
        return {ThicknessStatus::BudgetExceeded, {}, {}, {}, "Thickness allocation failed."};
    }
    catch (const std::exception &e)
    {
        return {ThicknessStatus::InternalError, {}, {}, {}, e.what()};
    }
    catch (...)
    {
        return {ThicknessStatus::InternalError, {}, {}, {}, "Unexpected thickness failure."};
    }
}
ThicknessPoint GetSampleCorner(const SurfaceMeshPayload &mesh, const ThicknessSample &sample,
                               std::size_t corner)
{
    // 先减局部参考再加回，避免大原点重心和的放大误差。
    const auto first = mesh.GetTriangles().at(sample.sourceTriangle * 3);
    const auto *origin = mesh.GetVertices().data() + first * 3;
    ThicknessPoint point{origin[0], origin[1], origin[2]};
    for (std::size_t k = 0; k < 3; ++k)
    {
        const auto id = mesh.GetTriangles().at(sample.sourceTriangle * 3 + k);
        const auto *vertex = mesh.GetVertices().data() + id * 3;
        for (std::size_t a = 0; a < 3; ++a)
            point[a] += (vertex[a] - origin[a]) * sample.barycentricCorners.at(corner)[k];
    }
    return point;
}
} // namespace ThicknessAlgorithm
