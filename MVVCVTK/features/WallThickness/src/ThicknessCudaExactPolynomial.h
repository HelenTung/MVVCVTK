#pragma once
#include "ThicknessCudaExactSign.h"

namespace ThicknessCudaExactPolynomial {
using namespace ThicknessCudaExactSign;
// 精确切点保留商的原始 binary64 操作数，避免先舍入为 double 后再判定次序。
struct Cut {double first=0,second=0,denominator=1;};
__device__ __noinline__ Integer Numerator(Cut c) {
    auto n=Subtract(Decode(c.first),Decode(c.second));if(c.denominator<0)n.sign=-n.sign;return n;
}
__device__ __noinline__ Integer Denominator(Cut c) {return Decode(fabs(c.denominator));}
__device__ __noinline__ int CompareCuts(Cut a,Cut b) {
    return Subtract(Multiply(Numerator(a),Denominator(b)),Multiply(Numerator(b),Denominator(a))).sign;
}
__device__ inline ThicknessMaterialField::Interval Bound(Cut c) {
    auto r=(ThicknessMaterialField::Interval(c.first)-ThicknessMaterialField::Interval(c.second))/fabs(c.denominator);
    return c.denominator<0?-r:r;
}
__device__ inline int Compare(Cut a,Cut b) {
    if(a.first==b.first && a.second==b.second && a.denominator==b.denominator)return 0;
    const auto x=Bound(a),y=Bound(b);
    return x.high<y.low?-1:x.low>y.high?1:CompareCuts(a,b);
}
__device__ __noinline__ Integer Fraction(double from,double delta,std::int64_t cell,Cut a,Cut other) {
    return Multiply(Add(Multiply(Subtract(Decode(from),Decode(double(cell))),Denominator(a)),
                        Multiply(Decode(delta),Numerator(a))),Denominator(other));
}
__device__ __noinline__ std::int64_t Cell(double from,double delta,Cut cut,std::int64_t low,std::int64_t high) {
    using namespace ThicknessMaterialField;
    if(delta==0)return Minimum<std::int64_t>(std::int64_t(floor(from)),high-1);
    const auto p=Interval(from)+Bound(cut)*Interval(delta);
    if(Floor(p.low)==Floor(p.high))return Minimum<std::int64_t>(std::int64_t(Floor(p.low)),high-1);
    auto base=std::int64_t(floor(from+(cut.first-cut.second)/cut.denominator*delta));
    const auto den=Denominator(cut),one=Shift(den,1074);
    auto f=Add(Multiply(Subtract(Decode(from),Decode(double(base))),den),Multiply(Decode(delta),Numerator(cut)));
    if(f.sign<0){--base;f=Add(f,one);}
    if(Subtract(f,one).sign>=0){++base;f=Subtract(f,one);}
    if(!f.sign && delta<0)--base;
    return Maximum(low,Minimum(base,high-1));
}
__device__ __noinline__ std::array<Integer,4> Bernstein(const std::array<double,8>& nodes,double threshold,
    const std::array<double,3>& from,const std::array<double,3>& delta,const std::array<std::int64_t,3>& cell,Cut a,Cut b) {
    std::array<Integer,4> result;
    const auto one=Shift(Multiply(Denominator(a),Denominator(b)),1074);
    for(unsigned corner=0;corner<8;++corner){
        const auto value=Subtract(Decode(nodes[corner]),Decode(threshold));
        for(unsigned mask=0;mask<8;++mask){
            const unsigned count=(mask&1U)+((mask>>1)&1U)+((mask>>2)&1U);
            Integer weight;weight.used=1;weight.sign=1;weight.digits[0]=1;
            for(unsigned axis=0;axis<3;++axis){
                auto f=(mask&(1U<<axis))?Fraction(from[axis],delta[axis],cell[axis],b,a):Fraction(from[axis],delta[axis],cell[axis],a,b);
                if(!(corner&(1U<<axis)))f=Subtract(one,f);
                weight=Multiply(weight,f);
            }
            auto term=Multiply(value,weight);
            if(count==0 || count==3)term=Add(term,Shift(term,1));
            result[count]=Add(result[count],term);
        }
    }
    // 全部系数具有共同正分母 3*(den(a)*den(b)*2^1074)^3*2^1074。
    return result;
}
__device__ __noinline__ void Half(std::array<Integer,4>& b,bool right) {
    const auto x=Add(b[0],b[1]),y=Add(b[1],b[2]),z=Add(b[2],b[3]);
    const auto xy=Add(x,y),yz=Add(y,z),mid=Add(xy,yz);
    if(right)b={mid,Shift(yz,1),Shift(z,2),Shift(b[3],3)};
    else b={Shift(b[0],3),Shift(x,2),Shift(xy,1),mid};
}
__device__ __noinline__ bool WidthAllowed(Cut a,Cut b,double length,double tolerance,unsigned depth) {
    const auto width=Subtract(Multiply(Numerator(b),Denominator(a)),Multiply(Numerator(a),Denominator(b)));
    const auto actual=Multiply(width,Decode(length));
    const auto permitted=Shift(Multiply(Decode(tolerance),Multiply(Denominator(a),Denominator(b))),depth);
    return Subtract(actual,permitted).sign<=0;
}
// DFS 只保留分支位；重新构造右子树避免每线程存放 257 组大整数。
template<class State>
__device__ __noinline__ bool Isolate(const std::array<Integer,4>& original,Cut a,Cut b,
    double low,double high,double length,double tolerance,std::size_t& visited,State& state,bool first,bool last) {
    std::array<Integer,4> coefficients=original;bool branches[257]{};unsigned depth=0;
    double left=low,right=high;bool atFirst=true,atLast=true;
    for(;;){
        if(++visited>4096 || depth>256)return false;
        int previous=0;unsigned changes=0;bool positive=false,negative=false;
        for(const auto& c:coefficients){const auto sign=c.sign;positive|=sign>0;negative|=sign<0;if(sign){if(previous&&previous!=sign)++changes;previous=sign;}}
        bool done=false;
        if(!positive&&!negative)return false;
        if(!positive||!negative){
            if(!coefficients[0].sign && !(first&&atFirst))state.event=true;
            if(!state.Span(positive?1:-1,left,right))return false;
            if(!coefficients[3].sign && !(last&&atLast))state.event=true;
            done=true;
        }else if(changes==1 && coefficients[0].sign*coefficients[3].sign==-1 && WidthAllowed(a,b,length,tolerance,depth)){
            if(!state.lastSign || state.event)return false;state.event=true;done=true;
        }
        if(!done){if(depth==256)return false;branches[depth++]=false;Half(coefficients,false);right=(left+right)*.5;atLast=false;continue;}
        while(depth && branches[depth-1])--depth;
        if(!depth)return true;
        branches[depth-1]=true;coefficients=original;left=low;right=high;atFirst=atLast=true;
        for(unsigned i=0;i<depth;++i){Half(coefficients,branches[i]);const auto middle=(left+right)*.5;
            if(branches[i]){left=middle;atFirst=false;}else{right=middle;atLast=false;}}
    }
}
}
