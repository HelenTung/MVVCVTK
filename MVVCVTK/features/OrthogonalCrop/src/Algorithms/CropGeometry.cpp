#include "Algorithms/CropGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
template<std::size_t count>
bool GetFinite(const std::array<double, count>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

double GetDot(const CropVectorDouble3Array& a, const CropVectorDouble3Array& b)
{
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}

CropVectorDouble3Array GetOffset(const CropVectorDouble3Array& p, const CropVectorDouble3Array& c)
{
    return {p[0]-c[0], p[1]-c[1], p[2]-c[2]};
}

bool SetUnitVector(CropVectorDouble3Array& value)
{
    const double scale=std::max({std::abs(value[0]),std::abs(value[1]),std::abs(value[2])});
    if(!std::isfinite(scale)||scale==0)return false;
    const double length=std::hypot(value[0],value[1],value[2]);
    // A normalized binary64 vector has a rounding envelope around unit length.
    // Preserve that frozen value on every archive/table/worker reconstruction.
    if(std::isfinite(length)&&std::abs(length-1)<=4*std::numeric_limits<double>::epsilon())return true;
    for(auto& component:value)component/=scale;
    const double scaledLength=std::hypot(value[0],value[1],value[2]);
    for(auto& component:value)component/=scaledLength;
    return GetFinite(value);
}

bool BuildInverse(const CropMatrixDouble16Array& input, CropMatrixDouble16Array& inverse)
{
    if (!GetFinite(input) || input[12]!=0 || input[13]!=0 || input[14]!=0 || input[15]!=1) return false;
    double rows[4][8]{};
    for (int row=0; row<4; ++row) {
        for (int column=0; column<4; ++column) rows[row][column]=input[row*4+column];
        rows[row][row+4]=1;
    }
    for (int column=0;column<4;++column) {
        int pivot=column;
        for (int row=column+1;row<4;++row) {
            if (std::abs(rows[row][column])>std::abs(rows[pivot][column])) pivot=row;
        }
        if (rows[pivot][column]==0 || !std::isfinite(rows[pivot][column])) return false;
        for (int index=0;index<8;++index) std::swap(rows[pivot][index],rows[column][index]);
        const auto divisor=rows[column][column];
        for (auto& value:rows[column]) value/=divisor;
        for (int row=0;row<4;++row) {
            if (row==column) continue;
            const auto factor=rows[row][column];
            for (int index=0;index<8;++index) rows[row][index]-=factor*rows[column][index];
        }
    }
    for (int row=0;row<4;++row) for (int column=0;column<4;++column) inverse[row*4+column]=rows[row][column+4];
    return GetFinite(inverse);
}
}

