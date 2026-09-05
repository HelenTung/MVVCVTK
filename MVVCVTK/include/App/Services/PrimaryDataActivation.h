#pragma once

#include "App/Services/DataCommitTypes.h"
#include "Data/DataService.h"
#include <optional>

class SharedInteractionState;

// 私有用例；不拥有 DataGraph 或共享状态，不改变数据提交与可见帧的边界。
class PrimaryDataActivation final {
public:
    PrimaryDataActivation(AbstractDataManager& data, SharedInteractionState& state);
    DataCommitResult SetDataCommit(DataTransaction transaction);
    void SetLoadReady(const std::optional<DataReadyState>& staged);
    static bool GetDataReadyState(const VtkImageGridSnapshot& snapshot,
        const SharedInteractionState* state, DataReadyState& ready);
private:
    void SetReady(const std::optional<DataReadyState>& staged, bool isLoad);
    AbstractDataManager& m_data;
    SharedInteractionState& m_state;
};
