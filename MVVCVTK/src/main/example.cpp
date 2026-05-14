/*
=====================================================================
example.cpp — 前端调用说明（MedicalVizService / StdRenderContext）

用途：
- 给前端/界面层一个统一的接入说明。
- 汇总当前已有功能、核心接口、调用顺序与典型伪代码。
- 作为 main.cpp 当前接入方式的说明文档。

重要约定：
1. 前后处理分离：
   - 前端只发“配置 / 加载 / 导出 / 交互”指令。
   - 真正的渲染重建与参数同步由主线程统一处理。
2. 数据类与状态类分离：
   - DataManager 负责数据加载与导出。
   - SharedInteractionState 负责共享数据状态。
   - 窗口实例自己的辅助元素显隐由各自 MedicalVizService 管理。
3. 回调时机：
   - SetFileLoadedAsync / SetFromBufferAsync / SetTransformedDataSavedAsync 的 onComplete
     均为【主线程延迟回调】。
   - 回调统一在 MedicalVizService::SetPendingUpdatesProcessed() 中分发。
4. Context 职责：
   - StdRenderContext 负责窗口、交互器、方向轴、相机风格与启动渲染循环。
   - 不在 Context 中承载业务回调。
=====================================================================
*/

/*
─────────────────────────────────────────────────────────────────────
一、系统角色与前端应持有的对象
─────────────────────────────────────────────────────────────────────

一条标准主线如下：

前端 UI
  ├─ 持有 MedicalVizService                 -> 业务调度层（每个窗口一份）
  ├─ 持有 StdRenderContext                  -> 渲染上下文（每个窗口一份）
  ├─ 共享 SharedInteractionState           -> 多窗口共享数据状态
  └─ 共享 DataManager                      -> 多窗口共享体数据来源

推荐前端持有对象：

    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedState = std::make_shared<SharedInteractionState>();

    auto serviceA = std::make_shared<MedicalVizService>(sharedDataMgr, sharedState);
    auto contextA = std::make_shared<StdRenderContext>();

角色说明：
- RawVolumeDataManager / TiffVolumeDataManager
  负责加载原始体数据、导出变换后体数据。

- SharedInteractionState
  负责保存：
  * 光标位置
  * 窗宽窗位
  * 传输函数
  * 材质
  * 模型矩阵
  * 加载状态
  * spacing

- MedicalVizService
  负责：
  * 接收前端调用
  * 写 SharedState
  * 发起异步加载 / 导出
  * 主线程统一重建管线 / 同步策略 / 分发回调
  * 管理当前窗口实例自己的辅助元素显隐（3D 彩色平面 / 2D 十字线 / 3D 标尺）

- StdRenderContext
  负责：
  * 绑定 renderWindow / renderer / interactor
  * 处理鼠标 / 键盘 / 定时器事件
  * 设置相机风格
  * 控制方向轴 / 朝向标记显隐
  * 启动渲染循环
*/

