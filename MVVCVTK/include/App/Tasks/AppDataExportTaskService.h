#pragma once

#include "AppInterfaces.h"
#include "AppState.h"
#include "AppTaskCallbackState.h"
#include <future>
#include <memory>
#include <optional>
#include <string>

// 导出任务需要在调用线程拍下视觉状态快照，再把重采样 / I/O 放到后台执行。
// 本 service 只负责构造任务和持有保存回调状态，不直接启动线程；
// 线程托管仍由 VizService 统一完成，避免导出、加载各自发明一套生命周期策略。
class AppDataExportTaskService
    : public std::enable_shared_from_this<AppDataExportTaskService>
{
public:
    AppDataExportTaskService(std::shared_ptr<AbstractDataManager> dataManager,
        std::shared_ptr<SharedInteractionState> sharedState);

    // 在调用线程快照 model-to-world 矩阵并构造后台数据导出；依赖或文件路径无效时投递失败回调并返回空。
    std::optional<std::packaged_task<void()>> BuildDataTask(
        const std::string& path,
        std::function<void(bool isSuccess)> onComplete);

    // path 是输出目录；rotationAngleDeg 为可选角度（度），currentMode 必须对应真实切片方向。
    // 构造阶段快照姿态、世界坐标游标与窗宽窗位，后台只消费这些快照并投递结果。
    std::optional<std::packaged_task<void()>> BuildSlicesTask(
        const std::string& path,
        std::optional<double> rotationAngleDeg,
        VizMode currentMode,
        std::function<void(bool isSuccess)> onComplete);

    // SendUpdates 消费线程领取保存完成门铃。
    bool ResetSaveCallback();
    // SendUpdates 消费线程接管 payload，并在锁外执行业务回调；本对象不负责切换线程。
    void SendSaveCallback();

private:
    void SetSaveCallbackReady(bool isSuccess, std::function<void(bool)> callback = nullptr);

    std::shared_ptr<AbstractDataManager> m_dataManager;
    std::shared_ptr<SharedInteractionState> m_sharedState;
    // 后台任务是生产者，VizService::SendUpdates 是消费者；状态对象串接同时完成的多个任务。
    AppTaskCallbackState m_saveCallback;
};
