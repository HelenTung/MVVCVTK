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
int GetGapFailCount();
int GetPartSceneFailCount();
int GetExportFailCount();
int GetLifecycleFailCount();
int GetLabelMapFailCount();
int StartLifecycleDeathCase(std::string_view caseName);
