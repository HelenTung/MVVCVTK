#include "ThicknessGrayRay.h"
#include "ThicknessProfile.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ThicknessGrayRay {
namespace {
using namespace ThicknessMaterialField;
bool GetNodes(const Field& field, const Index& cell, std::array<double,8>& nodes)
{
    for (unsigned corner = 0; corner < 8; ++corner) {
        auto index = cell;
        for (unsigned axis = 0; axis < 3; ++axis) {
            index[axis] += (corner >> axis) & 1U;
            if (index[axis] < field.extent[2*axis] || index[axis] > field.extent[2*axis+1]) return false;
        }
        if (!field.node(index,nodes[corner]) || !std::isfinite(nodes[corner])) return false;
    }
    return true;
}
double Value(const std::array<double,4>& p, double t)
{
    return ((p[3]*t+p[2])*t+p[1])*t+p[0];
}
// 三次多项式在导数根之间单调；切触和数值无法分辨的区间保留未知。
bool GetCuts(const std::array<double,4>& p, std::vector<double>& cuts)
{
    cuts = {0,1};
    const double a=3*p[3], b=2*p[2], c=p[1];
    const auto add = [&](double t) { if (std::isfinite(t) && t>0 && t<1) cuts.push_back(t); };
    if (a==0) {
        if (b!=0) add(-c/b);
    } else {
        const double d=b*b-4*a*c;
        if (!std::isfinite(d)) return false;
        if (d>=0) {
            const double q=-.5*(b+std::copysign(std::sqrt(d),b));
            if (q!=0) { add(q/a); add(c/q); }
            else add(-b/(2*a));
        }
    }
    std::sort(cuts.begin(),cuts.end());
    cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
    return std::all_of(p.begin(),p.end(),[](double v){return std::isfinite(v);});
}
}
bool GetSample(const Field& field, const Point& point, Sample& result)
{
    Index cell{}; Point fraction{};
    for (unsigned axis=0;axis<3;++axis) {
        if (!std::isfinite(point[axis]) || point[axis]<field.extent[2*axis]
            || point[axis]>field.extent[2*axis+1]) return false;
        cell[axis]=std::min<std::int64_t>(static_cast<std::int64_t>(std::floor(point[axis])),field.extent[2*axis+1]-1);
        fraction[axis]=point[axis]-double(cell[axis]);
    }
    std::array<double,8> nodes{};
    if (!GetNodes(field,cell,nodes)) return false;
    result={};
    for (unsigned corner=0;corner<8;++corner) {
        Point weight{};
        for (unsigned axis=0;axis<3;++axis) weight[axis]=(corner>>axis)&1U?fraction[axis]:1-fraction[axis];
        result.value+=nodes[corner]*weight[0]*weight[1]*weight[2];
        for (unsigned axis=0;axis<3;++axis)
            result.gradient[axis]+=nodes[corner]*((corner>>axis)&1U?1.:-1.)*weight[(axis+1)%3]*weight[(axis+2)%3];
    }
    return std::isfinite(result.value) && std::all_of(result.gradient.begin(),result.gradient.end(),[](double v){return std::isfinite(v);});
}
Result Trace(const Field& field, const Point& point, const Point& direction,
             double maxDistance, double entryAllowance, double tolerance, bool allowClippedEntrySupport)
{
    ThicknessProfile::Increment(ThicknessProfile::TraceCalls);
    ThicknessProfile::Scope profile(ThicknessProfile::TraceTime);
    Result result;
    double begin=-entryAllowance, end=maxDistance;
    for (unsigned axis=0;axis<3;++axis) {
        if (!std::isfinite(point[axis]) || !std::isfinite(direction[axis])) return result;
        if (direction[axis]==0) {
            if (point[axis]<field.extent[2*axis] || point[axis]>field.extent[2*axis+1]) {
                result.status=Status::MissingSupport; return result;
            }
        } else {
            double low=(field.extent[2*axis]-point[axis])/direction[axis];
            double high=(field.extent[2*axis+1]-point[axis])/direction[axis];
            if (low>high) std::swap(low,high);
            if (low>begin && !allowClippedEntrySupport) { result.status=Status::MissingSupport; return result; }
            begin=std::max(begin,low); end=std::min(end,high);
        }
    }
    if (!(begin<end) || begin>entryAllowance) { result.status=Status::MissingSupport; return result; }
    double current=begin;
    int lastSign=0;
    bool material=false;
    std::vector<double> cuts;
    cuts.reserve(4);
    const auto finishExit=[&](double crossing,bool verifyContact) {
        result.exit=crossing;result.observedUntil=crossing;
        if (!verifyContact) {result.status=Status::Found;return result;}
        // 必须在候选两侧观察到真实的材料→背景符号，不能用临界点的舍入残差制造出口。
        const double radius=std::max(4*tolerance,64*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(crossing)));
        Point before{},after{};
        for (unsigned axis=0;axis<3;++axis) {
            before[axis]=point[axis]+(crossing-radius)*direction[axis];
            after[axis]=point[axis]+(crossing+radius)*direction[axis];
        }
        const int left=field.GetMaterialAt(before),right=field.GetMaterialAt(after);
        result.status=left==1 && right==0?Status::Found
            :left==-2 || right==-2?Status::MissingSupport:Status::Ambiguous;
        return result;
    };
    while (current<end) {
        ThicknessProfile::Increment(ThicknessProfile::TraceCells);
        field.check();
        Index cell{}; Point local{};
        double next=end;
        for (unsigned axis=0;axis<3;++axis) {
            const double p=point[axis]+current*direction[axis];
            auto base=static_cast<std::int64_t>(std::floor(p));
            if (direction[axis]<0 && (double(base)-point[axis])/direction[axis]<=current) --base;
            if (direction[axis]>0 && (double(base+1)-point[axis])/direction[axis]<=current) ++base;
            cell[axis]=std::clamp<std::int64_t>(base,field.extent[2*axis],field.extent[2*axis+1]-1);
            local[axis]=p-double(cell[axis]);
            if (direction[axis]!=0) {
                const double face=double(cell[axis]+(direction[axis]>0?1:0));
                const double t=(face-point[axis])/direction[axis];
                if (t>current) next=std::min(next,t);
            }
        }
        if (!(next>current)) return result;
        const auto region=field.GetUniformRegion(cell);
        if (region.sign) {
            double regionEnd=end;
            bool inside=true;
            for (unsigned axis=0;axis<3;++axis) {
                const double coordinate=point[axis]+current*direction[axis];
                inside=inside && coordinate>=region.extent[2*axis] && coordinate<=region.extent[2*axis+1];
                if (direction[axis]!=0) {
                    const double face=region.extent[2*axis+(direction[axis]>0?1:0)];
                    const double crossing=(face-point[axis])/direction[axis];
                    if (crossing<=current) inside=false;
                    else regionEnd=std::min(regionEnd,crossing);
                }
            }
            if (inside && regionEnd>current) {
                ThicknessProfile::Increment(ThicknessProfile::TraceBlocks);
                // 严格同侧的全部角点证明块内没有阈值交点；不凭正块合成入口。
                if (!lastSign && region.sign>0) {result.status=Status::NoBoundary;return result;}
                if (lastSign && lastSign!=region.sign) return result;
                lastSign=region.sign;result.observedUntil=regionEnd;current=regionEnd;
                continue;
            }
        }
        std::array<double,8> nodes{};
        if (!GetNodes(field,cell,nodes)) { result.status=Status::MissingSupport; result.observedUntil=current; return result; }
        Point step{};
        for (unsigned axis=0;axis<3;++axis) step[axis]=(next-current)*direction[axis];
        double scale=std::abs(field.threshold);
        for (const auto value : nodes) scale=std::max(scale,std::abs(value));
        if (!(scale>0) || !std::isfinite(scale)) return result;
        for (auto& value : nodes) value/=scale;
        const auto polynomial=GetLinePolynomial(nodes,local,step,field.threshold/scale);
        if (!GetCuts(polynomial,cuts)) return result;
        double coefficientScale=1;
        for (const auto coefficient : polynomial) coefficientScale+=std::abs(coefficient);
        const double residualGuard=4096*std::numeric_limits<double>::epsilon()*coefficientScale;
        for (std::size_t i=1;i<cuts.size();++i) {
            const double left=cuts[i-1], right=cuts[i];
            double at=left;
            const double fl=Value(polynomial,left), fr=Value(polynomial,right);
            // 驻点与单元面残差接近零时都需复核；单元面可有不光滑的正值极小点。
            const bool verifyContact=std::abs(fl)<=residualGuard || std::abs(fr)<=residualGuard;
            if ((fl<0 && fr>0) || (fl>0 && fr<0)) {
                double lo=left,hi=right;
                for (unsigned iteration=0;iteration<64 && (hi-lo)*(next-current)>tolerance;++iteration) {
                    const double mid=(lo+hi)*.5, value=Value(polynomial,mid);
                    if (value==0) {lo=hi=mid;break;}
                    if ((value>0)==(fl>0)) lo=mid; else hi=mid;
                }
                at=(lo+hi)*.5;
                const double crossing=current+at*(next-current);
                if (fl<0) {
                    if (material) return result;
                    if (crossing>entryAllowance+tolerance) {result.status=Status::NoBoundary;return result;}
                    material=true;result.hasEntry=true;result.entry=crossing;
                } else {
                    if (!material) {result.status=Status::NoBoundary;return result;}
                    return finishExit(crossing,verifyContact);
                }
                lastSign=fr>0?1:-1;
                continue;
            }
            const double fm=Value(polynomial,(left+right)*.5);
            const int sign=fm>0?1:fm<0?-1:0;
            if (!sign) return result;
            // 相邻单元共享的阈值节点允许连续穿越；内部导数驻点触阈值则不消歧。
            if (i>1 && fl==0) return result;
            if (sign!=lastSign) {
                const double crossing=current+left*(next-current);
                if (sign>0) {
                    if (lastSign==0 && fl!=0) {result.status=Status::NoBoundary;return result;}
                    if (material || crossing>entryAllowance+tolerance) return result;
                    material=true;result.hasEntry=true;result.entry=crossing;
                } else if (material) {
                    return finishExit(crossing,verifyContact);
                }
                lastSign=sign;
            }
            if (material && right==1 && next==maxDistance && fr==0 && fl>0
                && polynomial[1]+2*polynomial[2]+3*polynomial[3]<0) {
                for (unsigned axis=0;axis<3;++axis) if (direction[axis]!=0) {
                    const double endpoint=point[axis]+next*direction[axis];
                    if (endpoint<=field.extent[2*axis] || endpoint>=field.extent[2*axis+1]) {
                        result.status=Status::MissingSupport;result.observedUntil=next;return result;
                    }
                }
                return finishExit(next,verifyContact);
            }
        }
        result.observedUntil=next;
        current=next;
    }
    result.status=end<maxDistance?Status::MissingSupport:Status::NoBoundary;
    return result;
}
}
