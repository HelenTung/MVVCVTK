//#pragma once
//#include "AppInterfaces.h"
//#include "InteractionRouter.h"
//#include <QObject>
//#include <QTimer>
//// 引入 QtVTK 相关的头文件，例如 QVTKOpenGLNativeWidget 提供的 renderWindow
//
//class QtRenderContext : public QObject, public AbstractRenderContext {
//    Q_OBJECT
//private:
//    std::shared_ptr<AbstractInteractiveService> m_interactiveService;
//    vtkSmartPointer<vtkRenderWindowInteractor>  m_interactor;
//    vtkSmartPointer<vtkCallbackCommand>         m_eventCallback;
//    vtkSmartPointer<vtkPropPicker>              m_picker;
//
//    // Qt 的定时器，用于替代 VTK Timer
//    QTimer* m_heartbeatTimer;
//
//    VizMode  m_currentMode = VizMode::Volume;
//    ToolMode m_toolMode = ToolMode::Navigation;
//    InteractionRouter m_interactionRouter;
//
//    void BuildInteractionRouter();
//
//public:
//    // 构造函数不再创建 RenderWindow，而是由外部（Qt Widget）注入
//    explicit QtRenderContext(vtkSmartPointer<vtkRenderWindow> qtRenderWindow, QObject* parent = nullptr);
//    ~QtRenderContext() override;
//
//    void InitInteractor() override;
//    void Start() override; // 在 Qt 中可能为空实现，或者只做初始化标志位
//    void BindService(std::shared_ptr<AbstractAppService> service) override;
//    void ApplyCameraStyleByVizMode(VizMode mode) override;
//
//private slots:
//    // 专门用于处理 Qt Timer 触发的更新
//    void OnQtTimerTick();
//
//protected:
//    void HandleVTKEvent(vtkObject* caller, long unsigned int eventId, void* callData) override;
//};


/*
─────────────────────────────────────────────────────────────────────
一、前处理与系统配置（IVisualConfigService）接口文档
─────────────────────────────────────────────────────────────────────

1. 调用时机
   - 绑定服务（BindService）后，数据加载（LoadFileAsync）前或后均可。
   - 时机为 Qt 窗口/Widget 刚创建且尚未加载任何数据时。
   - 仅修改 SharedState 或原子变量，不涉及底层 VTK 渲染计算，线程绝对安全。

2. 需要用到的类对象
   - 调用方持有：std::shared_ptr<MedicalVizService> (向上转型为 IVisualConfigService)
   - 参数载体：PreInitConfig 结构体 (位于 AppTypes.h，纯数据结构，利用 hasXXX 标志位控制有效字段)

3. 功能与调用流程

   - CommitVisualConfig(const PreInitConfig& cfg)
     说明：批量提交前处理配置（视图模式、背景色、材质、传输函数等）。采用一次锁+一次广播，VizMode 仅写原子变量，极大减少锁争用。
     时机：建议在所有初始参数组装完毕后统一调用。
     onComplete：无（同步极速完成）。

   - Config_SetVizMode(VizMode mode)
     说明：设置可视化模式（如体渲染/等值面/切片等），仅记录意图，实际管线重建在主线程处理。
     时机：随时调用（单项修改）。
     onComplete：无。

   - Config_SetBackground(const BackgroundColor& bg)
     说明：设置背景色，写入 SharedState（前处理阶段会同步直接写渲染器）。
     时机：随时调用。
     onComplete：无。

   - Config_SetMaterial / Config_SetOpacity / Config_SetWindowLevel / Config_SetIsoThreshold / Config_SetTransferFunction
     说明：分别设置材质、透明度、窗宽窗位、等值面阈值、传输函数，写入 SharedState。
     时机：随时调用。
     onComplete：无。

4. 典型调用流程示例（伪代码）
────────────────────────────
// 1. 实例化底层基建 (数据与状态隔离)
auto dataMgr = std::make_shared<RawVolumeDataManager>();
auto sharedState = std::make_shared<SharedInteractionState>();

// 2. 实例化业务调度器
auto service = std::make_shared<MedicalVizService>(dataMgr, sharedState);

// 3. 上下文绑定
auto context = std::make_shared<StdRenderContext>();
context->BindService(service);

// 4. 组装批量前处理参数
PreInitConfig cfg;
cfg.vizMode = VizMode::CompositeVolume; // 意图：体渲染+切片参考面
cfg.hasBgColor = true;
cfg.bgColor = { 0.08, 0.08, 0.12 };     // 深蓝色背景
cfg.hasWindowLevel = true;
cfg.windowLevel = { 400.0, 40.0 };      // 初始窗宽窗位
cfg.material = { 0.3, 0.6, 0.2, 15.0, 1.0, true }; // 材质参数

// 5. 提交配置
service->CommitVisualConfig(cfg);

// 6. 执行纯 UI 视角配置
context->ApplyCameraStyleByVizMode(cfg.vizMode);
context->ToggleOrientationAxes(true);
─────────────────────────────────────────────────────────────────────
*/