/*
─────────────────────────────────────────────────────────────────────
二、按 main.cpp 的标准建窗顺序
─────────────────────────────────────────────────────────────────────

main.cpp 当前采用：
- 共享一份 DataManager
- 共享一份 SharedInteractionState
- 为每个窗口创建单独的 MedicalVizService + StdRenderContext
- 通过 WindowConfig + GetWindowPair(...) 批量建窗

标准顺序：

1. 创建共享 DataManager
2. 创建共享 SharedInteractionState
3. 组装每个窗口的 WindowConfig
4. 为每个窗口创建一对 service/context
5. 设置每个窗口自己的辅助元素显隐
6. 由一个主窗口发起异步加载
7. 所有窗口先 Render 一次
8. 所有窗口 SetInteractorInitialized()
9. 选一个窗口进入 Start() 消息循环

伪代码：

    auto sharedDataMgr = std::make_shared<RawVolumeDataManager>();
    auto sharedState = std::make_shared<SharedInteractionState>();

    WindowConfig cfgA;
    cfgA.title = "Window A: Composite IsoSurface";
    cfgA.width = 600;
    cfgA.height = 600;
    cfgA.posX = 50;
    cfgA.posY = 50;
    cfgA.showAxes = true;
    cfgA.preInitCfg.vizMode = VizMode::CompositeIsoSurface;
    cfgA.preInitCfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, false };
    cfgA.preInitCfg.bgColor = { 0.05, 0.05, 0.05 };
    cfgA.preInitCfg.hasBgColor = true;

    WindowConfig cfgB;
    cfgB.title = "Window B: Top_down Slice";
    cfgB.width = 400;
    cfgB.height = 400;
    cfgB.posX = 50;
    cfgB.posY = 660;
    cfgB.preInitCfg.vizMode = VizMode::SliceTop_down;
    cfgB.preInitCfg.bgColor = { 0.0, 0.0, 0.0 };
    cfgB.preInitCfg.hasBgColor = true;
    cfgB.preInitCfg.windowLevel = { 400.0, 40.0 };
    cfgB.preInitCfg.hasWindowLevel = true;

    auto [serviceA, contextA] = GetWindowPair(cfgA, sharedDataMgr, sharedState);
    auto [serviceB, contextB] = GetWindowPair(cfgB, sharedDataMgr, sharedState);

    // 每个窗口单独控制自己的辅助元素显隐
    serviceA->SetElementVisible(VisFlags::Planes3D, true);
    serviceA->SetElementVisible(VisFlags::Ruler, false);
    serviceB->SetElementVisible(VisFlags::Crosshair, true);

    IDataLoaderService* loader = serviceA.get();
    loader->SetFileLoadedAsync(
        "E:/data/ct/1000X1000X1000.raw",
        [sharedState, serviceA](bool success)
        {
            if (!success) {
                std::cerr << "[onComplete] Volume data load failed.\n";
                return;
            }

            auto range = sharedState->GetDataRange();
            double isoVal = range[0] + (range[1] - range[0]) * 0.35;
            serviceA->SetIsoThreshold(isoVal);
        });

    contextA->SetRendered();
    contextB->SetRendered();

    contextA->SetInteractorInitialized();
    contextB->SetInteractorInitialized();

    // 任选一个窗口进入消息循环
    contextB->SetStarted();
*/

/*
─────────────────────────────────────────────────────────────────────
三、WindowConfig / GetWindowPair 当前调用方式
─────────────────────────────────────────────────────────────────────

main.cpp 当前通过 GetWindowPair(...) 统一完成建窗。
逻辑顺序如下：

    static std::pair<std::shared_ptr<MedicalVizService>,
                     std::shared_ptr<StdRenderContext>>
    GetWindowPair(const WindowConfig& cfg,
                  std::shared_ptr<AbstractDataManager> dataMgr,
                  std::shared_ptr<SharedInteractionState> sharedState)
    {
        auto service = std::make_shared<MedicalVizService>(dataMgr, sharedState);
        auto context = std::make_shared<StdRenderContext>();

        // 1) 绑定 service/context
        context->SetServiceBound(service);

        // 2) 批量提交前处理配置
        service->SetVisualConfig(cfg.preInitCfg);

        // 3) 设置窗口属性和相机风格
        context->SetWindowTitle(cfg.title);
        context->SetWindowSize(cfg.width, cfg.height);
        context->SetWindowPosition(cfg.posX, cfg.posY);
        context->SetCameraStyleByVizMode(cfg.preInitCfg.vizMode);
        if (cfg.showAxes)
            context->SetOrientationAxesVisible(true);

        // 4) 背景色由 service 同步到 renderer
        if (cfg.preInitCfg.hasBgColor)
            service->SetBackground(cfg.preInitCfg.bgColor);

        return { service, context };
    }

说明：
- showAxes 只控制当前 context 的方向轴 / 朝向标记。
- 不同窗口是否显示方向轴，可单独配置。
- 十字线 / 3D 彩色平面 / 3D 标尺，不由 WindowConfig 控制，
  而是由每个窗口自己的 service 在建窗后单独调用 SetElementVisible(...) 控制。
*/

