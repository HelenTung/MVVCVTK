// 测试用途：启动功能自动化测试，执行默认回归或指定 JSON 用例并导出记录。
#include "App/TestWindow.h"
#include "TestCaseRunner.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QMetaObject>
#include <QVTKOpenGLNativeWidget.h>
#include <iostream>

int main(int argc, char* argv[])
{
    auto format = QVTKOpenGLNativeWidget::defaultFormat(); format.setSamples(0); format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("功能自动化测试；默认执行合成数据、界面分派和生命周期回归。");
    parser.addHelpOption();
    parser.addOption({"case", "执行指定的 JSON 用例文件", "path"});
    parser.addOption({"record", "导出自动化操作记录", "path"});
    parser.addOption({"memory-budget-mib", "算法工作集上限（MiB），0 为按本机可用内存自动配置", "MiB", "0"});
    parser.process(app);
    bool valid = false; const auto budget = parser.value("memory-budget-mib").toULongLong(&valid);
    if (!valid) parser.showHelp(2);
    try {
    Manual::TestWindow window(budget);
    window.setWindowTitle("功能自动化测试");
    window.show();
    QMetaObject::invokeMethod(&window, [&] {
        const auto result = Manual::StartTestCase(window, parser.value("case"), parser.value("record"), !parser.isSet("case"));
        if (!window.GetSession()) app.exit(result);
    }, Qt::QueuedConnection);
    return app.exec();
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
