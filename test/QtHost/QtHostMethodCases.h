#pragma once

bool GetCaseResult(bool isExpected, const char* caseName);

int GetLoadFailCount();
int GetViewFailCount();
int GetCropFailCount();
int GetGapFailCount();
int GetExportFailCount();
int GetLifecycleFailCount();