std::optional<CropGeometry> CropGeometry::Build(CropOpItem operation)
{
    if (operation.recipeVersion!=1 || operation.boundaryPolicyVersion!=1
        || (operation.removalMode!=CropRemovalMode::KeepInside && operation.removalMode!=CropRemovalMode::RemoveInside)) return {};
    // Inactive fields are still serialized request content and must be valid
    // finite values; selecting another shape must not reveal hidden NaNs.
    if(!GetFinite(operation.boxToInputModelMatrix)||!GetFinite(operation.planeCenterInInputModel)
        ||!GetFinite(operation.planeNormalInInputModel)||!GetFinite(operation.centerInInputModel)||!GetFinite(operation.axisInInputModel)
        ||!std::isfinite(operation.radius)||operation.radius<=0||!std::isfinite(operation.height)||operation.height<=0
        ||operation.boxToInputModelMatrix[12]!=0||operation.boxToInputModelMatrix[13]!=0
        ||operation.boxToInputModelMatrix[14]!=0||operation.boxToInputModelMatrix[15]!=1
        ||(operation.planeNormalInInputModel==CropVectorDouble3Array{})||(operation.axisInInputModel==CropVectorDouble3Array{}))return {};
    CropGeometry result;
    switch (operation.geometryType) {
    case CropShape::Box:
        if (!BuildInverse(operation.boxToInputModelMatrix,result.m_boxInverse)) return {};
        for (int row=0;row<3;++row) {
            result.m_boxRowNorm[row]=std::hypot(result.m_boxInverse[row*4],result.m_boxInverse[row*4+1],result.m_boxInverse[row*4+2]);
            if (!std::isfinite(result.m_boxRowNorm[row]) || result.m_boxRowNorm[row]<=0) return {};
        }
        break;
    case CropShape::Plane:
        if (!GetFinite(operation.planeCenterInInputModel) || !GetFinite(operation.planeNormalInInputModel)
            || !SetUnitVector(operation.planeNormalInInputModel)) return {};
        break;
    case CropShape::Cylinder:
        if (!GetFinite(operation.centerInInputModel) || !GetFinite(operation.axisInInputModel)
            || !SetUnitVector(operation.axisInInputModel) || !std::isfinite(operation.radius) || operation.radius<=0
            || !std::isfinite(operation.height) || operation.height<=0) return {};
        break;
    case CropShape::Sphere:
        if (!GetFinite(operation.centerInInputModel) || !std::isfinite(operation.radius) || operation.radius<=0) return {};
        break;
    default: return {};
    }
    result.m_operation=std::move(operation);
    return result;
}

