// 测试用途：启动交互式功能手动测试窗口；自动回归由独立自动化项目执行。
#include "App/TestWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <iostream>

int main(int argc, char* argv[])
{
    auto format = QVTKOpenGLNativeWidget::defaultFormat(); format.setSamples(0); format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("全功能手动测试；结构化参数支持 JSON 导入。");
    parser.addHelpOption();
    parser.addOption({"memory-budget-mib", "算法工作集上限（MiB），0 为按本机可用内存自动配置", "MiB", "0"});
    parser.process(app);
    bool valid = false; const auto budget = parser.value("memory-budget-mib").toULongLong(&valid);
    if (!valid) parser.showHelp(2);
    try { Manual::TestWindow window(budget); window.show(); return app.exec(); }
    catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
