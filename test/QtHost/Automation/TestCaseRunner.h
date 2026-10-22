// 测试用途：自动执行实际页面入口的合成回归或指定用例，验证中文分派、业务日志和生命周期。
#pragma once
#include <QString>
#include <functional>
namespace Manual {
class TestWindow;
// 只驱动测试进程中的同步 popup/modal 入口；两个回调都在 GUI 所属线程调用。
// complete 返回 false 时仅重试就绪检查，true 表示已完成一次真实交互；异常在退出弹窗后重抛。
void DrivePopupInteraction(const std::function<void()>& open,const std::function<bool()>& complete,
    const char* failureMessage,int timeoutMs=5000);
int StartTestCase(TestWindow& window, const QString& path, const QString& recordPath, bool selfTest);
int StartCaptureConsistencyTest(TestWindow& window,const QString& output,const QString& recordPath);
}
