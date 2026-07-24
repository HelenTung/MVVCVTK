#pragma once

#include <string>
#include <string_view>

bool GetCaseResult(bool isExpected, const char* caseName);
const std::string& GetMethodExecutable();
void SetMethodExecutable(std::string executable);

int GetLoadFailCount();
int GetViewFailCount();
int GetCropFailCount();
int GetGapFailCount();
int GetExportFailCount();
int GetLifecycleFailCount();
int StartLifecycleDeathCase(std::string_view caseName);
