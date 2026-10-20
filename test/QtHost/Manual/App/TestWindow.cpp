// 测试用途：左侧对象、中间三维视窗、右侧功能参数，统一管理会话生命周期。
#include "TestWindow.h"
#include <limits>
#include "FeatureSetup.h"
#include "Support/JsonInput.h"
#include "Support/UiText.h"
#include "Support/ResultReport.h"
#include "Support/WorkbenchStyle.h"
#include "Support/ObjectBrowser.h"
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
#include <QMouseEvent>
#include <QStyle>
#include <QSettings>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QRegularExpression>
#include <algorithm>
#include <climits>
namespace Manual {
TestWindow::TestWindow(std::uint64_t budgetMiB)
{
    m_workflow.resources = GetTestResources(budgetMiB);
    setWindowTitle("MVVCVTK · 检测工作台");
    setAcceptDrops(true);
    auto bodyFont = QApplication::font();
    bodyFont.setPointSizeF(std::max(12.0, bodyFont.pointSizeF())); setFont(bodyFont);
    setStyleSheet(GetWorkbenchStyle());
    // 默认尺寸按屏幕工作区确定；用户可调整，业务切换不自动改变外窗。
    const auto screen=QApplication::primaryScreen();
    const auto available=screen ? screen->availableGeometry().size()-QSize(32,64) : QSize(1560,920);
    const auto defaultSize=QSize(1560,920).boundedTo(available);
    setMinimumSize(QSize(1000,720).boundedTo(defaultSize));
    resize(defaultSize);
    auto* root = new QWidget(this); root->setObjectName("workbench"); auto* layout = new QVBoxLayout(root); setCentralWidget(root);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    this->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(8,8,8,8); layout->setSpacing(6);
    auto* toolbarFrame = new QWidget(root); toolbarFrame->setObjectName("workbenchToolbar");
    auto* toolbar = new QHBoxLayout(toolbarFrame); toolbar->setContentsMargins(12,8,12,8); toolbar->setSpacing(8);
    auto* workbenchTitle = new QLabel("检测工作台", toolbarFrame); workbenchTitle->setObjectName("workbenchTitle");
    auto titleFont = font(); titleFont.setPointSizeF(titleFont.pointSizeF() * 1.125); titleFont.setWeight(QFont::DemiBold); workbenchTitle->setFont(titleFont);
    m_operationState = new QLabel("初始化中", toolbarFrame); m_operationState->setObjectName("operationState");
    m_operationState->setAccessibleName("会话状态：初始化中"); m_operationState->setToolTip("显示会话是否正在处理请求；操作结果与原因显示在对应模块内。");
    m_openVolume = new QPushButton("打开体数据…", root); m_openVolume->setObjectName("openVolume");
    m_openVolume->setProperty("primaryCommand", true);
    m_openVolume->setShortcut(QKeySequence::Open); m_openVolume->setEnabled(false);
    m_openVolume->setToolTip("选择输入文件，核对尺寸、间距等参数后点击“加载体数据”。（Ctrl+O）");
    m_stop = new QPushButton("停止计算", root); m_stop->setObjectName("stopCalculation"); m_stop->setEnabled(false);
    auto* tools = new QToolButton(root); tools->setObjectName("testTools"); tools->setText("测试工具");
    tools->setPopupMode(QToolButton::InstantPopup); auto* toolMenu = new QMenu(tools); tools->setMenu(toolMenu);
    auto* exportRecords = toolMenu->addAction("导出测试记录"); exportRecords->setObjectName("exportTestRecords");
    auto* openLog = toolMenu->addAction("打开本次日志"); openLog->setObjectName("openSessionLog");
    connect(openLog, &QAction::triggered, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(GetLogPath())); });
    auto* showReport = new QPushButton("报告与归档", root); showReport->setObjectName("showResultReport");
    auto* performance = toolMenu->addAction("开始性能采样"); performance->setCheckable(true);
    performance->setObjectName("performanceCapture");
    m_sourceAction=new QAction("查看来源关系",this);m_sourceAction->setObjectName("showInputSources");m_sourceAction->setEnabled(false);
    m_originalInputAction=new QAction("返回原始输入",this);m_originalInputAction->setObjectName("useOriginalInput");m_originalInputAction->setEnabled(false);
    toolbar->addWidget(workbenchTitle); toolbar->addSpacing(12); toolbar->addWidget(m_openVolume); toolbar->addWidget(m_stop);
    toolbar->addStretch(); toolbar->addWidget(m_operationState); toolbar->addSpacing(8); toolbar->addWidget(showReport); toolbar->addWidget(tools); layout->addWidget(toolbarFrame);
    connect(m_originalInputAction,&QAction::triggered,this,[this]{if(!m_originalRevision.isEmpty())if(auto* page=GetModule("Data"))page->SendAction("UseData",{{"graphRevision",m_originalRevision}});});
    connect(m_sourceAction,&QAction::triggered,this,[this] {
        const auto session=GetSession();const auto input=session?session->GetImageDescriptor():std::optional<ImageDescriptor>{};
        if(input)if(auto* page=static_cast<ModulePanel*>(m_pages->currentWidget())){page->ShowSources(input->dataRevision);m_showStatistics->setChecked(true);m_resultTabs->setCurrentIndex(1);}
    });
    connect(m_openVolume, &QPushButton::clicked, this, [this] {
        auto* page = GetModule("Data"); if (!m_isReady || m_isClosing || !page) return;
        const auto currentPath = page->GetParameterEditor("Load")->GetField("filePath")->GetValue().toString();
        QFileDialog dialog(this, "打开体数据", currentPath, "体数据 (*.raw *.RAW);;所有文件 (*)");
        dialog.setObjectName("inputFileDialog"); dialog.setFileMode(QFileDialog::ExistingFile);
        if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) SetInputFile(dialog.selectedFiles().first());
    });
    m_startupFeedback=new QLabel(root);m_startupFeedback->setObjectName("moduleFeedback");m_startupFeedback->setWordWrap(true);m_startupFeedback->setTextFormat(Qt::PlainText);m_startupFeedback->hide();layout->addWidget(m_startupFeedback);
    auto* navigation = new QWidget(root); navigation->setObjectName("workbenchNavigation");
    auto* navigationLayout = new QHBoxLayout(navigation); navigationLayout->setContentsMargins(6,4,6,4); navigationLayout->setSpacing(6);
    m_objectToggle = new QToolButton(navigation); m_objectToggle->setObjectName("toggleObjectColumn");
    m_objectToggle->setCheckable(true); m_objectToggle->setChecked(true); m_objectToggle->setFocusPolicy(Qt::StrongFocus);
    m_objectToggle->setShortcut(QKeySequence("Ctrl+Shift+L")); navigationLayout->addWidget(m_objectToggle);
    m_workspaceTabs = new QComboBox(navigation); m_workspaceTabs->setObjectName("workspaceTabs"); m_workspaceTabs->setAccessibleName("工作分类");
    for (const auto& title : {"数据与视图", "处理与建模", "检测与分析"}) m_workspaceTabs->addItem(title);
    navigationLayout->addWidget(m_workspaceTabs);
    m_featureTabs = new QComboBox(navigation); m_featureTabs->setObjectName("featureTabs"); m_featureTabs->setAccessibleName("当前模块");
    m_featureTabs->setMinimumContentsLength(6); m_featureTabs->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    navigationLayout->addWidget(m_featureTabs);
    auto* inputStrip=new QWidget(navigation);inputStrip->setObjectName("workbenchInput");
    auto* inputLayout=new QHBoxLayout(inputStrip);inputLayout->setContentsMargins(6,0,0,0);inputLayout->setSpacing(6);
    m_inputIdentity=new QLabel("当前输入 · 尚未加载体数据",inputStrip);m_inputIdentity->setObjectName("currentInputIdentity");
    m_inputIdentity->setTextFormat(Qt::PlainText);m_inputIdentity->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inputLayout->addWidget(m_inputIdentity,1);
    auto* inputDetails=new QToolButton(inputStrip);inputDetails->setText("输入详情");inputDetails->setObjectName("showInputDetails");inputDetails->setPopupMode(QToolButton::InstantPopup);
    auto* inputMenu=new QMenu(inputDetails);inputDetails->setMenu(inputMenu);inputLayout->addWidget(inputDetails);
    auto* geometryAction=inputMenu->addAction("查看几何与来源");
    connect(geometryAction,&QAction::triggered,this,[this] {
        QDialog details(this);details.setWindowTitle("当前输入详情");auto* fields=new QVBoxLayout(&details);
        auto* identity=new QLabel(m_inputIdentity->text()+"\n"+m_inputIdentity->toolTip()+"\n\n"+m_inputGeometry->text(),&details);
        identity->setTextFormat(Qt::PlainText);identity->setWordWrap(true);identity->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);fields->addWidget(identity);
        auto* close=new QPushButton("关闭",&details);fields->addWidget(close);connect(close,&QPushButton::clicked,&details,&QDialog::accept);details.resize(600,280);details.exec();
    });
    inputMenu->addAction(m_sourceAction);inputMenu->addAction(m_originalInputAction);
    m_inputGeometry=new QLabel(this);m_inputGeometry->setObjectName("currentInputGeometry");m_inputGeometry->hide();
    navigationLayout->addWidget(inputStrip,1);
    m_parameterToggle = new QToolButton(navigation);m_parameterToggle->setObjectName("toggleParameterColumn");
    m_parameterToggle->setCheckable(true);m_parameterToggle->setChecked(true);m_parameterToggle->setFocusPolicy(Qt::StrongFocus);
    m_parameterToggle->setShortcut(QKeySequence("Ctrl+Shift+R"));navigationLayout->addWidget(m_parameterToggle);layout->addWidget(navigation);
    m_measurements = new QLabel(root); m_measurements->setObjectName("businessMeasurements");
    m_measurements->setWordWrap(true); m_measurements->setMinimumWidth(0);
    m_measurements->setToolTip("内存为当前测试进程的 OS 实测值，峰值从进程启动累计。加载为请求到终态；首次绘制和逐帧耗时到 Host/VTK Render 返回，不包含 GPU 完成或屏幕呈现。分项不可相加。");

    auto* split = new QSplitter(root); m_split=split;split->setObjectName("workbenchSplit"); split->setChildrenCollapsible(false); split->setHandleWidth(8); layout->addWidget(split, 1);
    auto* objectColumn = new QWidget(split); objectColumn->setObjectName("objectColumn");
    objectColumn->setMinimumWidth(190);
    auto* objectLayout = new QVBoxLayout(objectColumn); objectLayout->setContentsMargins(8,8,8,8);
    auto* objectTitle = new QLabel("对象与结果", objectColumn); objectTitle->setObjectName("columnTitle"); objectLayout->addWidget(objectTitle);
    m_objects=new ObjectBrowser(objectColumn);objectLayout->addWidget(m_objects,1);
    m_objects->onAction=[this](const QString& module,const QString& action,const QJsonObject& patch) {
        auto* page=GetModule(module);if(!page || !page->GetActions().contains(action)){ShowMessage("当前构建未启用所选操作。");return;}
        auto values=patch;const auto objectId=values.take("$objectId").toString();
        if(module!="Data" && module!="Roi" && !objectId.isEmpty()) {
            page->Observe(false);const auto state=page->GetObservedState();
            const auto ref=objectId.section(':',1);
            const auto expected=module=="Surface"?objectId.startsWith("surface-preview:")?state["previewRequestId"]:state["mesh"]:
                module=="Wall"?state["result"]:module=="Gap"?state["resultSet"]:QJsonValue(QJsonValue::Undefined);
            if(!expected.isUndefined() && expected.toString()!=ref){ShowMessage("所选对象已经变化，请刷新目录后操作。");return;}
        }
        if(action=="UseData" || action=="GraphInfo" || action=="SetVolumeVisible" || (action=="Visibility" && patch.contains("isVisible"))) {
            auto params=page->GetActionParameters(action);for(auto it=values.begin();it!=values.end();++it)params[it.key()]=it.value();
            page->SendAction(action,params);
            if(action=="GraphInfo")if(auto* active=static_cast<ModulePanel*>(m_pages->currentWidget())) {
                active->ShowSources(GetRef(values["graphRevision"]));m_showStatistics->setChecked(true);m_resultTabs->setCurrentIndex(1);
            }
        } else {page->SetParameterPatch(action,values);m_pages->setCurrentWidget(page);}
    };
    m_browsers = new QStackedWidget(root); m_browsers->setObjectName("browserStack");
    m_pages = new QStackedWidget(root); m_pages->setMinimumWidth(340);
    m_pages->setObjectName("parameterPages");
    connect(m_workspaceTabs, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TestWindow::SetWorkspace);
    connect(m_featureTabs, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_isClosing || index < 0) return;
        auto* page = GetModule(m_featureTabs->itemData(index).toString());
        if (page) m_pages->setCurrentWidget(page);
    });
    connect(m_pages, &QStackedWidget::currentChanged, this, [this] { SyncNavigation(); });
    m_viewArea = new QWidget(split);m_viewArea->setObjectName("viewArea"); m_viewArea->setMinimumWidth(300); auto* viewLayout = new QVBoxLayout(m_viewArea);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    auto* viewHeading = new QHBoxLayout;
    auto* viewTitle = new QLabel("视图", m_viewArea); viewTitle->setObjectName("columnTitle"); viewHeading->addWidget(viewTitle); viewHeading->addStretch();
    auto* viewLayoutChoice = new QComboBox(m_viewArea); viewLayoutChoice->setObjectName("viewLayout");
    viewLayoutChoice->setAccessibleName("视图布局"); viewLayoutChoice->setToolTip("选择三维与切片、四视图或仅三维；分隔栏可拖动调整宽度。");
    viewLayoutChoice->addItem("三维与切片", "Inspection");viewLayoutChoice->addItem("四视图", "FourViews"); viewLayoutChoice->addItem("仅三维", "ThreeDOnly"); viewLayoutChoice->setMinimumWidth(96);
    viewHeading->addWidget(viewLayoutChoice); viewLayout->addLayout(viewHeading);
    auto* statisticsToggle=new QToolButton(m_viewArea);m_showStatistics=statisticsToggle;statisticsToggle->setObjectName("showBusinessStatistics");statisticsToggle->setText("统计与结果");statisticsToggle->setCheckable(true);viewHeading->addWidget(statisticsToggle);
    m_statisticsPages=new QStackedWidget(m_viewArea);m_statisticsPages->setObjectName("businessStatisticsPages");
    m_resultTabs=new QTabWidget(m_viewArea);m_resultTabs->setObjectName("workbenchResultTabs");
    m_resultTabs->addTab(m_statisticsPages,"统计与结果");m_resultTabs->addTab(m_browsers,"来源与历史");
    m_resultTabs->setMaximumHeight(210);
    auto* expandStatistics=new QToolButton(m_viewArea);expandStatistics->setObjectName("expandBusinessStatistics");expandStatistics->setText("放大统计");viewHeading->addWidget(expandStatistics);
    connect(statisticsToggle,&QToolButton::toggled,this,[this](bool shown) {
        if(auto* page=static_cast<ModulePanel*>(m_pages->currentWidget())) {
            m_statisticsPreferences[page->GetName()]=shown;m_resultTabs->setVisible(shown);
            if(shown)page->RefreshStatistics();else page->GetStatisticsWidget()->Cancel();
        }
    });
    expandStatistics->setCheckable(true);
    connect(expandStatistics,&QToolButton::toggled,this,[this,expandStatistics](bool expanded) {
        m_showStatistics->setChecked(true);m_resultTabs->setMaximumHeight(expanded?16777215:210);
        m_resultTabs->setMinimumHeight(expanded?300:0);expandStatistics->setText(expanded?"收起放大":"放大统计");
        for(auto* module:m_modules)module->GetStatisticsWidget()->SetCompact(!expanded);
    });
    connect(viewLayoutChoice, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this,viewLayoutChoice](int index) { SetViewLayout(viewLayoutChoice->itemData(index).toString()); });
    // 四个图像区域共用同一网格和同高标题栏；窗口缩放不改变等分关系。
    auto* viewGrid = new QGridLayout; m_viewGrid = viewGrid; viewGrid->setObjectName("fourViewGrid");
    viewGrid->setContentsMargins(0,0,0,0); viewGrid->setSpacing(6);
    for (int i = 0; i < 2; ++i) { viewGrid->setRowStretch(i, 1); viewGrid->setColumnStretch(i, 1); }
    viewLayout->addLayout(viewGrid, 1);
    viewLayout->addWidget(m_resultTabs,1);m_resultTabs->hide();
    const int headerHeight = std::max(40, fontMetrics().height()+18);
    auto* primaryFrame = new QWidget(m_viewArea); auto* primaryLayout = new QVBoxLayout(primaryFrame);
    primaryLayout->setContentsMargins(0,0,0,0); primaryLayout->setSpacing(6);
    auto* primaryHeader = new QWidget(primaryFrame); primaryHeader->setObjectName("viewHeader"); primaryHeader->setFixedHeight(headerHeight);
    auto* viewToolbar = new QHBoxLayout(primaryHeader); viewToolbar->setContentsMargins(0,0,0,0);
    viewToolbar->addWidget(new QLabel("三维", m_viewArea)); viewToolbar->addStretch();
    m_renderMode = new QComboBox(m_viewArea); m_renderMode->setObjectName("renderMode");
    m_renderMode->addItem("等值面", "CompositeIsoSurface"); m_renderMode->addItem("体渲染", "CompositeVolume");
    m_renderMode->setEnabled(false); viewToolbar->addWidget(m_renderMode);
    auto* fit = new QPushButton("适配", m_viewArea); fit->setObjectName("fitView"); fit->setToolTip("将当前三维内容适配到视窗。"); fit->setEnabled(false); viewToolbar->addWidget(fit);
    primaryLayout->addWidget(primaryHeader);
    viewGrid->addWidget(primaryFrame, 0, 0);
    auto window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    window->SetAlphaBitPlanes(1); window->SetMultiSamples(0);
    auto* widget = new QVTKOpenGLNativeWidget(primaryFrame); widget->setObjectName("primary3D");
    widget->setMinimumSize(160, 96); widget->setRenderWindow(window); primaryLayout->addWidget(widget, 1);
    m_views.push_back({"primary-3d", widget, window, primaryFrame}); widget->installEventFilter(this);
    const QStringList sliceIds{"slice-top-down", "slice-front-back", "slice-left-right"};
    const QStringList sliceLabels{"上下切片", "前后切片", "左右切片"};
    for (int index = 0; index < 3; ++index) {
        auto* frame = new QWidget(m_viewArea); auto* frameLayout = new QVBoxLayout(frame);
        frameLayout->setContentsMargins(0,0,0,0); frameLayout->setSpacing(6);
        auto* header = new QWidget(frame); header->setObjectName("viewHeader");header->setFixedHeight(headerHeight);
        auto* sliceToolbar=new QHBoxLayout(header);sliceToolbar->setContentsMargins(0,0,0,0);sliceToolbar->addWidget(new QLabel(sliceLabels[index],header));sliceToolbar->addStretch();
        auto* sliceFit=new QPushButton("适配",header);sliceFit->setObjectName("fit_"+sliceIds[index]);sliceToolbar->addWidget(sliceFit);
        connect(sliceFit,&QPushButton::clicked,this,[this,id=sliceIds[index]]{if(m_isReady)if(auto* page=GetModule("View"))page->SendAction("Reset",{{"viewId",id}});});frameLayout->addWidget(header);
        viewGrid->addWidget(frame, (index+1)/2, (index+1)%2);
        auto sliceWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New(); sliceWindow->SetAlphaBitPlanes(1); sliceWindow->SetMultiSamples(0);
        auto* slice = new QVTKOpenGLNativeWidget(frame); slice->setObjectName(sliceIds[index]); slice->setMinimumSize(160,96);
        slice->setRenderWindow(sliceWindow); frameLayout->addWidget(slice, 1); slice->installEventFilter(this);
        m_views.push_back({sliceIds[index], slice, sliceWindow, frame});
    }
    split->addWidget(m_pages); split->setStretchFactor(0, 0); split->setStretchFactor(1, 1); split->setStretchFactor(2, 0);
    split->setSizes({240, 840, 400});
    connect(m_objectToggle,&QToolButton::toggled,this,[this](bool shown){SetColumnVisible(0,shown);});
    connect(m_parameterToggle,&QToolButton::toggled,this,[this](bool shown){SetColumnVisible(2,shown);});
    connect(split,&QSplitter::splitterMoved,this,[this]{SaveColumns();});
    QSettings preferences(QCoreApplication::applicationDirPath()+"/QtFeatureWorkbench.ini",QSettings::IniFormat);
    m_restoringColumns=true;
    m_objectWidth=std::max(190,preferences.value("layout/objectWidth",240).toInt());
    m_parameterWidth=std::max(340,preferences.value("layout/parameterWidth",400).toInt());
    m_objectToggle->setChecked(preferences.value("layout/objectsVisible",true).toBool());
    m_parameterToggle->setChecked(preferences.value("layout/parametersVisible",true).toBool());
    SetColumnVisible(0,m_objectToggle->isChecked());SetColumnVisible(2,m_parameterToggle->isChecked());m_restoringColumns=false;
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
    auto* diagnosticDetails = new QWidget(root); diagnosticDetails->setObjectName("diagnosticDetails");
    auto* diagnosticLayout = new QVBoxLayout(diagnosticDetails); diagnosticLayout->setContentsMargins(0,0,0,0);
    diagnosticLayout->addWidget(m_measurements);
    auto* logLocation=new QLabel("流程和诊断保存到程序目录，可从“测试工具 → 打开本次日志”查看。",diagnosticDetails);
    logLocation->setWordWrap(true);diagnosticLayout->addWidget(logLocation);diagnosticLayout->addStretch();m_resultTabs->addTab(diagnosticDetails,"诊断");
    diagnosticDetails->hide();
    auto* reports = new QDialog(this); reports->setObjectName("operationReports"); reports->setWindowTitle("报告与归档"); reports->resize(1180,720);
    auto* reportLayout = new QVBoxLayout(reports); reportLayout->setContentsMargins(8,8,8,8);
    m_report = new ResultReport(reports); reportLayout->addWidget(m_report);
    auto* showDiagnostics=toolMenu->addAction("会话诊断");showDiagnostics->setObjectName("showDiagnostics");
    connect(showDiagnostics,&QAction::triggered,this,[this]{m_showStatistics->setChecked(true);m_resultTabs->setCurrentIndex(2);});
    m_workflow.onShowReports=[this,reports](const QString& source,const QString& result){m_report->ShowReports(source,result);reports->show();reports->raise();reports->activateWindow();};
    m_workflow.getReportAvailable=[this](const QString& module,const QString& result){return m_report->GetReportAvailable(module,result);};
    m_workflow.onShowBusinessReport=[this,reports](const QString& module,const QString& result){m_report->ShowReports({},result,module);reports->show();reports->raise();reports->activateWindow();};
    m_objects->onReport=m_workflow.onShowReports;
    m_report->onReadReports=[this](const QJsonObject& record,const QSet<QString>& known){
        if(m_isClosing)return QVector<ReportDocument>{};
        const auto provider=m_workflow.readReports.find(record["module"].toString());
        return provider==m_workflow.readReports.end()?QVector<ReportDocument>{}:provider->second(record,known);
    };
    m_report->onPrepareReports=[this](const QJsonObject& record,const QSet<QString>& known)->ReportPreparation {
        if(m_isClosing)return {};const auto provider=m_workflow.prepareReports.find(record["module"].toString());
        return provider==m_workflow.prepareReports.end()?ReportPreparation{}:provider->second(record,known);
    };
    m_report->getSources=[this]{QMap<QString,QString> sources;if(m_workflow.getPublishedGraph)
        for(const auto value:m_workflow.getPublishedGraph()["nodes"].toArray()) {const auto node=value.toObject();sources[node["ref"].toString()]=node["type"].toString();}return sources;};
    m_report->onLocate=[this](const ReportDocument& doc,ReportRowId row){
        const auto session=GetSession();const auto input=session?session->GetImageDescriptor():std::optional<ImageDescriptor>{};
        if(!input || GetRefText(input->dataRevision)!=doc.source)throw std::runtime_error("报告来源与当前输入不同，请明确选择正确数据后再定位。");
        if(doc.module=="Surface" && doc.id.startsWith("surface:")) {
            const auto object=GetId(GetReportCell(doc.table->Read(row),doc.identityColumn));
            if(object>std::numeric_limits<std::uint32_t>::max() || !m_workflow.highlightSurfaceObject
                || !m_workflow.highlightSurfaceObject(GetRef(doc.id.mid(8)),static_cast<std::uint32_t>(object)))throw std::runtime_error("视图未接纳指定历史对象的高亮请求");
            return;
        }
        const auto point=m_workflow.getRecordLocation(GetRef(doc.id.mid(4)),static_cast<std::size_t>(row),input->dataRevision);
        HostSessionSetRequest request;request.cursor=HostCursorParams{point,-1};
        const auto admission=session->SendRequest(std::move(request));if(admission.status!=HostAdmissionStatus::Accepted)throw std::runtime_error("视图未接纳定位请求");
    };
    connect(showReport,&QPushButton::clicked,this,[this]{m_workflow.onShowReports({},{});});
    m_records.onChanged = [this](const QJsonObject& record) {
        AppendLog(GetFlowText(record),GetModuleText(record["module"].toString()));
        const auto status = record["status"].toString();
        const auto tone = status=="Failed" || status=="Rejected" || status=="InvalidInput" ? "error"
            : status=="Cancelled" || status=="SucceededWithDisplayFailure" ? "warning" : !record["isTerminal"].toBool() ? "busy" : "neutral";
        if(!record["isTerminal"].toBool())m_lastOutcome.clear();
        else if(QString(tone)=="error" || QString(tone)=="warning")
            m_lastOutcome=GetModuleText(record["module"].toString())+" · "+(status=="Cancelled"?"已取消":status=="SucceededWithDisplayFailure"?"显示准备失败":status=="Rejected"?"操作被拒绝":status=="InvalidInput"?"参数无效":"操作失败");
        m_feedbackText=GetFlowText(record,false);
        if(auto* page=GetModule(record["module"].toString()))page->SetFeedback(m_feedbackText,tone);
        else ShowMessage(m_feedbackText,tone);
        m_report->SetRecord(record);
        if(record["isTerminal"].toBool() && QStringList{"Succeeded","SucceededWithDisplayFailure","FullyDetermined","Observed","Published"}.contains(record["status"].toString())) {
            const auto found=m_workflow.readReports.find(record["module"].toString());
            if(!m_isClosing && found!=m_workflow.readReports.end())m_report->QueueReport(record);
        }
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
    QWidget::setTabOrder(m_openVolume,m_stop); QWidget::setTabOrder(m_stop,showReport);
    QWidget::setTabOrder(showReport,tools); QWidget::setTabOrder(tools,m_objectToggle);
    QWidget::setTabOrder(m_objectToggle,m_workspaceTabs);QWidget::setTabOrder(m_workspaceTabs,m_featureTabs);
    QWidget::setTabOrder(m_featureTabs,inputDetails);QWidget::setTabOrder(inputDetails,m_parameterToggle);
    QMetaObject::invokeMethod(this, [this] { BuildSession(); }, Qt::QueuedConnection);
    UpdateMeasurements();
}
void TestWindow::SetWorkspace(int index)
{
    if (m_isClosing || index < 0 || index >= m_workspaceSelections.size()) return;
    const QStringList groups[]{ {"Data","View","Rotation"},{"Roi","Crop","Artifact","Part","Surface"},{"Alignment","Wall","Gap"} };
    auto* page=GetModule(m_workspaceSelections[index]);
    if(!page || page->GetActions().isEmpty())for(const auto& id:groups[index])if(auto* available=GetModule(id))if(!available->GetActions().isEmpty()){page=available;break;}
    if(page && !page->GetActions().isEmpty())m_pages->setCurrentWidget(page);
}
void TestWindow::SyncNavigation()
{
    if (m_isClosing || m_modules.empty()) return;
    auto* page = static_cast<ModulePanel*>(m_pages->currentWidget()); if (!page) return;
    const std::vector<QStringList> groups{{"Data", "View", "Rotation"}, {"Roi", "Crop", "Artifact", "Part", "Surface"}, {"Alignment", "Wall", "Gap"}};
    for(int i=0;i<static_cast<int>(groups.size());++i) {
        const bool enabled=std::any_of(groups[i].cbegin(),groups[i].cend(),[this](const QString& id){const auto* module=GetModule(id);return module && !module->GetActions().isEmpty();});
        m_workspaceTabs->setItemData(i,enabled?QVariant():QVariant(0),Qt::UserRole-1);
    }
    int workspace = -1;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) if (groups[i].contains(page->GetName())) workspace = i;
    if (workspace < 0) return;
    const QSignalBlocker workspaceBlocker(m_workspaceTabs), featureBlocker(m_featureTabs);
    m_workspaceTabs->setCurrentIndex(workspace); m_workspaceSelections[workspace] = page->GetName();
    QStringList current;
    for (int i = 0; i < m_featureTabs->count(); ++i) current.append(m_featureTabs->itemData(i).toString());
    QStringList expected;
    for(const auto& id:groups[workspace])if(auto* module=GetModule(id))if(!module->GetActions().isEmpty())expected.append(id);
    if (current.size() != expected.size() || !std::equal(current.cbegin(), current.cend(), expected.cbegin())) {
        m_featureTabs->clear();
        for (const auto& id : expected) {
            m_featureTabs->addItem(GetModule(id)->GetDisplayName(),id);
        }
    }
    m_featureTabs->setCurrentIndex(expected.indexOf(page->GetName()));
    m_browsers->setCurrentWidget(page->GetBrowser());
    m_statisticsPages->setCurrentWidget(page->GetStatisticsWidget());
    for(auto* module:m_modules)if(module!=page)module->GetStatisticsWidget()->Cancel();
    const bool statistics=m_statisticsPreferences.value(page->GetName(),height()>=900 && GetSession()->GetImageDescriptor().has_value() && page->GetName()!="Rotation" && !page->GetActions().isEmpty());
    {const QSignalBlocker blocker(m_showStatistics);m_showStatistics->setChecked(statistics);}m_resultTabs->setVisible(statistics);
    if(!statistics)page->GetStatisticsWidget()->Cancel();
    if(page->GetName()=="Crop" && m_workflow.ensureCropViews)m_workflow.ensureCropViews();
    if (m_isReady) {
        page->Observe(); AppendLog("切换功能：" + page->GetDisplayName());
        if (!page->GetNotice().isEmpty()) AppendLog(page->GetNotice());
        RefreshObjects();
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
    const auto state = m_isClosing ? QString("正在结束会话") : !m_isReady ? QString("初始化中")
        : module && !m_records.GetRecord(id)["isTerminal"].toBool() ? QString("处理中 · ")+module->GetDisplayName() : m_lastOutcome.isEmpty()?QString("等待操作"):m_lastOutcome;
    if(m_operationState->text()!=state)m_operationState->setText(state);
    m_operationState->setAccessibleName("会话状态："+state);
    m_stop->setEnabled(available);
    m_stop->setText(module && module->GetName()=="Crop" ? "返回源并结束裁剪" : "停止计算");
    m_stop->setToolTip(available ? module->stopDescription.isEmpty()?QString("请求停止当前计算；对应模块将报告实际结果。"):module->stopDescription : id ? "当前计算没有独立取消入口，请等待完成。" : "当前没有正在执行的计算。");
}
void TestWindow::UpdateMeasurements()
{
    UpdateStopCommand();
    // 当前业务页选择图例；视口不承载业务按钮，也不让多个图例互相覆盖。
    auto* color=dynamic_cast<ModulePanel*>(m_pages->currentWidget());
    if(color&&color->getColorParameters){
        const auto parameters=color->getColorParameters();
        if(!parameters.isEmpty()&&(m_colorModule!=color->GetName()||m_colorParameters!=parameters)){
            m_colorModule=color->GetName();m_colorParameters=parameters;
            for(auto* other:m_modules)if(other->setViewLegend)for(const auto& view:m_views){
                if(other->GetName()=="Alignment"&&view.id!="primary-3d")continue;
                other->setViewLegend(view.id,other==color&&parameters["hasLegend"].toBool()&&parameters["isVisible"].toBool(),-1);
            }
            if(parameters["isVisible"].toBool()&&color->getColorSurface){
                const auto surface=color->getColorSurface();
                if(!surface.isEmpty())for(auto* other:m_modules)if(other!=color&&other->getColorSurface&&other->setColorVisible&&other->getColorSurface()==surface)other->setColorVisible(false);
            }
        }
    }
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
    m_sourceAction->setEnabled(input.has_value());
    if(!input){m_inputIdentity->setText("当前输入 · 尚未加载体数据");m_inputGeometry->clear();m_inputSummary.clear();m_originalRevision.clear();m_originalInputAction->setEnabled(false);return;}
    const auto ref=GetRefText(input->dataRevision);
    QString kind="原始体数据", domain="源图像有效域"; QStringList parents;
    const auto graph=m_workflow.getPublishedGraph ? m_workflow.getPublishedGraph() : QJsonObject{};
    QMap<QString,QJsonObject> volumes;for(const auto value:graph["nodes"].toArray())if(value.toObject()["isVolume"].toBool())volumes[value.toObject()["ref"].toString()]=value.toObject();
    QSet<QString> visited,originals;QStringList pending{ref};
    for(int i=0;i<pending.size();++i) {
        const auto revision=pending[i];if(visited.contains(revision) || !volumes.contains(revision))continue;visited.insert(revision);
        const auto node=volumes.value(revision);bool hasSource=false;
        for(const auto parent:node["parents"].toArray())if(volumes.contains(parent.toString())){hasSource=true;pending.append(parent.toString());}
        if(!hasSource)originals.insert(revision);
    }
    m_originalRevision=originals.size()==1?*originals.cbegin():QString();m_originalInputAction->setEnabled(!m_isClosing && !m_originalRevision.isEmpty() && m_originalRevision!=ref);
    m_originalInputAction->setToolTip("只切换工作输入，保留正式结果与分支历史。\n"+m_originalRevision);
    for(const auto value:graph["nodes"].toArray()) {
        const auto node=value.toObject(); if(node["ref"]!=ref) continue;
        const auto producer=node["producer"].toString();
        if(producer.contains("crop",Qt::CaseInsensitive)){kind="裁剪结果";domain="裁剪保留区 ∩ 源有效域；网格可保持原尺寸";}
        else if(producer.contains("artifact",Qt::CaseInsensitive)){kind="校正结果";domain="校正体的有效域";}
        for(const auto parent:node["parents"].toArray()) parents.append(parent.toString());
    }
    const auto title=QString("当前输入 · %1 · %2 · r%3 · %4")
        .arg(kind,QString::fromStdString(input->metadata.identity.datasetId),QString::number(input->dataRevision.generation),ref.section(':',0,0).right(16));
    m_inputIdentity->setText(QString("%1 · %2 · r%3").arg(kind,QString::fromStdString(input->metadata.identity.datasetId),QString::number(input->dataRevision.generation)));
    m_inputIdentity->setToolTip("实际输入修订："+ref+"\n来源："+parents.join("\n")+"\n"+QString::fromStdString(input->metadata.source.uri));
    const auto bytes=input->metadata.source.byteSize;
    const auto geometry=QString("网格 %1 × %2 × %3 · 间距 %4 × %5 × %6 mm · RAS · %8 · 灰度单位 %9 · 有效域：%7")
        .arg(input->dims[0]).arg(input->dims[1]).arg(input->dims[2]).arg(input->spacing[0]).arg(input->spacing[1]).arg(input->spacing[2]).arg(domain)
        .arg(input->valueType==ImageValueType::Float32?QString("Float32"):QString("%1 字节标量").arg(input->componentBytes)).arg(QString::fromStdString(input->metadata.scalar.unit));
    m_inputGeometry->setText(geometry+QString(" · 数据 %1 字节 · 输入绑定版本 %2").arg(bytes).arg(input->bindingRevision));
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
        RefreshObjects();
    }, Qt::QueuedConnection);
}
void TestWindow::RefreshObjects()
{
    if(!m_isReady || m_isClosing || !m_objects)return;
    QJsonArray regions,nodes;std::vector<QJsonArray> projections;
    const auto session=GetSession();const auto input=session->GetImageDescriptor();
    for(const auto& roi:session->GetRoiDescriptors(true))regions.append(QJsonObject{
        {"roi",GetRefText(roi.revision)},{"name",QString::fromStdString(roi.metadata.name)},
        {"group",QString::fromStdString(roi.metadata.group)},{"description",QString::fromStdString(roi.metadata.description)},
        {"isArchived",roi.metadata.isArchived},{"source",GetRefText(roi.definition.source)}});
    const auto current=input?GetRefText(input->dataRevision):QString();
    const auto presentation=session->GetRenderViewState({"primary-3d"});
    const auto opacity=input && presentation && presentation->dataRevision==input->dataRevision?std::optional<double>(presentation->material.opacity):std::optional<double>{};
    const auto graph=m_workflow.getPublishedGraph?m_workflow.getPublishedGraph():QJsonObject{};
    for(auto* module:m_modules)projections.push_back(module->GetObjectNodes());
    if(graph==m_objectGraph && regions==m_objectRegions && current==m_objectInput && projections==m_objectModules && opacity==m_objectOpacity)return;
    m_objectGraph=graph;m_objectRegions=regions;m_objectInput=current;m_objectModules=projections;m_objectOpacity=opacity;
    const std::function<void(const QJsonArray&,const QString&)> append=[&](const QJsonArray& values,const QString& module) {
        for(const auto value:values){auto node=value.toObject();node["module"]=module;nodes.append(node);append(node["children"].toArray(),module);}
    };
    for(std::size_t i=0;i<m_modules.size();++i)append(projections[i],m_modules[i]->GetName());
    m_objects->SetObjects(graph,regions,nodes,current,GetModule("Roi")?GetModule("Roi")->GetActions():QStringList{},opacity);
}
void TestWindow::AppendLog(const QString& message,const QString& module)
{
    if(m_log.fileName().isEmpty()) {
        QDir directory(QCoreApplication::applicationDirPath());
        // 只轮转本工作台的日志，保留最近八个会话/分段，不触及其他程序文件。
        const auto previous=directory.entryInfoList({"QtFeatureWorkbench-*.txt"},QDir::Files,QDir::Time);
        for(int i=7;i<previous.size();++i)QFile::remove(previous[i].absoluteFilePath());
        m_log.setFileName(directory.filePath("QtFeatureWorkbench-"+QLocale::c().toString(QDateTime::currentDateTime(),"yyyyMMdd-HHmmss-zzz")+"-"+QString::number(QCoreApplication::applicationPid())+".txt"));
    }
    const auto logBytes=m_log.isOpen()?m_log.size():QFileInfo(m_log.fileName()).size();
    if(logBytes>=4*1024*1024) {
        m_log.close();const auto previous=m_log.fileName()+".previous.txt";
        if((QFile::exists(previous) && !QFile::remove(previous)) || !QFile::rename(m_log.fileName(),previous)) {
            ShowMessage("日志轮转失败，当前日志已保留；请检查程序目录权限或文件占用。","error");return;
        }
    }
    if(!m_log.isOpen() && !m_log.open(QIODevice::WriteOnly|QIODevice::Append)) {
        m_logFailure="无法写入会话日志："+m_log.errorString();ShowMessage(m_logFailure,"error");return;
    }
    const auto timestamp=QLocale::c().toString(QDateTime::currentDateTime(),"[yyyy-MM-dd HH:mm:ss.zzz] ");
    const auto lines=message.split(QRegularExpression("[\\r\\n]+"),QString::SkipEmptyParts);
    for(const auto& line:lines) {
        const auto bytes=(timestamp+"["+module.simplified()+"] "+line+"\n").toUtf8();
        if(m_log.write(bytes)!=bytes.size()){m_logFailure="日志写入失败："+m_log.errorString();ShowMessage(m_logFailure,"error");return;}
    }
    if(!m_log.flush()){m_logFailure="日志保存失败："+m_log.errorString();ShowMessage(m_logFailure,"error");}
}
void TestWindow::ShowMessage(const QString& message, const char* tone)
{
    m_feedbackText=message;
    if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget()))page->SetFeedback(message,tone);
    else if(QString(tone)!="neutral"){m_startupFeedback->setText(message);m_startupFeedback->setProperty("feedbackTone",tone);m_startupFeedback->show();}
}
QString TestWindow::GetFeedback() const { return m_feedbackText; }
void TestWindow::SetColumnVisible(int column,bool visible)
{
    auto* panel=m_split->widget(column);auto* toggle=column==0?m_objectToggle:m_parameterToggle;
    const auto sizes=m_split->sizes();
    if(!m_restoringColumns && !panel->isHidden() && !visible) {
        if(column==0)m_objectWidth=sizes[0];else m_parameterWidth=sizes[2];
    }
    // 收起前先把焦点交给始终可见的展开按钮，避免藏在不可见的参数里。
    if(!visible)toggle->setFocus(Qt::ShortcutFocusReason);
    panel->setVisible(visible);
    toggle->setText(column==0?(visible?"对象 ◀":"对象 ▶"):(visible?"参数 ▶":"参数 ◀"));
    toggle->setAccessibleName((visible?QString("收起"):QString("展开"))+(column==0?"对象与结果":"模块参数"));
    toggle->setToolTip(toggle->accessibleName()+(column==0?"（Ctrl+Shift+L）":"（Ctrl+Shift+R）"));
    const int total=m_split->width()-2*m_split->handleWidth();
    const int left=m_objectToggle->isChecked()?m_objectWidth:0,right=m_parameterToggle->isChecked()?m_parameterWidth:0;
    m_split->setSizes({left,std::max(1,total-left-right),right});
    if(!m_restoringColumns)SaveColumns();
}
void TestWindow::SaveColumns()
{
    if(m_restoringColumns)return;
    const auto sizes=m_split->sizes();
    if(m_objectToggle->isChecked() && sizes[0]>0)m_objectWidth=sizes[0];
    if(m_parameterToggle->isChecked() && sizes[2]>0)m_parameterWidth=sizes[2];
    QSettings preferences(QCoreApplication::applicationDirPath()+"/QtFeatureWorkbench.ini",QSettings::IniFormat);
    preferences.setValue("layout/objectWidth",m_objectWidth);preferences.setValue("layout/parameterWidth",m_parameterWidth);
    preferences.setValue("layout/objectsVisible",m_objectToggle->isChecked());preferences.setValue("layout/parametersVisible",m_parameterToggle->isChecked());
    preferences.sync();
    if(preferences.status()!=QSettings::NoError && !m_settingsFailureReported){m_settingsFailureReported=true;AppendLog("布局偏好无法保存，请检查程序目录的写入权限。");ShowMessage("布局偏好无法保存，请检查程序目录的写入权限。","warning");}
}
TestWindow::~TestWindow()
{
    SaveColumns();
    // 正常路径只在 Stop 成功后进入析构；页面先于运行记录/工作流成员销毁。
    m_isClosing = true;
    m_report->CancelTasks(true);m_report->onReadReports={};m_report->getSources={};
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
            page->onMessage = [this,page](const QString& message) { AppendLog(message,page->GetDisplayName());m_feedbackText=message;page->SetFeedback(message); };
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
            if(auto* page=GetModule(module)){m_pages->setCurrentWidget(page);m_showStatistics->setChecked(true);m_resultTabs->setCurrentIndex(0);page->RefreshStatistics();}
        };
        m_workflow.ensureCropViews=[this] {
            auto* choice=findChild<QComboBox*>("viewLayout");if(choice && choice->currentData()=="ThreeDOnly") {
                choice->setCurrentIndex(choice->findData("FourViews"));AppendLog("裁剪需三维与三个切片同步完成预览，已恢复四视图布局。");
            }
        };
        m_isReady = true; m_openVolume->setEnabled(true);m_startupFeedback->hide(); SyncNavigation();
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
    m_report->CancelTasks(true);
    // 原生报告和分页目录持有正式结果，须先释放才能退休其 OwnerRetirement 作用域。
    for(const auto& id:m_report->GetReportIds())m_report->RemoveReport(id);
    for(auto* module:m_modules) {
        module->CancelReadTasks();module->GetStatisticsWidget()->Cancel();
        if(module->onSessionClosing)module->onSessionClosing();
    }
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
