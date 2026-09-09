// 测试用途：依据本机资源为手动/自动化宿主设置算法预算，保留明确的有界覆盖参数。
#pragma once
#include <QString>
#include <QJsonObject>
#include <cstdint>
namespace Manual {
struct TestResources {
    std::uint64_t workingBytes = 512ULL * 1024 * 1024;
    std::uint64_t publishBytes = 256ULL * 1024 * 1024;
    std::uint64_t physicalBytes = 0, availableBytes = 0, commitAvailableBytes = 0;
    QJsonObject GetJson() const;
};
TestResources GetTestResources(std::uint64_t budgetMiB = 0);
}
