// 测试用途：组合十个功能页和五个视图，显示业务日志并管理窗口关闭流程。
#include "TestWindow.h"
#include "FeatureSetup.h"
#include "Support/JsonInput.h"
#include "Support/UiText.h"
#include <QApplication>
#include <QDateTime>
#include <QLocale>
#include <QMetaObject>
#include <QCloseEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
namespace Manual {
TestWindow::TestWindow(std::uint64_t budgetMiB)
{
    m_workflow.resources = GetTestResources(budgetMiB);
    setWindowTitle("全功能手动测试");
    setAcceptDrops(true);
    setStyleSheet("QTreeWidget { border: 1px solid #d6dde5; background: white; alternate-background-color: #f5f7fa; }"
        "QTreeWidget::item { padding: 6px 3px; } QTreeWidget::item:selected { background: #dceafb; color: #152b40; }"
        "QPushButton { padding: 5px 9px; border: 1px solid #bac8d6; border-radius: 4px; background: #f7f9fc; }"
        "QPushButton:hover { background: #e7f0fb; border-color: #5995cd; } QPushButton:pressed { background: #ccdef4; }"
        "QGroupBox { font-weight: 600; border: 1px solid #d6dde5; border-radius: 4px; margin-top: 9px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; } QLineEdit, QComboBox { min-height: 24px; }"
        "QPushButton:checked { background: #cce1f8; border: 1px solid #3677b5; color: #17416a; }"
        "QTabBar::tab { padding: 9px 12px; background: #e8edf3; border-bottom: 3px solid transparent; }"
        "QTabBar::tab:selected { background: #f8fbff; border-bottom: 3px solid #357fbc; color: #14558b; font-weight: 600; }");
    resize(1500, 950);
    auto* root = new QWidget(this); auto* layout = new QVBoxLayout(root); setCentralWidget(root);
    auto* toolbar = new QHBoxLayout;
    auto* stop = new QPushButton("停止当前计算", root);
    auto* exportRecords = new QPushButton("导出测试记录", root);
    auto* performance = new QPushButton("开始性能采样", root); performance->setCheckable(true);
    performance->setObjectName("performanceCapture");
    m_status = new QLabel("正在创建测试会话", root);
    toolbar->addWidget(m_status, 1); toolbar->addWidget(stop); toolbar->addWidget(performance); toolbar->addWidget(exportRecords); layout->addLayout(toolbar);
    m_featureTabs = new QTabBar(root); m_featureTabs->setObjectName("featureTabs");
    m_featureTabs->setExpanding(true); m_featureTabs->setUsesScrollButtons(true); m_featureTabs->setDrawBase(false);
    layout->addWidget(m_featureTabs);
    auto* split = new QSplitter(root); split->setChildrenCollapsible(false); layout->addWidget(split, 1);
    m_pages = new QStackedWidget(split); m_pages->setMinimumWidth(480);
    connect(m_featureTabs, &QTabBar::currentChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_pages, &QStackedWidget::currentChanged, m_featureTabs, &QTabBar::setCurrentIndex);
    connect(m_featureTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_isClosing || index < 0) return;
        AppendLog("切换功能：" + m_featureTabs->tabText(index));
        auto* panel = static_cast<ModulePanel*>(m_pages->currentWidget());
        if (panel && m_isReady) panel->Observe();
        if (panel && !panel->GetNotice().isEmpty()) AppendLog(panel->GetNotice());
    });
    m_viewArea = new QWidget(split); m_viewArea->setMinimumWidth(350); auto* viewLayout = new QVBoxLayout(m_viewArea);
    auto* mainViews = new QSplitter(m_viewArea); viewLayout->addWidget(mainViews, 3);
    auto* slices = new QWidget(m_viewArea); auto* sliceLayout = new QHBoxLayout(slices); viewLayout->addWidget(slices, 1);
    const QStringList ids{"primary-3d", "composite-volume", "slice-top-down", "slice-front-back", "slice-left-right"};
    const QStringList labels{"主三维", "体渲染", "上下切片", "前后切片", "左右切片"};
    for (int index = 0; index < ids.size(); ++index) {
        auto window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        // Qt 适配器第一次接管窗口前就确定透明/多重采样配置，不能等 Host 挂载后再改。
        window->SetAlphaBitPlanes(1); window->SetMultiSamples(0);
        auto* widget = new QVTKOpenGLNativeWidget(m_viewArea); widget->setMinimumSize(120, 100); widget->setRenderWindow(window);
        auto* frame = new QWidget(m_viewArea); auto* frameLayout = new QVBoxLayout(frame); frameLayout->setContentsMargins(0, 0, 0, 0);
        frameLayout->addWidget(new QLabel(labels[index], frame)); frameLayout->addWidget(widget, 1);
        if (index < 2) mainViews->addWidget(frame); else sliceLayout->addWidget(frame);
        m_views.push_back({ids[index], widget, window});
        widget->installEventFilter(this);
    }
    split->setSizes({580, 920});
    layout->addWidget(new QLabel("信息", root));
    m_log = new QPlainTextEdit(root); m_log->setReadOnly(true); m_log->setMaximumHeight(150); m_log->setMaximumBlockCount(1000); layout->addWidget(m_log);
    m_log->setObjectName("businessLog");
    m_log->setAccessibleName("操作说明与结果信息");
    m_log->setPlaceholderText("操作提交、执行进度和结果将在这里显示。");
    m_records.onChanged = [this](const QJsonObject& record) {
        AppendLog(GetFlowText(record));
        QueueObserve();
    };
    AppendLog("正在初始化五视图和功能测试会话。");
    m_pump.getVisible = [this](const std::string& id) {
        for (const auto& view : m_views)
            if (view.id.toStdString() == id) return view.widget->isVisible() && view.window->GetReadyForRendering();
        return false;
    };
    m_pump.onUpdated = [this] {
        const auto session = m_runtime.GetSession(); if (!session) return;
        const auto descriptor = session->GetImageDescriptor();
        const auto text = descriptor ? QString::fromStdString(descriptor->metadata.identity.datasetId) + QString("  ·  %1 × %2 × %3").arg(descriptor->dims[0]).arg(descriptor->dims[1]).arg(descriptor->dims[2]) : "拖入文件开始，或在数据页选择路径";
        if (m_status->text() != text) m_status->setText(text);
        QueueObserve();
    };
    m_pump.onRendered = [this] { QueueObserve(); };
    m_pump.onTiming = [this](const QString& name, qint64 ns) { m_records.AddTiming(name, ns); };
    m_workflow.getRenderPending = [this](const std::string& id) { return m_pump.GetIsRenderPending(id); };
    m_workflow.getViewRenderPending = [this](const std::string& id) { return m_pump.GetIsViewPending(id); };
    m_pump.onError = [this](const QString& message) { AppendLog(message); };
    connect(performance, &QPushButton::toggled, this, [this, performance](bool enabled) {
        performance->setText(enabled ? "结束性能采样" : "开始性能采样");
        if (enabled) {
            m_records.StartTiming(); AppendLog("性能采样已开始，请复现卡顿后点击结束；计时不会触发刷新或绘制。"); return;
        }
        m_records.StopTiming();
        const auto timings = m_records.GetTimings();
        const std::pair<QString, QString> labels[]{{"UI.ObserveAll", "界面观察（包含页面刷新）"}, {"Host.Update", "Host 更新"},
            {"Host.Render", "绘制请求（包含各视图）"}, {"Render.primary-3d", "主三维绘制"}, {"Render.composite-volume", "体渲染绘制"},
            {"Render.slice-top-down", "上下切片绘制"}, {"Render.slice-front-back", "前后切片绘制"}, {"Render.slice-left-right", "左右切片绘制"}};
        for (const auto& label : labels) {
            const auto value = timings[label.first].toObject();
            AppendLog(QString("性能 · %1：%2 次，累计 %3 ms，平均 %4 ms，最长 %5 ms，超过 16 ms：%6 次")
                .arg(label.second).arg(value["count"].toDouble(), 0, 'f', 0).arg(value["totalMs"].toDouble(), 0, 'f', 2)
                .arg(value["meanMs"].toDouble(), 0, 'f', 2).arg(value["maxMs"].toDouble(), 0, 'f', 2).arg(value["over16ms"].toDouble(), 0, 'f', 0));
        }
        AppendLog("性能采样已结束；分项存在包含关系，请勿相加。导出测试记录可保存本次计时与各页面明细。");
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        const auto id = m_workflow.GetBusyOperation();
        if (!id) { AppendLog("当前没有正在执行的计算；交互工具可在对应业务页退出。"); return; }
        const auto record = m_records.GetRecord(id);
        auto* module = GetModule(record["module"].toString());
        if (module && module->onStop) { AppendLog("请求停止：" + module->GetDisplayName()); module->onStop(); }
        else AppendLog("当前操作没有独立取消入口，请等待完成或关闭测试会话。");
    });
    connect(exportRecords, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "导出测试记录", {}, "JSON (*.json)");
        if (path.isEmpty()) return;
        try { auto records = m_records.GetRecords(); records["resources"] = m_workflow.resources.GetJson(); records["hostDiagnostics"] = GetDiagnostics(); ExportJson(path, records); AppendLog("测试记录已导出：" + path); }
        catch (const std::exception& e) { AppendLog("导出失败：" + QString::fromUtf8(e.what())); }
    });
    QMetaObject::invokeMethod(this, [this] { BuildSession(); }, Qt::QueuedConnection);
}
void TestWindow::QueueObserve()
{
    if (m_observeQueued || m_isClosing) return;
    m_observeQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_observeQueued = false;
        if (m_isClosing) return;
        const TestTiming timing(m_records, "UI.ObserveAll");
        for (auto* panel : m_modules) {
            const bool active = panel == m_pages->currentWidget();
            // 隐藏页仅保留任务终态消费，以及裁剪/表面跨页业务状态。
            if (!active && !panel->GetHasPending() && !panel->observeInBackground) continue;
            const TestTiming moduleTiming(m_records, "Observe." + panel->GetName());
            panel->Observe(active);
        }
    }, Qt::QueuedConnection);
}
void TestWindow::AppendLog(const QString& message)
{
    m_log->appendPlainText(QLocale::c().toString(QDateTime::currentDateTime(), "[HH:mm:ss.zzz] ") + message);
}
TestWindow::~TestWindow()
{
    // 正常路径只在 Stop 成功后进入析构；页面先于运行记录/工作流成员销毁。
    for (auto* module : m_modules) delete module;
    m_modules.clear();
}
void TestWindow::BuildSession()
{
    if (m_isReady || m_isClosing) return;
    try {
        HostSessionConfig config;
        const HostRenderViewRole roles[]{HostRenderViewRole::Primary3D, HostRenderViewRole::Composite3D,
            HostRenderViewRole::TopDownSlice, HostRenderViewRole::FrontBackSlice, HostRenderViewRole::LeftRightSlice};
        const HostRenderMode modes[]{HostRenderMode::CompositeIsoSurface, HostRenderMode::CompositeVolume,
            HostRenderMode::SliceTopDown, HostRenderMode::SliceFrontBack, HostRenderMode::SliceLeftRight};
        for (std::size_t index = 0; index < m_views.size(); ++index) {
            HostRenderViewConfig view; view.id = m_views[index].id.toStdString(); view.role = roles[index];
            view.renderWindow = m_views[index].window; view.window.viewInit.viewMode = modes[index];
            view.window.isAxesVisible = index == 0;
            config.renderViews.push_back(std::move(view));
        }
        m_pump.SetConfig(config);
        if (!m_runtime.BuildSession(std::move(config))) throw std::runtime_error("测试会话创建失败");
        m_pump.SetSession(m_runtime.GetSession());
        m_modules = BuildModules({m_runtime, m_workflow, m_records}, m_pages);
        AppendLog(QString("本次算法工作集上限 %1 GiB；伪影累计发布上限 %2 GiB。按启动时可用内存留出余量；可用 --memory-budget-mib 指定更小上限。").arg(m_workflow.resources.workingBytes / (1024.*1024.*1024.), 0, 'f', 2).arg(m_workflow.resources.publishBytes / (1024.*1024.*1024.), 0, 'f', 2));
        for (auto* page : m_modules) {
            m_pages->addWidget(page); m_featureTabs->addTab(page->GetDisplayName());
            page->onMessage = [this](const QString& message) { AppendLog(message); };
        }
        m_workflow.onCopyParameters = [this](const QString& module, const QString& action, const QJsonObject& patch) {
            auto* page = GetModule(module);
            if (!page || !page->GetActions().contains(action)) throw std::invalid_argument("当前构建未启用目标模块");
            page->SetParameterPatch(action, patch);
        };
        m_workflow.onNavigate = [this](const QString& module, const QString& action, const QJsonObject& patch) {
            auto* page = GetModule(module);
            if (!page || !page->GetActions().contains(action)) { AppendLog("当前构建未启用目标步骤：" + GetModuleText(module)); return; }
            try {
                page->SetParameterPatch(action, patch); m_pages->setCurrentWidget(page);
                if ((module == "Part" || module == "PartEdit") && patch["target"].isObject())
                    page->SelectPartTarget(patch["target"].toObject());
            }
            catch (const std::exception& error) { AppendLog("切换步骤失败：" + QString::fromUtf8(error.what())); }
        };
        m_workflow.getActionAvailable = [this](const QString& module, const QString& action) {
            const auto* page = GetModule(module); return page && page->GetActions().contains(action);
        };
        m_isReady = true;
        QStringList enabled;
        for (auto* page : m_modules) if (!page->GetActions().isEmpty()) enabled.append(page->GetDisplayName());
        AppendLog("测试会话已就绪：" + enabled.join("、") + "。请选择“数据输入 → 加载体数据”开始测试。");
        m_pump.SendUpdates();
    } catch (const std::exception& error) {
        m_failure = QString::fromUtf8(error.what()); m_status->setText(m_failure); AppendLog("初始化失败：" + m_failure);
    }
}
ModulePanel* TestWindow::GetModule(const QString& name) const
{
    for (auto* module : m_modules) if (module->GetName() == name) return module;
    return nullptr;
}
void TestWindow::SetViewsVisible(bool visible) { m_viewArea->setVisible(visible); m_pump.SetVisible(); }
bool TestWindow::StopSession()
{
    if (!m_isClosing) AppendLog("正在关闭测试会话，停止计算并释放视图资源。");
    m_isClosing = true; m_workflow.Stop(); m_pump.Stop();
    const auto session = m_runtime.GetSession();
    const auto current = GetDescriptor(session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{});
    if (!m_runtime.Stop()) { m_status->setText("正在停止：保留资源，等待重试"); return false; }
    m_records.StopPending(current);
    for (auto& view : m_views) view.widget->setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
    if (m_isReady) AppendLog("测试会话已停止。");
    m_isReady = false;
    return true;
}
void TestWindow::closeEvent(QCloseEvent* event)
{
    if (StopSession()) event->accept();
    else {
        event->ignore();
        // 仅退出当前调用栈后重试一次；持续失败交给用户再次关闭，不保留周期重试。
        if (!m_closeRetryQueued) {
            m_closeRetryQueued = true;
            QMetaObject::invokeMethod(this, [this] {
                m_closeRetryQueued = false;
                if (StopSession()) close();
                else AppendLog("停止尚未完成，资源已保留；可再次关闭窗口重试。");
            }, Qt::QueuedConnection);
        }
    }
}
void TestWindow::showEvent(QShowEvent* event) { QMainWindow::showEvent(event); m_pump.SetVisible(); }
bool TestWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_visibilityQueued && (event->type() == QEvent::Show || event->type() == QEvent::Resize || event->type() == QEvent::WindowStateChange || event->type() == QEvent::Paint)) {
        m_visibilityQueued = true;
        QMetaObject::invokeMethod(&m_pump, [this] { m_visibilityQueued = false; m_pump.SetVisible(); }, Qt::QueuedConnection);
    }
    return QMainWindow::eventFilter(watched, event);
}
void TestWindow::dragEnterEvent(QDragEnterEvent* event)
{
    const auto urls = event->mimeData()->urls(); if (urls.size() == 1 && urls.first().isLocalFile()) event->acceptProposedAction();
}
void TestWindow::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls(); if (urls.size() != 1 || !urls.first().isLocalFile()) return;
    auto* panel = GetModule("Data"); if (!panel) return;
    panel->SelectAction("Load"); panel->SetDroppedPath(urls.first().toLocalFile()); m_pages->setCurrentWidget(panel); event->acceptProposedAction();
}
}