/*
─────────────────────────────────────────────────────────────────────
二、数据加载、导入与导出（IDataLoaderService & IDataExportService）接口文档
─────────────────────────────────────────────────────────────────────

1. 调用时机与设计规范
   - 必须在主线程（或 Qt 调度线程）发起异步调用，防止重入（内部状态为 Loading 时会拒绝新请求）。
   - 警告：所有的 onComplete 回调均在【后台工作线程】执行！
   - 在 onComplete 中：
     允许：读写 SharedState（内部自带锁）、通过 Qt 的线程安全机制（如 queued 信号槽、QMetaObject::invokeMethod）通知主线程更新 UI。
     禁止：直接调用任何 VTK 渲染接口（Render/Update/Modified）或直接修改 Qt 控件状态。

2. 需要用到的类对象
   - 调用方持有：std::shared_ptr<MedicalVizService> (向上转型为 IDataLoaderService 或 IDataExportService)
   - 状态查询：std::shared_ptr<SharedInteractionState>

3. 功能与调用流程

   - LoadFileAsync(const std::string& path, std::function<void(bool success)> onComplete)
     说明：异步从磁盘加载 RAW 或 TIFF 序列文件。
     时机：用户通过 UI 选择文件后调用。
     onComplete 可写内容：
       - 调用 SharedState 获取实际数据极值（GetDataRange），推算并设置等值面阈值或窗宽窗位。
       - 发射信号通知主线程隐藏 Loading 动画。

   - SetFromBufferAsync(const float* data, const std::array<int, 3>& dims,
                        const std::array<float, 3>& spacing, const std::array<float, 3>& origin,
                        std::function<void(bool success)> onComplete)
     说明：异步从内存 Buffer 导入数据（用于工业 CT 算法直连，后台执行分配与拷贝）。
     时机：上游重建算法产生结果内存块后调用。
     onComplete 可写内容：同上。

   - SaveTransformedDataAsync(const std::string& path, std::function<void(bool success)> onComplete)
     说明：异步保存当前经过模型仿射变换（旋转/平移/缩放）后的体数据（重新采样并裁剪对齐），导出为 RAW 文件。
     时机：用户调整好模型姿态，点击“导出”后调用。
     onComplete 可写内容：发信号通知主线程弹窗提示“保存成功/失败”。

   - CancelLoad()
     说明：请求取消当前加载任务（设置原子标记，底层尽力取消）。
     时机：用户主动点击“取消加载”时调用。
     onComplete：无。

   - GetLoadState() const
     说明：获取当前加载状态（Idle/Loading/Succeeded/Failed）。
     时机：随时调用，用于 UI 状态判断（如正在 Loading 时禁用其他按钮）。
     onComplete：无。

4. 典型调用流程示例（伪代码）
────────────────────────────
// 假设在 Qt 的某个槽函数中
void VolumeRenderWidget::OnLoadDataRequested(const QString& filePath)
{
    // 更新 UI 状态（主线程）
    ui->loadingOverlay->show();
    ui->btnLoad->setEnabled(false);

    // 转换为抽象接口
    IDataLoaderService* loader = m_vizService.get();

    // 捕获必要的指针，发起异步加载
    auto sharedState = m_sharedState;
    auto vizService = m_vizService;

    loader->LoadFileAsync(
        filePath.toStdString(),
        [this, sharedState, vizService](bool success) {
            // 警告：当前处于后台加载线程！

            if (success) {
                // 根据实际数据范围，自动推算最佳窗宽窗位
                auto range = sharedState->GetDataRange();
                double ww = (range[1] - range[0]) * 0.6;
                double wc = range[0] + (range[1] - range[0]) * 0.5;

                // 线程安全：只写 SharedState
                vizService->Config_SetWindowLevel(ww, wc);
            }

            // 安全地通知主线程更新 UI
            QMetaObject::invokeMethod(this, [this, success]() {
                ui->loadingOverlay->hide();
                ui->btnLoad->setEnabled(true);

                if (!success) {
                    QMessageBox::warning(this, "错误", "数据加载失败！");
                }
            }, Qt::QueuedConnection);
        }
    );
}
─────────────────────────────────────────────────────────────────────
*/

