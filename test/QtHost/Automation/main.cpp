// 测试用途：启动功能自动化测试，执行默认回归或指定 JSON 用例并导出记录。
#include "App/TestWindow.h"
#include "TestCaseRunner.h"
#include "ReportWorkflowTests.h"
#include "ReportLargeTests.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QMetaObject>
#include <QVTKOpenGLNativeWidget.h>
#include <iostream>
#include <climits>
#include <cstring>
#include <string>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
struct DesktopHandle {
    HDESK value=nullptr;
    ~DesktopHandle(){if(value)CloseDesktop(value);}
};
int RunOnBackgroundDesktop()
{
    auto name=L"MVVCVTK-QtTest-"+std::to_wstring(GetCurrentProcessId());
    DesktopHandle desktop{CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr)};
    if(!desktop.value){std::cerr<<"Cannot create Qt test desktop: "<<GetLastError()<<std::endl;return 2;}
    wchar_t image[32768]{};const auto length=GetModuleFileNameW(nullptr,image,32768);
    if(!length || length>=32768){std::cerr<<"Cannot resolve Qt test executable."<<std::endl;return 2;}
    auto command=std::wstring(GetCommandLineW())+L" --background-child="+name;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.lpDesktop=name.data();
    startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    // 在加载 Qt/VTK 前指定子进程桌面；不切换用户当前显示的桌面。
    if(!CreateProcessW(image,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)){
        std::cerr<<"Cannot launch background Qt test: "<<GetLastError()<<std::endl;return 2;
    }
    CloseHandle(child.hThread);
    std::cout<<"BACKGROUND_DESKTOP: MVVCVTK-QtTest-"<<GetCurrentProcessId()<<" child="<<child.dwProcessId<<std::endl;
    DWORD result=2;
    if(WaitForSingleObject(child.hProcess,INFINITE)==WAIT_OBJECT_0)GetExitCodeProcess(child.hProcess,&result);
    else{TerminateProcess(child.hProcess,2);WaitForSingleObject(child.hProcess,5000);}
    CloseHandle(child.hProcess);
    return static_cast<int>(result);
}
bool IsBackgroundDesktop(const std::string& token)
{
    const std::wstring expected(token.begin(),token.end());wchar_t actual[256]{};DWORD size=0;
    return expected.rfind(L"MVVCVTK-QtTest-",0)==0
        && GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,actual,sizeof(actual),&size)
        && expected==actual;
}
#endif
}

int main(int argc, char* argv[])
{
    bool background=false;std::string childDesktop;
    for(int i=1;i<argc;++i){
        background=background || std::strcmp(argv[i],"--background")==0;
        const std::string arg=argv[i];const std::string prefix="--background-child=";
        if(arg.rfind(prefix,0)==0)childDesktop=arg.substr(prefix.size());
    }
    if(background){
#ifdef Q_OS_WIN
        if(childDesktop.empty())return RunOnBackgroundDesktop();
        if(!IsBackgroundDesktop(childDesktop)){std::cerr<<"Qt test desktop isolation mismatch."<<std::endl;return 2;}
        std::cout<<"BACKGROUND_CHILD_DESKTOP: "<<childDesktop<<std::endl;
#else
        std::cerr<<"Background Qt tests require Windows desktop isolation."<<std::endl;return 2;
#endif
    }else if(!childDesktop.empty()){std::cerr<<"Background child requires desktop isolation."<<std::endl;return 2;}
    auto format = QVTKOpenGLNativeWidget::defaultFormat(); format.setSamples(0); format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("功能自动化测试；默认执行合成数据、界面分派和生命周期回归。");
    parser.addHelpOption();
    parser.addOption({"case", "执行指定的 JSON 用例文件", "path"});
    parser.addOption({"record", "导出自动化操作记录", "path"});
    parser.addOption({"report-spatial-only", "仅复跑完整 CSV 的空间配对阶段"});
    parser.addOption({"report-csv", "复用已保存的完整 CSV 验证生产配对", "path"});
    parser.addOption({"gap-boundary-reports-only", "仅验证 Gap 边界对象的实际报告、列表、统计及 CSV"});
    parser.addOption({"gap-report-output", "Gap 报告定向验证输出目录", "path"});
    parser.addOption({"gap-report-real-input", "可选的32立方真实 float32 小样本", "path"});
    parser.addOption({"reports-only", "仅执行独立报告导入、比较、分页与选择回归，不创建业务会话"});
    parser.addOption({"background", "在独立后台桌面运行实际 Qt/VTK 窗口，不显示在用户桌面"});
    parser.addOption({"background-child", "内部测试子进程桌面标识", "name"});
    parser.addOption({"memory-budget-mib", "算法工作集上限（MiB），0 为不启用估算工作集硬限额", "MiB", "0"});
    parser.process(app);
    bool valid = false; const auto budget = parser.value("memory-budget-mib").toULongLong(&valid);
    if (!valid) parser.showHelp(2);
    try {
    if(parser.isSet("report-csv")){Manual::CheckSavedReportComparisons(parser.value("report-csv"),parser.isSet("report-spatial-only"));return 0;}
    if(parser.isSet("gap-boundary-reports-only")){Manual::CheckGapBoundaryReports(parser.value("gap-report-output"),parser.value("gap-report-real-input"));return 0;}
    if(parser.isSet("reports-only")){Manual::CheckReportWorkflow();return 0;}
    Manual::TestWindow window(budget);
    window.setWindowTitle("功能自动化测试");
    if(parser.isSet("background")){window.setAttribute(Qt::WA_ShowWithoutActivating,true);window.setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);window.setWindowFlag(Qt::WindowTransparentForInput,true);}
    window.show();
    QMetaObject::invokeMethod(&window, [&] {
        const auto result = Manual::StartTestCase(window, parser.value("case"), parser.value("record"), !parser.isSet("case"));
        if (!window.GetSession()) app.exit(result);
    }, Qt::QueuedConnection);
    return app.exec();
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
