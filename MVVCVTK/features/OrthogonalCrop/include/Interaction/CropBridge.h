#pragma once

// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/include/Interaction/CropBridge.h
// 分类: Service / Interaction Bridge
// CropBridge - 正交裁切交互桥接服务
// 说明: 连接 widget、数据后端、preview 窗口与 submit reload 通道，
//       管理交互态、预览刷新顺序和提交收尾。
// =====================================================================
// 交互主链路：
// 1. SwitchCropBox 进入交互态，内部生成默认 widget bounds 并挂接 vtkBoxWidget2
// 2. OnBoxWidget 持续记录 widget world bounds 与交互 phase
// 3. Released 或显式切换预览时，按当前几何调用 BuildBoxRequest / BuildPlaneRequest
// 4. Box / Plane 各自刷新入口构造本几何 request，再按显式数据源请求体渲染 / 网格结果
// 5. SendPreview 把结果交给预览接管层，由接管层应用叠加层 / 三维主显示状态
// 6. SendSubmit 复用 request/router/algorithm 链路生成 submit image，再通过注入的 reload handler 回写主数据

#include "AppTypes.h"
#include "OrthogonalCropTypes.h"

#include <vtkPolyData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <functional>
#include <memory>
#include <vector>

class InteractiveService;
class vtkRenderer;

class CropBridge {
public:
    // host/session 只注入“如何 reload 主数据”的能力；submit 的时机和生命周期仍由 bridge 控制。
    using ReloadSubmitter = std::function<bool(
        vtkSmartPointer<vtkImageData> image,
        std::function<void(bool isSuccess)> onComplete)>;

    // 公共边界只暴露初始化、窗口接入和用户命令动作。
    // 内部状态切换、后端路由细节和 VTK 预览接管都留在私有实现里。

    // 构造时绑定 widget bounds 回调，把 VTK 交互事件转入本类状态机。
    CropBridge();
    ~CropBridge();

    CropBridge(const CropBridge&) = delete;
    CropBridge& operator=(const CropBridge&) = delete;
    CropBridge(CropBridge&&) noexcept;
    CropBridge& operator=(CropBridge&&) noexcept;

    // 以下一组接口把 host 注入的 image 输入转发给 backend router，并保存版本戳。
    void SetInputImage(vtkSmartPointer<vtkImageData> image, DataVersion version);
    void ClearInputImage();
    vtkSmartPointer<vtkImageData> GetInputImage() const;

    // 以下一组接口把 polydata 输入转发给 backend router。
    void SetInputPolyData(vtkSmartPointer<vtkPolyData> polyData);

    // 设置 backend router 的首选数据源。
    void SetPreferredDataSource(OrthogonalCropDataSource dataSource);

    // 主 interactor 由 3D 参考窗口提供，widget 只会挂到这个 interactor 上。
    void SetPrimaryInteractor(vtkRenderWindowInteractor* interactor);

    // 参考渲染器只供算法内部保存/恢复相机状态，不写入 SharedState。
    void SetReferenceRenderer(vtkRenderer* renderer);

    // 参考渲染服务负责 world / active input model 坐标互转。
    void SetReferenceRenderService(std::shared_ptr<InteractiveService> referenceService);

    // preview 服务列表决定哪些窗口会收到裁切主预览与设脏刷新；几何参照线框只在 reference 目标显示。
    void SetPreviewRenderServices(std::vector<std::shared_ptr<InteractiveService>> previewRenderServices);

    // 设置 submit 使用的主数据 reload 能力；bridge 只保存能力函数，不直接依赖具体窗口服务类型。
    void SetSubmitReloadHandler(ReloadSubmitter reloadSubmitter);

    // 执行当前 image submit：构建 request、经 router/algorithm 取结果，再把 submit image 提交到主数据 reload 通道。
    void SendSubmit();

    // 宿主命令触发的裁切模式 toggle 入口。
    bool SwitchCropBox();

    // 宿主命令触发的平面裁切模式 toggle 入口。
    bool SwitchCropPlane();

    // 宿主命令触发的显式退出入口。
    bool ExitCrop();

    // 宿主只需要知道裁切链路是否已激活，用于决定退出命令是否应被裁切消费。
    bool GetCropActive() const;

    // 切换 preview 开关与 removal mode。
    void SwitchPreview(CropRemovalMode removalMode);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