/*
─────────────────────────────────────────────────────────────────────
四、前处理配置接口（IVisualConfigService）
─────────────────────────────────────────────────────────────────────

这些接口只表达“配置意图”，不直接做耗时渲染。
适合：
- 窗口刚建立时设置默认参数
- 前端参数面板修改后同步到底层

1) SetVisualConfig(const PreInitConfig& cfg)
   功能：
   - 批量提交前处理配置
   - 推荐作为窗口初始化默认入口
   典型字段：
   - cfg.vizMode
   - cfg.bgColor / cfg.hasBgColor
   - cfg.material
   - cfg.tfNodes / cfg.hasTF
   - cfg.windowLevel / cfg.hasWindowLevel
   - cfg.isoThreshold / cfg.hasIso
   - cfg.spacing / cfg.hasSpacing

2) SetVizMode(VizMode mode)
   功能：
   - 切换显示模式
   - 例如：CompositeVolume / CompositeIsoSurface / SliceTop_down

3) SetBackground(const BackgroundColor& bg)
   功能：
   - 设置背景色

4) SetMaterial(const MaterialParams& mat)
   功能：
   - 设置材质（环境光/漫反射/高光/透明度等）

5) SetOpacity(double opacity)
   功能：
   - 单独调整透明度

6) SetWindowLevel(double ww, double wc)
   功能：
   - 设置窗宽窗位

7) SetIsoThreshold(double val)
   功能：
   - 设置等值面阈值

8) SetTransferFunction(const std::vector<TFNode>& nodes)
   功能：
   - 设置体渲染传输函数

9) SetSpacing(double sx, double sy, double sz)
   功能：
   - 设置当前体数据 spacing
   - 适合在导入 Buffer 数据后补充体素尺度

前端伪代码：

    void UiController::ApplyDefaultConfig(MedicalVizService* service)
    {
        PreInitConfig cfg;
        cfg.vizMode = VizMode::CompositeIsoSurface;
        cfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, false };
        cfg.hasBgColor = true;
        cfg.bgColor = { 0.05, 0.05, 0.05 };
        cfg.hasIso = true;
        cfg.isoThreshold = 1800.0;
        service->SetVisualConfig(cfg);
    }
*/

/*
─────────────────────────────────────────────────────────────────────
五、数据加载与导出接口（IDataLoaderService / IDataExportService）
─────────────────────────────────────────────────────────────────────

【线程模型】
- 前端在主线程发起请求。
- 实际加载/导出在后台线程执行。
- onComplete 在主线程延迟回调。
- 回调中适合做：
  * UI 状态切换
  * 业务状态记录
  * 继续调用 service 的轻量接口
- 回调中不建议直接承载底层渲染细节。

A. 加载接口

1) SetFileLoadedAsync(
      const std::string& path,
      const std::array<float, 3>& spacing,
      const std::array<float, 3>& origin,
      std::function<void(bool)> onComplete)
   功能：
   - 从磁盘加载 RAW 或 TIFF 序列
    - RAW 建议显式传入 spacing / origin；TIFF 若希望沿用文件元数据可传 {0,0,0}
   - 内部防重入：Loading 状态下会拒绝新请求

2) SetReloadFromBufferAsync(...)
   功能：
   - 从上游算法给出的内存块导入体数据
   - 适合工业 CT 重建算法直接对接

3) GetFileLoadState() const / GetReloadLoadState() const
   功能：
   - 按需查询文件流加载状态、重载加载状态
   - Idle / Loading / Succeeded / Failed

4) SetLoadCanceled()
   功能：
   - 当前仅为兼容接口，尚未实现可中断加载

B. 导出接口

5) SetTransformedDataSavedAsync(const std::string& path = {}, std::function<void(bool)> onComplete = nullptr)
   功能：
   - 导出当前模型变换后的体数据
   - path 为空时，内部会根据当前已加载源路径推导默认导出路径
   - 常用于姿态校正后保存 RAW

6) SetSliceImagesSavedAsync(const std::string& path = {}, const double angle = 0.0, std::function<void(bool)> onComplete = nullptr)
   功能：
   - 按当前切片方向导出原始体数据的全部切片图
   - 灰度映射直接沿用当前窗宽窗位
   - angle 用于在当前切片方向基础上附加导出角度
   - path 为空时，内部会根据当前已加载源路径推导默认导出目录

前端加载伪代码：

    void MainWindow::OnLoadClicked(const QString& path)
    {
        ui->loadingOverlay->show();
        ui->btnLoad->setEnabled(false);

        m_service->SetFileLoadedAsync(path.toStdString(),
            [this, service = m_service, state = m_sharedState](bool success)
            {
                ui->loadingOverlay->hide();
                ui->btnLoad->setEnabled(true);

                if (!success) {
                    QMessageBox::warning(this, "错误", "数据加载失败");
                    return;
                }

                auto range = state->GetDataRange();
                double ww = range[1] - range[0];
                double wc = (range[0] + range[1]) * 0.5;
                service->SetWindowLevel(ww, wc);
            });
    }

前端导出伪代码：

    void MainWindow::OnExportClicked()
    {
        ui->btnExport->setEnabled(false);

        m_service->SetTransformedDataSavedAsync({},
            [this](bool success)
            {
                ui->btnExport->setEnabled(true);
                QMessageBox::information(this,
                    success ? "提示" : "错误",
                    success ? "导出成功" : "导出失败");
            });
    }

    void MainWindow::OnExportSlicesClicked()
    {
        ui->btnExportSlices->setEnabled(false);

        m_service->SetSliceImagesSavedAsync({}, 0.0,
            [this](bool success)
            {
                ui->btnExportSlices->setEnabled(true);
                QMessageBox::information(this,
                    success ? "提示" : "错误",
                    success ? "切片导出成功" : "切片导出失败");
            });
    }
*/

