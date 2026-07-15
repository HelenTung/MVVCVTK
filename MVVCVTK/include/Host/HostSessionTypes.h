#pragma once

#include "AppTypes.h"

#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

class vtkRenderer;
class vtkRenderWindowInteractor;

// role 是 host/session 层给窗口打的业务标签，不是固定窗口序号。
// 为什么放在公共类型里：Qt / 上位机需要按角色把自己的 QVTKOpenGLNativeWidget 映射回 session，
// 而 feature 只关心“哪个角色能作为参考 / overlay 目标”，不应该知道窗口创建顺序。
enum class HostRenderViewRole {
    Primary3D,
    Composite3D,
    TopDownSlice,
    FrontBackSlice,
    LeftRightSlice,
    Auxiliary
};

// 独立 VTK 调试入口的输入协议。它由 main 或上位机适配层填充，由 host router Impl 消费。
// 为什么不放进 feature：固定按键是宿主交互事实，feature 只暴露 Start/Switch/Exit 等稳定命令。
// 字段默认保持未分配，是为了让 Qt / 上位机创建 session 后不会隐式继承调试程序的快捷键。
struct HostHotkeyBindings {
    // RenderContext 层的模型变换模式切换键，只服务独立 VTK 调试窗口。
    char modelSwitchKey = 0;
    // 调试导出键：保存当前变换后的体数据，真实上位机应走显式命令。
    char saveTransformedDataKey = 0;
    // 调试导出键：按当前切片角度保存 slice 图片。
    char saveSliceImagesKey = 0;
    // 裁切盒交互模式切换键；按键只触发 host 命令，不进入裁切插件。
    char cropSwitchKey = 0;
    // 平面裁切交互模式切换键；与 cropSwitchKey 分开是为了保持两种 widget 状态边界。
    char planarSwitchKey = 0;
    // 孔隙 overlay 键：未进入显示模式时进入，已进入时只切换显隐。
    char gapSwitchKey = 0;
    // 调试预览键：保留平面法向内侧。
    char keepInsidePreviewKey = 0;
    // 调试预览键：移除平面法向内侧。
    char removeInsidePreviewKey = 0;
    // 调试提交键的主键位；host router Impl 额外要求 isCtrlDown，避免裸数字键触发 VTK 内建行为。
    char submitKey = 0;
    // 使用 key symbol 而不是 char，是因为 Escape 这类控制键没有稳定可打印字符。
    std::string exitKeySym;
};

// 初始体数据的物理几何配置，由 main / 上位机数据命令提供，由 VtkAppHostSession 加载链路消费。
// RAW 体数据没有自描述的物理坐标，所以 spacing/origin 必须作为同一组外部事实传入。
// 这里不提供默认构造，是为了避免 session 在缺少上位机元数据时悄悄使用假 geometry。
struct VolumeGeometryConfig {
    explicit VolumeGeometryConfig(
        std::array<float, 3> spacingValues,
        std::array<float, 3> originValues)
        : spacing(spacingValues)
        , origin(originValues)
    {
    }

    // 体素间距，按 input image 的 X/Y/Z 轴顺序传入；裁切和孔隙体积计算都依赖这个尺度。
    std::array<float, 3> spacing;
    // 输入模型坐标原点，和 spacing 一起决定 index -> physical point 的仿射位置。
    std::array<float, 3> origin;
};

// 初始加载只是 host 启动阶段的可选数据命令，不是算法或 feature 的隐式前置条件。
// 路径和 geometry 默认都为空，是因为它们属于上位机 / 调试入口的外部事实，不能写进通用 session 配置。
struct InitialVolumeLoadConfig {
    // true 表示宿主明确要求 session 启动后加载 filePath；false 表示等待后续数据命令。
    bool isInitialLoadEnabled = false;
    // 外部体数据路径。空路径即使 isInitialLoadEnabled=true 也会被拒绝，避免 DataManager 进入不明确 I/O。
    std::string filePath;
    // RAW 必需的物理几何元数据；optional 表示“宿主尚未下发”，不是使用默认值。
    std::optional<VolumeGeometryConfig> geometry;
};

// 内存重载命令自有一份连续 float 体素，避免异步链借用 Qt / 上位机的临时内存。
// ReloadVolume 按值接收并移动该 DTO；底层任务在同步接纳阶段再建立工作线程快照。
struct HostVolumeBufferRequest {
    // X 变化最快的连续体素；元素数必须严格等于 dimensions 三轴乘积。
    std::vector<float> voxels;
    // 体素维度，固定按 X/Y/Z 排列；任一维非正或乘积溢出时拒绝命令。
    std::array<int, 3> dimensions{ 0, 0, 0 };
    // 内存数据同样不携带物理坐标；缺少 spacing/origin 时拒绝命令。
    std::optional<VolumeGeometryConfig> geometry;
};

