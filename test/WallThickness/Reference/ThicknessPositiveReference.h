#pragma once
#include "ThicknessPositivePath.h"
// 冻结 CPU 参照的正材料证书，仅用于对照测试。
namespace ThicknessMaterialField {
// 仅证明整段严格正；与原区间根隔离使用相同的 Bernstein 二等分和符号条件。
// 遇零/不确定系数、非正端点或原访问预算上限时立即交回精确路径。
inline bool GetPositiveBernstein(std::array<Interval,4> b,std::size_t& visited)
{
    struct Pending {std::array<Interval,4> b;unsigned depth;};
    Pending stack[53];unsigned count=0,depth=0;
    for (;;) {
        if (++visited>4096 || depth>52) return false;
        bool positive=true;
        for (unsigned i=0;i<4;++i) {
            const auto sign=GetSign(b[i]);if (!sign) return false;
            positive=positive && sign>0;
        }
        if (positive) {
            if (!count) return true;
            const auto& pending=stack[--count];b=pending.b;depth=pending.depth;continue;
        }
        if (GetSign(b[0])!=1 || GetSign(b[3])!=1 || depth==52) return false;
        const auto x=(b[0]+b[1])/2,y=(b[1]+b[2])/2,z=(b[2]+b[3])/2;
        const auto xy=(x+y)/2,yz=(y+z)/2,mid=(xy+yz)/2;
        stack[count++]={{mid,yz,z,b[3]},depth+1};b={b[0],x,xy,mid};++depth;
    }
}

template<class Field>
inline bool GetPositiveCertificate(const Field& field,const std::array<double,3>& from,
 const std::array<double,3>& delta,double length,double epsilon,std::size_t& piecesOut)
    {
        if (!IsFinite(length) || !IsFinite(epsilon) || epsilon<=0 || length<=4*epsilon) return false;
        if (!IsFinite(field.threshold)) return false;
        double maximumPieces=3;
        for (unsigned axis=0;axis<3;++axis) {
            if (!IsFinite(from[axis]) || !IsFinite(delta[axis]) || field.extent[2*axis]>=field.extent[2*axis+1]) return false;
            const auto end=Interval(from[axis])+Interval(delta[axis]);
            if (Minimum(from[axis],end.low)<field.extent[2*axis]
                || Maximum(from[axis],end.high)>field.extent[2*axis+1]) return false;
            maximumPieces+=Ceil(Abs(delta[axis]));
        }
        // 旧实现每条非同号单元最多一次隔离访问；保留其 4096 次预算和 10000 cuts 门槛。
        if (maximumPieces>4090) return false;
        const Interval begin=Interval(epsilon)/length, finish=Interval(1)-begin;
        if (!(begin.high<finish.low)) return false;
        auto current=begin;
        std::array<std::int64_t,3> cell{};
        for (unsigned axis=0;axis<3;++axis) {
            if (delta[axis]==0) cell[axis]=Minimum<std::int64_t>(std::int64_t(Floor(from[axis])),field.extent[2*axis+1]-1);
            else {
                const auto p=Interval(from[axis])+current*Interval(delta[axis]);
                if (Floor(p.low)!=Floor(p.high)) return false;
                cell[axis]=std::int64_t(Floor(p.low));
            }
            if (cell[axis]<field.extent[2*axis] || cell[axis]>=field.extent[2*axis+1]) return false;
        }
        std::size_t pieces=0,visited=0;
        while (true) {
            field.check();
            if (++pieces>4090) return false;
            const auto region=field.GetUniformRegion(cell);
            if (region.sign<0) return false;
            const bool positiveBlock=region.sign>0;
            auto next=finish;
            int crossed=-1;
            for (unsigned axis=0;axis<3;++axis) {
                if (delta[axis]==0) continue;
                const double face=positiveBlock ? double(region.extent[2*axis+(delta[axis]>0?1:0)])
                    : double(cell[axis]+(delta[axis]>0?1:0));
                auto crossing=(Interval(face)-Interval(from[axis]))/Abs(delta[axis]);
                if (delta[axis]<0) crossing=-crossing;
                if (!(crossing.low>current.high)) return false;
                if (crossing.high<next.low) {next=crossing;crossed=int(axis);}
                else if (!(next.high<crossing.low)) return false;
            }
            if (!positiveBlock) {
                std::array<double,8> raw{};
                bool uniform=true;
                for (unsigned corner=0;corner<8;++corner) {
                    auto index=cell;
                    for (unsigned axis=0;axis<3;++axis) index[axis]+=(corner>>axis)&1U;
                    if (!field.node(index,raw[corner]) || !IsFinite(raw[corner])) return false;
                    uniform=uniform && raw[corner]>field.threshold;
                }
                if (!uniform) {
                    std::array<Interval,8> nodes{};
                    std::array<Interval,3> local{},step{};
                    for (unsigned corner=0;corner<8;++corner) nodes[corner]=GetInterval(raw[corner],raw[corner]);
                    for (unsigned axis=0;axis<3;++axis) {
                        const auto p=Interval(from[axis])+current*Interval(delta[axis])-Interval(double(cell[axis]));
                        const auto d=(next-current)*Interval(delta[axis]);
                        // 包含旧 Q→double 的一 ULP 包围，不能比原证书缩小。
                        local[axis]=GetInterval(p.low,p.high);step[axis]=GetInterval(d.low,d.high);
                    }
                    const auto p=GetLinePolynomial(nodes,local,step,Interval(field.threshold));
                    const std::array<Interval,4> b{p[0],p[0]+p[1]/3,p[0]+Interval(2)*p[1]/3+p[2]/3,
                        p[0]+p[1]+p[2]+p[3]};
                    if (!GetPositiveBernstein(b,visited)) return false;
                }
            }
            if (crossed<0) {
                piecesOut=pieces;return true;
            }
            current=next;
            // 面上的归属由已证明的交点方向决定；其他轴须证明仍在同一单元。
            for (unsigned axis=0;axis<3;++axis) {
                if (int(axis)==crossed) cell[axis]=positiveBlock
                    ? region.extent[2*axis+(delta[axis]>0?1:0)]-(delta[axis]<0?1:0)
                    : cell[axis]+(delta[axis]>0?1:-1);
                else if (delta[axis]!=0) {
                    const auto p=Interval(from[axis])+current*Interval(delta[axis]);
                    if (Floor(p.low)!=Floor(p.high)) return false;
                    cell[axis]=std::int64_t(Floor(p.low));
                }
                if (cell[axis]<field.extent[2*axis] || cell[axis]>=field.extent[2*axis+1]) return false;
            }
        }
    }

}