/*
─────────────────────────────────────────────────────────────────────
六、交互接口（AbstractInteractiveService）
─────────────────────────────────────────────────────────────────────

这些接口都是“轻量同步调用”：
- 只更新状态或写脏标记
- 不直接在前端调用处做 Render()
- 画面刷新由主线程统一节流处理

1) SetSliceScrolled(int delta)
   功能：
   - 在 2D 切片视图中切换切片层

2) SetWindowLevelAdjusted(int totalDx, int totalDy, int viewWidth, int viewHeight,
                          double startWW, double startWC)
   功能：
   - 根据鼠标拖拽增量调整窗宽窗位

3) SetCursorWorldPosition(double worldPos[3], int axis = -1)
   功能：
   - 更新十字光标世界坐标
   - 驱动多视图联动

4) SetElementVisible(uint32_t flagBit, bool show)
   功能：
   - 控制当前窗口实例的辅助元素显隐
   - 常用标志位：
     * VisFlags::Planes3D
     * VisFlags::Crosshair
     * VisFlags::Ruler

5) SetInteracting(bool val)
   功能：
   - 告知系统当前是否处于高频交互状态
   - 影响渲染更新速率

6) SetModelTransform(double translate[3], double rotate[3], double scale[3])
   功能：
   - 通过数值方式设置模型仿射变换

7) SetModelTransformReset()
   功能：
   - 重置模型变换

8) GetCursorWorld() / GetModelMatrix() / GetWindowLevel()
   功能：
   - 给前端读取当前关键状态

前端交互伪代码：

    void SliceWidget::wheelEvent(QWheelEvent* event)
    {
        int delta = event->angleDelta().y() > 0 ? 1 : -1;
        if (event->modifiers() & Qt::ControlModifier) {
            delta *= 5;
        }
        m_service->SetSliceScrolled(delta);
        event->accept();
    }

    void SliceWidget::mouseMoveEvent(QMouseEvent* event)
    {
        if (event->buttons() & Qt::RightButton) {
            int dx = event->pos().x() - m_lastMousePos.x();
            int dy = event->pos().y() - m_lastMousePos.y();
            m_lastMousePos = event->pos();

            auto wl = m_service->GetWindowLevel();
            m_service->SetWindowLevelAdjusted(
                dx,
                dy,
                width(),
                height(),
                wl.windowWidth,
                wl.windowCenter);
        }
    }

    void MainWindow::OnShowPlanesChanged(bool checked)
    {
        m_service->SetElementVisible(VisFlags::Planes3D, checked);
    }

    void MainWindow::OnShowCrosshairChanged(bool checked)
    {
        m_service->SetElementVisible(VisFlags::Crosshair, checked);
    }

    void MainWindow::OnShowRulerChanged(bool checked)
    {
        m_service->SetElementVisible(VisFlags::Ruler, checked);
    }

    void MainWindow::OnTransformChanged()
    {
        double t[3] = { ui->tx->value(), ui->ty->value(), ui->tz->value() };
        double r[3] = { ui->rx->value(), ui->ry->value(), ui->rz->value() };
        double s[3] = { 1.0, 1.0, 1.0 };
        m_service->SetModelTransform(t, r, s);
    }
*/

