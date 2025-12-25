#pragma once
#include "AppInterfaces.h"
#include <vtkCubeAxesActor.h>
#include <map>
#include "AppState.h"
#include <vtkTable.h>

/**
 * @class MedicalVizService
 * @brief 具体业务服务类：负责可视化策略管理、交互逻辑以及数据分析。
 */
class MedicalVizService : public AbstractAppService {
private:
    // --- 成员变量 ---
    vtkSmartPointer<vtkCubeAxesActor> m_cubeAxes;                 // 空间坐标轴 Actor
    std::map<VizMode, std::shared_ptr<AbstractVisualStrategy>> m_strategyCache; // 策略缓存池
    std::shared_ptr<SharedInteractionState> m_sharedState;        // 全局共享交互状态
public:
    // ==========================================
    // Group 1: 生命周期与初始化 (Lifecycle)
    // ==========================================
    MedicalVizService(std::shared_ptr<AbstractDataManager> dataMgr,
        std::shared_ptr<SharedInteractionState> state);

    // 加载数据文件 (会重置视图和缓存)
    void LoadFile(const std::string& path);

    // ==========================================
    // Group 2: 可视化模式切换 (Visualization Control)
    // ==========================================
    // 切换模式：3D 体渲染 (Volume Rendering)
    void ShowVolume();

    // 切换模式：3D 等值面 (IsoSurface)
    void ShowIsoSurface();

    // 切换模式：2D 切片视图 (Axial/Coronal/Sagittal)
    void ShowSlice(VizMode sliceMode);

    // 切换模式：3D 组合视图 (主渲染 + 三切面参考)
    void Show3DPlanes(VizMode renderMode);


    // ==========================================
    // Group 3: 交互与状态同步 (Interaction & State)
    // ==========================================

    // 处理来自 UI 或 滚轮的交互输入 (如切片层级变更)
    void UpdateInteraction(int delta);

    // [回调] 当共享状态(SharedState)发生改变时触发，用于更新当前视图
    void OnStateChanged();

    // 获取平面对应的轴向索引 (用于 3D 拖拽判定: 0=X, 1=Y, 2=Z, -1=None)
    int GetPlaneAxis(vtkActor* actor);

    // 获取共享状态指针
    std::shared_ptr<SharedInteractionState> GetSharedState() {
        return m_sharedState;
    }


    // ==========================================
    // Group 4: 数据分析与导出 (Data Analysis)
    // ==========================================

    // 计算并获取直方图数据 (vtkTable格式)
    vtkSmartPointer<vtkTable> GetHistogramData(int binCount = 2048);

    // 生成并保存直方图图片到本地
    void SaveHistogramImage(const std::string& filePath, int binCount = 2048);


private:
    // ==========================================
    // Group 5: 内部辅助函数 (Internal Helpers)
    // ==========================================

    // 获取或懒加载对应的可视化策略
    std::shared_ptr<AbstractVisualStrategy> GetStrategy(VizMode mode);

    // 更新 3D 场景中的空间坐标轴 (CubeAxes)
    void UpdateAxes();

    // 将共享状态的光标重置到图像中心
    void ResetCursorCenter();

    // 清空策略缓存 (用于加载新数据时)
    void ClearCache();

};
