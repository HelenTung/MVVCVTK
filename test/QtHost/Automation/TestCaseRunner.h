// 测试用途：自动执行实际页面入口的合成回归或指定用例，验证中文分派、业务日志和生命周期。
#pragma once
#include <QString>
namespace Manual {
class TestWindow;
int StartTestCase(TestWindow& window, const QString& path, const QString& recordPath, bool selfTest);
}
