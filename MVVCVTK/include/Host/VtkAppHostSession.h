#pragma once

#include "AppTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

// 当前类只表达“独立 VTK 宿主如何组装一次应用会话”：
// 1. main 只启动会话，避免后续上位机协议继续堆进入口函数。
// 2. 具体热键属于 host/session 配置；feature 插件只暴露命令能力，不保存固定键位。
class VtkAppHostSession final {
public:
    // role 是跨宿主的业务语义，不是固定窗口序号。
    // Qt / 上位机可以改变窗口数量和布局，但裁切、孔隙 overlay、主循环承载点仍按 role 选择。
    enum class HostRenderViewRole {
        Primary3D,
        Composite3D,
        TopDownSlice,
        FrontBackSlice,
        LeftRightSlice,
        Auxiliary
    };

    // 热键只属于当前独立 VTK host；feature 插件和 core 只接收稳定业务命令。
    // 这样后续 Qt action 或上位机协议可以替换输入映射，而不需要改插件内部。
    struct HostHotkeyBindings {
        char modelTransformToggleKey = 'm';
        char saveTransformedDataKey = 's';
        char saveSliceImagesKey = 't';
        char cropToggleKey = 'o';
        char planarCropToggleKey = 'p';
        char gapOverlayToggleKey = 'j';
        char keepInsidePreviewKey = '1';
        char removeInsidePreviewKey = '2';
        char submitKey = '3';
        std::string exitKeySym = "Escape";
    };

    // 这是独立 VTK 调试入口的默认加载配置，不是算法或上位机协议的一部分。
    // Qt / 上位机接入时应通过自己的 controller 提供数据来源。
    struct InitialVolumeLoadConfig {
        std::string filePath = "F:\\data\\1000x1000x1000.raw";
        std::array<float, 3> spacing = { 0.02125f, 0.02125f, 0.02125f };
        std::array<float, 3> origin = { 0.0f, 0.0f, 0.0f };
        int histogramBinCount = 2048;
        std::string histogramFilePath = "";
    };

    // 每个 config 描述一个宿主视图的创建/接管意图。
    // renderWindow 可为空，表示独立 VTK host 自建窗口；非空则表示外部宿主注入窗口所有权。
    struct HostRenderViewConfig {
        std::string id;
        HostRenderViewRole role = HostRenderViewRole::Auxiliary;
        WindowConfig window;
        // Qt host 可注入 vtkGenericOpenGLRenderWindow；host 只接收 VTK 基类，避免把 Qt widget 类型带入业务层。
        vtkSmartPointer<vtkRenderWindow> renderWindow;
        bool includeInCropPreview = true;
        bool startStandaloneEventLoop = false;
    };

    // endpoint 是 Initialize 之后给宿主层使用的观察视图。
    // 这里不暴露 service，是为了避免 Qt / 上位机绕过 host/session 直接改 feature 或 core 状态。
    struct HostRenderViewEndpoint {
        std::string id;
        HostRenderViewRole role = HostRenderViewRole::Auxiliary;
        // endpoint 只暴露非拥有指针；真实生命周期由 VtkAppHostSession/外部 Qt window 共同持有。
        vtkRenderer* renderer = nullptr;
        vtkRenderWindow* renderWindow = nullptr;
        vtkRenderWindowInteractor* interactor = nullptr;
    };

    struct Config {
        HostHotkeyBindings hotkeys;
        InitialVolumeLoadConfig initialVolume;
        // 空列表表示使用独立 VTK host 的默认调试视图；非空则完全由宿主决定窗口拓扑。
        std::vector<HostRenderViewConfig> renderViews;
    };

    VtkAppHostSession();
    explicit VtkAppHostSession(Config config);
    ~VtkAppHostSession();

    VtkAppHostSession(const VtkAppHostSession&) = delete;
    VtkAppHostSession& operator=(const VtkAppHostSession&) = delete;
    VtkAppHostSession(VtkAppHostSession&&) noexcept;
    VtkAppHostSession& operator=(VtkAppHostSession&&) noexcept;

    void Initialize();
    void Start();

    // endpoint 指针只在当前 session 生命周期内有效；外部宿主按 id/role 绑定 widget，不接管所有权。
    const std::vector<HostRenderViewEndpoint>& GetRenderViewEndpoints() const;
    const HostRenderViewEndpoint* GetRenderViewEndpoint(const std::string& id) const;
    const HostRenderViewEndpoint* GetPrimaryRenderViewEndpoint() const;

    // 默认五视图只是保持现有独立 VTK 调试体验，不代表项目架构要求固定窗口数。
    static std::vector<HostRenderViewConfig> BuildDefaultRenderViewConfigs();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