/*
─────────────────────────────────────────────────────────────────────
七、RenderContext 侧常用能力
─────────────────────────────────────────────────────────────────────

StdRenderContext 主要给前端提供：

1) SetServiceBound(service)
   - 绑定业务服务与渲染上下文

2) SetWindowTitle / SetWindowSize / SetWindowPosition
   - 设置窗口属性

3) SetCameraStyleByVizMode(mode)
   - 根据显示模式切换 2D / 3D 交互风格

4) SetOrientationAxesVisible(show)
   - 控制当前窗口自己的方向轴 / 朝向标记

5) SetInteractorInitialized()
   - 初始化 interactor / timer / 路由

6) SetStarted()
   - 启动渲染循环

7) SetToolMode(mode)
   - 切换工具模式
   - 例如：Navigation / ModelTransform

补充：
- 默认键盘中已接入部分快捷键：
  * M：切换模型变换模式
  * S：导出变换后体数据
  * Esc：回到导航模式
*/

/*
─────────────────────────────────────────────────────────────────────
八、前端推荐封装方式
─────────────────────────────────────────────────────────────────────

建议前端不要到处散着直接调底层接口，而是做一层薄封装：

    class VizWindowFacade {
    public:
        void Init(const WindowConfig& cfg);
        void LoadFile(const std::string& path);
        void ImportBuffer(...);
        void ExportCurrent();
        void SetMode(VizMode mode);
        void SetToolMode(ToolMode mode);
        void SetPlanesVisible(bool show);
        void SetCrosshairVisible(bool show);
        void SetRulerVisible(bool show);
        void SetOrientationAxesVisible(bool show);
        void SetWindowLevel(double ww, double wc);
        void SetTransform(...);
    };

好处：
- 前端按钮 / 面板 / 鼠标事件不直接依赖过多底层细节
- 每个窗口实例自己的显隐控制可以集中封装
- 后续切换 StdRenderContext / QtRenderContext / 多窗口布局更容易
- 业务调用顺序可以在 facade 中统一收口
*/

/*
─────────────────────────────────────────────────────────────────────
九、最小前端接入清单
─────────────────────────────────────────────────────────────────────

如果只做一个最小可用前端，至少接这几条：

1. 初始化：
   - DataManager
   - SharedInteractionState
   - MedicalVizService（每个窗口一份）
   - StdRenderContext（每个窗口一份）
   - SetServiceBound
   - SetInteractorInitialized

2. 首次配置：
   - SetVisualConfig
   - SetCameraStyleByVizMode
   - 按窗口选择是否 SetOrientationAxesVisible

3. 加载：
   - SetFileLoadedAsync
   - 回调里做 UI 状态恢复

4. 基础交互：
   - SetSliceScrolled
   - SetWindowLevelAdjusted
   - SetCursorWorldPosition
   - SetElementVisible
    - SetToolMode

5. 测量回调：
   - SetResultCallback
   - GetResults
   - SetResultVisible
   - SetResultsFileSaved

6. 导出：
   - SetTransformedDataSavedAsync

7. 启动：
   - SetStarted

这样即可完成：
- 数据加载
- 3D / 2D 显示
- 切片滚动
- 窗宽窗位调节
- 当前窗口实例的十字线 / 彩色平面 / 标尺显隐
- 独立测量实例的创建 / 预览 / 历史显示 / CSV 导出
- 模型变换后导出
─────────────────────────────────────────────────────────────────────
*/