// 每个 config 描述一个宿主视图的创建/接管意图，由上位机 / main 填充，由 HostRenderViewSet 消费。
// 为什么只放窗口事实：窗口数量、role、是否参与预览属于 host 拓扑，不能固化到 feature 插件。
struct HostRenderViewConfig {
    // 稳定视图标识，供 Qt widget 映射和后续命令按 id 定向；为空时保持为空，不能被 id 类命令寻址。
    std::string id;
    // 视图业务角色，供裁切 reference、overlay 目标和主循环承载点选择。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    // 单个窗口的渲染初始配置，仍复用现有 WindowConfig，避免 HostRenderViewSet 重写渲染细节。
    WindowConfig window;
    // Qt host 可注入 vtkGenericOpenGLRenderWindow；host 只接收 VTK 基类，避免把 Qt widget 类型带入业务层。
    vtkSmartPointer<vtkRenderWindow> renderWindow;
    // true 表示当请求允许使用配置默认预览窗口时，此视图可接收裁切 preview overlay。
    bool isCropPreviewIncluded = true;
    // 独立 VTK host 只能选择一个 interactor 进入阻塞 Start；Qt host 不依赖这个字段。
    bool isEventLoopEnabled = false;
};

// endpoint 是 BuildSession 之后给宿主层使用的非拥有观察视图。
// 这里不暴露 service，是为了避免 Qt / 上位机绕过 host/session 直接改 feature 或 core 状态。
struct HostRenderViewEndpoint {
    // 与 HostRenderViewConfig::id 对应，外部宿主用它把 endpoint 绑定到自己的 widget。
    std::string id;
    // 与 HostRenderViewConfig::role 对应，外部宿主可按角色查找主 3D 或切片窗口。
    HostRenderViewRole role = HostRenderViewRole::Auxiliary;
    // 非拥有 renderer 观察指针；对象由对应 StdRenderContext 持有，session 销毁或视图集合重建后失效。
    vtkRenderer* renderer = nullptr;
    // 非拥有窗口观察指针；窗口可能由 StdRenderContext 创建，也可能由 Qt 注入，endpoint 不参与引用计数。
    vtkRenderWindow* renderWindow = nullptr;
    // 非拥有 interactor 观察指针；其生命周期跟随 renderWindow，外部不得删除或另行启动其事件循环。
    vtkRenderWindowInteractor* interactor = nullptr;
};

// 运行期单视图配置请求。optional 表示调用方是否要求更新对应字段；
// 字段存在不代表请求已被接受或底层管线已完成同步。
// 不能复用 PreInitConfig 做局部更新，否则其默认 mode/material 会覆盖当前状态。
struct HostViewConfig {
    // 优先按 id 定位目标窗口，适合 Qt widget 已建立 id 映射的场景。
    std::string viewId;
    // viewId 为空时是否允许按 role 查找目标窗口。
    bool isViewRoleUsed = false;
    // role fallback 的目标窗口。
    HostRenderViewRole viewRole = HostRenderViewRole::Auxiliary;

    // 存在时同时切换目标 service 的可视化管线和 context 相机样式；缺失时保持当前模式。
    std::optional<VizMode> mode;
    // 存在时整组写入材质参数；若同一命令还给出 opacity，后者按分发顺序覆盖其中的透明度。
    std::optional<MaterialParams> material;
    // 存在时写入全局材质透明度，通常取 [0, 1]；host 不裁剪数值，缺失时保持当前值。
    std::optional<double> opacity;
    // 存在时整体替换传输函数节点；节点 position/opacity/RGB 均按 AppTypes 的归一化契约解释。
    std::optional<std::vector<TFNode>> tfNodes;
    // 存在时写入等值面标量阈值，单位与当前输入标量一致；缺失时保持当前阈值。
    std::optional<double> iso;
    // 存在时写入归一化 RGB 背景色，每个分量通常取 [0, 1]。
    std::optional<BackgroundColor> background;
    // 存在时写入输入图像 X/Y/Z 物理 spacing；数组顺序固定，单位由输入数据协议决定。
    std::optional<std::array<double, 3>> spacing;
    // 存在时整组写入切片窗宽/窗位，二者单位均与当前输入标量一致。
    std::optional<WindowLevelParams> windowLevel;
};

