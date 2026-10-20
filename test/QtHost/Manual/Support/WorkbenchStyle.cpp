#include "WorkbenchStyle.h"
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <algorithm>
#include <utility>
namespace Manual {
namespace {
QColor AuditColor(const QColor& color)
{
    if(!qApp->property("uiAuditGrayscale").toBool())return color;
    const auto gray=qGray(color.rgb());return QColor(gray,gray,gray,color.alpha());
}
}
WorkbenchPlotColors GetWorkbenchPlotColors()
{
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    WorkbenchPlotColors colors=dark?WorkbenchPlotColors{QColor("#222e3c"), QColor("#b8c5d4"), QColor("#526174"),
        QColor("#8fbfdf"), QColor("#efb56f"), QColor("#ff8d8d")}:WorkbenchPlotColors{QColor("#ffffff"), QColor("#526174"), QColor("#d8e0e8"),
        QColor("#37688b"), QColor("#90521c"), QColor("#b62d31")};
    for(auto* color:{&colors.surface,&colors.text,&colors.grid,&colors.series,&colors.comparison,&colors.marker})*color=AuditColor(*color);
    return colors;
}

QString GetWorkbenchStyle()
{
    // 颜色按用途集中定义；深色系统调色板使用对应的表面与文字。
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    const auto applicationFont=QApplication::font();
    const auto bodyPixels=std::max(16.0,applicationFont.pixelSize()>0?static_cast<double>(applicationFont.pixelSize()):applicationFont.pointSizeF()*96.0/72.0);
    auto style = QStringLiteral(R"(
QWidget { color: @text; font-family: "Microsoft YaHei UI"; font-size: @bodyPixelspx; }
QMainWindow, QWidget#workbench { background: @canvas; }
QWidget#workbenchToolbar, QWidget#workbenchNavigation, QWidget#workbenchInput,
QWidget#objectColumn, QWidget#parameterPages { background: @surface; }
QWidget#workbenchToolbar, QWidget#objectColumn, QWidget#parameterPages { border-radius: 6px; }
QToolBar { background: @surface; border: 0; border-bottom: 1px solid @outline; spacing: 4px; padding: 4px; }
QToolBar#contextToolbar { padding: 0; border: 0; spacing: 0; }
QToolBar#contextToolbar QToolButton { padding: 5px 8px; border-radius: 0; }
QToolBar#quickToolbar QPushButton, QToolBar#quickToolbar QToolButton { padding: 5px 6px; }
QDockWidget { color: @text; background: @surface; }
QDockWidget::title { background: @subtle; padding: 6px 8px; text-align: left; }
QMainWindow::separator { width: 6px; height: 6px; background: @canvas; }
QMainWindow::separator:hover { background: @outline; }
QLabel#currentInputIdentity { padding: 2px 0; color: @muted; }
QLabel#columnTitle, QLabel#moduleTitle { color: @text; font-weight: 600; }
QLabel#currentInputGeometry, QLabel#businessInput, QLabel#operationDescription,
QLabel#moduleNotice, QLabel#selectedObjectSummary { color: @muted; }
QLabel#businessStage { color: @status; background: @statusSurface; padding: 4px; border-radius: 4px; font-weight: 600; }
QLabel#businessNext { color: @text; }
QLabel#businessDisplay { color: @muted; }
QLabel#businessRecovery, QLabel#commandRequirement { color: @warning; background: @warningSurface; padding: 8px; border-radius: 4px; }
QLabel#moduleTitle, QLabel#workbenchTitle { font-size: @headingPixelspx; font-weight: 600; }
QLabel#operationTitle { font-weight: 600; }
QLabel#parameterTitle { color: @muted; font-weight: 400; }
QLabel#parameterUnit { color: @muted; }
QWidget#operationPrerequisites { background: @warningSurface; border-radius: 4px; }
QToolButton#showOperationPrerequisites { text-align: left; color: @warning; background: @warningSurface; border: 0; padding: 2px 4px; }
QToolButton#showOperationPrerequisites:focus { border: 1px dashed @accent; padding: 1px 3px; }
QWidget[propertyRow="true"] { border-bottom: 1px solid @subtle; }
QWidget#viewHeader { background: @subtle; }
QWidget#viewHeader QPushButton, QWidget#viewHeader QToolButton { padding: 2px 4px; min-height: 22px; }
QWidget#viewHeader QComboBox { padding: 2px 4px; min-height: 22px; }
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
QLineEdit, QComboBox { min-height: 24px; padding: 4px 6px; border: 1px solid @outline; border-radius: 4px; background: @surface; color: @text; selection-background-color: @accent; selection-color: @onAccent; }
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
QTabBar#workflowSteps::tab { padding: 5px 6px; background: @surface; }
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
    for (const auto& color : colors) style.replace(color.first, AuditColor(QColor(color.second)).name());
    style.replace("@bodyPixels", QString::number(bodyPixels, 'f', 0));
    style.replace("@headingPixels", QString::number(bodyPixels * 1.125, 'f', 0));
    return style;
}
}
