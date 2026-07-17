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
// 5. SendPreview 把结果交给 CropPreviewPlug，由接管层应用叠加层 / 三维主显示状态
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

struct CropViewRequest final {
    vtkSmartPointer<vtkImageData> inputImage;
    vtkSmartPointer<vtkPolyData> inputPolyData;
    OrthogonalCropDataSource dataSource = OrthogonalCropDataSource::ImageData;
    vtkSmartPointer<vtkRenderWindowInteractor> interactor;
    vtkSmartPointer<vtkRenderer> renderer;
    std::shared_ptr<InteractiveService> referenceService;
    std::vector<std::shared_ptr<InteractiveService>> previewServices;
};

class CropBridge {
public:
    // host/session 只注入“如何 reload 主数据”的能力；submit 的时机和生命周期仍由 bridge 控制。
    // image 是本次 submit 生成并移交给 reload 链的图像；返回值只表示请求是否被接受，
    // 最终成功与否由 reload 消费者通过 onComplete 回传。
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

    bool StartView(CropViewRequest request);
    void ClearBindings();
    // 数据事务完成后只刷新 owning image；窗口绑定仍由完整 CropViewRequest 管理。
    void SetInputSnapshot(vtkSmartPointer<vtkImageData> image);

    // 设置 submit 使用的主数据 reload 能力；bridge 只保存能力函数，不直接依赖具体窗口服务类型。
    void SetSubmitReloadHandler(ReloadSubmitter reloadSubmitter);

    // 从当前 widget 几何和 backend image 输入构造 submit request，不复用 preview 结果；
    // 只有 reload 请求被接受才返回 true，最终结果由完成回调收尾。
    bool SendSubmit(std::function<void(bool isSuccess)> onComplete = nullptr);

    // 宿主命令触发的裁切模式 switch 入口。
    bool SwitchCropBox();

    // 宿主命令触发的平面裁切模式 switch 入口。
    bool SwitchCropPlane();

    // 宿主命令触发的显式退出入口。
    bool ExitCrop();

    // 宿主只需要知道裁切链路是否已激活，用于决定退出命令是否应被裁切消费。
    bool GetCropActive() const;

    // 切换 preview 显示意图与 removal mode；拖拽中只记录意图，释放后再执行后端预览。
    void SwitchPreview(CropRemovalMode removalMode);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
