// 冻结 CPU 有理数实现只作为测试参照；运行产品不调用它。
#include "ThicknessMaterialField.h"
#include "ThicknessCudaProbe.h"
#include <iostream>
#include <random>
int main() {
    std::vector<CudaPathProbe> probes;
    CudaPathProbe p;p.epsilon=.125;p.maximumTrim=0;
    for(unsigned i=0;i<8;++i)p.nodes[i]=double(i&1U)-.125;
    probes.push_back(p); // 中央区间端点恰好为零，允许零 trim。
    p={};p.from={0,0,.5};p.delta={1,1,0};p.length=std::sqrt(2.);p.maximumTrim=0;
    for(unsigned i=0;i<8;++i)p.nodes[i]=((i&1U)?1.:-1.)*((i&2U)?1.:-1.);
    probes.push_back(p); // t=.5 的内部切触必须无效。
    const double a=std::ldexp(1.,-100),tiny=std::ldexp(1.,-252);
    p.epsilon=std::ldexp(1.,-110);
    for(unsigned i=0;i<8;++i)p.nodes[i]=(i&3U)==0?a*a+tiny:(i&3U)==3?1.:-a;
    probes.push_back(p); // 52 层浮点隔离无法认证，但精确场整段严格正。
    for(unsigned i=0;i<8;++i)p.nodes[i]=(i&3U)==0?a*a-tiny:(i&3U)==3?1.:-a;
    probes.push_back(p); // 相邻反例具有内部背景，不能误认证。
    std::mt19937_64 random(713239);
    for(unsigned i=0;i<160;++i){p={};
        for(auto& n:p.nodes)n=(double(random()%2001)-1000)/997.;
        for(unsigned axis=0;axis<3;++axis){p.from[axis]=.05+.9*double(random()%10001)/10000;
            p.delta[axis]=.05+.9*double(random()%10001)/10000-p.from[axis];}
        p.length=std::hypot(p.delta[0],p.delta[1],p.delta[2]);probes.push_back(p);
    }
    // 两个 face quotient 舍入相同或非常接近，仍须保留精确切面次序。
    for(unsigned i=0;i<24;++i){p={};p.extent={0,2,0,2,0,2};p.from={.1,.1,.5};
        p.delta={1.5,std::nextafter(1.5,i&1U?2.:1.),0};p.length=std::hypot(p.delta[0],p.delta[1]);
        for(auto& n:p.nodes)n=(double(random()%2001)-1000)/997.;probes.push_back(p);}
    for(unsigned i=0;i<4;++i){p={};p.from={.5,.5,.5};p.delta={.25,0,0};p.maximumTrim=1;
        const auto magnitude=i<2?std::numeric_limits<double>::max():std::numeric_limits<double>::denorm_min();
        for(unsigned c=0;c<8;++c)p.nodes[c]=(i&1U)&&!(c&1U)?-magnitude:magnitude;
        probes.push_back(p);}
    const auto result=RunCudaPathProbes(probes);unsigned failures=0,valid=0;
    for(std::size_t i=0;i<probes.size();++i){const auto& x=probes[i];ThicknessMaterialField::Field field;
        field.extent=x.extent;field.threshold=x.threshold;
        field.node=[&](const auto& index,double& value){std::size_t offset=0,stride=1;
            for(unsigned a=0;a<3;++a){offset+=std::size_t(index[a]-x.extent[2*a])*stride;stride*=std::size_t(x.extent[2*a+1]-x.extent[2*a]+1);}value=x.nodes[offset];return true;};
        const auto reference=field.GetMaterialPath(x.from,x.delta,x.length,x.epsilon,x.maximumTrim,x.tolerance);
        const bool expected=reference.status==ThicknessMaterialField::Status::Valid;valid+=expected;
        const auto& actual=result[i];
        if(!actual.valueSampleEquivalent){++failures;std::cerr<<"value/overflow contract differs "<<i<<'\n';}
        if(actual.length!=std::hypot(x.delta[0],x.delta[1],x.delta[2])){++failures;std::cerr<<"vector length differs "<<i<<'\n';}
        if(actual.valid!=expected || (expected&&(std::abs(actual.front-reference.trim[0])>2*x.tolerance || std::abs(actual.back-reference.trim[1])>2*x.tolerance))){
            ++failures;std::cerr<<"path="<<i<<" GPU="<<actual.valid<<" reference="<<int(reference.status)<<" reason="<<reference.reason<<'\n';}
        if((i==0||i==2)&&!expected){++failures;std::cerr<<"positive control failed "<<i<<'\n';}
    }
    std::cout<<"paths="<<probes.size()<<" valid="<<valid<<" differences="<<failures<<'\n';return failures?1:0;
}
