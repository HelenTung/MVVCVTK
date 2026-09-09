#pragma once
#include "OrthogonalCropTypes.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace CropMeshExact {
struct Failure final { CropFailure reason; const char* message; };
[[noreturn]] inline void Precision()
{
    throw Failure{CropFailure::PrecisionNotMet,"Mesh predicates exceed the supported exact arithmetic range."};
}

// Error-free expansions of binary64 inputs. The mesh translation unit uses /fp:strict.
// Refuse overflow, expansion exhaustion and products whose residual could be flushed to zero.
class Number final {
public:
    Number()=default;
    explicit Number(double value) { Add(value); }
    int Sign() const noexcept {return size ? (parts[size-1]>0?1:-1):0;}
    double Value() const noexcept {double value=0;for(std::size_t i=0;i<size;++i)value+=parts[i];return value;}
    Number operator-() const {Number out=*this;for(std::size_t i=0;i<size;++i)out.parts[i]=-out.parts[i];return out;}
    friend Number operator+(Number a,const Number& b) {for(std::size_t i=0;i<b.size;++i)a.Add(b.parts[i]);return a;}
    friend Number operator-(Number a,const Number& b) {for(std::size_t i=0;i<b.size;++i)a.Add(-b.parts[i]);return a;}
    friend Number operator*(const Number& a,const Number& b) {
        Number out;
        for(std::size_t i=0;i<a.size;++i)for(std::size_t j=0;j<b.size;++j)out.Product(a.parts[i],b.parts[j]);
        return out;
    }
private:
    void Product(double a,double b) {
        if(a==0||b==0)return;
        const double product=a*b;
        if(!std::isfinite(product)||std::abs(product)<0x1p-916)Precision();
        const double remainder=std::fma(a,b,-product);
        Add(remainder);Add(product);
    }
    void Add(double value) {
        if(value==0)return;
        if(!std::isfinite(value)||std::abs(value)<std::numeric_limits<double>::min())Precision();
        // Grow-expansion: full TwoSum accepts an arbitrary new scalar; the
        // emitted residuals remain ordered and nonoverlapping by induction.
        // No FastTwoSum magnitude precondition or external sorting is used.
        std::array<double,128> next{};std::size_t count=0;
        for(std::size_t i=0;i<size;++i) {
            const double sum=value+parts[i];
            if(!std::isfinite(sum))Precision();
            const double virtualB=sum-value;
            const double error=(value-(sum-virtualB))+(parts[i]-virtualB);
            if(error!=0) {if(count==next.size())Precision();next[count++]=error;}
            value=sum;
        }
        if(value!=0) {if(count==next.size())Precision();next[count++]=value;}
        parts=next;size=count;
    }
    std::array<double,128> parts{};
    std::size_t size=0;
};
using Point=std::array<Number,3>;
inline Point Sub(const Point& a,const Point& b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline Point Scale(const Point& p,const Number& scale) {return {p[0]*scale,p[1]*scale,p[2]*scale};}
inline Number Dot(const Point& a,const Point& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline Point Values(const CropVectorDouble3Array& p) {return {Number(p[0]),Number(p[1]),Number(p[2])};}
inline Point Radial(const Point& p,const CropVectorDouble3Array& axis) {const auto a=Values(axis);return Sub(p,Scale(a,Dot(p,a)));}
}
