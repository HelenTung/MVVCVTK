#pragma once
#include "Render/Support/RenderTextStyle.h"

#include <vtkLookupTable.h>
#include <vtkDoubleArray.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <sstream>
#include <iomanip>
#include <optional>
#include <vector>
#include <vtkMath.h>

// 仅复用显示色带和色标排版；数值范围及业务单位由所属 Feature 决定。
namespace AnalysisColorStyle {
enum class RampMode { Constant, Gradient, Rainbow, InverseRainbow, HueLoop, Diverging };
enum class BlendMode { Constant, Inclined, InverseInclined };

struct RampSegment final {
    std::optional<double> lower, upper;
    RampMode mode = RampMode::Gradient;
    std::array<double, 3> lowColor{0.84,0.294,0.294};
    std::array<double, 3> highColor{0.294,0.294,0.84};
};

struct RampParams final {
    RampMode mode = RampMode::InverseRainbow;
    std::array<double, 3> constantColor{0.70, 0.70, 0.70};
    std::array<double, 3> lowColor{0.84, 0.294, 0.294};
    std::array<double, 3> highColor{0.294, 0.294, 0.84};
    std::array<double, 3> belowColor{0.64, 0.29, 0.78};
    std::array<double, 3> aboveColor{0.84, 0.29, 0.65};
    std::array<double, 3> invalidColor{128/255.0,128/255.0,128/255.0};
    bool isReversed = false;
    // 公差上下限均属于区间内；独立于普通半开分段。
    std::optional<std::array<double,2>> tolerance;
    std::array<double,3> toleranceColor{97/255.0,179/255.0,113/255.0};
    std::vector<RampSegment> segments;
    BlendMode blend = BlendMode::Constant;
    std::array<double,2> opacityRange{0.15,1.0};
};

inline bool GetModeValid(RampMode mode) noexcept
{
    return mode==RampMode::Constant || mode==RampMode::Gradient || mode==RampMode::Rainbow
        || mode==RampMode::InverseRainbow || mode==RampMode::HueLoop || mode==RampMode::Diverging;
}
inline bool GetColorValid(const std::array<double,3>& color) noexcept
{
    return std::all_of(color.begin(),color.end(),[](double value) {
        return std::isfinite(value) && value>=0 && value<=1;
    });
}

inline bool GetRampValid(const RampParams& params) noexcept
{
    if (!GetModeValid(params.mode) || params.segments.size()>1024) return false;
    for (const auto& color : {params.constantColor, params.lowColor, params.highColor,
             params.belowColor, params.aboveColor, params.invalidColor, params.toleranceColor})
        if (!GetColorValid(color)) return false;
    if (params.tolerance && (!std::isfinite((*params.tolerance)[0])
        || !std::isfinite((*params.tolerance)[1]) || (*params.tolerance)[0]>(*params.tolerance)[1])) return false;
    if (params.blend!=BlendMode::Constant && params.blend!=BlendMode::Inclined
        && params.blend!=BlendMode::InverseInclined) return false;
    if (!std::isfinite(params.opacityRange[0]) || !std::isfinite(params.opacityRange[1])
        || params.opacityRange[0]<0 || params.opacityRange[1]>1
        || params.opacityRange[0]>params.opacityRange[1]) return false;
    for (std::size_t index=0;index<params.segments.size();++index) {
        const auto& segment=params.segments[index];
        if (!GetModeValid(segment.mode) || !GetColorValid(segment.lowColor) || !GetColorValid(segment.highColor)
            || (segment.lower && !std::isfinite(*segment.lower))
            || (segment.upper && !std::isfinite(*segment.upper))
            || (segment.lower && segment.upper && *segment.lower>=*segment.upper)
            || (!segment.lower && index!=0) || (!segment.upper && index+1!=params.segments.size())
            || ((!segment.lower || !segment.upper) && segment.mode!=RampMode::Constant)) return false;
        if (index && (!segment.lower || !params.segments[index-1].upper
            || *segment.lower!=*params.segments[index-1].upper)) return false;
    }
    return true;
}

template<class Ref> std::string GetResultLabel(const Ref& ref)
{
    std::ostringstream label;label<<"#"<<std::hex<<std::setfill('0');
    for(std::size_t i=0;i<ref.entityId.bytes.size();++i){if(i==8)label<<"\n";label<<std::setw(2)<<unsigned(ref.entityId.bytes[i]);}
    label<<" / r"<<std::dec<<ref.generation;return label.str();
}

template<class Display> std::array<double,2> GetLegendPosition(const Display& display,const std::string& viewId)
{
    const auto i=display.legendPositions.find(viewId);
    return i==display.legendPositions.end() ? display.legendPosition:i->second;
}
template<class Display> bool GetLegendLayoutValid(const Display& display)
{
    const auto valid=[](const std::array<double,2>& p){return std::isfinite(p[0]) && std::isfinite(p[1]) && p[0]>=0 && p[0]<=.8 && p[1]>=.1 && p[1]<=.8;};
    if(!valid(display.legendPosition))return false;
    for(const auto& p:display.legendPositions)if(p.first.empty() || !valid(p.second))return false;
    return true;
}

template<class Band> void SetTailColors(Band& band)
{
    // 无界端段是旧字段与分段表达的唯一解析来源；保存时消除两份冲突颜色。
    if(!band.segments.empty() && !band.segments.front().lower)band.belowColor=band.segments.front().lowColor;
    if(!band.segments.empty() && !band.segments.back().upper)band.aboveColor=band.segments.back().lowColor;
}

inline bool GetRampEqual(const RampParams& a,const RampParams& b)
{
    if(a.mode!=b.mode || a.constantColor!=b.constantColor || a.lowColor!=b.lowColor || a.highColor!=b.highColor
        || a.belowColor!=b.belowColor || a.aboveColor!=b.aboveColor || a.invalidColor!=b.invalidColor
        || a.isReversed!=b.isReversed || a.tolerance!=b.tolerance || a.toleranceColor!=b.toleranceColor
        || a.blend!=b.blend || a.opacityRange!=b.opacityRange || a.segments.size()!=b.segments.size())return false;
    for(std::size_t i=0;i<a.segments.size();++i) {
        const auto& x=a.segments[i];const auto& y=b.segments[i];
        if(x.lower!=y.lower || x.upper!=y.upper || x.mode!=y.mode || x.lowColor!=y.lowColor || x.highColor!=y.highColor)return false;
    }
    return true;
}

inline double GetFraction(double lower,double upper,double value)
{
    if (lower==upper) return .5;
    const double width=upper-lower;
    return std::clamp(std::isfinite(width) ? (value-lower)/width
        : (value*.5-lower*.5)/(upper*.5-lower*.5),0.0,1.0);
}

inline std::array<double,3> GetSegmentColor(const RampSegment& segment,double value)
{
    if (segment.mode==RampMode::Constant) return segment.lowColor;
    const double fraction=GetFraction(*segment.lower,*segment.upper,value);
    std::array<double,3> color{};
    if (segment.mode==RampMode::Diverging) {
        const std::array<double,3> low{.294,.294,.84},middle{.88,.88,.88},high{.84,.294,.294};
        const auto& a=fraction<.5 ? low:middle;
        const auto& b=fraction<.5 ? middle:high;
        const double f=fraction<.5 ? 2*fraction:2*fraction-1;
        for(int k=0;k<3;++k)color[k]=a[k]+f*(b[k]-a[k]);
    } else if (segment.mode==RampMode::Gradient) {
        for (int component=0;component<3;++component)
            color[component]=segment.lowColor[component]+fraction*(segment.highColor[component]-segment.lowColor[component]);
    } else {
        const double hue=segment.mode==RampMode::Rainbow ? (1-fraction)*2/3
            : segment.mode==RampMode::InverseRainbow ? fraction*2/3 : fraction;
        const double hsv[3]{hue,.65,.84}; vtkMath::HSVToRGB(hsv,color.data());
    }
    return color;
}
inline double GetOpacity(double fraction,const RampParams& params)
{
    if (params.blend==BlendMode::Constant) return 1;
    const double position=params.blend==BlendMode::InverseInclined ? 1-fraction : fraction;
    return params.opacityRange[0]+std::clamp(position,0.0,1.0)*(params.opacityRange[1]-params.opacityRange[0]);
}

inline std::array<double,4> GetMappedColor(const std::array<double,2>& range,
    const RampParams& params,double value)
{
    std::array<double,3> color=params.invalidColor;
    double alpha=1;
    if (std::isfinite(value)) {
        const double fraction=GetFraction(range[0],range[1],value);
        alpha=GetOpacity(fraction,params);
        if(params.tolerance) {
            color=value<(*params.tolerance)[0] ? params.belowColor
                : value>(*params.tolerance)[1] ? params.aboveColor:params.toleranceColor;
        } else if(value<range[0])color=!params.segments.empty() && !params.segments.front().lower ? params.segments.front().lowColor:params.belowColor;
        else if(value>range[1])color=!params.segments.empty() && !params.segments.back().upper ? params.segments.back().lowColor:params.aboveColor;
        else if(!params.segments.empty()) {
            color=params.aboveColor;
            if(params.segments.front().lower && value<*params.segments.front().lower)color=params.belowColor;
            else for(std::size_t i=0;i<params.segments.size();++i) {
                const auto& s=params.segments[i];
                // 最后一个有限右端含端点；无穷端段不把数据中的无穷当作有效值。
                const bool isLastFinite=s.upper && (i+1==params.segments.size() || !params.segments[i+1].upper);
                if((!s.lower || value>=*s.lower) && (!s.upper || value<*s.upper || (isLastFinite && value==*s.upper))) {
                    const double v=params.isReversed && s.lower && s.upper
                        ? (1-GetFraction(*s.lower,*s.upper,value))* *s.upper+GetFraction(*s.lower,*s.upper,value)* *s.lower:value;
                    color=GetSegmentColor(s,v);break;
                }
            }
        } else if(params.mode==RampMode::Constant)color=params.constantColor;
        else {
            RampSegment s{0,1,params.mode,params.lowColor,params.highColor};
            color=GetSegmentColor(s,params.isReversed ? 1-fraction:fraction);
        }
    }
    return {color[0],color[1],color[2],alpha};
}

inline vtkSmartPointer<vtkLookupTable> BuildRamp(
    const std::array<double, 2>& range, const RampParams& params)
{
    auto table = vtkSmartPointer<vtkLookupTable>::New();
    table->SetNumberOfTableValues(256);
    auto internalRange=range;
    if(internalRange[0]==internalRange[1]) {
        const double padding=std::max(1.0,std::abs(range[0]))*16*std::numeric_limits<double>::epsilon();
        if(std::isfinite(range[0]-padding))internalRange[0]=range[0]-padding;
        if(std::isfinite(range[1]+padding))internalRange[1]=range[1]+padding;
    }
    table->SetTableRange(internalRange.data());
    const bool isLowRed = params.mode != RampMode::Rainbow;
    table->SetHueRange(isLowRed ? 0.0 : 2.0 / 3.0,
        params.mode == RampMode::HueLoop ? 1.0 : isLowRed ? 2.0 / 3.0 : 0.0);
    // 保持原有颜色方向，降低饱和度和亮度；同一 LUT 同时用于结果与色标。
    table->SetSaturationRange(0.65, 0.65);
    table->SetValueRange(0.84, 0.84);
    table->SetRampToLinear();
    table->SetBelowRangeColor(params.belowColor[0], params.belowColor[1], params.belowColor[2], 1);
    table->SetAboveRangeColor(params.aboveColor[0], params.aboveColor[1], params.aboveColor[2], 1);
    table->UseBelowRangeColorOn();
    table->UseAboveRangeColorOn();
    table->Build();
    table->SetNanColor(params.invalidColor[0],params.invalidColor[1],params.invalidColor[2],1);
    for(int index=0;index<256;++index) {
        const double f=index/255.0;
        const auto color=GetMappedColor(range,params,(1-f)*range[0]+f*range[1]);
        table->SetTableValue(index,color.data());
    }
    auto tails=params;SetTailColors(tails);
    const std::array<double,4> below{tails.belowColor[0],tails.belowColor[1],tails.belowColor[2],GetOpacity(0,params)};
    const std::array<double,4> above{tails.aboveColor[0],tails.aboveColor[1],tails.aboveColor[2],GetOpacity(1,params)};
    table->SetBelowRangeColor(below.data());table->SetAboveRangeColor(above.data());
    return table;

}

inline vtkSmartPointer<vtkLookupTable> BuildRamp(
    const std::array<double, 2>& range, bool isLowRed)
{
    RampParams params;
    params.mode = isLowRed ? RampMode::InverseRainbow : RampMode::Rainbow;
    return BuildRamp(range, params);
}

inline std::array<double,4> GetMappedColor(vtkLookupTable& table,const RampParams& params,double value)
{
    const auto* mappedRange=table.GetRange();
    return GetMappedColor({mappedRange[0],mappedRange[1]},params,value);

}

inline vtkSmartPointer<vtkDoubleArray> BuildLabels(const std::array<double, 2>& range)
{
    auto labels = vtkSmartPointer<vtkDoubleArray>::New();
    const double span = range[1] - range[0];
    const double scale = std::max({std::abs(range[0]), std::abs(range[1]), 1.0});
    if (span <= 32 * std::numeric_limits<double>::epsilon() * scale) {
        labels->InsertNextValue(range[0]);
        return labels;
    }
    if (!std::isfinite(span)) {
        labels->InsertNextValue(range[0]); labels->InsertNextValue(range[1]);
        return labels;
    }
    const double rawStep = span / 10;
    const double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    if (magnitude == 0 || !std::isfinite(magnitude)) {
        labels->InsertNextValue(range[0]); return labels;
    }
    const double fraction = rawStep / magnitude;
    const double factor = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 2.5 ? 2.5 : fraction <= 5 ? 5 : 10;
    const long double step = factor * magnitude;
    const long double first = std::ceil(static_cast<long double>(range[0]) / step);
    // 仅在原映射范围内放置等步长刻度，绝不为漂亮端点扩展业务显示范围。
    for (int index = 0; index < 32; ++index) {
        double value = static_cast<double>((first + index) * step);
        if (value > range[1]) break;
        if (value < range[0] || (labels->GetNumberOfValues() && value <= labels->GetValue(labels->GetNumberOfValues() - 1))) continue;
        if (std::abs(value) < double(step) * 1e-10) value = 0;
        labels->InsertNextValue(value);
    }
    if (!labels->GetNumberOfValues()) labels->InsertNextValue(range[0]);
    return labels;
}

inline std::string GetLabelFormat(vtkDoubleArray& labels)
{
    if (labels.GetNumberOfValues() < 2) return "%.6g";
    const double step = labels.GetValue(1) - labels.GetValue(0);
    if (!std::isfinite(step) || step <= 0) return "%.6g";
    const int exponent = static_cast<int>(std::floor(std::log10(step)));
    const double fraction = step / std::pow(10.0, exponent);
    const int decimals = std::max(0, -exponent) + (std::abs(fraction - std::round(fraction)) > 1e-7 ? 1 : 0);
    const double scale = std::max(std::abs(labels.GetValue(0)), std::abs(labels.GetValue(labels.GetNumberOfValues() - 1)));
    if (decimals > 6 || scale >= 1e7) {
        const int digits = static_cast<int>(std::clamp(std::ceil(std::log10(scale) - std::log10(step)) + 1, 2.0, 16.0));
        return "%." + std::to_string(digits) + "e";
    }
    return "%." + std::to_string(decimals) + "f";
}

inline void SetLegend(vtkScalarBarActor& legend, const char* title)
{
    legend.SetTitle(title);
    if (auto* lookup = legend.GetLookupTable()) {
        const auto* range = lookup->GetRange();
        auto labels = BuildLabels({range[0], range[1]});
        legend.SetCustomLabels(labels);
        legend.UseCustomLabelsOn();
        legend.SetLabelFormat(GetLabelFormat(*labels).c_str());
    }
    legend.DrawBelowRangeSwatchOn();legend.SetBelowRangeAnnotation(u8"下侧无界段 → −∞");
    legend.DrawAboveRangeSwatchOn();legend.SetAboveRangeAnnotation(u8"上侧无界段 → +∞");
    legend.DrawNanAnnotationOn();legend.SetNanAnnotation(u8"无效／未测");
    legend.SetWidth(0.20);
    legend.SetBarRatio(0.13);
    legend.SetMaximumNumberOfColors(256);
    legend.SetTextPad(2);
    legend.SetHeight(0.60);
    legend.SetPosition(0.025, 0.20);
    legend.SetMaximumWidthInPixels(80);
    legend.SetMaximumHeightInPixels(260);
    legend.SetUnconstrainedFontSize(true);
    for (auto* text : {legend.GetTitleTextProperty(), legend.GetLabelTextProperty(),
             legend.GetAnnotationTextProperty()}) {
        RenderTextStyle::SetFont(*text);
        text->SetFontSize(11);
        text->SetColor(1, 1, 1);
        text->BoldOff(); text->ItalicOff(); text->ShadowOff();
    }
}
}
