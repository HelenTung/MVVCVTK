#pragma once
#include <cstdint>

namespace ThicknessCudaExactSign {
// binary64 节点和三轴坐标均为二进制有理数；Q1 的四因子共同尺度为 2^4296。
// 有限节点最大指数 1023；16384 位同时覆盖 dyadic 坐标和 epsilon/length 的有理端点。
struct Integer {
    // 低位整字的零通过 offset 表示；只复制/清零有效字，保持完全相同的整数值。
    std::uint32_t digits[512];
    unsigned used=0,offset=0;int sign=0;
    __device__ Integer()=default;
    __device__ Integer(const Integer& other) { *this=other; }
    __device__ Integer& operator=(const Integer& other) {
        used=other.used;offset=other.offset;sign=other.sign;
        for(unsigned i=0;i<used;++i)digits[i]=other.digits[i];return *this;
    }
};
__device__ inline std::uint32_t Word(const Integer& a,unsigned index) {
    return index>=a.offset && index-a.offset<a.used?a.digits[index-a.offset]:0;
}
__device__ inline int Compare(const Integer& a,const Integer& b) {
    const auto na=a.used?a.offset+a.used:0,nb=b.used?b.offset+b.used:0;
    if(na!=nb)return na>nb?1:-1;
    for(unsigned i=na;i;--i)if(Word(a,i-1)!=Word(b,i-1))return Word(a,i-1)>Word(b,i-1)?1:-1;
    return 0;
}
__device__ inline void Normalize(Integer& a) {
    while(a.used && !a.digits[a.used-1])--a.used;
    if(!a.used){a.sign=0;a.offset=0;return;}
    unsigned first=0;while(!a.digits[first])++first;
    if(first){a.used-=first;a.offset+=first;for(unsigned i=0;i<a.used;++i)a.digits[i]=a.digits[i+first];}
}
__device__ __noinline__ Integer Add(const Integer& a,const Integer& b) {
    if(!a.sign)return b;if(!b.sign)return a;
    Integer r;
    r.offset=a.offset<b.offset?a.offset:b.offset;
    if(a.sign==b.sign) {
        r.sign=a.sign;r.used=(a.offset+a.used>b.offset+b.used?a.offset+a.used:b.offset+b.used)-r.offset;std::uint64_t carry=0;
        for(unsigned i=0;i<r.used;++i){const auto value=std::uint64_t(Word(a,r.offset+i))+Word(b,r.offset+i)+carry;r.digits[i]=std::uint32_t(value);carry=value>>32;}
        if(carry)r.digits[r.used++]=std::uint32_t(carry);
    } else {
        const auto comparison=Compare(a,b);if(!comparison)return r;
        const auto& large=comparison>0?a:b;const auto& small=comparison>0?b:a;
        r.sign=large.sign;r.used=large.offset+large.used-r.offset;std::uint64_t borrow=0;
        for(unsigned i=0;i<r.used;++i){const auto value=std::uint64_t(Word(small,r.offset+i))+borrow;const auto original=std::uint64_t(Word(large,r.offset+i));r.digits[i]=std::uint32_t(original-value);borrow=original<value;}
    }
    Normalize(r);return r;
}
__device__ __noinline__ Integer Subtract(const Integer& a,Integer b) {b.sign=-b.sign;return Add(a,b);}
__device__ __noinline__ Integer Shift(const Integer& a,unsigned bits) {
    Integer r;if(!a.sign)return r;r.sign=a.sign;const unsigned words=bits/32,part=bits%32;
    r.offset=a.offset+words;r.used=a.used+(part?1:0);for(unsigned i=0;i<r.used;++i)r.digits[i]=0;
    for(unsigned i=0;i<a.used;++i){const auto value=std::uint64_t(a.digits[i])<<part;r.digits[i]|=std::uint32_t(value);if(part)r.digits[i+1]|=std::uint32_t(value>>32);}
    Normalize(r);return r;
}
__device__ __noinline__ Integer Multiply(const Integer& a,const Integer& b) {
    Integer r;if(!a.sign||!b.sign)return r;r.sign=a.sign*b.sign;r.used=a.used+b.used;r.offset=a.offset+b.offset;
    for(unsigned i=0;i<r.used;++i)r.digits[i]=0;
    for(unsigned i=0;i<a.used;++i){if(!a.digits[i])continue;std::uint64_t carry=0;for(unsigned j=0;j<b.used;++j){const auto value=std::uint64_t(a.digits[i])*b.digits[j]+r.digits[i+j]+carry;r.digits[i+j]=std::uint32_t(value);carry=value>>32;}r.digits[i+b.used]=std::uint32_t(carry);}
    Normalize(r);return r;
}
__device__ __noinline__ Integer Decode(double value) {
    const auto bits=static_cast<std::uint64_t>(__double_as_longlong(value));
    const unsigned exponent=unsigned((bits>>52)&2047);
    const auto mantissa=(bits&((std::uint64_t(1)<<52)-1))+(exponent?(std::uint64_t(1)<<52):0);
    Integer r;if(!mantissa)return r;r.sign=bits>>63?-1:1;r.used=2;r.digits[0]=std::uint32_t(mantissa);r.digits[1]=std::uint32_t(mantissa>>32);Normalize(r);
    return Shift(r,exponent?exponent-1:0);
}
template<class Field>
__device__ __noinline__ int GetSign(const Field& field,const std::array<double,3>& point,const std::array<std::int64_t,3>& cell) {
    const auto one=Decode(1.0);Integer factors[3][2];
    for(unsigned axis=0;axis<3;++axis){factors[axis][1]=Subtract(Decode(point[axis]),Decode(double(cell[axis])));factors[axis][0]=Subtract(one,factors[axis][1]);}
    Integer sum;
    for(unsigned corner=0;corner<8;++corner){auto index=cell;for(unsigned axis=0;axis<3;++axis)index[axis]+=(corner>>axis)&1U;double value=0;if(!field.node(index,value))return -2;
        Integer weight;weight.used=1;weight.sign=1;weight.digits[0]=1;
        for(unsigned axis=0;axis<3;++axis)weight=Multiply(weight,factors[axis][(corner>>axis)&1U]);
        sum=Add(sum,Multiply(Decode(value),weight));
    }
    sum=Subtract(sum,Shift(Decode(field.threshold),3222));return sum.sign>0?1:sum.sign<0?0:-1;
}
}