/*
─────────────────────────────────────────────────────────────────────
三、核心交互与多视图联动（AbstractInteractiveService）接口文档
─────────────────────────────────────────────────────────────────────

1. 调用时机与设计规范
   - 时机：数据加载完成且初始画面渲染出后，在软件运行的全生命周期内，响应用户的鼠标、键盘、滚轮或 UI 控件操作时随时调用。
   - 规范：所有的交互接口均为【同步、非阻塞】的极速调用。它们仅仅修改 SharedState 中的数值并打上脏标记（UpdateFlags），绝对不会直接触发耗时的 Render()。
   - 渲染机制：真正的视图更新由主线程定时器（如 Qt 的 QTimer 每 30ms 滴答一次，或底层 TimeUpdateHandler）捕获脏标记后统一执行。前端只管“疯狂发指令”，底层自动做“节流渲染”。

2. 需要用到的类对象
   - 调用方持有：std::shared_ptr<MedicalVizService> (向上转型为 AbstractInteractiveService)
   - 交互参数：通常来自 Qt 的 QMouseEvent、QWheelEvent 提取的坐标/增量，或 UI 面板滑块的数值。

3. 功能与调用流程

   - ScrollSlice(int delta)
     说明：滚动 2D MPR 视图（轴状/冠状/矢状）的切片位置。底层会自动判断当前视图方向并对最大/最小层数进行钳制。
     时机：在切片视图中滚动鼠标滚轮时调用。
     onComplete：无。

   - AdjustWindowLevel(double deltaWW, double deltaWC)
     说明：动态调整图像的窗宽（对比度）和窗位（亮度）。接收的是【增量】而非绝对值。
     时机：用户在视图中按住右键拖拽鼠标时，根据鼠标移动的像素差（dx, dy）换算为 delta 后调用。
     onComplete：无。

   - UpdateCursorFromWorldPosition(double worldPos[3], int axis = -1)
     说明：三视图与十字准星联动的核心接口。将鼠标在某一个视图中点击的物理坐标（世界坐标）同步到系统，底层会自动驱动其他所有视图的切片跳转到该位置。
     时机：Shift+左键拖拽十字准星，或在 3D 视图中拖拽参考切面时调用。
     onComplete：无。

   - SetElementVisible(uint32_t flagBit, bool show)
     说明：控制场景中辅助元素的显隐。flagBit 使用 VisFlags 枚举（如 VisFlags::Crosshair 十字准星，VisFlags::ClipPlanes 3D参考面）。
     时机：用户勾选 UI 面板上的 CheckBox 时调用。
     onComplete：无。

   - TransformModel(double translate[3], double rotate[3], double scale[3])
     说明：通过数值对 3D 模型进行精确的仿射变换（常用于工业 CT 姿态校正）。
     时机：用户在 UI 面板的 SpinBox 中输入旋转/平移数值后调用。
     onComplete：无。

4. 典型调用流程示例（伪代码）
────────────────────────────
// 示例 A：响应滚轮滚动 -> 切换切片
void SliceRenderWidget::wheelEvent(QWheelEvent *event)
{
    // 1. 判断滚轮方向（上滚 +1，下滚 -1）
    int delta = event->angleDelta().y() > 0 ? 1 : -1;

    // 2. Ctrl 键加速大步进翻页
    if (event->modifiers() & Qt::ControlModifier) {
        delta *= 5;
    }

    // 3. 抽象调用：底层自动处理越界钳制并通知所有相关视图
    m_vizService->ScrollSlice(delta);

    event->accept();
}

// 示例 B：响应鼠标右键拖拽 -> 动态调整窗宽窗位
void SliceRenderWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::RightButton)
    {
        // 1. 获取鼠标物理像素偏移量
        int dx = event->pos().x() - m_lastMousePos.x();
        int dy = event->pos().y() - m_lastMousePos.y();
        m_lastMousePos = event->pos();

        // 2. 转换像素偏移为 WW/WC 增量 (可配置灵敏度系数)
        double sensitivity = 2.0;
        double deltaWW = dx * sensitivity;
        double deltaWC = dy * sensitivity;

        // 3. 抽象调用：底层处理修改与脏标记分发
        m_vizService->AdjustWindowLevel(deltaWW, deltaWC);
    }
}

// 示例 C：响应 UI 面板 CheckBox -> 显隐 3D 参考切面
void MainWindow::on_checkBoxShowPlanes_toggled(bool checked)
{
    // 纯状态写入，底层通过 UpdateFlags::Visibility 安全同步
    m_vizService->SetElementVisible(VisFlags::ClipPlanes, checked);
}
─────────────────────────────────────────────────────────────────────
*/