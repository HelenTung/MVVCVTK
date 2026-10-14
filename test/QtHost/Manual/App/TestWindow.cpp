// 测试用途：左侧对象、中间三维视窗、右侧功能参数，统一管理会话生命周期。
#include "TestWindow.h"
#include "FeatureSetup.h"
#include "Support/JsonInput.h"
#include "Support/UiText.h"
#include "Support/ResultReport.h"
#include "Support/WorkbenchStyle.h"
#include <QMenu>
#include <QAction>
#include <QTabWidget>
#include <QApplication>
#include <QDateTime>
#include <QLocale>
#include <QMetaObject>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include "Support/ParameterEditor.h"
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QToolButton>
#include <QSignalBlocker>
#include <QDialog>
#include <QScreen>
#include <algorithm>
namespace Manual {
TestWindow::TestWindow(std::uint64_t budgetMiB)
{
    m_workflow.resources = GetTestResources(budgetMiB);
    setWindowTitle("全功能手动测试");
    setAcceptDrops(true);
    setFont(QFont("Microsoft YaHei UI", 9));
    setStyleSheet(GetWorkbenchStyle());
    // 默认尺寸按屏幕工作区确定；用户可调整，业务切换不自动改变外窗。
    const auto screen=QApplication::primaryScreen();
    const auto available=screen ? screen->availableGeometry().size()-QSize(32,64) : QSize(1560,920);
    const auto defaultSize=QSize(1560,920).boundedTo(available);
    setMinimumSize(QSize(1100,640).boundedTo(defaultSize));
    // Windows 的默认跟踪上限受屏幕限制；明确给宽松上限以容纳向外展开的诊断。
    setMaximumSize(32767,32767);resize(defaultSize);
    auto* root = new QWidget(this); root->setObjectName("workbench"); auto* layout = new QVBoxLayout(root); setCentralWidget(root);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    this->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(12,10,12,10); layout->setSpacing(8);
    auto* toolbarFrame = new QWidget(root); toolbarFrame->setObjectName("workbenchToolbar");
    auto* toolbar = new QHBoxLayout(toolbarFrame); toolbar->setContentsMargins(12,10,12,10);
    m_openVolume = new QPushButton("打开体数据…", root); m_openVolume->setObjectName("openVolume");
    m_openVolume->setProperty("primaryCommand", true);
    m_openVolume->setShortcut(QKeySequence::Open); m_openVolume->setEnabled(false);
    m_openVolume->setToolTip("选择输入文件，核对尺寸、间距等参数后点击“加载体数据”。（Ctrl+O）");
    m_stop = new QPushButton("停止计算", root); m_stop->setObjectName("stopCalculation"); m_stop->setEnabled(false);
    auto* tools = new QToolButton(root); tools->setObjectName("testTools"); tools->setText("测试工具");
    tools->setPopupMode(QToolButton::InstantPopup); auto* toolMenu = new QMenu(tools); tools->setMenu(toolMenu);
    auto* exportRecords = toolMenu->addAction("导出测试记录"); exportRecords->setObjectName("exportTestRecords");
    auto* showReport = new QPushButton("结果报告 / CSV", root); showReport->setObjectName("showResultReport");
    auto* performance = toolMenu->addAction("开始性能采样"); performance->setCheckable(true);
    performance->setObjectName("performanceCapture");
    m_sourceButton=new QPushButton("查看来源",toolbarFrame);m_sourceButton->setObjectName("showInputSources");m_sourceButton->setEnabled(false);
    toolbar->addWidget(m_openVolume);toolbar->addWidget(m_sourceButton);toolbar->addStretch(); toolbar->addWidget(m_stop); toolbar->addWidget(showReport); toolbar->addWidget(tools); layout->addWidget(toolbarFrame);
    connect(m_sourceButton,&QPushButton::clicked,this,[this] {
        const auto session=GetSession();const auto input=session?session->GetImageDescriptor():std::optional<ImageDescriptor>{};
        if(input)if(auto* page=static_cast<ModulePanel*>(m_pages->currentWidget()))page->ShowSources(input->dataRevision);
    });
    connect(m_openVolume, &QPushButton::clicked, this, [this] {
        auto* page = GetModule("Data"); if (!m_isReady || m_isClosing || !page) return;
        const auto currentPath = page->GetParameterEditor("Load")->GetField("filePath")->GetValue().toString();
        QFileDialog dialog(this, "打开体数据", currentPath, "体数据 (*.raw *.RAW);;所有文件 (*)");
        dialog.setObjectName("inputFileDialog"); dialog.setFileMode(QFileDialog::ExistingFile);
        if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) SetInputFile(dialog.selectedFiles().first());
    });
    auto* navigation = new QWidget(root); navigation->setObjectName("workbenchNavigation");
    auto* navigationLayout = new QVBoxLayout(navigation); navigationLayout->setContentsMargins(8,4,8,0); navigationLayout->setSpacing(0);
    m_workspaceTabs = new QTabBar(navigation); m_workspaceTabs->setObjectName("workspaceTabs");
    m_workspaceTabs->setExpanding(false); m_workspaceTabs->setDrawBase(false);
    for (const auto& title : {"数据与视图", "处理与建模", "检测与分析"}) m_workspaceTabs->addTab(title);
    navigationLayout->addWidget(m_workspaceTabs);
    m_featureTabs = new QTabBar(navigation); m_featureTabs->setObjectName("featureTabs"); m_featureTabs->setMinimumHeight(40);
    m_featureTabs->setExpanding(false); m_featureTabs->setUsesScrollButtons(true); m_featureTabs->setDrawBase(false);
    navigationLayout->addWidget(m_featureTabs); layout->addWidget(navigation);
    m_measurements = new QLabel(root); m_measurements->setObjectName("businessMeasurements");
    m_measurements->setWordWrap(true); m_measurements->setMinimumWidth(0);
    m_measurements->setToolTip("内存为当前测试进程的 OS 实测值，峰值从进程启动累计。加载为请求到终态；首次绘制和逐帧耗时到 Host/VTK Render 返回，不包含 GPU 完成或屏幕呈现。分项不可相加。");

