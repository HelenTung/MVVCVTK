#pragma once

#include <vtkCoordinate.h>
#include <vtkSmartPointer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

// 仓内文字排版与字体资源选择；不解释业务结果，也不进入 SDK 安装面。
namespace RenderTextStyle {
inline const std::string& GetFontPath()
{
    static const std::string path = [] {
        for (const char* variable : {"WINDIR", "SystemRoot"}) {
            char* value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, variable) != 0) continue;
            const std::unique_ptr<char, decltype(&std::free)> root(value, &std::free);
            if (!root) continue;
            for (const char* name : {"msyh.ttc", "simhei.ttf", "simsun.ttc"}) {
                const auto candidate = std::filesystem::path(root.get()) / "Fonts" / name;
                std::error_code error;
                if (candidate.is_absolute() && std::filesystem::is_regular_file(candidate, error))
                    return candidate.string();
            }
        }
        return std::string{};
    }();
    return path;
}

inline void SetFont(vtkTextProperty& property)
{
    const auto& path = GetFontPath();
    if (!path.empty()) {
        property.SetFontFamily(VTK_FONT_FILE);
        property.SetFontFile(path.c_str());
    }
}

inline void SetCaption(vtkTextActor& actor, const char* text, int row)
{
    actor.SetInput(text);
    actor.PickableOff();
    auto anchor = vtkSmartPointer<vtkCoordinate>::New();
    anchor->SetCoordinateSystemToNormalizedViewport();
    anchor->SetValue(0.5, 1.0);
    actor.GetPositionCoordinate()->SetCoordinateSystemToViewport();
    actor.GetPositionCoordinate()->SetReferenceCoordinate(anchor);
    actor.SetPosition(0, -12 - row * 16);
    auto* property = actor.GetTextProperty();
    SetFont(*property);
    property->SetFontSize(11);
    property->SetColor(1, 1, 1);
    property->SetJustificationToCentered();
    property->SetVerticalJustificationToTop();
    property->BoldOff(); property->ItalicOff(); property->ShadowOn();
}
}
