// 测试用途：声明 Qt 宿主方法回归的共享辅助及各套件入口。
#pragma once

#include <string>
#include <string_view>

bool GetCaseResult(bool isExpected, const char* caseName);
const std::string& GetMethodExecutable();
void SetMethodExecutable(std::string executable);

int GetLoadFailCount();
int GetRenderProductFailCount();
int GetViewFailCount();
int GetCropFailCount();
int GetCropLifecycleFailCount();
int GetCropArchiveFailCount();
int GetCropRealFailCount();
int GetGapFailCount();
int GetPartSceneFailCount();
int GetExportFailCount();
int GetLifecycleFailCount();
int GetLabelMapFailCount();
int StartLifecycleDeathCase(std::string_view caseName);
