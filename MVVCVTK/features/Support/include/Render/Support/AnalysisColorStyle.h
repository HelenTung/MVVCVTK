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

// 仅复用显示色带和色标排版；数值范围及业务单位由所属 Feature 决定。
namespace AnalysisColorStyle {
inline vtkSmartPointer<vtkLookupTable> BuildRamp(
    const std::array<double, 2>& range, bool isLowRed)
{
    auto table = vtkSmartPointer<vtkLookupTable>::New();
    table->SetNumberOfTableValues(256);
    table->SetTableRange(range.data());
    table->SetHueRange(isLowRed ? 0.0 : 2.0 / 3.0, isLowRed ? 2.0 / 3.0 : 0.0);
    // 保持原有颜色方向，降低饱和度和亮度；同一 LUT 同时用于结果与色标。
    table->SetSaturationRange(0.65, 0.65);
    table->SetValueRange(0.84, 0.84);
    table->SetRampToLinear();
    table->SetBelowRangeColor(0.64, 0.29, 0.78, 1);
    table->SetAboveRangeColor(0.64, 0.29, 0.78, 1);
    table->UseBelowRangeColorOn();
    table->UseAboveRangeColorOn();
    table->Build();
    return table;
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

inline void SetLegend(vtkScalarBarActor& legend, const char* title, bool isRight = false)
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
    legend.SetTextPad(2);
    legend.SetHeight(0.60);
    legend.SetPosition(isRight ? 0.78 : 0.025, 0.20);
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
