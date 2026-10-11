#pragma once
#include "ThicknessPositivePath.h"
#include "ThicknessCudaExactPolynomial.h"

namespace ThicknessCudaMaterialPath {
using namespace ThicknessMaterialField;
struct Result {bool valid=false;double front=0,back=0;};
struct State {
    bool material=false,ended=false,event=false;
    int lastSign=0;double first=0,last=0;
    __device__ bool Span(int sign,double low,double high) {
        if(event){if(!lastSign || sign==lastSign)return false;event=false;}
        lastSign=sign;
        if(sign>0){if(ended)return false;if(!material)first=low;material=true;last=high;}
        else if(material)ended=true;
        return true;
    }
};
__device__ inline bool Isolate(std::array<Interval,4> b,double low,double high,double tolerance,
                              std::size_t& visited,State& state)
{
    struct Pending {std::array<Interval,4> b;double low,high;unsigned depth;};
    Pending stack[53];unsigned count=0,depth=0;
    for (;;) {
        if(++visited>4096 || depth>52)return false;
        int signs[4]{};unsigned changes=0;int previous=0;bool positive=false,negative=false;
        for(unsigned i=0;i<4;++i){signs[i]=GetSign(b[i]);
            if(!signs[i])return false;
            positive|=signs[i]>0;negative|=signs[i]<0;if(previous&&signs[i]!=previous)++changes;previous=signs[i];}
        bool done=false;
        if(!positive||!negative){if(!positive&&!negative)return false;if(!state.Span(positive?1:-1,low,high))return false;done=true;}
        else if(changes==1 && signs[0]*signs[3]==-1 && high-low<=tolerance){
            if(!state.lastSign || state.event)return false;state.event=true;done=true;
        }
        if(done){if(!count)return true;const auto& next=stack[--count];b=next.b;low=next.low;high=next.high;depth=next.depth;continue;}
        if(depth==52)return false;
        const auto x=(b[0]+b[1])/2,y=(b[1]+b[2])/2,z=(b[2]+b[3])/2;
        const auto xy=(x+y)/2,yz=(y+z)/2,mid=(xy+yz)/2;const auto at=(low+high)*.5;
        if(!(at>low&&at<high))return false;
        stack[count++]={{mid,yz,z,b[3]},at,high,depth+1};b={b[0],x,xy,mid};high=at;++depth;
    }
}
template<class Field>
__device__ Result Get(const Field& field,const std::array<double,3>& from,const std::array<double,3>& delta,
                     double length,double epsilon,double maximumTrim,double tolerance)
{
    Result result;
    if(!IsFinite(length)||!IsFinite(epsilon)||!IsFinite(maximumTrim)||!IsFinite(tolerance)
        ||length<=4*epsilon||epsilon<=0||maximumTrim<0||tolerance<=0||!IsFinite(field.threshold))return result;
    std::size_t pieces=0;
    for(unsigned axis=0;axis<3;++axis){
        if(!IsFinite(from[axis])||!IsFinite(delta[axis])||field.extent[2*axis]>=field.extent[2*axis+1])return result;
        const auto end=Interval(from[axis])+Interval(delta[axis]);
        if(from[axis]>=field.extent[2*axis] && from[axis]<=field.extent[2*axis+1]
           && end.low>=field.extent[2*axis] && end.high<=field.extent[2*axis+1])continue;
        using namespace ThicknessCudaExactSign;
        const auto a=Decode(from[axis]),b=Add(a,Decode(delta[axis]));
        const auto lo=Decode(double(field.extent[2*axis])),hi=Decode(double(field.extent[2*axis+1]));
        if(Subtract(a,lo).sign<0||Subtract(hi,a).sign<0||Subtract(b,lo).sign<0||Subtract(hi,b).sign<0)return result;
    }
    using ThicknessCudaExactPolynomial::Cut;
    State state;std::size_t visited=0;double current=epsilon/length,finish=1-current;
    Cut currentCut{epsilon,0,length};const Cut finishCut{length,epsilon,length};
    const double begin=current;
    std::array<bool,3> known{};std::array<std::int64_t,3> knownCells{};
    Interval currentBound=Interval(epsilon)/length;const auto finishBound=Interval(1)-currentBound;
    while(ThicknessCudaExactPolynomial::Compare(currentCut,finishCut)<0){
        // 与原 10000 cuts（含两端）预算一致：最多 9999 个区间。
        if(++pieces>=10000)return result;
        std::array<std::int64_t,3> cell{};double next=finish;Interval nextBound=finishBound;Cut nextCut=finishCut;
        for(unsigned axis=0;axis<3;++axis){
            cell[axis]=known[axis]?knownCells[axis]:ThicknessCudaExactPolynomial::Cell(from[axis],delta[axis],currentCut,field.extent[2*axis],field.extent[2*axis+1]);
        }
        const auto region=field.GetUniformRegion(cell);const bool uniform=region.sign!=0;
        std::array<Cut,3> crossings{};std::array<double,3> faces{};
        for(unsigned axis=0;axis<3;++axis)if(delta[axis]!=0){
            const double face=uniform?double(region.extent[2*axis+(delta[axis]>0?1:0)]):double(cell[axis]+(delta[axis]>0?1:0));
            const Cut crossing{face,from[axis],delta[axis]};
            crossings[axis]=crossing;faces[axis]=face;
            if(ThicknessCudaExactPolynomial::Compare(crossing,currentCut)<=0)return result;
            if(ThicknessCudaExactPolynomial::Compare(crossing,nextCut)<0){nextCut=crossing;next=(face-from[axis])/delta[axis];nextBound=ThicknessCudaExactPolynomial::Bound(crossing);}
        }
        for(unsigned axis=0;axis<3;++axis){
            known[axis]=delta[axis]!=0 && ThicknessCudaExactPolynomial::Compare(crossings[axis],nextCut)==0;
            if(known[axis])knownCells[axis]=std::int64_t(faces[axis])-(delta[axis]<0?1:0);
        }
        if(uniform){if(!state.Span(region.sign,current,next))return result;}
        else {
            std::array<Interval,8> nodes{};std::array<double,8> raw{};int sign=0;bool same=true;
            for(unsigned corner=0;corner<8;++corner){auto index=cell;for(unsigned axis=0;axis<3;++axis)index[axis]+=(corner>>axis)&1U;
                double value=0;if(!field.node(index,value))return result;raw[corner]=value;nodes[corner]=GetInterval(value,value);
                const int side=value>field.threshold?1:value<field.threshold?-1:0;if(!side||(sign&&side!=sign))same=false;sign=side;
            }
            if(same&&sign){if(!state.Span(sign,current,next))return result;}
            else {
                std::array<Interval,3> local{},step{};
                for(unsigned axis=0;axis<3;++axis){const auto p=Interval(from[axis])+currentBound*Interval(delta[axis])-Interval(double(cell[axis]));const auto d=(nextBound-currentBound)*Interval(delta[axis]);local[axis]=GetInterval(p.low,p.high);step[axis]=GetInterval(d.low,d.high);}
                const auto p=GetLinePolynomial(nodes,local,step,Interval(field.threshold));
                const std::array<Interval,4> b{p[0],p[0]+p[1]/3,p[0]+Interval(2)*p[1]/3+p[2]/3,p[0]+p[1]+p[2]+p[3]};
                const auto savedState=state;const auto savedVisited=visited;
                if(!Isolate(b,current,next,tolerance/length,visited,state)){
                    state=savedState;visited=savedVisited;
                    const auto exact=ThicknessCudaExactPolynomial::Bernstein(raw,field.threshold,from,delta,cell,currentCut,nextCut);
                    if(!ThicknessCudaExactPolynomial::Isolate(exact,currentCut,nextCut,current,next,length,tolerance,visited,state,
                        ThicknessCudaExactPolynomial::Compare(currentCut,Cut{epsilon,0,length})==0,
                        ThicknessCudaExactPolynomial::Compare(nextCut,finishCut)==0))return result;
                }
            }
        }
        current=next;currentBound=nextBound;currentCut=nextCut;
    }
    if(!state.material||state.event)return result;
    result.front=state.first==begin?0:Maximum(0.,state.first*length-epsilon);
    result.back=state.last==finish?0:Maximum(0.,(1-state.last)*length-epsilon);
    result.valid=(state.last-state.first)*length>2*epsilon && result.front<=maximumTrim+epsilon && result.back<=maximumTrim+epsilon;
    return result;
}
}
