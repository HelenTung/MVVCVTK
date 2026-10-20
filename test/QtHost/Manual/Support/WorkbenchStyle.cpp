#include "WorkbenchStyle.h"
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <algorithm>
#include <utility>
namespace Manual {
WorkbenchPlotColors GetWorkbenchPlotColors()
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    if (dark) return {QColor("#222e3c"), QColor("#b8c5d4"), QColor("#526174"),
        QColor("#8fbfdf"), QColor("#efb56f"), QColor("#f5cb88")};
    return {QColor("#ffffff"), QColor("#526174"), QColor("#d8e0e8"),
        QColor("#37688b"), QColor("#90521c"), QColor("#875215")};
}

QString GetWorkbenchStyle()
{
    // 颜色按用途集中定义；深色系统调色板使用对应的表面与文字。
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const auto bodyPoints = std::max(12.0, QApplication::font().pointSizeF());
    auto style = QStringLiteral(R"(
QWidget { color: @text; font-size: @bodyPointspt; }
QMainWindow, QWidget#workbench { background: @canvas; }
QWidget#workbenchToolbar, QWidget#workbenchNavigation, QWidget#workbenchInput,
QWidget#objectColumn, QWidget#parameterPages { background: @surface; }
QWidget#workbenchToolbar, QWidget#objectColumn, QWidget#parameterPages { border-radius: 6px; }
QWidget#parameterPages, QWidget#parameterPages QWidget { font-family: "Microsoft YaHei UI"; }
QLabel#currentInputIdentity, QLabel#columnTitle, QLabel#moduleTitle { color: @text; font-weight: 600; }
QLabel#currentInputGeometry, QLabel#businessInput, QLabel#operationDescription,
QLabel#moduleNotice, QLabel#selectedObjectSummary { color: @muted; }
QLabel#businessStage { color: @status; background: @statusSurface; padding: 4px; border-radius: 4px; font-weight: 600; }
QLabel#businessNext { color: @text; }
QLabel#businessDisplay { color: @muted; }
QLabel#businessRecovery, QLabel#commandRequirement { color: @warning; background: @warningSurface; padding: 8px; border-radius: 4px; }
QLabel#moduleTitle, QLabel#workbenchTitle { font-size: @headingPointspt; font-weight: 600; }
QLabel#operationTitle { font-weight: 600; }
QLabel#parameterTitle { color: @muted; font-weight: 400; }
QLabel#parameterUnit { color: @muted; }
QWidget#operationPrerequisites { background: @warningSurface; border-radius: 4px; }
QToolButton#showOperationPrerequisites { text-align: left; color: @warning; background: @warningSurface; border: 0; padding: 2px 4px; }
QToolButton#showOperationPrerequisites:focus { border: 1px dashed @accent; padding: 1px 3px; }
QWidget[propertyRow="true"] { border-bottom: 1px solid @subtle; }
QWidget#viewHeader { background: @surface; }
QWidget#operationFields, QWidget#sectionFields { background: @surface; }
QWidget#businessContext { background: @subtle; border-radius: 4px; }
QWidget#businessCommandFooter { border-top: 1px solid @outline; }
QLabel#businessMeasurements { background: @subtle; padding: 8px; color: @muted; }
QLabel#operationState { color: @muted; background: transparent; border: none; padding: 0; }
QLabel#moduleFeedback { color: @muted; border-left: 3px solid @outline; padding-left: 6px; }
QLabel#moduleFeedback[feedbackTone="neutral"] { color: @muted; border-left-color: @outline; }
QLabel#moduleFeedback[feedbackTone="busy"] { color: @status; border-left-color: @status; background: @statusSurface; }
QLabel#moduleFeedback[feedbackTone="warning"] { color: @warning; border-left-color: @warning; background: @warningSurface; }
QLabel#moduleFeedback[feedbackTone="error"] { color: @error; border-left-color: @error; background: @errorSurface; }
QTreeWidget, QTableView { border: 1px solid @outline; background: @surface; alternate-background-color: @subtle; gridline-color: @outline; }
QTreeWidget::item { padding: 6px 3px; }
QTreeWidget::item:selected, QTableView::item:selected { background: @selection; color: @text; }
QHeaderView::section { background: @subtle; color: @muted; border: 0; border-bottom: 1px solid @outline; padding: 6px; }
QPushButton, QToolButton { padding: 5px 10px; min-height: 22px; border: 1px solid @outline; border-radius: 4px; background: @surface; color: @text; }
QPushButton:hover, QToolButton:hover { background: @subtle; border-color: @accent; }
QPushButton:pressed, QToolButton:pressed, QPushButton:checked, QToolButton:checked { background: @selection; color: @accent; }
QPushButton:focus, QToolButton:focus { border-color: @accent; border-style: dashed; }
QPushButton:disabled, QToolButton:disabled { background: @subtle; color: @disabled; border-color: @outline; }
QPushButton[primaryCommand="true"] { background: @accent; color: @onAccent; border-color: @accent; font-weight: 600; padding: 7px 12px; }
QPushButton[primaryCommand="true"]:hover { background: @accentHover; }
QPushButton[primaryCommand="true"]:focus { border: 2px dotted @onAccent; }
QPushButton[primaryCommand="true"]:disabled { background: @subtle; color: @disabled; border-color: @outline; }
QGroupBox { background: @surface; border: 1px solid @outline; border-radius: 4px; margin-top: 10px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @muted; }
QLineEdit, QComboBox { min-height: 24px; padding: 3px 6px; border: 1px solid @outline; border-radius: 4px; background: @surface; color: @text; selection-background-color: @accent; selection-color: @onAccent; }
QLineEdit:disabled, QComboBox:disabled { background: @subtle; color: @disabled; }
QLineEdit:focus, QComboBox:focus { border-color: @accent; }
QComboBox QAbstractItemView { background: @surface; color: @text; selection-background-color: @selection; selection-color: @text; }
QCheckBox { spacing: 6px; border: 1px solid transparent; padding: 1px; }
QCheckBox:focus { border-color: @accent; border-style: dashed; }
QTabBar { border: 1px solid transparent; }
QTabBar:focus { border-color: @accent; border-style: dashed; }
QTreeWidget:focus, QTableView:focus { border-color: @accent; }
QTabBar::tab { padding: 8px 10px; background: @subtle; color: @muted; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: @surface; border-bottom: 2px solid @accent; color: @accent; font-weight: 600; }
QTabBar::tab:hover { color: @accent; }
QTabBar#workspaceTabs::tab { background: @surface; padding: 10px 16px; }
QTabBar#workspaceTabs::tab:selected { background: @selection; }
QTabWidget::pane { border: 1px solid @outline; background: @surface; }
QToolButton[parameterSection="true"] { text-align: left; background: @subtle; border: 0; border-radius: 2px; color: @text; font-weight: 600; padding: 2px 6px; }
QToolButton[parameterSection="true"]:focus { border: 1px dashed @accent; padding: 1px 5px; }
QPlainTextEdit { border: 1px solid @outline; background: @surface; color: @text; }
QMenu, QToolTip { background: @surface; color: @text; border: 1px solid @outline; }
QMenu::item { padding: 8px 24px; }
QMenu::item:selected { background: @selection; }
QMenu::item:disabled { color: @disabled; }
QSplitter::handle { background: @canvas; }
QSplitter::handle:horizontal { background: @outline; border-left: 3px solid @canvas; border-right: 3px solid @canvas; }
QSplitter::handle:hover { background: @outline; }
QScrollBar:vertical { background: @subtle; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: @outline; min-height: 28px; border-radius: 5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)");
    const std::pair<const char*, QString> colors[]{
        {"@canvas", dark ? "#18212c" : "#edf1f5"},
        {"@surface", dark ? "#222e3c" : "#ffffff"},
        {"@text", dark ? "#e5edf5" : "#263446"},
        {"@muted", dark ? "#b8c5d4" : "#526174"},
        {"@selection", dark ? "#263f5b" : "#e8f1fb"},
        {"@subtle", dark ? "#293747" : "#f3f6fa"},
        {"@outline", dark ? "#7a8da4" : "#7c8a9e"},
        {"@disabled", dark ? "#94a3b8" : "#6b7b8f"},
        {"@warningSurface", dark ? "#493820" : "#fff4e5"},
        {"@warning", dark ? "#f5cb88" : "#875215"},
        {"@errorSurface", dark ? "#4a282b" : "#fff0ef"},
        {"@error", dark ? "#ffb4b4" : "#a62b26"},
        {"@onAccent", dark ? "#152436" : "#ffffff"},
        {"@accentHover", dark ? "#b5d7ff" : "#164c83"},
        {"@accent", dark ? "#9ac9ff" : "#1e5fa3"},
        {"@statusSurface", dark ? "#2e3b48" : "#edf2f6"},
        {"@status", dark ? "#c1cbd4" : "#40566d"}};
    for (const auto& color : colors) style.replace(color.first, color.second);
    style.replace("@bodyPoints", QString::number(bodyPoints, 'f', 1));
    style.replace("@headingPoints", QString::number(bodyPoints * 1.125, 'f', 1));
    return style;
}
}
