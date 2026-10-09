#pragma once
#include "ThicknessMath.h"
#include <stdexcept>
#include <vector>

namespace ThicknessDisplaySampling {
using Point=ThicknessPoint;
using Corners=std::array<Point,3>;
struct Triangle {Corners bary{{Point{1,0,0},Point{0,1,0},Point{0,0,1}}};double fraction=1;};
// 最长边二等分；每条原始共享边只按自身长度细分，避免把远处大三角形的密度传播到全网格。
template<class Check,class Emit>
void Visit(const Corners& original,double spacing,const Check& check,const Emit& emit)
{
    const auto point=[&](const Point& bary) {
        Point p{};
        for (unsigned k=0;k<3;++k) p=ThicknessMath::Add(p,ThicknessMath::Scale(original[k],bary[k]));
        return p;
    };
    std::vector<Triangle> pending;
    Triangle current;
    for (;;) {
        check();
        std::array<Point,3> points{};
        for (unsigned k=0;k<3;++k) points[k]=point(current.bary[k]);
        unsigned edge=0;double longest=0;
        for (unsigned k=0;k<3;++k) {
            const auto length=ThicknessMath::Length(ThicknessMath::Sub(points[k],points[(k+1)%3]));
            if (length>longest) {longest=length;edge=k;}
        }
        if (longest<=spacing) {
            emit(current);
            if (pending.empty()) return;
            current=pending.back();pending.pop_back();continue;
        }
        const auto a=current.bary[edge],b=current.bary[(edge+1)%3],c=current.bary[(edge+2)%3];
        const auto middle=ThicknessMath::Scale(ThicknessMath::Add(a,b),.5);
        if (middle==a || middle==b) throw std::runtime_error("Display spacing exceeds coordinate precision.");
        const double fraction=current.fraction*.5;
        pending.push_back({Corners{{middle,b,c}},fraction});
        current={Corners{{a,middle,c}},fraction};
    }
}
}