// RenderContext 热键只服务独立 VTK 调试宿主，例如模型变换和本地导出。
// 这一层不触发 feature，只让 StdRenderContext 暴露现有工具能力；Qt / 上位机应默认关闭。
struct HostContextInput {
    // false 时不向 RenderContext 安装任何 host 按键回调。
    bool isHotkeyEnabled = false;
    // 按 id 指定哪些窗口接收 RenderContext 调试键；空列表不代表所有窗口。
    std::vector<std::string> targetViewIds;
    // 按 role 指定哪些窗口接收 RenderContext 调试键；与 id 取并集。
    std::vector<HostRenderViewRole> targetViewRoles;
};

// host 导出命令参数。它只描述上位机请求和外部 I/O 目的地，不参与具体 service 执行。
// 为什么用 flag：上位机只声明“请求哪类导出”，下层绑定按配置判断是否进入对应导出状态。
struct HostDataExportConfig {
    // true 表示本次命令请求导出变换后的体数据；false 表示保持空闲。
    bool hasVolumeExport = false;
    // 空路径表示不允许 standalone hotkey 导出变换后的体数据。
    std::string transformedDataOutputPath;
    // true 表示本次命令请求导出切片图片；false 表示保持空闲。
    bool hasSliceExport = false;
    // 空目录表示不允许 standalone hotkey 导出切片图片。
    std::string sliceOutputDir;
    // 优先按 id 指定来源切片视图，适合 Qt 已经掌握 widget -> view id 映射的场景。
    std::string sliceSourceViewId;
    // sliceSourceViewId 为空时，是否允许按 role 查找来源切片视图。
    bool isSliceRoleUsed = false;
    // role fallback 的来源切片视图。
    HostRenderViewRole sliceSourceRole = HostRenderViewRole::TopDownSlice;
    // 可选自定义切片偏转角，单位 degree；未设置时只使用当前共享模型矩阵。
    std::optional<double> sliceAngleDeg;
};

// 裁切激活请求把“参考窗口”和“预览窗口”拆开描述。
// 调用方：上位机命令或 standalone hotkey；消费方：HostFeatureBindings::ConfigureOrthogonalCrop。
// 参考窗口提供坐标互转与 widget interactor，预览窗口只接收 overlay/dirty 刷新。
struct HostCropViewRequest {
    // 优先级最高的参考窗口选择方式；适合 Qt 已经拿到具体窗口 id 的场景。
    std::string referenceViewId;
    // referenceViewId 为空时，是否允许按 role 选择第一个参考窗口。
    bool isReferenceRoleUsed = false;
    // role fallback 的参考窗口角色，默认 Primary3D 只是语义默认，不代表固定第一个窗口。
    HostRenderViewRole referenceRole = HostRenderViewRole::Primary3D;
    // 显式预览窗口 id 列表；空列表表示调用方没有指定，不自动全选。
    std::vector<std::string> previewViewIds;
    // 显式预览窗口 role 列表；与 previewViewIds 取并集。
    std::vector<HostRenderViewRole> previewViewRoles;
    // true 才允许退回 HostRenderViewConfig::isCropPreviewIncluded，避免漏配请求时污染所有窗口。
    bool isPreviewViewsUsed = false;
};

// 裁切预览命令只表达“保留哪一侧”的业务意图，底层 CropRemovalMode 仍由裁切模块维护。
enum class HostCropPreviewMode {
    KeepInside,
    RemoveInside
};

// 孔隙等值面阈值来源。按数据范围比例计算时，真实阈值为 min + (max - min) * ratio。
// 为什么不用裸 bool：绝对阈值和范围比例是两种不同业务来源，枚举能防止上位机参数被误解释。
enum class HostGapAnalysisIsoMode {
    DataRangeRatio,
    AbsoluteValue
};

// 孔隙表面提取参数属于一次宿主分析命令，而不是 timer observer 的隐式经验值。
// 调用方必须显式传入，是为了让不同材料、不同批次或上位机配方可以独立控制分析策略。
struct HostGapSurface {
    // 选择 isoValue 的来源；默认只作为字段初值，真正激活时仍要求 request.algorithm 存在。
    HostGapAnalysisIsoMode isoMode = HostGapAnalysisIsoMode::DataRangeRatio;
    // 数据范围比例，单位为归一化比例；isoMode=DataRangeRatio 时使用。
    double dataRangeRatio = 0.0;
    // 绝对灰度阈值；isoMode=AbsoluteValue 时使用。
    double absoluteIsoValue = 0.0;
};

