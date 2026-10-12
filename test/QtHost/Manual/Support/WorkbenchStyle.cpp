#include "WorkbenchStyle.h"
namespace Manual {
QString GetWorkbenchStyle()
{
    return QStringLiteral(R"(
QMainWindow, QWidget#workbench { background: #eaf0f6; color: #23364b; }
QWidget#workbenchToolbar, QWidget#workbenchNavigation, QWidget#diagnosticBar,
QWidget#objectColumn, QWidget#parameterPages { background: #ffffff; }
QWidget#currentInputBar { background: #ffffff; border: 1px solid #dae3ed; border-radius: 4px; }
QLabel#currentInputIdentity { color: #214c7a; font-weight: 600; }
QLabel#currentInputGeometry, QLabel#businessInput { color: #62758b; }
QLabel#businessStage { color: #36516f; background: #edf3fa; padding: 7px; border-radius: 4px; font-weight: 600; }
QLabel#businessNext { color: #36516f; }
QLabel#businessDisplay { color: #62758b; font-size: 11px; }
QLabel#businessRecovery { color: #946021; background: #fff8ed; padding: 6px; }
QLabel#columnTitle, QLabel#moduleTitle { color: #214c7a; font-weight: 600; }
QLabel#moduleTitle { font-size: 16px; }
QLabel#operationTitle { font-weight: 600; color: #214c7a; }
QLabel#operationDescription, QLabel#moduleNotice { color: #62758b; }
QLabel#commandRequirement { color: #946021; background: #fff8ed; padding: 7px; border-radius: 3px; }
QWidget#viewHeader { background: #ffffff; }
QLabel#businessMeasurements { background: #edf3fa; padding: 7px; color: #536579; }
QTreeWidget { border: 1px solid #dae3ed; background: white; alternate-background-color: #f5f8fc; }
QTreeWidget::item { padding: 6px 3px; }
QTreeWidget::item:selected { background: #e3eefb; color: #214c7a; }
QHeaderView::section { background: #f2f6fb; color: #62758b; border: 0; padding: 6px; }
QPushButton, QToolButton { padding: 5px 9px; min-height: 22px; border: 1px solid #cbd9e8; border-radius: 4px; background: #ffffff; color: #36516f; }
QPushButton:hover, QToolButton:hover { background: #edf4fc; border-color: #7ba4cc; }
QPushButton:pressed, QToolButton:pressed, QPushButton:checked { background: #dceafb; color: #195f9e; }
QPushButton:disabled, QToolButton:disabled { background: #f2f5f9; color: #92a0b1; border-color: #dae3ed; }
QPushButton[primaryCommand="true"] { background: #2463a6; color: white; border-color: #2463a6; font-weight: 600; padding: 7px 12px; }
QPushButton[primaryCommand="true"]:hover { background: #19558f; }
QPushButton[primaryCommand="true"]:disabled { background: #e3eaf2; color: #92a0b1; border-color: #dae3ed; }
QGroupBox { border: 1px solid #dae3ed; border-radius: 4px; margin-top: 8px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #36516f; }
QLineEdit, QComboBox { min-height: 24px; padding: 2px 5px; border: 1px solid #cbd9e8; border-radius: 3px; background: white; color: #23364b; }
QLineEdit:disabled, QComboBox:disabled { background: #f2f5f9; color: #92a0b1; }
QLineEdit:focus, QComboBox:focus { border-color: #2463a6; }
QTabBar::tab { padding: 8px 10px; background: #edf3fa; color: #62758b; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: #f7faff; border-bottom: 2px solid #2463a6; color: #195f9e; font-weight: 600; }
QTabBar#workspaceTabs::tab { background: white; border: 0; padding: 9px 16px; }
QTabBar#workspaceTabs::tab:selected { background: #e3eefb; color: #195f9e; }
QToolButton[parameterSection="true"] { text-align: left; background: #edf3fa; border: 0; border-radius: 0; color: #36516f; font-weight: 600; padding: 7px; }
QPlainTextEdit { border: 1px solid #dae3ed; background: #ffffff; color: #23364b; }
QMenu { background: white; color: #23364b; border: 1px solid #dae3ed; }
QMenu::item { padding: 7px 24px; }
QMenu::item:selected { background: #e3eefb; }
QMenu::item:disabled { color: #92a0b1; }
QSplitter::handle { background: #eaf0f6; }
)");
}
}
