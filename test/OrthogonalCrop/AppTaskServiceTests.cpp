#include "Tasks/AppDataExportTaskService.h"

#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

class ExportDataStub final : public AbstractDataManager
{
public:
    vtkSmartPointer<vtkImageData> GetVtkImage() const override
    {
        return nullptr;
    }

    DataVersion GetDataVersion() const override
    {
        return 0;
    }

    bool SetDataLoaded(
        const std::string&,
        const std::array<float, 3>&,
        const std::array<float, 3>&) override
    {
        return false;
    }

    bool ExportData(
        const std::string& path,
        const std::array<double, 16>&) override
    {
        return path == "A";
    }
};

void SetExpect(bool isExpected, const std::string& message, int& failureCount)
{
    if (!isExpected) {
        std::cerr << message << '\n';
        ++failureCount;
    }
}

void StartTaskOrder(int& failureCount)
{
    auto dataManager = std::make_shared<ExportDataStub>();
    auto sharedState = std::make_shared<SharedInteractionState>();
    auto service = std::make_shared<AppDataExportTaskService>(dataManager, sharedState);

    std::vector<char> callbackOrder;
    std::optional<bool> resultA;
    std::optional<bool> resultB;

    auto taskA = service->BuildDataTask("A", [&](bool isSuccess) {
        callbackOrder.push_back('A');
        resultA = isSuccess;
    });
    auto taskB = service->BuildDataTask("B", [&](bool isSuccess) {
        callbackOrder.push_back('B');
        resultB = isSuccess;
    });

    SetExpect(taskA.has_value(), "export task A should be built.", failureCount);
    SetExpect(taskB.has_value(), "export task B should be built.", failureCount);
    if (!taskA || !taskB) {
        return;
    }

    (*taskB)();
    (*taskA)();
    if (service->ResetSaveCallback()) {
        service->SendSaveCallback();
    }

    SetExpect(
        callbackOrder == std::vector<char>{ 'B', 'A' },
        "export callbacks should run once each in completion order B,A.",
        failureCount);
    SetExpect(
        resultA.has_value() && *resultA,
        "export task A callback should receive its successful result.",
        failureCount);
    SetExpect(
        resultB.has_value() && !*resultB,
        "export task B callback should receive its failed result.",
        failureCount);
}

} // namespace

int GetAppTaskFailCount()
{
    int failureCount = 0;
    StartTaskOrder(failureCount);
    return failureCount;
}