double CropGeometry::GetSignedDistance(const CropVectorDouble3Array& point) const noexcept
{
    if (!GetFinite(point)) return std::numeric_limits<double>::quiet_NaN();
    switch (m_operation.geometryType) {
    case CropShape::Box: {
        double distance=-std::numeric_limits<double>::infinity();
        for (int row=0;row<3;++row) {
            const auto q=m_boxInverse[row*4]*point[0]+m_boxInverse[row*4+1]*point[1]
                +m_boxInverse[row*4+2]*point[2]+m_boxInverse[row*4+3];
            distance=std::max(distance,(std::abs(q)-(1.0+1.0e-6))/m_boxRowNorm[row]);
        }
        return distance;
    }
    case CropShape::Plane:
        return -GetDot(GetOffset(point,m_operation.planeCenterInInputModel),m_operation.planeNormalInInputModel);
    case CropShape::Sphere: {
        const auto offset=GetOffset(point,m_operation.centerInInputModel);
        return std::hypot(offset[0],offset[1],offset[2])-m_operation.radius;
    }
    case CropShape::Cylinder: {
        const auto offset=GetOffset(point,m_operation.centerInInputModel);
        const auto axial=GetDot(offset,m_operation.axisInInputModel);
        const auto& axis=m_operation.axisInInputModel;
        const auto radial=std::hypot(offset[0]-axial*axis[0],offset[1]-axial*axis[1],offset[2]-axial*axis[2])-m_operation.radius;
        const auto cap=std::abs(axial)-m_operation.height*0.5;
        return std::hypot(std::max(radial,0.0),std::max(cap,0.0))+std::min(std::max(radial,cap),0.0);
    }
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

bool CropGeometry::GetInside(const CropVectorDouble3Array& point) const noexcept
{
    const auto distance=GetSignedDistance(point);
    return m_operation.geometryType==CropShape::Plane ? distance<0 : distance<=0;
}

bool CropGeometry::GetKept(const CropVectorDouble3Array& point) const noexcept
{
    if (!GetFinite(point)) return false;
    const auto distance=GetSignedDistance(point);
    if (!std::isfinite(distance)) return false;
    const bool inside=m_operation.geometryType==CropShape::Plane ? distance<0 : distance<=0;
    return m_operation.removalMode==CropRemovalMode::KeepInside ? inside : !inside;
}

namespace {
struct FloatRange final { double lo=0,hi=0; };
class FloatIntervals final {
public:
    bool valid=true;
    std::size_t operations=0;
    FloatRange Value(double value,double error=0) noexcept {
        if(!std::isfinite(value)||!std::isfinite(error)||error<0)return Invalid();
        const double lo=value-error,hi=value+error;
        if(!Finite(lo,hi))return Invalid();
        const double flo=static_cast<float>(lo),fhi=static_cast<float>(hi);
        auto result=FloatRange{std::min(lo,flo),std::max(hi,fhi)};
        // GPUs may flush a subnormal operand to signed zero.
        if((flo!=0&&std::abs(flo)<std::numeric_limits<float>::min())
            ||(fhi!=0&&std::abs(fhi)<std::numeric_limits<float>::min())) {
            result.lo=std::min(result.lo,0.0);result.hi=std::max(result.hi,0.0);
        }
        return result;
    }
    FloatRange Add(FloatRange a,FloatRange b) noexcept {
        const FloatRange sum{a.lo+b.lo,a.hi+b.hi};
        return Round(sum,std::max(std::abs(sum.lo),std::abs(sum.hi)),1);
    }
    FloatRange Neg(FloatRange a) noexcept {return {-a.hi,-a.lo};}
    FloatRange Sub(FloatRange a,FloatRange b) noexcept {return Add(a,Neg(b));}
    FloatRange Mul(FloatRange a,FloatRange b) noexcept {
        const std::array<double,4> values{a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi};
        const auto range=std::minmax_element(values.begin(),values.end());
        return Round({*range.first,*range.second},std::max(std::abs(*range.first),std::abs(*range.second)),1);
    }
    FloatRange Square(FloatRange a) noexcept {
        const double largest=std::max(a.lo*a.lo,a.hi*a.hi);
        const double smallest=a.lo<=0&&a.hi>=0?0:std::min(a.lo*a.lo,a.hi*a.hi);
        return Round({smallest,largest},largest,1);
    }
    FloatRange Abs(FloatRange a) noexcept {
        return {a.lo<=0&&a.hi>=0?0:std::min(std::abs(a.lo),std::abs(a.hi)),std::max(std::abs(a.lo),std::abs(a.hi))};
    }
    template<std::size_t count>
    FloatRange Dot(const std::array<FloatRange,count>& a,const std::array<FloatRange,count>& b) noexcept {
        FloatRange sum{};double magnitude=0;
        for(std::size_t i=0;i<count;++i) {
            const auto product=Mul(a[i],b[i]);sum.lo+=product.lo;sum.hi+=product.hi;
            magnitude+=std::max(std::abs(product.lo),std::abs(product.hi));
        }
        // sum(abs(products)), rather than abs(sum), covers cancellation and any dot/FMA order.
        return Round(sum,magnitude,2*count);
    }
    template<std::size_t count>
    FloatRange NormSquared(const std::array<FloatRange,count>& value) noexcept {
        FloatRange sum{};double magnitude=0;
        for(const auto& component:value) {
            const auto squared=Square(component);sum.lo+=squared.lo;sum.hi+=squared.hi;
            magnitude+=std::max(std::abs(squared.lo),std::abs(squared.hi));
        }
        return Round(sum,magnitude,2*count);
    }
private:
    FloatRange Invalid() noexcept {valid=false;return {};}
    bool Finite(double lo,double hi) const noexcept {
        return std::isfinite(lo)&&std::isfinite(hi)&&lo<=hi
            &&std::max(std::abs(lo),std::abs(hi))<=std::numeric_limits<float>::max();
    }
    FloatRange Round(FloatRange value,double magnitude,std::size_t count) noexcept {
        constexpr double u=0x1p-24;
        if(count>std::numeric_limits<std::size_t>::max()-operations)return Invalid();
        operations+=count;
        if(!valid||static_cast<double>(operations)*u>=1||!Finite(value.lo,value.hi)||!std::isfinite(magnitude))return Invalid();
        const double nu=static_cast<double>(count)*u;
        const double error=(nu/(1-nu))*magnitude+(magnitude==0?0:std::numeric_limits<float>::min());
        value.lo=std::nextafter(value.lo-error,-std::numeric_limits<double>::infinity());
        value.hi=std::nextafter(value.hi+error,std::numeric_limits<double>::infinity());
        if(!Finite(value.lo,value.hi))return Invalid();
        return value;
    }
};
}

CropFloatBounds CropGeometry::GetFloatBounds(const CropVectorDouble3Array& point,
    const CropVectorDouble3Array& inputError) const noexcept
{
    CropFloatBounds result;FloatIntervals math;
    std::array<FloatRange,3> p;
    for(int i=0;i<3;++i)p[i]=math.Value(point[i],inputError[i]);
    std::array<FloatRange,3> signs{};
    const auto& op=m_operation;
    switch(op.geometryType) {
    case CropShape::Box: {
        const std::array<FloatRange,4> position{p[0],p[1],p[2],math.Value(1)};
        const auto threshold=math.Value(1.0+1.0e-6);
        for(int row=0;row<3;++row) {
            std::array<FloatRange,4> coefficients;
            for(int i=0;i<4;++i)coefficients[i]=math.Value(m_boxInverse[row*4+i]);
            signs[row]=math.Sub(math.Abs(math.Dot(coefficients,position)),threshold);
        }
        result.predicateCount=3;break;
    }
    case CropShape::Plane: {
        std::array<FloatRange,3> d,n;
        for(int i=0;i<3;++i) {d[i]=math.Sub(p[i],math.Value(op.planeCenterInInputModel[i]));n[i]=math.Value(op.planeNormalInInputModel[i]);}
        signs[0]=math.Neg(math.Dot(d,n));result.predicateCount=1;break;
    }
    case CropShape::Sphere: case CropShape::Cylinder: {
        std::array<FloatRange,3> d,axis;
        for(int i=0;i<3;++i) {d[i]=math.Sub(p[i],math.Value(op.centerInInputModel[i]));axis[i]=math.Value(op.axisInInputModel[i]);}
        result.predicateCount=1;
        if(op.geometryType==CropShape::Cylinder) {
            const auto axial=math.Dot(d,axis);
            for(int i=0;i<3;++i)d[i]=math.Sub(d[i],math.Mul(axial,axis[i]));
            signs[1]=math.Sub(math.Abs(axial),math.Value(op.height*0.5));result.predicateCount=2;
        }
        signs[0]=math.Sub(math.NormSquared(d),math.Square(math.Value(op.radius)));break;
    }
    default:return result;
    }
    result.operationCount=math.operations;
    if(!math.valid)return result;
    bool inside=true,outside=false;
    for(std::size_t i=0;i<result.predicateCount;++i) {
        result.predicates[i]={signs[i].lo,signs[i].hi};
        if(op.geometryType==CropShape::Plane) {inside=inside&&signs[i].hi<0;outside=outside||signs[i].lo>=0;}
        else {inside=inside&&signs[i].hi<=0;outside=outside||signs[i].lo>0;}
    }
    if(!inside&&!outside)result.classification=CropPointClassification::BoundaryBand;
    else result.classification=(op.removalMode==CropRemovalMode::KeepInside?inside:outside)
        ?CropPointClassification::Kept:CropPointClassification::Removed;
    return result;
}

std::optional<CropVectorDouble3Array> CropGeometry::GetAffineFloatError(
    const CropMatrixDouble16Array& matrix,const CropVectorDouble3Array& point,
    const CropVectorDouble3Array& inputError) noexcept
{
    if(!GetFinite(matrix)||matrix[12]!=0||matrix[13]!=0||matrix[14]!=0||matrix[15]!=1)return {};
    FloatIntervals math;std::array<FloatRange,4> position;
    for(int i=0;i<3;++i)position[i]=math.Value(point[i],inputError[i]);position[3]=math.Value(1);
    CropVectorDouble3Array error;
    for(int row=0;row<3;++row) {
        std::array<FloatRange,4> coefficients;
        for(int i=0;i<4;++i)coefficients[i]=math.Value(matrix[row*4+i]);
        const auto bounds=math.Dot(coefficients,position);
        const double exact=matrix[row*4]*point[0]+matrix[row*4+1]*point[1]+matrix[row*4+2]*point[2]+matrix[row*4+3];
        error[row]=std::max(std::abs(bounds.lo-exact),std::abs(bounds.hi-exact));
        if(!std::isfinite(exact)||!std::isfinite(error[row]))return {};
    }
    return math.valid?std::optional<CropVectorDouble3Array>{error}:std::nullopt;
}

bool CropGeometry::GetOperationsSame(const CropOpItem& a,const CropOpItem& b) noexcept
{
    if (a.geometryType!=b.geometryType || a.removalMode!=b.removalMode
        || a.recipeVersion!=b.recipeVersion || a.boundaryPolicyVersion!=b.boundaryPolicyVersion) return false;
    switch (a.geometryType) {
    case CropShape::Box: return a.boxToInputModelMatrix==b.boxToInputModelMatrix;
    case CropShape::Plane: return a.planeCenterInInputModel==b.planeCenterInInputModel && a.planeNormalInInputModel==b.planeNormalInInputModel;
    case CropShape::Cylinder: return a.centerInInputModel==b.centerInInputModel && a.axisInInputModel==b.axisInInputModel && a.radius==b.radius && a.height==b.height;
    case CropShape::Sphere: return a.centerInInputModel==b.centerInInputModel && a.radius==b.radius;
    default: return false;
    }
}

std::optional<CropVectorDouble3Array> CropGeometry::GetAffineFloatErrorOnBounds(
    const CropMatrixDouble16Array& matrix,const CropBoundsDouble6Array& bounds,const CropVectorDouble3Array& inputError) noexcept
{
    for(int axis=0;axis<3;++axis)if(!std::isfinite(bounds[axis*2])||!std::isfinite(bounds[axis*2+1])||bounds[axis*2]>bounds[axis*2+1])return {};
    if(!GetFinite(matrix)||matrix[12]!=0||matrix[13]!=0||matrix[14]!=0||matrix[15]!=1)return {};
    constexpr double unit=0x1p-24,floor=std::numeric_limits<float>::min();
    constexpr double gamma=8*unit/(1-8*unit),doubleGuard=32*0x1p-53;
    std::array<double,4> magnitude{0,0,0,1},error{};
    for(int axis=0;axis<3;++axis) {
        if(!std::isfinite(inputError[axis])||inputError[axis]<0)return {};
        magnitude[axis]=std::max(std::abs(bounds[axis*2]),std::abs(bounds[axis*2+1]));
        error[axis]=inputError[axis]+unit*(magnitude[axis]+inputError[axis])+floor;
    }
    CropVectorDouble3Array result{};
    // Coefficient and coordinate quantization are bounded over the entire
    // domain. Exactly representable endpoints alone do not bound interior casts.
    for(int row=0;row<3;++row) {
        double perturbation=0,products=0;
        for(int col=0;col<4;++col) {
            const double coefficient=matrix[row*4+col];
            const float encoded=static_cast<float>(coefficient);if(!std::isfinite(encoded))return {};
            const double coefficientError=std::abs(double(encoded)-coefficient)+floor;
            perturbation+=std::abs(coefficient)*error[col]+coefficientError*(magnitude[col]+error[col]);
            products+=(std::abs(coefficient)+coefficientError)*(magnitude[col]+error[col]);
        }
        if(!std::isfinite(products)||products>std::numeric_limits<float>::max())return {};
        result[row]=std::nextafter((perturbation+gamma*products+16*floor)*(1+doubleGuard),INFINITY);
        if(!std::isfinite(result[row]))return {};
    }
    return result;
}
