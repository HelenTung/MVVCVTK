#pragma once

#include <vtkLookupTable.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>
#include <algorithm>
#include <array>

// 仅复用显示色带和色标排版；数值范围及业务单位由所属 Feature 决定。
namespace AnalysisColorStyle {
inline vtkSmartPointer<vtkLookupTable> BuildRamp(
    const std::array<double, 2>& range, bool isLowRed)
{
    auto table = vtkSmartPointer<vtkLookupTable>::New();
    table->SetNumberOfTableValues(256);
    table->SetTableRange(range.data());
    table->SetHueRange(isLowRed ? 0.0 : 2.0 / 3.0, isLowRed ? 2.0 / 3.0 : 0.0);
    table->SetSaturationRange(1, 1);
    table->SetValueRange(1, 1);
    table->SetBelowRangeColor(0.75, 0, 1, 1);
    table->SetAboveRangeColor(0.75, 0, 1, 1);
    table->UseBelowRangeColorOn();
    table->UseAboveRangeColorOn();
    table->Build();
    return table;
}

inline void SetLegend(vtkScalarBarActor& legend, const char* title, bool isRight = false)
{
    legend.SetTitle(title);
    legend.SetNumberOfLabels(11);
    legend.SetLabelFormat("%.2f");
    legend.SetWidth(0.12);
    legend.SetHeight(0.60);
    legend.SetPosition(isRight ? 0.76 : 0.025, 0.20);
    legend.SetMaximumWidthInPixels(100);
    legend.SetMaximumHeightInPixels(300);
    legend.SetUnconstrainedFontSize(true);
    for (auto* text : {legend.GetTitleTextProperty(), legend.GetLabelTextProperty(),
             legend.GetAnnotationTextProperty()}) {
        text->SetFontSize(12);
        text->SetColor(1, 1, 1);
        text->BoldOff(); text->ItalicOff(); text->ShadowOff();
    }
}
}