/*
─────────────────────────────────────────────────────────────────────
十、核心 UML 类图与接口设计模式
─────────────────────────────────────────────────────────────────────

以下类图使用 PlantUML 文本表达，可直接复制到 PlantUML 工具中渲染：

@startuml
skinparam classAttributeIconSize 0

interface AbstractDataManager
class BaseDataManager
class RawVolumeDataManager
class TiffVolumeDataManager

abstract class AbstractAppService
abstract class AbstractInteractiveService
class MedicalVizService

interface IVisualConfigService
interface IDataLoaderService
interface IDataExportService

abstract class AbstractRenderContext
class StdRenderContext

abstract class AbstractVisualStrategy
class BaseVisualStrategy
class VolumeStrategy
class IsoSurfaceStrategy
class SliceStrategy
class MultiSliceStrategy
class ColoredPlanesStrategy
class CompositeStrategy
class GapMeshOverlayStrategy
class GapSliceOverlayStrategy
class StrategyFactory

class InteractionRouter
interface IInteractionHandler
class TimeUpdateHandler
class Viewer2DHandler
class Viewer3DHandler

interface IMeasurementService
class MeasurementService
class MeasurementInteractionHost
interface IMeasurementStrategy
class LengthMeasurementStrategy
class AngleMeasurementStrategy

AbstractDataManager <|-- BaseDataManager
BaseDataManager <|-- RawVolumeDataManager
BaseDataManager <|-- TiffVolumeDataManager

AbstractAppService <|-- AbstractInteractiveService
AbstractInteractiveService <|-- MedicalVizService
IVisualConfigService <|.. MedicalVizService
IDataLoaderService <|.. MedicalVizService
IDataExportService <|.. MedicalVizService

AbstractRenderContext <|-- StdRenderContext
AbstractRenderContext o--> AbstractAppService : bind service
StdRenderContext --> AbstractInteractiveService : hold interactive service
StdRenderContext o--> InteractionRouter
StdRenderContext o--> IMeasurementService
StdRenderContext o--> MeasurementInteractionHost

AbstractVisualStrategy <|-- BaseVisualStrategy
BaseVisualStrategy <|-- VolumeStrategy
BaseVisualStrategy <|-- IsoSurfaceStrategy
BaseVisualStrategy <|-- SliceStrategy
BaseVisualStrategy <|-- MultiSliceStrategy
BaseVisualStrategy <|-- ColoredPlanesStrategy
BaseVisualStrategy <|-- CompositeStrategy
BaseVisualStrategy <|-- GapMeshOverlayStrategy
BaseVisualStrategy <|-- GapSliceOverlayStrategy
BaseVisualStrategy <|-- LengthMeasurementStrategy
BaseVisualStrategy <|-- AngleMeasurementStrategy
StrategyFactory ..> AbstractVisualStrategy : create by VizMode

InteractionRouter o--> IInteractionHandler
IInteractionHandler <|.. TimeUpdateHandler
IInteractionHandler <|.. Viewer2DHandler
IInteractionHandler <|.. Viewer3DHandler

IMeasurementService <|.. MeasurementService
MeasurementInteractionHost --> IMeasurementService
MeasurementInteractionHost --> AbstractInteractiveService
MeasurementInteractionHost o--> IMeasurementStrategy
IMeasurementStrategy <|.. LengthMeasurementStrategy
IMeasurementStrategy <|.. AngleMeasurementStrategy
@enduml

接口与设计模式对应：

1) AbstractVisualStrategy / IMeasurementStrategy / AbstractDataConverter<InputT, OutputT>
   - Strategy 模式
   - 说明：把渲染算法、测量算法、数据变换算法抽象成可替换实现，前端或业务层只依赖抽象。

2) IInteractionHandler + InteractionRouter
   - Chain of Responsibility + Strategy 模式
   - 说明：各个 Handler 独立处理事件，Router 依据 FirstMatch / Broadcast 策略完成责任链式分发。

3) IVisualConfigService / IDataLoaderService / IDataExportService / AbstractInteractiveService
   - Facade 模式（按职责拆分的门面接口）
   - 说明：界面层只面向配置、加载、导出、交互这几组入口调用，底层状态同步、线程切换与 VTK 管线更新由 MedicalVizService 收口。

4) AbstractRenderContext
   - Template Method + Bridge 模式
   - 说明：抽象层固定 renderer / renderWindow 绑定流程，并把相机风格、交互器初始化、事件处理等步骤留给 StdRenderContext 落地。

5) IMeasurementService
   - Observer 模式
   - 说明：MeasurementService 内部维护观察者列表，MeasurementInteractionHost 通过回调接收测量状态变化并同步 overlay。

6) MeasurementInteractionHost
   - Adapter 模式
   - 说明：把 VTK 拾取、鼠标坐标和交互服务转换成 IMeasurementService 可理解的测量输入，再把业务结果映射回本地 overlay strategy。

7) AbstractDataManager
   - Strategy 模式
   - 说明：RawVolumeDataManager / TiffVolumeDataManager 提供可切换的数据加载实现，业务层始终依赖统一数据管理抽象。

8) StrategyFactory
   - Simple Factory 模式
   - 说明：根据 VizMode 统一创建具体 AbstractVisualStrategy，隔离对象创建细节。

9) CompositeStrategy
   - Composite 模式
   - 说明：内部组合主显示策略与参考平面策略，对外仍以单个 AbstractVisualStrategy 参与管线。

阅读建议：
- 前端主接入点优先看 MedicalVizService、StdRenderContext、MeasurementService。
- 新增显示模式时，优先新增 AbstractVisualStrategy 派生类并接入 StrategyFactory。
- 新增交互行为时，优先新增 IInteractionHandler 派生类并接入 InteractionRouter。
*/
