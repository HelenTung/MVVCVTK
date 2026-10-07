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
#include <optional>
#include <vector>
#include <vtkMath.h>

// 仅复用显示色带和色标排版；数值范围及业务单位由所属 Feature 决定。
namespace AnalysisColorStyle {
enum class RampMode { Constant, Gradient, Rainbow, InverseRainbow, HueLoop };
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
    std::vector<RampSegment> segments;
    BlendMode blend = BlendMode::Constant;
    std::array<double,2> opacityRange{0.15,1.0};
};

inline bool GetModeValid(RampMode mode) noexcept
{
    return mode==RampMode::Constant || mode==RampMode::Gradient || mode==RampMode::Rainbow
        || mode==RampMode::InverseRainbow || mode==RampMode::HueLoop;
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
             params.belowColor, params.aboveColor})
        if (!GetColorValid(color)) return false;
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

inline double GetFraction(double lower,double upper,double value)
{
    const double width=upper-lower;
    return std::clamp(std::isfinite(width) ? (value-lower)/width
        : (value*.5-lower*.5)/(upper*.5-lower*.5),0.0,1.0);
}

inline std::array<double,3> GetSegmentColor(const RampSegment& segment,double value)
{
    if (segment.mode==RampMode::Constant) return segment.lowColor;
    const double fraction=GetFraction(*segment.lower,*segment.upper,value);
    std::array<double,3> color{};
    if (segment.mode==RampMode::Gradient) {
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

inline vtkSmartPointer<vtkLookupTable> BuildRamp(
    const std::array<double, 2>& range, const RampParams& params)
{
    auto table = vtkSmartPointer<vtkLookupTable>::New();
    table->SetNumberOfTableValues(256);
    table->SetTableRange(range.data());
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
    if (params.mode == RampMode::Constant || params.mode == RampMode::Gradient)
        for (int index = 0; index < 256; ++index) {
            std::array<double, 3> color = params.constantColor;
            if (params.mode == RampMode::Gradient)
                for (int component = 0; component < 3; ++component)
                    color[component] = params.lowColor[component]
                        + (params.highColor[component] - params.lowColor[component]) * index / 255.0;
            table->SetTableValue(index, color[0], color[1], color[2], 1);
        }
    if (!params.segments.empty() || params.blend!=BlendMode::Constant) {
        // 分段只改变显示映射；边界按输入值域处理，不参与业务数据筛选。
        for (int index=0;index<256;++index) {
            const double fraction=index/255.0;
            const double value=(1-fraction)*range[0]+fraction*range[1];
            double rgba[4]; table->GetTableValue(index,rgba);
            if (!params.segments.empty()) {
                std::array<double,3> color=params.aboveColor;
                if (params.segments.front().lower && value<*params.segments.front().lower) color=params.belowColor;
                else for (const auto& segment:params.segments) {
                    if ((!segment.lower || value>=*segment.lower)
                        && (!segment.upper || value<*segment.upper
                            || (&segment==&params.segments.back() && value==*segment.upper))) {
                        color=GetSegmentColor(segment,value);break;
                    }
                }
                std::copy(color.begin(),color.end(),rgba);
            }
            rgba[3]=GetOpacity(fraction,params);
            table->SetTableValue(index,rgba);
        }
        const auto& below=!params.segments.empty() && !params.segments.front().lower
            ? params.segments.front().lowColor : params.belowColor;
        const auto& above=!params.segments.empty() && !params.segments.back().upper
            ? params.segments.back().lowColor : params.aboveColor;
        table->SetBelowRangeColor(below[0],below[1],below[2],GetOpacity(0,params));
        table->SetAboveRangeColor(above[0],above[1],above[2],GetOpacity(1,params));
    }
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
    std::array<double,4> rgba{};table.GetColor(value,rgba.data());rgba[3]=table.GetOpacity(value);
    const auto* range=table.GetRange();
    if(value<range[0] || value>range[1])return rgba;
    // 正式值按实际分段边界着色，窄区间不依赖色标纹理的离散采样。
    if(!params.segments.empty()) {
        std::array<double,3> color=params.aboveColor;
        if(params.segments.front().lower && value<*params.segments.front().lower)color=params.belowColor;
        else for(const auto& segment:params.segments)
            if((!segment.lower || value>=*segment.lower)
                && (!segment.upper || value<*segment.upper
                    || (&segment==&params.segments.back() && value==*segment.upper))) {
                color=GetSegmentColor(segment,value);break;
            }
        std::copy(color.begin(),color.end(),rgba.begin());
    }
    if(params.blend!=BlendMode::Constant)
        rgba[3]=GetOpacity(GetFraction(range[0],range[1],value),params);
    return rgba;
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