    auto* split = new QSplitter(root); split->setChildrenCollapsible(false); layout->addWidget(split, 1);
    auto* objectColumn = new QWidget(split); objectColumn->setObjectName("objectColumn");
    auto* objectToggle=new QToolButton(toolbarFrame);objectToggle->setObjectName("toggleObjectColumn");objectToggle->setText("收起对象栏");objectToggle->setCheckable(true);objectToggle->setChecked(true);toolbar->insertWidget(2,objectToggle);
    auto objectSizes=std::make_shared<QList<int>>();
    connect(objectToggle,&QToolButton::toggled,this,[split,objectColumn,objectToggle,objectSizes](bool shown){if(!shown)*objectSizes=split->sizes();objectColumn->setVisible(shown);if(shown && !objectSizes->isEmpty())split->setSizes(*objectSizes);objectToggle->setText(shown?"收起对象栏":"展开对象栏");});
    auto* objectLayout = new QVBoxLayout(objectColumn); objectLayout->setContentsMargins(8,8,8,8);
    auto* objectTitle = new QLabel("对象与结果", objectColumn); objectTitle->setObjectName("columnTitle"); objectLayout->addWidget(objectTitle);
    m_browsers = new QStackedWidget(objectColumn); objectLayout->addWidget(m_browsers,1); m_browsers->setObjectName("browserStack"); m_browsers->setMinimumWidth(220);
    m_pages = new QStackedWidget(root); m_pages->setMinimumWidth(320);
    m_pages->setObjectName("parameterPages");
    connect(m_workspaceTabs, &QTabBar::currentChanged, this, &TestWindow::SetWorkspace);
    connect(m_featureTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_isClosing || index < 0) return;
        auto* page = GetModule(m_featureTabs->tabData(index).toString());
        if (page) m_pages->setCurrentWidget(page);
    });
    connect(m_pages, &QStackedWidget::currentChanged, this, [this] { SyncNavigation(); });
    m_viewArea = new QWidget(split); m_viewArea->setMinimumWidth(480); auto* viewLayout = new QVBoxLayout(m_viewArea);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    auto* viewHeading = new QHBoxLayout;
    auto* viewTitle = new QLabel("视图", m_viewArea); viewTitle->setObjectName("columnTitle"); viewHeading->addWidget(viewTitle); viewHeading->addStretch();
    auto* viewLayoutChoice = new QComboBox(m_viewArea); viewLayoutChoice->setObjectName("viewLayout");
    viewLayoutChoice->addItem("三维与切片", "Inspection");viewLayoutChoice->addItem("四视图", "FourViews"); viewLayoutChoice->addItem("仅三维", "ThreeDOnly"); viewLayoutChoice->setMinimumWidth(96);
    viewHeading->addWidget(viewLayoutChoice); viewLayout->addLayout(viewHeading);
    auto* statisticsToggle=new QToolButton(m_viewArea);m_showStatistics=statisticsToggle;statisticsToggle->setObjectName("showBusinessStatistics");statisticsToggle->setText("统计与结果");statisticsToggle->setCheckable(true);viewHeading->addWidget(statisticsToggle);
    m_statisticsPages=new QStackedWidget(m_viewArea);m_statisticsPages->setObjectName("businessStatisticsPages");
    auto* statisticsDialog=new QDialog(this);statisticsDialog->setObjectName("businessStatisticsDialog");statisticsDialog->setWindowTitle("当前业务统计与结果");statisticsDialog->resize(960,620);
    auto* statisticsLayout=new QVBoxLayout(statisticsDialog);
    m_statisticsPages->setMaximumHeight(180);
    auto* expandStatistics=new QToolButton(m_viewArea);expandStatistics->setObjectName("expandBusinessStatistics");expandStatistics->setText("放大统计");viewHeading->addWidget(expandStatistics);
    connect(statisticsToggle,&QToolButton::toggled,this,[this,statisticsDialog](bool shown) {
        if(auto* page=static_cast<ModulePanel*>(m_pages->currentWidget())) {
            m_statisticsPreferences[page->GetName()]=shown;m_statisticsPages->setVisible(shown);
            if(shown)page->RefreshStatistics();else {page->GetStatisticsWidget()->Cancel();statisticsDialog->close();}
        }
    });
    connect(expandStatistics,&QToolButton::clicked,this,[this,statisticsDialog,statisticsLayout] {
        m_showStatistics->setChecked(true);m_statisticsPages->setMaximumHeight(16777215);
        for(auto* module:m_modules)module->GetStatisticsWidget()->SetCompact(false);
        statisticsLayout->addWidget(m_statisticsPages);m_statisticsPages->show();statisticsDialog->show();statisticsDialog->raise();
    });
    connect(statisticsDialog,&QDialog::finished,this,[this,viewLayout](int){
        for(auto* module:m_modules)module->GetStatisticsWidget()->SetCompact(true);
        viewLayout->addWidget(m_statisticsPages);m_statisticsPages->setMaximumHeight(180);m_statisticsPages->setVisible(m_showStatistics->isChecked());
    });
    connect(viewLayoutChoice, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this,viewLayoutChoice](int index) { SetViewLayout(viewLayoutChoice->itemData(index).toString()); });
    // 四个图像区域共用同一网格和同高标题栏；窗口缩放不改变等分关系。
    auto* viewGrid = new QGridLayout; m_viewGrid = viewGrid; viewGrid->setObjectName("fourViewGrid");
    viewGrid->setContentsMargins(0,0,0,0); viewGrid->setSpacing(6);
    for (int i = 0; i < 2; ++i) { viewGrid->setRowStretch(i, 1); viewGrid->setColumnStretch(i, 1); }
    viewLayout->addLayout(viewGrid, 1);
    viewLayout->addWidget(m_statisticsPages);m_statisticsPages->hide();
    constexpr int headerHeight = 36;
    auto* primaryFrame = new QWidget(m_viewArea); auto* primaryLayout = new QVBoxLayout(primaryFrame);
    primaryLayout->setContentsMargins(0,0,0,0); primaryLayout->setSpacing(6);
    auto* primaryHeader = new QWidget(primaryFrame); primaryHeader->setObjectName("viewHeader"); primaryHeader->setFixedHeight(headerHeight);
    auto* viewToolbar = new QHBoxLayout(primaryHeader); viewToolbar->setContentsMargins(0,0,0,0);
    viewToolbar->addWidget(new QLabel("3D 视窗", m_viewArea)); viewToolbar->addStretch();
    m_renderMode = new QComboBox(m_viewArea); m_renderMode->setObjectName("renderMode");
    m_renderMode->addItem("等值面", "CompositeIsoSurface"); m_renderMode->addItem("体渲染", "CompositeVolume");
    m_renderMode->setEnabled(false); viewToolbar->addWidget(m_renderMode);
    auto* fit = new QPushButton("适配视图", m_viewArea); fit->setObjectName("fitView"); fit->setEnabled(false); viewToolbar->addWidget(fit);
    primaryLayout->addWidget(primaryHeader);
    viewGrid->addWidget(primaryFrame, 0, 0);
    auto window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    window->SetAlphaBitPlanes(1); window->SetMultiSamples(0);
    auto* widget = new QVTKOpenGLNativeWidget(primaryFrame); widget->setObjectName("primary3D");
    widget->setMinimumSize(160, 160); widget->setRenderWindow(window); primaryLayout->addWidget(widget, 1);
    m_views.push_back({"primary-3d", widget, window, primaryFrame}); widget->installEventFilter(this);
    const QStringList sliceIds{"slice-top-down", "slice-front-back", "slice-left-right"};
    const QStringList sliceLabels{"上下切片", "前后切片", "左右切片"};
    for (int index = 0; index < 3; ++index) {
        auto* frame = new QWidget(m_viewArea); auto* frameLayout = new QVBoxLayout(frame);
        frameLayout->setContentsMargins(0,0,0,0); frameLayout->setSpacing(6);
        auto* header = new QLabel(sliceLabels[index], frame); header->setObjectName("viewHeader"); header->setFixedHeight(headerHeight); frameLayout->addWidget(header);
        viewGrid->addWidget(frame, (index+1)/2, (index+1)%2);
        auto sliceWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New(); sliceWindow->SetAlphaBitPlanes(1); sliceWindow->SetMultiSamples(0);
        auto* slice = new QVTKOpenGLNativeWidget(frame); slice->setObjectName(sliceIds[index]); slice->setMinimumSize(160,96);
        slice->setRenderWindow(sliceWindow); frameLayout->addWidget(slice, 1); slice->installEventFilter(this);
        m_views.push_back({sliceIds[index], slice, sliceWindow, frame});
    }
    split->addWidget(m_pages); split->setStretchFactor(0, 0); split->setStretchFactor(1, 1); split->setStretchFactor(2, 0);
    split->setSizes({230, 890, 420});
    if(height()<900)viewLayoutChoice->setCurrentIndex(viewLayoutChoice->findData("FourViews"));
    else SetViewLayout("Inspection");
    connect(m_renderMode, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (auto* page = GetModule("View")) {
            auto parameters = GetJson(R"({"viewId":"primary-3d","mode":null,"iso":null,"opacity":null,"quality":null,"axes":null,"windowLevel":null,"transfer":null})");
            parameters["mode"] = m_renderMode->itemData(index).toString(); page->SendAction("Set", parameters);
            const auto state = m_runtime.GetSession()->GetRenderViewState({"primary-3d"});
            if (state) { const QSignalBlocker blocker(m_renderMode); m_renderMode->setCurrentIndex(
                state->viewMode == HostRenderMode::Volume || state->viewMode == HostRenderMode::CompositeVolume ? 1 : 0); }
        }
    });
    connect(fit, &QPushButton::clicked, this, [this] {
        if (auto* page = GetModule("View")) page->SendAction("Reset", {{"viewId", "primary-3d"}});
    });
    auto* diagnosticBar = new QWidget(root); diagnosticBar->setObjectName("diagnosticBar");
    auto* diagnosticHeading = new QHBoxLayout(diagnosticBar); diagnosticHeading->setContentsMargins(8,4,8,4);
    auto* logToggle = new QToolButton(diagnosticBar); logToggle->setObjectName("toggleDiagnostics"); logToggle->setText("诊断"); logToggle->setCheckable(true);
    logToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); logToggle->setArrowType(Qt::RightArrow);
    m_latestMessage = new QLabel(diagnosticBar); m_latestMessage->setObjectName("latestMessage");
    m_latestMessage->setMinimumWidth(0); m_latestMessage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* messages=new QWidget(diagnosticBar);auto* messageLayout=new QVBoxLayout(messages);messageLayout->setContentsMargins(0,0,0,0);messageLayout->setSpacing(2);
    m_latestMessage->setWordWrap(true);messageLayout->addWidget(m_latestMessage);
    m_messageData=new QLabel("数据 · 尚未加载体数据",messages);m_messageData->setObjectName("messageDataSummary");m_messageData->setMinimumWidth(0);m_messageData->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);m_messageData->setWordWrap(true);messageLayout->addWidget(m_messageData);
    diagnosticHeading->addWidget(messages,1);diagnosticHeading->addWidget(logToggle);layout->addWidget(diagnosticBar);
    auto* diagnosticDetails = new QWidget(root); diagnosticDetails->setObjectName("diagnosticDetails");
    auto* diagnosticLayout = new QVBoxLayout(diagnosticDetails); diagnosticLayout->setContentsMargins(0,0,0,0);
    diagnosticLayout->addWidget(m_measurements);
    m_inputIdentity=new QLabel("当前输入 · 尚未加载体数据",diagnosticDetails);m_inputIdentity->setObjectName("currentInputIdentity");m_inputIdentity->setWordWrap(true);diagnosticLayout->addWidget(m_inputIdentity);
    m_inputGeometry=new QLabel(diagnosticDetails);m_inputGeometry->setObjectName("currentInputGeometry");m_inputGeometry->setWordWrap(true);diagnosticLayout->addWidget(m_inputGeometry);
    m_log = new QPlainTextEdit(diagnosticDetails); m_log->setReadOnly(true); m_log->setMaximumBlockCount(1000); m_log->setMaximumHeight(120);
    diagnosticLayout->addWidget(m_log); layout->addWidget(diagnosticDetails); diagnosticDetails->hide();
    auto* reports = new QDialog(this); reports->setObjectName("operationReports"); reports->setWindowTitle("运行结果报告"); reports->resize(940,620);
    auto* reportLayout = new QVBoxLayout(reports); reportLayout->setContentsMargins(8,8,8,8);
    m_report = new ResultReport(reports); reportLayout->addWidget(m_report);
    auto diagnosticFromMaximized=std::make_shared<bool>(false);auto normalGeometryBeforeDiagnostics=std::make_shared<QRect>();auto expandedDiagnosticGeometry=std::make_shared<QRect>();
    connect(logToggle, &QToolButton::toggled, this, [this,diagnosticDetails,logToggle,layout,split,diagnosticFromMaximized,normalGeometryBeforeDiagnostics,expandedDiagnosticGeometry](bool visible) {
        const bool restoreMaximized=!visible && *diagnosticFromMaximized && geometry()==*expandedDiagnosticGeometry;
        if(visible){*diagnosticFromMaximized=isMaximized();*normalGeometryBeforeDiagnostics=normalGeometry();}
        if(isMaximized()){const auto before=size();showNormal();resize(before);layout->activate();}
        const auto outerHeight=height();const auto extra=(visible?diagnosticDetails->sizeHint().height():diagnosticDetails->height())+layout->spacing();
        diagnosticDetails->setVisible(visible);resize(width(),height()+(visible?extra:-extra));layout->activate();
        // 首次显示后按实际布局高度补偿；隐藏时扣除同一块空间。
        this->layout()->activate();layout->activate();
        const auto desiredHeight=outerHeight+(visible?diagnosticDetails->height()+layout->spacing():-extra);
        if(height()!=desiredHeight){resize(width(),desiredHeight);this->layout()->activate();layout->activate();}
        if(visible)*expandedDiagnosticGeometry=geometry();else if(restoreMaximized){setGeometry(*normalGeometryBeforeDiagnostics);showMaximized();}
        logToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(showReport,&QPushButton::clicked,this,[reports]{reports->show();reports->raise();reports->activateWindow();});
    m_log->setObjectName("businessLog");
    m_log->setAccessibleName("业务流程诊断");
    m_log->setPlaceholderText("业务步骤、提交、进度、结果与输入切换的完整流程将在这里显示。");
    m_records.onChanged = [this](const QJsonObject& record) {
        AppendLog(GetFlowText(record));
        ShowMessage(GetFlowText(record,false));
        m_report->SetRecord(record);
        UpdateMeasurements();
        QueueObserve();
    };
    AppendLog("正在初始化一个三维与三个切片视窗。");
    ShowMessage("打开体数据，核对输入参数后开始业务。");
    m_pump.getVisible = [this](const std::string& id) {
        for (const auto& view : m_views)
            if (view.id.toStdString() == id) return view.widget->isVisible() && view.window->GetReadyForRendering();
        return false;
    };
    m_pump.onUpdated = [this, fit] {
        const auto session = m_runtime.GetSession(); if (!session) return;
        const auto applied = session->GetRenderViewState({"primary-3d"});
        if (applied) { const QSignalBlocker blocker(m_renderMode); m_renderMode->setCurrentIndex(
            applied->viewMode == HostRenderMode::Volume || applied->viewMode == HostRenderMode::CompositeVolume ? 1 : 0); }
        const auto descriptor = session->GetImageDescriptor();
        m_renderMode->setEnabled(descriptor.has_value()); fit->setEnabled(descriptor.has_value());
        UpdateInputIdentity();
        UpdateMeasurements();
        QueueObserve();
    };
    m_pump.onViewRendered = [this](const HostViewRenderResult& rendered) {
        const auto session = m_runtime.GetSession();
        const auto state = session ? session->GetRenderViewState({rendered.viewId}) : std::optional<HostRenderViewState>{};
        if (state) m_records.SetRendered(QString::fromStdString(rendered.viewId), GetRefText(state->dataRevision), rendered.durationUs);
    };
    m_pump.onRendered = [this] { UpdateMeasurements(); QueueObserve(); };
    m_pump.onTiming = [this](const QString& name, qint64 ns) { m_records.AddTiming(name, ns); };
    m_workflow.getRenderPending = [this](const std::string& id) { return m_pump.GetIsRenderPending(id); };
    m_workflow.getViewRenderPending = [this](const std::string& id) { return m_pump.GetIsViewPending(id); };
    m_pump.onError = [this](const QString& message) { AppendLog(message); };
    connect(performance, &QAction::toggled, this, [this, performance, tools](bool enabled) {
        performance->setText(enabled ? "结束性能采样" : "开始性能采样");
        tools->setText(enabled ? "测试工具 · 采样中" : "测试工具");
        if (enabled) {
            m_records.StartTiming(); AppendLog("性能采样已开始，请复现卡顿后点击结束；计时不会触发刷新或绘制。"); return;
        }
        m_records.StopTiming();
        const auto timings = m_records.GetTimings();
        const std::pair<QString, QString> labels[]{{"UI.ObserveAll", "界面观察（包含页面刷新）"}, {"Host.Update", "Host 更新"},
            {"Host.Render", "绘制请求"}, {"Render.primary-3d", "三维视窗绘制"}, {"Render.slice-top-down", "上下切片绘制"},
            {"Render.slice-front-back", "前后切片绘制"}, {"Render.slice-left-right", "左右切片绘制"}};
        for (const auto& label : labels) {
            const auto value = timings[label.first].toObject();
            AppendLog(QString("性能 · %1：%2 次，累计 %3 ms，平均 %4 ms，最长 %5 ms，超过 16 ms：%6 次")
                .arg(label.second).arg(value["count"].toDouble(), 0, 'f', 0).arg(value["totalMs"].toDouble(), 0, 'f', 2)
                .arg(value["meanMs"].toDouble(), 0, 'f', 2).arg(value["maxMs"].toDouble(), 0, 'f', 2).arg(value["over16ms"].toDouble(), 0, 'f', 0));
        }
        AppendLog("性能采样已结束；分项存在包含关系，请勿相加。导出测试记录可保存本次计时与各页面明细。");
    });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        const auto id = m_workflow.GetBusyOperation();
        if (!id) { AppendLog("当前没有正在执行的计算；交互工具可在对应业务页退出。"); return; }
        const auto record = m_records.GetRecord(id);
        auto* module = GetModule(record["module"].toString());
        if (module && module->onStop) { AppendLog("请求停止：" + module->GetDisplayName()); module->onStop(); }
        else AppendLog("当前操作没有独立取消入口，请等待完成或关闭测试会话。");
    });
    connect(exportRecords, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "导出测试记录", {}, "JSON (*.json)");
        if (path.isEmpty()) return;
        try { auto records = m_records.GetRecords(); records["resources"] = m_workflow.resources.GetJson(); records["hostDiagnostics"] = GetDiagnostics(); ExportJson(path, records); AppendLog("测试记录已导出：" + path); }
        catch (const std::exception& e) { AppendLog("导出失败：" + QString::fromUtf8(e.what())); }
    });
    QMetaObject::invokeMethod(this, [this] { BuildSession(); }, Qt::QueuedConnection);
    UpdateMeasurements();
}
void TestWindow::SetWorkspace(int index)
{
    if (m_isClosing || index < 0 || index >= m_workspaceSelections.size()) return;
    if (auto* page = GetModule(m_workspaceSelections[index])) m_pages->setCurrentWidget(page);
}
void TestWindow::SyncNavigation()
{
    if (m_isClosing || m_modules.empty()) return;
    auto* page = static_cast<ModulePanel*>(m_pages->currentWidget()); if (!page) return;
    const std::vector<QStringList> groups{{"Data", "View", "Rotation"}, {"Artifact", "Roi", "Crop", "Part", "Surface"}, {"Alignment", "Wall", "Gap"}};
    int workspace = -1;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) if (groups[i].contains(page->GetName())) workspace = i;
    if (workspace < 0) return;
    const QSignalBlocker workspaceBlocker(m_workspaceTabs), featureBlocker(m_featureTabs);
    m_workspaceTabs->setCurrentIndex(workspace); m_workspaceSelections[workspace] = page->GetName();
    QStringList current;
    for (int i = 0; i < m_featureTabs->count(); ++i) current.append(m_featureTabs->tabData(i).toString());
    QStringList expected;
    for(const auto& id:QStringList{"Data","View","Rotation","Artifact","Roi","Crop","Part","Surface","Alignment","Wall","Gap"})if(GetModule(id))expected.append(id);
    if (current.size() != expected.size() || !std::equal(current.cbegin(), current.cend(), expected.cbegin())) {
        while (m_featureTabs->count()) m_featureTabs->removeTab(0);
        for (const auto& id : expected) {
            const auto tab = m_featureTabs->addTab(GetModule(id)->GetDisplayName()); m_featureTabs->setTabData(tab, id);
        }
    }
    m_featureTabs->setCurrentIndex(expected.indexOf(page->GetName()));
    m_browsers->setCurrentWidget(page->GetBrowser());
    m_statisticsPages->setCurrentWidget(page->GetStatisticsWidget());
    for(auto* module:m_modules)if(module!=page)module->GetStatisticsWidget()->Cancel();
    const bool statistics=m_statisticsPreferences.value(page->GetName(),page->GetName()!="Rotation" && !page->GetActions().isEmpty());
    {const QSignalBlocker blocker(m_showStatistics);m_showStatistics->setChecked(statistics);}m_statisticsPages->setVisible(statistics);
    if(!statistics)page->GetStatisticsWidget()->Cancel();
    if(page->GetName()=="Crop" && m_workflow.ensureCropViews)m_workflow.ensureCropViews();
    if (m_isReady) {
        page->Observe(); AppendLog("切换功能：" + page->GetDisplayName());
        if (!page->GetNotice().isEmpty()) AppendLog(page->GetNotice());
    }
}
void TestWindow::SetViewLayout(const QString& mode)
{
    if (m_isClosing || m_views.size() != 4) return;
    auto effective=mode;
    if(mode=="ThreeDOnly" && m_pages->currentWidget() && static_cast<ModulePanel*>(m_pages->currentWidget())->GetName()=="Crop") {
        effective="FourViews";auto* choice=findChild<QComboBox*>("viewLayout");const QSignalBlocker blocker(choice);choice->setCurrentIndex(choice->findData(effective));
        AppendLog("裁剪确认需要四个视图的呈现结果，已恢复四视图布局。");
    }
    const bool isThreeDOnly=effective=="ThreeDOnly",isInspection=effective=="Inspection";
    for (const auto& view : m_views) m_viewGrid->removeWidget(view.frame);
    for(int row=0;row<3;++row){m_viewGrid->setRowStretch(row,isInspection || row<2 ? 1 : 0);m_viewGrid->setRowMinimumHeight(row,0);}
    m_viewGrid->setColumnStretch(0,isInspection ? 3 : 1);m_viewGrid->setColumnStretch(1,isInspection ? 1 : 1);
    if (isThreeDOnly) m_viewGrid->addWidget(m_views.front().frame, 0, 0, 2, 2);
    else if(isInspection){m_viewGrid->addWidget(m_views.front().frame,0,0,3,1);for(int i=1;i<4;++i)m_viewGrid->addWidget(m_views[i].frame,i-1,1);}
    for (int i = 0; i < 4; ++i) {
        if (!isThreeDOnly && !isInspection) m_viewGrid->addWidget(m_views[i].frame, i/2, i%2);
        m_views[i].frame->setVisible(!isThreeDOnly || i == 0);
    }
    m_viewGrid->activate(); m_pump.SetVisible();
}
void TestWindow::UpdateStopCommand()
{
    const auto id = m_workflow.GetBusyOperation();
    const auto* module = id ? GetModule(m_records.GetRecord(id)["module"].toString()) : nullptr;
    const bool available = m_isReady && !m_isClosing && module && module->onStop && !m_records.GetRecord(id)["isTerminal"].toBool();
    m_stop->setEnabled(available);
    m_stop->setText(module && module->GetName()=="Crop" ? "返回源并结束裁剪" : "停止计算");
    m_stop->setToolTip(available ? module->stopDescription.isEmpty()?QString("请求停止当前计算；消息栏将报告实际结果。"):module->stopDescription : id ? "当前计算没有独立取消入口，请等待完成。" : "当前没有正在执行的计算。");
}
void TestWindow::UpdateMeasurements()
{
    UpdateStopCommand();
    const auto measured = m_records.GetMeasurements();
    const auto memory = measured["memory"].toObject();
    const auto load = measured["load"].toObject();
    const auto first = load["firstRenderedViews"].toObject()["primary-3d"].toObject();
    const auto render = m_records.GetTimings()["Render.primary-3d"].toObject();
    const auto mib = [&](const char* key) { return memory["available"].toBool()
        ? QString::number(memory[key].toString().toULongLong() / 1048576.0, 'f', 1) : QString("不可用"); };
    const auto elapsed = load["status"] == "Succeeded" ? load["requestToCompleteMs"].toString() + " ms" : QString("等待成功加载");
    const auto text = QString("加载到终态：%1  ·  首次三维绘制返回：%2  ·  三维绘制：%3 次，平均 %4 ms / 最长 %5 ms\n私有提交：%6 MiB  ·  工作集：%7 MiB  ·  进程提交峰值：%8 MiB  ·  工作集峰值：%9 MiB")
        .arg(elapsed).arg(first.isEmpty() ? "等待绘制" : first["requestToRenderReturnMs"].toString() + " ms（从加载请求起）")
        .arg(render["count"].toDouble(), 0, 'f', 0).arg(render["meanMs"].toDouble(), 0, 'f', 2).arg(render["maxMs"].toDouble(), 0, 'f', 2)
        .arg(mib("privateBytes")).arg(mib("workingSetBytes")).arg(mib("peakCommitBytes")).arg(mib("peakWorkingSetBytes"));
    if (m_measurements->text() != text) m_measurements->setText(text);
}
void TestWindow::UpdateInputIdentity()
{
    const auto session=m_runtime.GetSession();
    const auto input=session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{};
    m_sourceButton->setEnabled(input.has_value());
    if(!input){m_inputIdentity->setText("当前输入 · 尚未加载体数据");m_inputGeometry->clear();m_messageData->setText("数据 · 尚未加载体数据");m_inputSummary.clear();return;}
    const auto ref=GetRefText(input->dataRevision);
    QString kind="原始体数据", domain="源图像有效域"; QStringList parents;
    const auto graph=m_workflow.getPublishedGraph ? m_workflow.getPublishedGraph() : QJsonObject{};
    for(const auto value:graph["nodes"].toArray()) {
        const auto node=value.toObject(); if(node["ref"]!=ref) continue;
        const auto producer=node["producer"].toString();
        if(producer.contains("crop",Qt::CaseInsensitive)){kind="裁剪结果";domain="裁剪保留区 ∩ 源有效域；网格可保持原尺寸";}
        else if(producer.contains("artifact",Qt::CaseInsensitive)){kind="校正结果";domain="校正体的有效域";}
        for(const auto parent:node["parents"].toArray()) parents.append(parent.toString());
    }
    const auto title=QString("当前输入 · %1 · %2 · r%3 · %4")
        .arg(kind,QString::fromStdString(input->metadata.identity.datasetId),QString::number(input->dataRevision.generation),ref.section(':',0,0).right(16));
    m_inputIdentity->setText(title);
    m_inputIdentity->setToolTip("实际输入修订："+ref+"\n来源："+parents.join("\n")+"\n"+QString::fromStdString(input->metadata.source.uri));
    const auto bytes=input->metadata.source.byteSize;
    const auto geometry=QString("网格 %1 × %2 × %3 · 间距 %4 × %5 × %6 mm · 原始灰度 · 有效域：%7")
        .arg(input->dims[0]).arg(input->dims[1]).arg(input->dims[2]).arg(input->spacing[0]).arg(input->spacing[1]).arg(input->spacing[2]).arg(domain);
    m_inputGeometry->setText(geometry+QString(" · 数据 %1 字节 · 输入绑定版本 %2").arg(bytes).arg(input->bindingRevision));
    const auto summary=QString("数据 · %1 · %2 · %3 × %4 × %5 · %6 GiB · 间距 %7 × %8 × %9 mm")
        .arg(kind,QString::fromStdString(input->metadata.identity.datasetId)).arg(input->dims[0]).arg(input->dims[1]).arg(input->dims[2]).arg(bytes/(1024.*1024.*1024.),0,'f',2).arg(input->spacing[0]).arg(input->spacing[1]).arg(input->spacing[2]);
    m_messageData->setText(summary);m_messageData->setToolTip(m_inputIdentity->toolTip()+"\n"+m_inputGeometry->text());
    const auto identity=ref+":"+QString::number(input->bindingRevision);
    if(identity!=m_inputSummary){AppendLog("输入切换 · "+title+" · "+m_inputGeometry->text()+" · 完整修订 "+ref);m_inputSummary=identity;}
}
void TestWindow::QueueObserve()
{
    if (m_observeQueued || m_isClosing) return;
    m_observeQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_observeQueued = false;
        if (m_isClosing) return;
        UpdateStopCommand();
        UpdateInputIdentity();
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
    const auto text = QLocale::c().toString(QDateTime::currentDateTime(), "[HH:mm:ss.zzz] ") + message;
    m_log->appendPlainText(text);
}
void TestWindow::ShowMessage(const QString& message)
{
    m_latestMessage->setText("消息 · "+message.simplified());m_latestMessage->setToolTip(message);
}
TestWindow::~TestWindow()
{
    // 正常路径只在 Stop 成功后进入析构；页面先于运行记录/工作流成员销毁。
    m_isClosing = true;
    const QSignalBlocker pagesBlocker(m_pages), workspaceBlocker(m_workspaceTabs), featureBlocker(m_featureTabs);
    for (auto* module : m_modules) delete module;
    m_modules.clear();
}
void TestWindow::BuildSession()
{
    if (m_isReady || m_isClosing) return;
    try {
        HostSessionConfig config;
        const HostRenderViewRole roles[]{HostRenderViewRole::Primary3D, HostRenderViewRole::TopDownSlice, HostRenderViewRole::FrontBackSlice, HostRenderViewRole::LeftRightSlice};
        const HostRenderMode modes[]{HostRenderMode::CompositeIsoSurface, HostRenderMode::SliceTopDown, HostRenderMode::SliceFrontBack, HostRenderMode::SliceLeftRight};
        for (std::size_t index = 0; index < m_views.size(); ++index) {
            const auto& target = m_views[index];
            HostRenderViewConfig view; view.id = target.id.toStdString(); view.role = roles[index];
            view.renderWindow = target.window; view.window.viewInit.viewMode = modes[index];
            view.window.isAxesVisible = index == 0;
            config.renderViews.push_back(std::move(view));
        }
        m_pump.SetConfig(config);
        if (!m_runtime.BuildSession(std::move(config))) throw std::runtime_error("测试会话创建失败");
        m_pump.SetSession(m_runtime.GetSession());
        m_modules = BuildModules({m_runtime, m_workflow, m_records}, m_pages);
        if (m_workflow.resources.hasExplicitWorkingLimit)
            AppendLog(QString("显式工作集上限 %1 GiB；伪影单次发布上限 %2 GiB。").arg(m_workflow.resources.workingBytes / (1024.*1024.*1024.), 0, 'f', 2).arg(m_workflow.resources.publishBytes / (1024.*1024.*1024.), 0, 'f', 2));
        else
            AppendLog("未设置算法工作集硬上限；内存快照仅用于诊断和可选缓存保留。");
        for (auto* page : m_modules) {
            m_browsers->addWidget(page->GetBrowser()); m_pages->addWidget(page);
            m_statisticsPages->addWidget(page->GetStatisticsWidget());
            page->GetStatisticsWidget()->SetCompact(true);
            page->onMessage = [this](const QString& message) { AppendLog(message);ShowMessage(message); };
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
                if (page->GetName() == "Part" && patch["target"].isObject()) {
                    page->SelectPartTarget(patch["target"].toObject());
                    page->SelectAction(action);
                }
            }
            catch (const std::exception& error) { AppendLog("切换步骤失败：" + QString::fromUtf8(error.what())); }
        };
        m_workflow.getActionAvailable = [this](const QString& module, const QString& action) {
            const auto* page = GetModule(module); return page && page->GetActions().contains(action);
        };
        m_workflow.onShowStatistics=[this](const QString& module) {
            if(auto* page=GetModule(module)){m_pages->setCurrentWidget(page);m_showStatistics->setChecked(true);page->RefreshStatistics();}
        };
        m_workflow.ensureCropViews=[this] {
            auto* choice=findChild<QComboBox*>("viewLayout");if(choice && choice->currentData()=="ThreeDOnly") {
                choice->setCurrentIndex(choice->findData("FourViews"));AppendLog("裁剪需三维与三个切片同步完成预览，已恢复四视图布局。");
            }
        };
        m_isReady = true; m_openVolume->setEnabled(true); SyncNavigation();
        if (auto* data = GetModule("Data")) data->SelectAction("Load");
        QStringList enabled;
        for (auto* page : m_modules) if (!page->GetActions().isEmpty()) enabled.append(page->GetDisplayName());
        AppendLog("业务入口：" + enabled.join("、"));
        m_pump.SendUpdates();
    } catch (const std::exception& error) {
        m_failure = QString::fromUtf8(error.what());AppendLog("初始化失败：" + m_failure);ShowMessage("初始化失败："+m_failure);
    }
}
ModulePanel* TestWindow::GetModule(const QString& name) const
{
    // 保留已有自动化用例的模块标识；界面与新记录统一使用零件业务页。
    const auto resolved = name == "PartEdit" ? QString("Part") : name;
    for (auto* module : m_modules) if (module->GetName() == resolved) return module;
    return nullptr;
}
void TestWindow::SetViewsVisible(bool visible) { m_viewArea->setVisible(visible);m_pump.SetVisible();for(auto* page:m_modules)if(!page->GetStatisticsWidget()->isVisible())page->GetStatisticsWidget()->Cancel();if(visible && m_isReady)static_cast<ModulePanel*>(m_pages->currentWidget())->RefreshStatistics(); }
bool TestWindow::StopSession()
{
    if (!m_isClosing) AppendLog("正在关闭测试会话，停止计算并释放视图资源。");
    m_isClosing = true; m_openVolume->setEnabled(false); m_stop->setEnabled(false); m_workflow.Stop(); m_pump.Stop();
    for(auto* module:m_modules)module->GetStatisticsWidget()->Cancel();
    const auto session = m_runtime.GetSession();
    const auto current = GetDescriptor(session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{});
    if (!m_runtime.Stop()) { ShowMessage("正在结束当前任务，请稍候再关闭。"); return false; }
    m_records.StopPending(current);
    for (auto& view : m_views) view.widget->setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
    if (m_isReady) AppendLog("测试会话已停止。");
    m_isReady = false; m_renderMode->setEnabled(false);
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
    // 原生鼠标/键盘仍仅走 QVTK；失焦只发送 SDK 生命周期取消。
    if (m_isReady && !m_isClosing && (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide)) {
        const auto session = m_runtime.GetSession();
        const auto endpoint = session ? session->GetInputEndpoint() : nullptr;
        if (endpoint) for (const auto& view : m_views) if (view.widget == watched) {
            HostInputEvent cancel; cancel.viewId = view.id.toStdString(); cancel.kind = HostInputKind::Cancel;
            (void)endpoint->SendInput(cancel);
            break;
        }
    }
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
    SetInputFile(urls.first().toLocalFile()); event->acceptProposedAction();
}
void TestWindow::SetInputFile(const QString& path)
{
    auto* panel = GetModule("Data"); if (!m_isReady || m_isClosing || !panel) return;
    const QFileInfo file(path);
    if (!file.isFile()) { AppendLog("请选择存在的体数据文件。");ShowMessage("请选择存在的体数据文件。"); return; }
    try {
        m_pages->setCurrentWidget(panel);
        panel->SetParameterPatch("Load", {{"filePath", file.absoluteFilePath()}});
        AppendLog("已选择体数据：" + file.absoluteFilePath() + "。请核对尺寸、间距等参数后点击“加载体数据”。");ShowMessage("已选择体数据，请核对尺寸与间距后加载。");
    } catch (const std::exception& error) { AppendLog("选择文件失败：" + QString::fromUtf8(error.what()));ShowMessage("选择文件失败："+QString::fromUtf8(error.what())); }
}
}
