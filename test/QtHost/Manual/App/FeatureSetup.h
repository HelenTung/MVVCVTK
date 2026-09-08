// 测试用途：按构建开关创建并挂载七项功能，组合手动和自动化共用的测试页面。
#pragma once
#include "Modules/ModulePanel.h"
#include <vector>
namespace Manual {
std::vector<ModulePanel*> BuildModules(TestContext context, QWidget* parent);
}
