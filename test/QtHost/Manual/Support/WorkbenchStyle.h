// 测试用途：统一手动工作台与自动化窗口的 Qt 外观。
#pragma once
#include <QColor>
#include <QString>
namespace Manual {
struct WorkbenchPlotColors {
    QColor surface;
    QColor text;
    QColor grid;
    QColor series;
    QColor comparison;
    QColor marker;
};

WorkbenchPlotColors GetWorkbenchPlotColors();
QString GetWorkbenchStyle();
}
