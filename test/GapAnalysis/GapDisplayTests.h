// 测试用途：验证孔隙结果显示、视图切换、退出清理、失败重试和回调边界。
#pragma once

class GapDisplaySuite final {
public:
    int GetFailCount() const;
};

int GetGapMaskGeometryFailCount();
int GetGapRealSmallFailCount(const char* rawPath, const char* referencePath = nullptr, bool writeReference = false);

int GetGapConnectivityFailCount(const char* referencePath, bool writeReference);

int GetGapMaskBridgeFailCount();
