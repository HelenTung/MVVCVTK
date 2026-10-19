#include "WorkbenchStyle.h"
#include <QApplication>
#include <QPalette>
#include <utility>
namespace Manual {
QString GetWorkbenchStyle()
{
    // 颜色按用途集中定义；深色系统调色板使用对应的表面与文字。
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    auto style = QStringLiteral(R"(
QWidget { color: @text; }
QMainWindow, QWidget#workbench { background: @canvas; }
QWidget#workbenchToolbar, QWidget#workbenchNavigation, QWidget#workbenchInput,
QWidget#diagnosticBar, QWidget#objectColumn, QWidget#parameterPages { background: @surface; }
QWidget#workbenchToolbar, QWidget#objectColumn, QWidget#parameterPages { border-radius: 6px; }
QLabel#currentInputIdentity, QLabel#columnTitle, QLabel#moduleTitle { color: @text; font-weight: 600; }
QLabel#currentInputGeometry, QLabel#businessInput, QLabel#operationDescription,
QLabel#moduleNotice, QLabel#selectedObjectSummary, QLabel#messageDataSummary { color: @muted; }
QLabel#businessStage { color: @accent; background: @selection; padding: 8px; border-radius: 4px; font-weight: 600; }
QLabel#businessNext { color: @text; }
QLabel#businessDisplay { color: @muted; }
QLabel#businessRecovery, QLabel#commandRequirement { color: @warning; background: @warningSurface; padding: 8px; border-radius: 4px; }
QLabel#moduleTitle { font-size: 16px; }
QLabel#operationTitle, QLabel#parameterTitle { font-weight: 600; }
QWidget#viewHeader { background: @surface; }
QWidget#operationFields, QWidget#sectionFields { background: @surface; }
QWidget#businessContext { background: @subtle; border-radius: 4px; }
QWidget#businessCommandFooter { border-top: 1px solid @outline; }
QLabel#businessMeasurements { background: @subtle; padding: 8px; color: @muted; }
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
QPushButton[primaryCommand="true"]:disabled { background: @subtle; color: @disabled; border-color: @outline; }
QGroupBox { background: @surface; border: 1px solid @outline; border-radius: 4px; margin-top: 10px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @muted; }
QLineEdit, QComboBox { min-height: 24px; padding: 3px 6px; border: 1px solid @outline; border-radius: 4px; background: @surface; color: @text; selection-background-color: @accent; selection-color: @onAccent; }
QLineEdit:disabled, QComboBox:disabled { background: @subtle; color: @disabled; }
QLineEdit:focus, QComboBox:focus { border-color: @accent; }
QComboBox QAbstractItemView { background: @surface; color: @text; selection-background-color: @selection; selection-color: @text; }
QCheckBox { spacing: 6px; }
QTabBar::tab { padding: 8px 10px; background: @subtle; color: @muted; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: @surface; border-bottom: 2px solid @accent; color: @accent; font-weight: 600; }
QTabBar::tab:hover { color: @accent; }
QTabBar#workspaceTabs::tab { background: @surface; padding: 10px 16px; }
QTabBar#workspaceTabs::tab:selected { background: @selection; }
QTabWidget::pane { border: 1px solid @outline; background: @surface; }
QToolButton[parameterSection="true"] { text-align: left; background: @subtle; border: 0; border-radius: 4px; color: @text; font-weight: 600; padding: 8px; }
QPlainTextEdit { border: 1px solid @outline; background: @surface; color: @text; }
QMenu, QToolTip { background: @surface; color: @text; border: 1px solid @outline; }
QMenu::item { padding: 8px 24px; }
QMenu::item:selected { background: @selection; }
QMenu::item:disabled { color: @disabled; }
QSplitter::handle { background: @canvas; }
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
        {"@outline", dark ? "#52657b" : "#cbd5e1"},
        {"@disabled", dark ? "#94a3b8" : "#6b7b8f"},
        {"@warningSurface", dark ? "#493820" : "#fff4e5"},
        {"@warning", dark ? "#f5cb88" : "#875215"},
        {"@onAccent", dark ? "#152436" : "#ffffff"},
        {"@accentHover", dark ? "#b5d7ff" : "#164c83"},
        {"@accent", dark ? "#9ac9ff" : "#1e5fa3"}};
    for (const auto& color : colors) style.replace(color.first, color.second);
    return style;
}
}