// 孔隙候选筛选参数直接对应 GapAnalysis 插件的 VoidDetectionParams。
// 放在 host DTO 中是为了避免 main / Qt 上位机包含插件内部头后再散落设置字段。
struct HostGapVoidConfig {
    // 灰度下限，按当前 vtkImageData 标量单位解释。
    float grayMin = 0.0f;
    // 灰度上限，按当前 vtkImageData 标量单位解释。
    float grayMax = 0.0f;
    // 最小孔隙体积，单位 mm^3；依赖体数据 spacing 的物理尺度。
    float minVolumeMM3 = 0.0f;
    // 法向/结构张量判定角度阈值，单位 degree。
    float angleThresholdDeg = 0.0f;
    // 张量局部窗口半径/尺寸，语义由 GapAnalysis 插件保持。
    int tensorWindowSize = 0;
    // 二值形态腐蚀迭代次数，语义由 GapAnalysis 插件保持。
    int erosionIterations = 0;
};

// 一次孔隙分析命令的算法参数集合。
// surface 决定等值面输入阈值，voidDetection 决定孔隙候选筛选；二者随同激活请求一起进入 host binding。
struct HostGapConfig {
    // 等值面阈值来源配置；host 校验后转换为 GapAnalysisSurfaceRequest。
    HostGapSurface surface;
    // 孔隙候选筛选配置；host 校验后转换为 VoidDetectionParams，与 surface 同批次下发。
    HostGapVoidConfig voidDetection;
};

// host/session 主线程 TimerEvent 承载窗口配置。
// 为什么显式配置：standalone 可以选择进入 Start() 的 interactor，Qt / 上位机可以选择自己的事件泵窗口；
// session 不再猜测“第一个窗口”或“默认五窗口中的某个窗口”。当前 GapAnalysis 是第一个 tick 使用者，
// 后续其他异步 feature 也应复用这一条 host/session 事件泵，而不是各自新建 feature 专属 observer。
struct HostTimerEventPumpConfig {
    // false 表示不追加 Host feature timer handler；每个 context 的基础 Timer observer 不受此字段控制。
    bool isTimerEnabled = false;
    // 优先按 id 选择 timer view，适合 Qt 已经建立 widget 到 view id 的映射。
    std::string timerViewId;
    // timerViewId 为空时是否允许按 role 选择第一个事件泵窗口。
    bool isTimerRoleUsed = false;
    // role fallback 的事件泵窗口；它只决定 TimerEvent 来源，不决定 overlay 目标。
    HostRenderViewRole timerViewRole = HostRenderViewRole::Primary3D;
};

// 孔隙分析显示请求决定 overlay 投递目标和算法参数；是否运行算法由显式进入显示模式后再触发。
// 这样“显示/隐藏 overlay”和“是否退出孔隙分析模式”可以分开处理。
struct HostGapViewRequest {
    // 显式 overlay 目标 id 列表；空列表不代表所有窗口。
    std::vector<std::string> targetViewIds;
    // 显式 overlay 目标 role 列表；与 targetViewIds 取并集。
    std::vector<HostRenderViewRole> targetViewRoles;
    // true 才允许退回默认 overlay role，防止未指定目标时全局接管。
    bool isDefaultOverlayUsed = false;
    // 一次分析运行所需参数；没有参数时 host 会拒绝进入分析链路，避免使用隐藏经验默认值。
    std::optional<HostGapConfig> algorithm;
};

// 独立 VTK host 的 feature 输入绑定，由 main 配置，由 HostFeatureBindings 安装到指定窗口。
// 热键监听范围和 feature 激活目标分开写，是为了避免“哪个窗口收键”被误当成“feature 作用在哪些窗口”。
struct HostCommandInputConfig {
    // false 时不安装 standalone feature input handler；Qt / 上位机应走 VtkAppHostSession 的显式命令。
    bool isHotkeyEnabled = false;
    // 哪些窗口监听 standalone feature 按键；空列表不代表所有窗口。
    std::vector<std::string> targetViewIds;
    // 哪些 role 监听 standalone feature 按键；与 targetViewIds 取并集。
    std::vector<HostRenderViewRole> targetViewRoles;
    // hotkey 触发裁切时复用的业务目标请求，和监听范围相互独立。
    HostCropViewRequest cropViewRequest;
    // hotkey 触发孔隙显示时复用的业务目标请求，和监听范围相互独立。
    HostGapViewRequest gapViewRequest;
};
