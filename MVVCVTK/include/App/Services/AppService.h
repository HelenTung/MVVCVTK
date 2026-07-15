#pragma once
// =====================================================================
// AppService.h — VizService 主线程编排层
//
// 继承关系：
//   InteractiveService  — 交互接口（StdRenderContext 通过此类型持有）
//
// 主线职责：
//   1. 构造 / 绑定渲染上下文
//   2. 前处理配置：多数只写 SharedState；SetSpacing 还同步更新 DataManager 的 vtkImageData 外壳
//   3. 加载 / 导出：委托任务服务构建后台任务，主线程统一回调
//   4. 交互：读取状态、计算结果、回写 SharedState
//   5. 主线程编排：消费 pending 事件，按顺序触发重建 / 同步 / 回调
// =====================================================================

#include "AppInterfaces.h"
#include <vtkMatrix4x4.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class IStateEventSource;
class SharedInteractionState;

class VizService
    : public InteractiveService
{
public:
    // ================================================================
    // 构造 / 析构
    // 功能：绑定 DataManager、SharedInteractionState 与状态事件源三个核心对象。
    // 作用：建立 VizService 作为“状态调度中枢”的运行基础。
    // ================================================================
    VizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state,
        std::shared_ptr<IStateEventSource> stateEventSource);
    ~VizService() override;

    // ================================================================
    // RenderContext 绑定
    // 功能：接入 renderer / renderWindow，并注册状态事件源观察回调。
    // 作用：把状态广播和渲染后处理主循环连起来。
    // ================================================================
    void SetRenderContext(vtkSmartPointer<vtkRenderWindow> win,
        vtkSmartPointer<vtkRenderer> ren) override;

    // ================================================================
    // 视觉配置 — 前处理 / 运行期配置意图
    // 调用时机：SetServiceBound 之后，LoadFileAsync 之前（或之后均可）
    // 线程语义：多数方法只写带锁的 SharedState；SetSpacing 先提交 DataManager 的 spacing，再广播状态。
    // 除 SetSpacing 会 ShallowCopy/更新 vtkImageData 外，其余方法只登记配置意图。
    // 实现依赖对象由 Impl 持有。
    // ================================================================
    void SetVizMode(VizMode mode);
    void SetMaterial(const MaterialParams& mat);
    void SetOpacity(double opacity);
    void SetTransferFunction(const std::vector<TFNode>& nodes);
    void SetIsoThreshold(double val);
    void SetBackground(const BackgroundColor& bg);
    void SetSpacing(double sx, double sy, double sz);
    void SetWindowLevel(double ww, double wc);
    void SetVisualConfig(const PreInitConfig& cfg);

    // ================================================================
    // 文件流加载 / 重载加载入口
    // 功能：发起后台加载任务，并通过主线程回调把结果返回给 UI / 上层。
    // 作用：把文件流加载、重载加载与渲染同步解耦。
    // 实现依赖对象由 Impl 与基础服务状态共同持有。
    // spacing / origin 必须由宿主数据命令显式传入；RAW 体数据不携带这些物理事实，service 不提供样本默认值。
    // ================================================================
    void LoadFileAsync(const std::string& path,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete = nullptr);

    // 重载入口：从上游重建缓冲区导入体数据，命名显式带 Reload。
    bool ReloadFromBufferAsync(
        const float* data,
        const std::array<int, 3>& dims,
        const std::array<float, 3>& spacing,
        const std::array<float, 3>& origin,
        std::function<void(bool isSuccess)> onComplete = nullptr);

    // 状态查询：当前只对外暴露 File / Reload 两组状态。
    LoadState GetFileLoadState() const;
    LoadState GetReloadLoadState() const;

    // ================================================================
    // 数据导出任务
    // 功能：发起后台保存任务，并把保存完成结果延迟回到主线程。
    // 作用：把导出 I/O 与当前渲染线程解耦。
    // 实现依赖对象由 Impl 与基础服务状态共同持有。
    // ================================================================
    void ExportDataAsync(const std::string& path,
        std::function<void(bool isSuccess)> onComplete = nullptr);
    void ExportSlicesAsync(const std::string& path,
        std::optional<double> rotationAngleDeg = std::nullopt,
        std::function<void(bool isSuccess)> onComplete = nullptr);

    // ================================================================
    // InteractiveService — 交互接口
    // 功能：处理切片滚动、光标联动、模型矩阵同步、显隐切换等交互入口。
    // 作用：读取 SharedState / 当前策略 / 交互计算服务，并回写状态。
    // 实现依赖对象由 Impl 与基础服务状态共同持有。
    // 说明：这一组是交互主线，已尽量保持“读状态 → 算结果 → 回状态”的单一路径。
    // ================================================================
    void SetSliceScroll(int delta) override;
    void SetCursorWorldPosition(double worldPos[3], int axis = -1) override;
    std::array<double, 3> GetCursorWorld() override;
    void SetInteracting(bool isInteracting) override;
    int GetPlaneAxis(vtkActor* actor) override;
    vtkProp3D* GetMainProp() override;
    void SetModelMatrix(vtkMatrix4x4* modelToWorldMatrix) override;
    void SetElementVisible(uint32_t flagBit, bool isVisible) override;
    void SetWindowLevelDrag(int totalDx, int totalDy, int viewWidth, int viewHeight, double startWW, double startWC) override;

    std::array<double, 16> GetModelMatrix() override;
    WindowLevelParams GetWindowLevel() const override;
    int GetNavigationAxis() const override;

    // 模型变换扩展：统一走 InteractionComputeService 的静态计算逻辑。
    // 实现依赖对象由 Impl 持有。
    void SetModelTransform(double translate[3], double rotate[3], double scale[3]);
    void SetModelTransformReset();
    void GetModelPositionFromWorld(const double worldPos[3], double modelPos[3]) const override;
    void GetWorldPositionFromModel(const double modelPos[3], double worldPos[3]) const override;

    // ================================================================
    // AbstractAppService — 主线程编排入口
    // 功能：Timer 心跳驱动，只编排各类 pending 状态的消费顺序。
    // 作用：按固定顺序消费 pending image、导出回调、缓存清理、加载状态、策略同步和加载回调。
    // 实现依赖对象由 Impl 与基础服务状态共同持有。
    // 固定顺序：
    //   1. SetCurrentFromPending -> SetReloadDataReady
    //   2. ResetSaveCallback -> SendSaveCallback
    //   3. 缓存清理请求 -> ClearStrategyCache
    //   4. 完整 load 终态队列 -> ClearLoadFail/BuildPipeline
    //   5. 非 load 结构请求与策略同步 -> BuildPipeline/SetStrategyState
    //   6. owner 释放 admission -> Send*LoadCallback；Host-owned 事务由 Host 统一释放
    // ================================================================
    void SendUpdates() override;
    bool GetDirty() const override;
    void SetDirty() override;
    bool ResetDirty() override;
    void SetCurrentStrategy(std::shared_ptr<AbstractVisualStrategy> newStrategy) override;
    void AttachOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy) override;
    void RemoveOverlayStrategy(std::shared_ptr<AbstractVisualStrategy> strategy) override;
    void ClearOverlayStrategies() override;

private:
    class Impl;
    std::shared_ptr<Impl> m_impl;
};
