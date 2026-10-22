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
#include <QLineEdit>
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
#include <QDockWidget>
#include <QToolBar>
#include <QTabBar>
#include <QMenuBar>
#include <QPushButton>
#include <QAbstractButton>
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
#include <QResizeEvent>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QIcon>
namespace Manual {
namespace {
QIcon GetFitViewIcon()
{
    QPixmap pixels(16,16);pixels.fill(Qt::transparent);QPainter painter(&pixels);painter.setPen(QPen(GetWorkbenchPlotColors().text,1.5));
    for(const auto corner:{QPoint(3,3),QPoint(12,3),QPoint(3,12),QPoint(12,12)}){const auto dx=corner.x()<8?3:-3,dy=corner.y()<8?3:-3;painter.drawLine(corner,corner+QPoint(dx,0));painter.drawLine(corner,corner+QPoint(0,dy));}
    return QIcon(pixels);
}
QIcon GetCloseDockIcon()
{
    QPixmap pixels(16,16);pixels.fill(Qt::transparent);QPainter painter(&pixels);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(GetWorkbenchPlotColors().text,1.5));
    painter.drawLine(QPointF(4,4),QPointF(12,12));painter.drawLine(QPointF(12,4),QPointF(4,12));return QIcon(pixels);
}
}
class InputIdentityLabel final : public QLabel {
public:
    explicit InputIdentityLabel(QWidget* parent) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    }
    void SetIdentity(const QString& text,const QString& details={})
    {
        if(m_fullText==text&&m_details==details)return;
        m_fullText=text;m_details=details;UpdateLineHeight();setAccessibleName(text);setAccessibleDescription(details);setToolTip(text+(details.isEmpty()?QString():"\n"+details));SetDisplayText();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QLabel::resizeEvent(event);SetDisplayText(); }
    void showEvent(QShowEvent* event) override {QLabel::showEvent(event);SetDisplayText();}
    void changeEvent(QEvent* event) override
    {
        QLabel::changeEvent(event);
        if(event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange){UpdateLineHeight();SetDisplayText();}
    }
private:
    QString m_fullText,m_details;
    int m_lineHeight=-1;
    void UpdateLineHeight(){const auto height=fontMetrics().height()+4;if(height!=m_lineHeight){m_lineHeight=height;setMinimumHeight(height);}}
    void SetDisplayText()
    {
        const auto display=fontMetrics().elidedText(m_fullText,Qt::ElideRight,contentsRect().width());
        if(text()!=display)QLabel::setText(display);
    }
};
TestWindow::TestWindow(std::uint64_t budgetMiB)
{
    m_workflow.resources = GetTestResources(budgetMiB);
    setWindowTitle("MVVCVTK · 检测工作台");
    setAcceptDrops(true);
    // 默认尺寸按屏幕工作区确定；用户可调整，业务切换不自动改变外窗。
    const auto screen=QApplication::primaryScreen();
    const auto available=screen ? screen->availableGeometry().size()-QSize(32,64) : QSize(1560,920);
    const auto defaultSize=QSize(1560,920).boundedTo(available);
    setMinimumSize(QSize(1000,720).boundedTo(defaultSize));
    resize(defaultSize);
    // 所有内容都由同一个主窗口停靠布局管理；空中央区不抢占视窗空间。
    auto* root = new QWidget(this); root->setObjectName("workbench");
    root->setMaximumSize(0,0); setCentralWidget(root); root->hide();
    setDockOptions(QMainWindow::AllowNestedDocks|QMainWindow::AllowTabbedDocks|QMainWindow::AnimatedDocks);
    setAnimated(false); setTabPosition(Qt::AllDockWidgetAreas,QTabWidget::North);
    setCorner(Qt::BottomLeftCorner,Qt::BottomDockWidgetArea);setCorner(Qt::BottomRightCorner,Qt::RightDockWidgetArea);
    m_layoutTimer.setSingleShot(true);m_layoutTimer.setInterval(120);
    connect(&m_layoutTimer,&QTimer::timeout,this,&TestWindow::SaveLayout);
    auto* toolbar = addToolBar("快速操作");toolbar->setObjectName("quickToolbar");toolbar->setFloatable(true);
    auto* workbenchTitle = new QLabel("工作台",toolbar);workbenchTitle->setObjectName("workbenchTitle");toolbar->addWidget(workbenchTitle);toolbar->addSeparator();
    m_openVolume = new QPushButton("打开",toolbar);m_openVolume->setObjectName("openVolume");m_openVolume->setProperty("primaryCommand",true);
    m_openVolume->setShortcut(QKeySequence::Open);m_openVolume->setEnabled(false);toolbar->addWidget(m_openVolume);
    m_openVolume->setToolTip("选择体数据，核对尺寸和间距后导入（Ctrl+O）。");
    m_histogramAction = new QAction("直方图",this);m_histogramAction->setObjectName("showGrayHistogram");
    m_histogramAction->setShortcut(QKeySequence("Ctrl+Shift+H"));m_histogramAction->setToolTip("打开灰度直方图；拖动红线，释放后提交显示阈值（Ctrl+Shift+H）。");
    m_histogramAction->setCheckable(true);toolbar->addAction(m_histogramAction);
    connect(m_histogramAction,&QAction::triggered,this,[this](bool shown){if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget()))m_grayPreferences[page->GetName()]=shown;m_grayDock->setVisible(shown);if(shown)ShowHistogram();});
    m_showStatistics=new QToolButton(this);m_showStatistics->setObjectName("showBusinessStatistics");m_showStatistics->setText("分析结果");m_showStatistics->setCheckable(true);m_showStatistics->hide();
    m_showStatistics->setAccessibleName("显示或收起分析结果");
    auto* showReport=new QPushButton("报告",toolbar);showReport->setObjectName("showResultReport");toolbar->addWidget(showReport);
    auto* windows=new QToolButton(toolbar);windows->setText("窗口");windows->setObjectName("windowLayoutMenu");windows->setAccessibleName("窗口与布局");windows->setToolTip("窗口与布局：显示、浮动、布局预设与恢复默认");windows->setPopupMode(QToolButton::InstantPopup);
    m_windowMenu=new QMenu(windows);windows->setMenu(m_windowMenu);toolbar->addWidget(windows);
    auto* reset=m_windowMenu->addAction("恢复默认布局",this,&TestWindow::ResetLayout);reset->setObjectName("resetWorkbenchLayout");
    auto* layoutChoice=new QComboBox(this);layoutChoice->setObjectName("viewLayout");layoutChoice->setAccessibleName("视图布局预设");
    layoutChoice->addItem("四视图","FourViews");layoutChoice->addItem("三维与切片","Inspection");layoutChoice->addItem("仅三维","ThreeDOnly");layoutChoice->hide();
    for(int i=0;i<layoutChoice->count();++i){const auto mode=layoutChoice->itemData(i).toString();auto* action=m_windowMenu->addAction(layoutChoice->itemText(i),this,[this,layoutChoice,i,mode]{const QSignalBlocker blocker(layoutChoice);layoutChoice->setCurrentIndex(i);SetViewLayout(mode);});action->setObjectName("viewLayout_"+mode);}
    connect(layoutChoice,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,layoutChoice](int i){SetViewLayout(layoutChoice->itemData(i).toString());});
    auto* lock=m_windowMenu->addAction("锁定窗口位置");lock->setObjectName("lockWorkbenchLayout");lock->setCheckable(true);
    connect(lock,&QAction::toggled,this,[this](bool locked){for(auto* dock:findChildren<QDockWidget*>(QString(),Qt::FindDirectChildrenOnly))dock->setFeatures(locked?QDockWidget::DockWidgetClosable:QDockWidget::DockWidgetFeatures(QDockWidget::DockWidgetClosable|QDockWidget::DockWidgetMovable|QDockWidget::DockWidgetFloatable));for(auto* action:m_windowMenu->findChildren<QAction*>())if(action->objectName().startsWith("float_"))action->setEnabled(!locked);});
    m_windowMenu->addSeparator();
    m_stop=new QPushButton("停止",toolbar);m_stop->setObjectName("stopCalculation");m_stop->setEnabled(false);toolbar->addWidget(m_stop);
    auto* tools=new QToolButton(toolbar);tools->setObjectName("testTools");tools->setText("测试");tools->setToolTip("测试工具");tools->setPopupMode(QToolButton::InstantPopup);auto* toolMenu=new QMenu(tools);tools->setMenu(toolMenu);toolbar->addWidget(tools);
    auto* exportRecords=toolMenu->addAction("导出测试记录");exportRecords->setObjectName("exportTestRecords");
    auto* openLog=toolMenu->addAction("打开本次日志");openLog->setObjectName("openSessionLog");connect(openLog,&QAction::triggered,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(GetLogPath()));});
    auto* performance=toolMenu->addAction("开始性能采样");performance->setCheckable(true);performance->setObjectName("performanceCapture");
    m_operationState=new InputIdentityLabel(toolbar);m_operationState->setObjectName("operationState");m_operationState->setFixedWidth(76);m_operationState->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred);static_cast<InputIdentityLabel*>(m_operationState)->SetIdentity("初始化中");
    m_startupFeedback=new QLabel(this);m_startupFeedback->setObjectName("moduleFeedback");m_startupFeedback->setWordWrap(true);m_startupFeedback->hide();
    m_measurements=new QLabel(this);m_measurements->setObjectName("businessMeasurements");m_measurements->setWordWrap(true);m_measurements->setMinimumWidth(0);
    m_measurements->setToolTip("内存为当前测试进程的 OS 实测值。首次绘制和逐帧耗时到 Host/VTK Render 返回，不包含 GPU 完成或屏幕呈现；分项不可相加。");
    auto* navigation=toolbar;
    m_objectToggle=new QToolButton(this);m_objectToggle->setObjectName("toggleObjectColumn");m_objectToggle->setCheckable(true);m_objectToggle->setChecked(true);m_objectToggle->setFocusPolicy(Qt::StrongFocus);m_objectToggle->hide();
    m_steps=new QTabBar(navigation);m_steps->setObjectName("workflowSteps");m_steps->setAccessibleName("检测工作步骤");m_steps->setExpanding(false);m_steps->setDrawBase(false);m_steps->setUsesScrollButtons(false);m_steps->setElideMode(Qt::ElideNone);m_steps->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    for(const auto* text:{"数据准备","表面测定","ROI·裁剪","分析测量","结果"})m_steps->addTab(text);navigation->addWidget(m_steps);
    m_parameterToggle=new QToolButton(this);m_parameterToggle->setObjectName("toggleParameterColumn");m_parameterToggle->setCheckable(true);m_parameterToggle->setChecked(true);m_parameterToggle->setFocusPolicy(Qt::StrongFocus);m_parameterToggle->hide();
    // 原模块标识继续供请求和场景脚本定位，用户通过步骤和一层工具按钮导航。
    m_workspaceTabs=new QComboBox(this);m_workspaceTabs->setObjectName("workspaceTabs");for(int i=0;i<4;++i)m_workspaceTabs->addItem(m_steps->tabText(i));m_workspaceTabs->hide();
    m_featureTabs=new QComboBox(this);m_featureTabs->setObjectName("featureTabs");m_featureTabs->hide();
    connect(m_steps,&QTabBar::currentChanged,this,[this](int i){if(i==4){m_resultDock->show();m_resultDock->raise();if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget()))page->RefreshStatistics();}else SetWorkspace(i);});
    m_contextTools=new QToolBar("当前工具",toolbar);m_contextTools->setObjectName("contextToolbar");m_contextTools->setMovable(false);m_contextTools->setFloatable(false);m_contextToolsAction=toolbar->addWidget(m_contextTools);
    auto* identityBar=new QWidget(this);identityBar->setObjectName("inputToolbar");identityBar->hide();
    m_inputIdentity=new InputIdentityLabel(identityBar);m_inputIdentity->setObjectName("currentInputIdentity");m_inputIdentity->setMinimumWidth(180);m_inputIdentity->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);m_inputIdentity->SetIdentity("当前输入 · 尚未加载体数据");m_inputIdentity->hide();
    auto* inputDetails=new QToolButton(identityBar);inputDetails->setObjectName("showInputDetails");inputDetails->setText("输入详情");inputDetails->setPopupMode(QToolButton::InstantPopup);auto* inputMenu=new QMenu(inputDetails);inputDetails->setMenu(inputMenu);inputDetails->hide();
    m_sourceAction=new QAction("查看来源关系",this);m_sourceAction->setObjectName("showInputSources");m_sourceAction->setEnabled(false);
    m_originalInputAction=new QAction("返回原始输入",this);m_originalInputAction->setObjectName("useOriginalInput");m_originalInputAction->setEnabled(false);inputMenu->addAction(m_sourceAction);inputMenu->addAction(m_originalInputAction);
    QList<QAction*> trailingActions;
    for(auto* command:{static_cast<QWidget*>(m_stop),static_cast<QWidget*>(windows),static_cast<QWidget*>(showReport),static_cast<QWidget*>(tools)})for(auto* action:toolbar->actions())if(toolbar->widgetForAction(action)==command)trailingActions.append(action);
    for(auto* action:trailingActions)toolbar->removeAction(action);auto* flexible=new QWidget(toolbar);flexible->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);toolbar->addWidget(flexible);toolbar->addSeparator();toolbar->addWidget(m_operationState);for(auto* action:trailingActions)toolbar->addAction(action);
    inputMenu->setTitle("当前输入详情");toolMenu->addMenu(inputMenu);
    m_inputGeometry=new QLabel(this);m_inputGeometry->setObjectName("currentInputGeometry");m_inputGeometry->hide();
    inputMenu->addAction("查看几何与来源",this,[this]{QDialog details(this);details.setWindowTitle("当前输入详情");auto* fields=new QVBoxLayout(&details);auto* identity=new QLabel(m_inputIdentity->toolTip()+"\n\n"+m_inputGeometry->text(),&details);identity->setWordWrap(true);identity->setTextFormat(Qt::PlainText);identity->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);fields->addWidget(identity);auto* close=new QPushButton("关闭",&details);fields->addWidget(close);connect(close,&QPushButton::clicked,&details,&QDialog::accept);details.resize(600,280);details.exec();});
    connect(m_originalInputAction,&QAction::triggered,this,[this]{if(!m_originalRevision.isEmpty())if(auto* page=GetModule("Data"))page->SendAction("UseData",{{"graphRevision",m_originalRevision}});});
    connect(m_sourceAction,&QAction::triggered,this,[this]{const auto input=GetSession()?GetSession()->GetImageDescriptor():std::optional<ImageDescriptor>{};if(input)if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget())){page->ShowSources(input->dataRevision);m_objectDock->show();m_objectDock->raise();findChild<QComboBox*>("sceneView")->setCurrentIndex(2);}});
    connect(m_openVolume,&QPushButton::clicked,this,[this]{auto* page=GetModule("Data");if(!m_isReady||m_isClosing||!page)return;QFileDialog dialog(this,"打开体数据",page->GetParameterEditor("Load")->GetField("filePath")->GetValue().toString(),"体数据 (*.raw *.RAW);;所有文件 (*)");dialog.setObjectName("inputFileDialog");dialog.setFileMode(QFileDialog::ExistingFile);if(dialog.exec()==QDialog::Accepted&&!dialog.selectedFiles().isEmpty())SetInputFile(dialog.selectedFiles().first());});
    auto* scenePanel=new QWidget(this);auto* sceneLayout=new QVBoxLayout(scenePanel);sceneLayout->setContentsMargins(0,0,0,0);sceneLayout->setSpacing(8);
    auto* sceneHeader=new QHBoxLayout;auto* sceneView=new QComboBox(scenePanel);sceneView->setObjectName("sceneView");sceneView->setAccessibleName("场景筛选");sceneView->addItems({"场景","历史","来源"});sceneHeader->addWidget(sceneView);
    auto* sceneStack=new QStackedWidget(scenePanel);sceneStack->setObjectName("sceneContent");m_objects=new ObjectBrowser(sceneStack);sceneStack->addWidget(m_objects);
    auto* search=m_objects->findChild<QLineEdit*>("objectSearch");if(search){m_objects->layout()->removeWidget(search);sceneHeader->addWidget(search,1);search->setPlaceholderText("搜索对象");}
    sceneLayout->addLayout(sceneHeader);sceneLayout->addWidget(sceneStack,1);
    m_browsers=new QStackedWidget(sceneStack);m_browsers->setObjectName("browserStack");sceneStack->addWidget(m_browsers);
    connect(sceneView,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,sceneStack](int index){sceneStack->setCurrentIndex(index==0?0:1);if(index)if(auto* browser=qobject_cast<QTabWidget*>(m_browsers->currentWidget()))browser->setCurrentIndex(index);});
    m_objectDock=CreateDock("sceneDock","场景与对象",scenePanel);scenePanel->setMinimumWidth(220);
    m_objects->onAction=[this](const QString& module,const QString& action,const QJsonObject& patch){auto* page=GetModule(module);if(!page||!page->GetActions().contains(action)){ShowMessage("当前构建未启用所选操作。");return;}auto values=patch;const auto objectId=values.take("$objectId").toString();if(module!="Data"&&module!="Roi"&&!objectId.isEmpty()){page->Observe(false);const auto state=page->GetObservedState();const auto ref=objectId.section(':',1);const auto expected=module=="Surface"?objectId.startsWith("surface-preview:")?state["previewRequestId"]:state["mesh"]:module=="Wall"?state["result"]:module=="Gap"?state["resultSet"]:QJsonValue(QJsonValue::Undefined);if(!expected.isUndefined()&&expected.toString()!=ref){ShowMessage("所选对象已经变化，请刷新目录后操作。");return;}}if(action=="UseData"||action=="GraphInfo"||action=="SetVolumeVisible"||(action=="Visibility"&&patch.contains("isVisible"))){auto params=page->GetActionParameters(action);for(auto it=values.begin();it!=values.end();++it)params[it.key()]=it.value();page->SendAction(action,params);if(action=="GraphInfo")if(auto* active=dynamic_cast<ModulePanel*>(m_pages->currentWidget())){active->ShowSources(GetRef(values["graphRevision"]));m_objectDock->show();findChild<QComboBox*>("sceneView")->setCurrentIndex(2);}}else{page->SetParameterPatch(action,values);m_pages->setCurrentWidget(page);m_parameterDock->show();m_parameterDock->raise();}};
    m_pages=new QStackedWidget(this);m_pages->setObjectName("parameterPages");m_pages->setMinimumWidth(340);m_pages->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);m_parameterDock=CreateDock("parameterDock","工具属性",m_pages);
    connect(m_workspaceTabs,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&TestWindow::SetWorkspace);
    connect(m_featureTabs,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int i){if(!m_isClosing&&i>=0)if(auto* page=GetModule(m_featureTabs->itemData(i).toString()))m_pages->setCurrentWidget(page);});
    connect(m_pages,&QStackedWidget::currentChanged,this,[this]{if(m_isReady)SyncNavigation();});
    m_grayPages=new QStackedWidget(this);m_grayPages->setObjectName("grayStatisticsPages");m_grayPages->setMinimumSize(300,260);m_grayDock=CreateDock("grayHistogramDock","灰度直方图 · 显示阈值 / 表面初值",m_grayPages);
    m_statisticsPages=new QStackedWidget(this);m_statisticsPages->setObjectName("businessStatisticsPages");m_resultTabs=new QTabWidget(this);m_resultTabs->setObjectName("workbenchResultTabs");m_resultTabs->addTab(m_statisticsPages,"分析结果");m_resultDock=CreateDock("analysisResultDock","分析结果与明细",m_resultTabs);
    connect(m_showStatistics,&QToolButton::toggled,this,[this](bool shown){m_resultDock->setVisible(shown);if(shown){m_resultDock->raise();if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget()))page->RefreshStatistics();}else for(auto* page:m_modules)page->GetStatisticsWidget()->Cancel();});
    connect(m_objectToggle,&QToolButton::toggled,this,[this](bool visible){SetColumnVisible(0,visible);});connect(m_parameterToggle,&QToolButton::toggled,this,[this](bool visible){SetColumnVisible(2,visible);});
    const QStringList ids{"primary-3d","slice-top-down","slice-front-back","slice-left-right"},titles{"三维","上下切片","前后切片","左右切片"};
    QPushButton* fit=nullptr;
    for(int i=0;i<4;++i){auto* frame=new QWidget(this);auto* fields=new QVBoxLayout(frame);fields->setContentsMargins(0,0,0,0);fields->setSpacing(2);auto* header=new QWidget(frame);header->setObjectName("viewHeader");auto* commands=new QHBoxLayout(header);commands->setContentsMargins(4,0,4,0);
        auto* viewTitle=new InputIdentityLabel(header);viewTitle->setObjectName("viewTitle");viewTitle->SetIdentity(titles[i]);commands->addWidget(viewTitle,1);
        if(i==0){m_renderMode=new QComboBox(header);m_renderMode->setObjectName("renderMode");m_renderMode->setAccessibleName("三维呈现方式");m_renderMode->addItem("等值面","CompositeIsoSurface");m_renderMode->addItem("体渲染","CompositeVolume");m_renderMode->setEnabled(false);m_renderMode->setSizeAdjustPolicy(QComboBox::AdjustToContents);commands->addWidget(m_renderMode);}
        auto* fitButton=new QPushButton(header);fitButton->setProperty("dockTitleButton",true);fitButton->setIcon(GetFitViewIcon());fitButton->setObjectName(i==0?"fitView":"fit_"+ids[i]);fitButton->setAccessibleName("适配"+titles[i]);fitButton->setToolTip("适配"+titles[i]);commands->addWidget(fitButton);if(i==0){fit=fitButton;fit->setEnabled(false);}else connect(fitButton,&QPushButton::clicked,this,[this,id=ids[i]]{if(m_isReady)if(auto* page=GetModule("View"))page->SendAction("Reset",{{"viewId",id}});});
        auto window=vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();window->SetAlphaBitPlanes(1);window->SetMultiSamples(0);auto* widget=new QVTKOpenGLNativeWidget(frame);widget->setObjectName(i==0?"primary3D":ids[i]);widget->setMinimumSize(160,96);widget->setRenderWindow(window);fields->addWidget(widget,1);widget->installEventFilter(this);
        auto* dock=CreateDock("viewDock_"+ids[i],titles[i],frame);
        auto* closeButton=new QToolButton(header);closeButton->setProperty("dockTitleButton",true);closeButton->setFocusPolicy(Qt::StrongFocus);closeButton->setIcon(GetCloseDockIcon());closeButton->setAccessibleName("关闭"+titles[i]);closeButton->setToolTip(closeButton->accessibleName());commands->addWidget(closeButton);connect(closeButton,&QToolButton::clicked,dock,&QDockWidget::close);
        dock->setTitleBarWidget(header);header->installEventFilter(this);m_views.push_back({ids[i],widget,window,frame,dock});}
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
    auto* diagnosticDetails = new QWidget(this); diagnosticDetails->setObjectName("diagnosticDetails");
    auto* diagnosticLayout = new QVBoxLayout(diagnosticDetails); diagnosticLayout->setContentsMargins(0,0,0,0);
    diagnosticLayout->addWidget(m_measurements);
    auto* logLocation=new QLabel("流程和诊断保存到程序目录，可从“测试工具 → 打开本次日志”查看。",diagnosticDetails);
    logLocation->setWordWrap(true);diagnosticLayout->addWidget(logLocation);diagnosticLayout->addStretch();m_resultTabs->addTab(diagnosticDetails,"诊断");
    diagnosticDetails->hide();
    auto* reportContent=new QWidget(this);auto* reports=CreateDock("reportDock","报告与归档",reportContent);m_reportDock=reports;reports->setObjectName("reportDock");
    auto* reportLayout = new QVBoxLayout(reportContent); reportLayout->setContentsMargins(8,8,8,8);
    m_report = new ResultReport(reportContent); reportLayout->addWidget(m_report);
    auto* showDiagnostics=toolMenu->addAction("会话诊断");showDiagnostics->setObjectName("showDiagnostics");
    connect(showDiagnostics,&QAction::triggered,this,[this]{m_showStatistics->setChecked(true);m_resultTabs->setCurrentIndex(1);});
    m_workflow.onShowReports=[this,reports](const QString& source,const QString& result){if(m_isClosing)return;m_report->ShowReports(source,result);if(!m_reportOpened){m_reportOpened=true;reports->setFloating(true);reports->resize(1180,720);}reports->show();reports->raise();reports->activateWindow();};
    m_workflow.getReportAvailable=[this](const QString& module,const QString& result){return m_report->GetReportAvailable(module,result);};
    m_workflow.onShowBusinessReport=[this,reports](const QString& module,const QString& result){if(m_isClosing)return;m_report->ShowReports({},result,module);if(!m_reportOpened){m_reportOpened=true;reports->setFloating(true);reports->resize(1180,720);}reports->show();reports->raise();reports->activateWindow();};
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
        m_responseStep=record["module"].toString()+"."+record["action"].toString();
        m_responseOperation=record["operationId"].toString();
        if(qApp->property("measureUiResponsiveness").toBool()&&!record["isTerminal"].toBool()){std::fprintf(stdout,"UI_PHASE operation=%s phase=%s\n",m_responseOperation.toUtf8().constData(),m_responseStep.toUtf8().constData());std::fflush(stdout);}
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
        if(!record["isTerminal"].toBool()){
            static_cast<InputIdentityLabel*>(m_operationState)->SetIdentity("处理中",GetModuleText(record["module"].toString()));m_operationState->repaint();
        }
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
    m_pump.onTiming = [this](const QString& name, qint64 ns) {m_records.AddTiming(name,ns);SetSynchronousSpan(name,ns);};
    m_pump.onPhaseStarting=[this](const QString& phase){const auto module=m_responseStep.section('.',0,0),action=m_responseStep.section('.',1);static_cast<InputIdentityLabel*>(m_operationState)->SetIdentity(phase=="Host.Update"?"准备中":"绘制中",m_isReady?GetModuleText(module)+" · "+GetActionText(module,action):QString("初始化显示环境"));m_operationState->repaint();};
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
    m_layoutReady=true;ResetLayout();RestoreLayout();
    StartResponseProbe();QMetaObject::invokeMethod(this, [this] { BuildSession(); }, Qt::QueuedConnection);
    UpdateMeasurements();
}
void TestWindow::SetWorkspace(int index)
{
    if (m_isClosing || index < 0 || index >= m_workspaceSelections.size()) return;
    const QStringList groups[]{{"Data","View","Rotation","Artifact"},{"Surface"},{"Roi","Crop"},{"Part","Alignment","Wall","Gap"}};
    auto* page=GetModule(m_workspaceSelections[index]);
    if(!page || page->GetActions().isEmpty())for(const auto& id:groups[index])if(auto* available=GetModule(id))if(!available->GetActions().isEmpty()){page=available;break;}
    if(page && !page->GetActions().isEmpty())m_pages->setCurrentWidget(page);
}
void TestWindow::SyncNavigation()
{
    if (m_isClosing || m_modules.empty()) return;
    auto* page = static_cast<ModulePanel*>(m_pages->currentWidget()); if (!page) return;
    const std::vector<QStringList> groups{{"Data","View","Rotation","Artifact"},{"Surface"},{"Roi","Crop"},{"Part","Alignment","Wall","Gap"}};
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
    {const QSignalBlocker blocker(m_steps);m_steps->setCurrentIndex(workspace);}
    for(int i=0;i<4;++i)m_steps->setTabEnabled(i,m_workspaceTabs->itemData(i,Qt::UserRole-1)!=QVariant(0));
    QStringList tools;for(auto* action:m_contextTools->actions())tools.append(action->data().toString());
    if(tools.size()!=expected.size()||!std::equal(tools.cbegin(),tools.cend(),expected.cbegin())){m_contextTools->clear();for(const auto& id:expected){const QMap<QString,QString> labels{{"Data","导入"},{"View","显示"},{"Rotation","姿态"},{"Artifact","校正"},{"Surface","表面参数"},{"Roi","区域"},{"Crop","裁剪"},{"Part","分割"},{"Alignment","测量"},{"Wall","壁厚"},{"Gap","孔隙"}};auto* action=m_contextTools->addAction(labels.value(id,GetModule(id)->GetDisplayName()));action->setToolTip(GetModule(id)->GetDisplayName());action->setData(id);action->setCheckable(true);connect(action,&QAction::triggered,this,[this,id]{if(auto* target=GetModule(id)){m_pages->setCurrentWidget(target);m_parameterDock->show();m_parameterDock->raise();}});}}
    for(auto* action:m_contextTools->actions())action->setChecked(action->data().toString()==page->GetName());
    m_contextTools->setMinimumWidth(0);m_contextTools->setMinimumWidth(m_contextTools->sizeHint().width());
    m_contextToolsAction->setVisible(expected.size()>1);
    QList<QWidget*> navigation{m_openVolume,findChild<QToolBar*>("quickToolbar")->widgetForAction(m_histogramAction),m_steps};
    if(expected.size()>1)for(auto* action:m_contextTools->actions())navigation.append(m_contextTools->widgetForAction(action));
    navigation.append({m_stop,findChild<QToolButton*>("windowLayoutMenu"),findChild<QPushButton*>("showResultReport"),findChild<QToolButton*>("testTools")});
    for(int i=0;i<navigation.size();++i)if(navigation[i]){navigation[i]->setFocusPolicy(Qt::StrongFocus);if(i>0&&navigation[i-1]&&navigation[i]->window()==navigation[i-1]->window())QWidget::setTabOrder(navigation[i-1],navigation[i]);}
    m_parameterDock->setWindowTitle(page->GetDisplayName()+" · 工具属性");
    static_cast<QVBoxLayout*>(page->layout())->insertWidget(1,m_inputIdentity);m_inputIdentity->show();
    m_browsers->setCurrentWidget(page->GetBrowser());
    if(auto* browser=qobject_cast<QTabWidget*>(page->GetBrowser())){browser->tabBar()->hide();const auto index=findChild<QComboBox*>("sceneView")->currentIndex();if(index==1||index==2)browser->setCurrentIndex(index);}
    m_statisticsPages->setCurrentWidget(page->GetStatisticsWidget());m_grayPages->setCurrentWidget(page->GetGrayStatisticsWidget());
    for(auto* module:m_modules)if(module!=page){module->GetStatisticsWidget()->Cancel();module->GetGrayStatisticsWidget()->Cancel();}
    if(m_resultDock->isVisible())page->RefreshStatistics();else page->GetStatisticsWidget()->Cancel();
    if(m_lastModule!=page->GetName()){
        const bool defaultVisible=page->GetName()=="View"||page->GetName()=="Surface";
        const bool shown=m_lastModule.isEmpty()&&m_hadSavedLayout?!m_grayDock->isHidden():m_grayPreferences.value(page->GetName(),defaultVisible);
        m_syncGray=true;m_grayDock->setVisible(shown);{const QSignalBlocker blocker(m_histogramAction);m_histogramAction->setChecked(shown);}m_syncGray=false;m_lastModule=page->GetName();
    }
    if(m_grayDock->isVisible())page->RefreshGrayStatistics();else page->GetGrayStatisticsWidget()->Cancel();
    if(page->GetName()=="Crop" && m_workflow.ensureCropViews)m_workflow.ensureCropViews();
    if (m_isReady) {
        page->Observe(); AppendLog("切换功能：" + page->GetDisplayName());
        if (!page->GetNotice().isEmpty()) AppendLog(page->GetNotice());
        RefreshObjects();
    }
}
void TestWindow::SetViewLayout(const QString& mode)
{
    if(m_isClosing || m_views.size()!=4)return;
    if(mode=="ThreeDOnly"){
        m_viewLayoutMode=mode;m_viewsVisible=true;
        for(int i=0;i<4;++i)m_views[i].dock->setVisible(i==0);
        if(m_pages->currentWidget() && static_cast<ModulePanel*>(m_pages->currentWidget())->GetName()=="Crop"){
            m_viewLayoutMode="FourViews";auto* choice=findChild<QComboBox*>("viewLayout");const QSignalBlocker blocker(choice);choice->setCurrentIndex(choice->findData("FourViews"));
            for(const auto& view:m_views)view.dock->show();
        }
        layout()->activate();m_pump.SetVisible();if(!m_restoringLayout)m_layoutTimer.start();return;
    }
    const bool wasRestoring=m_restoringLayout;m_restoringLayout=true;m_viewLayoutMode=mode;m_viewsVisible=true;
    for(const auto& view:m_views){view.dock->setFloating(false);removeDockWidget(view.dock);}
    auto* main=m_views[0].dock;addDockWidget(Qt::LeftDockWidgetArea,main);
    if(m_objectDock->isVisible()&&!m_objectDock->isFloating())splitDockWidget(m_objectDock,main,Qt::Horizontal);
    if(mode=="Inspection"){
        splitDockWidget(main,m_views[1].dock,Qt::Horizontal);splitDockWidget(m_views[1].dock,m_views[2].dock,Qt::Vertical);splitDockWidget(m_views[2].dock,m_views[3].dock,Qt::Vertical);
        resizeDocks({main,m_views[1].dock},{3,1},Qt::Horizontal);
    }else{
        splitDockWidget(main,m_views[1].dock,Qt::Horizontal);splitDockWidget(main,m_views[2].dock,Qt::Vertical);splitDockWidget(m_views[1].dock,m_views[3].dock,Qt::Vertical);
        resizeDocks({main,m_views[1].dock},{1,1},Qt::Horizontal);resizeDocks({main,m_views[2].dock},{1,1},Qt::Vertical);resizeDocks({m_views[1].dock,m_views[3].dock},{1,1},Qt::Vertical);
    }
    for(int i=0;i<4;++i)m_views[i].dock->setVisible(mode!="ThreeDOnly"||i==0);
    if(mode=="ThreeDOnly" && m_pages->currentWidget() && static_cast<ModulePanel*>(m_pages->currentWidget())->GetName()=="Crop")for(const auto& view:m_views)view.dock->show();
    layout()->activate();
    const auto settle=[this,mode]{
        if(m_isClosing||m_viewLayoutMode!=mode)return;
        const int available=std::max(320,width()-(m_objectDock->isVisible()&&!m_objectDock->isFloating()?260:0)-(m_parameterDock->isVisible()&&!m_parameterDock->isFloating()?400:0)-24);
        if(mode=="FourViews"){
            resizeDocks({m_views[0].dock,m_views[1].dock},{available/2,available/2},Qt::Horizontal);
            resizeDocks({m_views[0].dock,m_views[2].dock},{400,400},Qt::Vertical);resizeDocks({m_views[1].dock,m_views[3].dock},{400,400},Qt::Vertical);
        }else if(mode=="Inspection")resizeDocks({m_views[0].dock,m_views[1].dock},{available*3/4,available/4},Qt::Horizontal);
    };
    settle();QMetaObject::invokeMethod(this,settle,Qt::QueuedConnection);
    m_restoringLayout=wasRestoring;m_pump.SetVisible();if(!m_restoringLayout)m_layoutTimer.start();
}
void TestWindow::UpdateStopCommand()
{
    const auto id = m_workflow.GetBusyOperation();
    const auto* module = id ? GetModule(m_records.GetRecord(id)["module"].toString()) : nullptr;
    const bool available = m_isReady && !m_isClosing && module && module->onStop && !m_records.GetRecord(id)["isTerminal"].toBool();
    const auto state = m_isClosing ? QString("正在结束会话") : !m_isReady ? QString(m_failure.isEmpty()?"初始化中":"初始化失败")
        : module && !m_records.GetRecord(id)["isTerminal"].toBool() ? QString("处理中 · ")+module->GetDisplayName() : m_lastOutcome.isEmpty()?QString("就绪"):m_lastOutcome;
    auto shown=state;
    if(m_isClosing)shown="关闭中";
    else if(!m_isReady)shown=m_failure.isEmpty()?"初始化":"失败";
    else if(module)shown="处理中";
    else if(!m_lastOutcome.isEmpty())shown=m_lastOutcome.contains("拒绝")?"被拒绝":m_lastOutcome.contains("显示准备失败")?"异常":m_lastOutcome.contains("取消")?"已取消":"失败";
    static_cast<InputIdentityLabel*>(m_operationState)->SetIdentity(shown,state==shown?QString():state);
    m_operationState->setAccessibleName("会话状态："+state);
    m_stop->setEnabled(available);
    m_stop->setText("停止");
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
    if(!input){m_inputIdentity->SetIdentity("当前输入 · 尚未加载体数据");m_inputGeometry->clear();m_inputSummary.clear();m_originalRevision.clear();m_originalInputAction->setEnabled(false);return;}
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
    m_inputIdentity->SetIdentity(QString("%1 · %2 · r%3").arg(kind,QString::fromStdString(input->metadata.identity.datasetId),QString::number(input->dataRevision.generation)),
        "实际输入修订："+ref+"\n来源："+parents.join("\n")+"\n"+QString::fromStdString(input->metadata.source.uri));
    m_grayDock->setWindowTitle("灰度直方图 · "+QString::fromStdString(input->metadata.identity.datasetId));
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
        QElapsedTimer observeClock;observeClock.start();
        for (auto* panel : m_modules) {
            const bool active = panel == m_pages->currentWidget();
            // 隐藏页仅保留任务终态消费，以及裁剪/表面跨页业务状态。
            if (!active && !panel->GetHasPending() && !panel->observeInBackground) continue;
            const TestTiming moduleTiming(m_records, "Observe." + panel->GetName());
            panel->Observe(active);
        }
        RefreshObjects();SetSynchronousSpan("UI.ObserveAll",observeClock.nsecsElapsed());
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
    m_objects->SetObjects(graph,regions,nodes,current,GetModule("Roi")?GetModule("Roi")->GetActions():QStringList{},opacity,input?QString::fromStdString(input->metadata.identity.datasetId):QString());
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
QDockWidget* TestWindow::CreateDock(const QString& name,const QString& title,QWidget* content)
{
    auto* dock=new QDockWidget(title,this);dock->setObjectName(name);dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(QDockWidget::DockWidgetClosable|QDockWidget::DockWidgetMovable|QDockWidget::DockWidgetFloatable);dock->setWidget(content);dock->installEventFilter(this);addDockWidget(Qt::LeftDockWidgetArea,dock);
    for(auto* button:dock->findChildren<QAbstractButton*>(QString(),Qt::FindDirectChildrenOnly)){
        button->setProperty("dockTitleButton",true);button->setFocusPolicy(Qt::StrongFocus);
        const bool close=button->objectName()=="qt_dockwidget_closebutton";
        const auto update=[dock,button,close]{button->setAccessibleName((close?QString("关闭"):dock->isFloating()?QString("停靠"):QString("浮动"))+dock->windowTitle());button->setToolTip(button->accessibleName());};
        update();connect(dock,&QDockWidget::topLevelChanged,button,update);connect(dock,&QDockWidget::windowTitleChanged,button,update);
    }
    QAction* visibility=dock->toggleViewAction();
    if(name=="sceneDock"||name=="parameterDock"){
        visibility=new QAction(title,this);visibility->setCheckable(true);visibility->setChecked(true);visibility->setObjectName(name=="sceneDock"?"toggleObjectDock":"toggleParameterDock");visibility->setShortcut(QKeySequence(name=="sceneDock"?"Ctrl+Shift+L":"Ctrl+Shift+R"));
        connect(visibility,&QAction::triggered,this,[this,name](bool shown){auto* toggle=name=="sceneDock"?m_objectToggle:m_parameterToggle;if(toggle->isChecked()!=shown)toggle->setChecked(shown);else SetColumnVisible(name=="sceneDock"?0:2,shown);});
    }
    m_windowMenu->addAction(visibility);auto* floatingMenu=m_windowMenu->findChild<QMenu*>("floatingWindows");if(!floatingMenu){floatingMenu=m_windowMenu->addMenu("浮动窗口");floatingMenu->setObjectName("floatingWindows");}
    auto* floating=floatingMenu->addAction(title);floating->setCheckable(true);floating->setObjectName("float_"+name);
    connect(floating,&QAction::toggled,dock,&QDockWidget::setFloating);connect(dock,&QDockWidget::topLevelChanged,this,[floating](bool value){const QSignalBlocker blocker(floating);floating->setChecked(value);});
    connect(dock,&QDockWidget::visibilityChanged,this,[this,dock,visibility](bool visible){
        if(!visible&&dock->isHidden()&&!m_restoringLayout&&(dock==m_objectDock||dock==m_parameterDock))dock->setProperty("expandedSize",dock->size());
        const QSignalBlocker visibilityBlocker(visibility);visibility->setChecked(!dock->isHidden());
        auto* toggle=dock==m_objectDock?m_objectToggle:dock==m_parameterDock?m_parameterToggle:dock==m_resultDock?m_showStatistics:nullptr;if(toggle){const QSignalBlocker blocker(toggle);toggle->setChecked(!dock->isHidden());}
        if(dock==m_grayDock){const QSignalBlocker blocker(m_histogramAction);m_histogramAction->setChecked(!dock->isHidden());if(!m_syncGray&&!m_restoringLayout&&m_isReady)if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget()))m_grayPreferences[page->GetName()]=!dock->isHidden();}
        if(dock==m_grayDock||dock==m_resultDock)for(auto* page:m_modules){if(!visible){if(dock==m_grayDock)page->GetGrayStatisticsWidget()->Cancel();else page->GetStatisticsWidget()->Cancel();}else if(page==m_pages->currentWidget()){if(dock==m_grayDock)page->RefreshGrayStatistics();else page->RefreshStatistics();}}
        if(!m_restoringLayout&&!m_isClosing)m_layoutTimer.start();m_pump.SetVisible();
    });
    connect(dock,&QDockWidget::dockLocationChanged,this,[this]{if(!m_restoringLayout&&!m_isClosing)m_layoutTimer.start();});
    connect(dock,&QDockWidget::topLevelChanged,this,[this]{if(!m_restoringLayout&&!m_isClosing)m_layoutTimer.start();m_pump.SetVisible();});return dock;
}
QString TestWindow::GetLayoutPath() const
{
    const auto configured=qApp->property("workbenchLayoutPath").toString();
    return configured.isEmpty()?QCoreApplication::applicationDirPath()+"/QtFeatureWorkbench.ini":configured;
}
void TestWindow::StartResponseProbe()
{
    if(!qApp->property("measureUiResponsiveness").toBool()||m_responseClock.isValid())return;
    m_responseClock.start();m_lastResponseMs=0;m_responseTimer.setTimerType(Qt::PreciseTimer);m_responseTimer.setInterval(50);
    connect(&m_responseTimer,&QTimer::timeout,this,&TestWindow::SampleResponse);m_responseTimer.start();
}
void TestWindow::SampleResponse()
{
    if(!m_responseClock.isValid())return;const auto now=m_responseClock.elapsed();const auto gap=now-m_lastResponseMs;if(gap<50)return;
    const bool business=m_readyAtMs>=0&&m_lastResponseMs>=m_readyAtMs;
    const auto delay=std::max<qint64>(0,gap-50);m_lastResponseMs=now;m_maxResponseMs=std::max(m_maxResponseMs,delay);
    if(m_responseDelays.size()<200000)m_responseDelays.push_back(delay);else ++m_droppedSamples;
    if(business&&m_businessResponseDelays.size()<200000)m_businessResponseDelays.push_back(delay);
    if(delay>100)++m_over100Ms;if(delay>1000)++m_over1000Ms;
    if(delay>100){
        if(m_responseStalls.size()==256){m_responseStalls.removeFirst();++m_droppedStalls;}
        m_responseStalls.append(QJsonObject{{"step",m_responseStep},{"operationId",m_responseOperation},{"elapsedMs",static_cast<double>(now)},
            {"delayMs",static_cast<double>(delay)},{"duringStartup",!business},{"longestSynchronousPhase",m_longestPhase},{"synchronousMs",m_longestSpanNs/1000000.}});
    }
    m_longestSpanNs=0;m_longestPhase.clear();
}
void TestWindow::SetSynchronousSpan(const QString& phase,qint64 durationNs)
{
    if(!m_responseClock.isValid())return;
    if(durationNs>m_longestSpanNs){m_longestSpanNs=durationNs;m_longestPhase=phase;}
    if(durationNs>100000000){
        if(m_synchronousSpans.size()==256){m_synchronousSpans.removeFirst();++m_droppedStalls;}
        m_synchronousSpans.append(QJsonObject{{"step",m_responseStep},{"operationId",m_responseOperation},{"phase",phase},
            {"durationMs",durationNs/1000000.},{"elapsedMs",static_cast<double>(m_responseClock.elapsed())}});
    }
}
QJsonObject TestWindow::GetDiagnostics() const
{
    auto diagnostics=m_pump.GetDiagnostics();auto delays=m_responseDelays;QJsonValue p95,maximum;
    if(!delays.empty()){const auto rank=(delays.size()*95+99)/100-1;std::nth_element(delays.begin(),delays.begin()+rank,delays.end());p95=static_cast<double>(delays[rank]);maximum=static_cast<double>(m_maxResponseMs);}
    diagnostics["uiResponsiveness"]=QJsonObject{{"enabled",m_responseClock.isValid()},{"periodMs",50},{"directFeedbackLimitMs",100},{"progressLimitMs",1000},
        {"samples",static_cast<double>(m_responseDelays.size())},{"droppedSamples",static_cast<double>(m_droppedSamples)},{"maximumDelayMs",maximum},{"p95DelayMs",p95},
        {"over100Ms",static_cast<double>(m_over100Ms)},{"over1000Ms",static_cast<double>(m_over1000Ms)},{"truncatedEntries",static_cast<double>(m_droppedStalls)},
        {"stalls",m_responseStalls},{"synchronousSpans",m_synchronousSpans}};
    auto runtime=m_businessResponseDelays;QJsonObject business{{"samples",static_cast<double>(runtime.size())}};
    if(!runtime.empty()){std::sort(runtime.begin(),runtime.end());business["maximumDelayMs"]=static_cast<double>(runtime.back());business["p95DelayMs"]=static_cast<double>(runtime[(runtime.size()*95+99)/100-1]);business["over100Ms"]=static_cast<double>(std::count_if(runtime.begin(),runtime.end(),[](qint64 value){return value>100;}));business["over1000Ms"]=static_cast<double>(std::count_if(runtime.begin(),runtime.end(),[](qint64 value){return value>1000;}));}
    auto response=diagnostics["uiResponsiveness"].toObject();response["business"]=business;response["startupElapsedMs"]=static_cast<double>(m_readyAtMs);response["startupTimings"]=m_startupTimings;diagnostics["uiResponsiveness"]=response;
    return diagnostics;
}
void TestWindow::SetColumnVisible(int column,bool visible)
{
    auto* dock=column==0?m_objectDock:m_parameterDock;auto* toggle=column==0?m_objectToggle:m_parameterToggle;
    const bool wasHidden=dock->isHidden();
    if(!visible&&!wasHidden)dock->setProperty("expandedSize",dock->size());
    const auto expanded=dock->property("expandedSize").toSize();
    if(!visible)findChild<QToolButton*>("windowLayoutMenu")->setFocus(Qt::ShortcutFocusReason);dock->setVisible(visible);if(visible)dock->raise();
    if(visible&&wasHidden&&expanded.isValid()){
        if(dock->isFloating())dock->resize(expanded);
        else{
            const auto area=dockWidgetArea(dock);const bool vertical=area==Qt::TopDockWidgetArea||area==Qt::BottomDockWidgetArea;const auto requested=vertical?expanded.height():expanded.width();
            // Qt 5 对其余停靠区沿用已扩张的尺寸提示；有界收敛只调整原生布局，不重建视窗或数据。
            for(int attempt=0;attempt<8;++attempt){const auto before=vertical?dock->height():dock->width();resizeDocks({dock},{requested},vertical?Qt::Vertical:Qt::Horizontal);layout()->activate();const auto actual=vertical?dock->height():dock->width();if(std::abs(actual-requested)<=1||actual==before)break;}
        }
    }
    toggle->setText((column==0?QString("场景"):QString("属性"))+(visible?" ▾":" ▸"));
    toggle->setAccessibleName((visible?QString("收起"):QString("展开"))+(column==0?"场景与对象":"工具属性"));
    toggle->setToolTip(toggle->accessibleName()+(column==0?"（Ctrl+Shift+L）":"（Ctrl+Shift+R）"));
    if(!m_restoringLayout)SaveLayout();
}
void TestWindow::SaveLayout()
{
    if(m_restoringLayout||!m_layoutReady)return;
    QSettings preferences(GetLayoutPath(),QSettings::IniFormat);
    preferences.remove("layout");preferences.setValue("docks/state",saveState(2));preferences.setValue("docks/geometry",saveGeometry());
    for(auto* dock:{m_objectDock,m_parameterDock}){
        if(!dock->isHidden())dock->setProperty("expandedSize",dock->size());
        preferences.setValue("docks/"+dock->objectName()+"/expandedSize",dock->property("expandedSize").toSize());
    }
    if(auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget()))preferences.setValue("docks/module",page->GetName());
    preferences.remove("docks/grayVisibility");for(auto it=m_grayPreferences.begin();it!=m_grayPreferences.end();++it)preferences.setValue("docks/grayVisibility/"+it.key(),it.value());
    preferences.setValue("docks/reportOpened",m_reportOpened);preferences.setValue("docks/viewPreset",m_viewLayoutMode);preferences.setValue("docks/locked",findChild<QAction*>("lockWorkbenchLayout")->isChecked());preferences.sync();
    if(preferences.status()!=QSettings::NoError&&!m_settingsFailureReported){m_settingsFailureReported=true;AppendLog("布局无法保存，请检查配置位置的写入权限。");ShowMessage("布局无法保存，请检查配置位置的写入权限。","warning");}
}
void TestWindow::RestoreLayout()
{
    QSettings preferences(GetLayoutPath(),QSettings::IniFormat);const auto state=preferences.value("docks/state").toByteArray();
    if(state.isEmpty()){m_restoringLayout=false;return;}m_hadSavedLayout=true;m_savedModule=preferences.value("docks/module","Data").toString();for(const auto& key:preferences.allKeys())if(key.startsWith("docks/grayVisibility/"))m_grayPreferences[key.section('/',2)]=preferences.value(key).toBool();
    m_restoringLayout=true;const bool restored=restoreState(state,2);if(restored){restoreGeometry(preferences.value("docks/geometry").toByteArray());m_viewLayoutMode=preferences.value("docks/viewPreset","FourViews").toString();m_reportOpened=preferences.value("docks/reportOpened",false).toBool();}
    m_restoringLayout=false;if(!restored){ResetLayout();return;}
    for(auto* dock:{m_objectDock,m_parameterDock})dock->setProperty("expandedSize",preferences.value("docks/"+dock->objectName()+"/expandedSize"));
    const auto screens=QApplication::screens();
    const auto visible=[&screens](const QRect& bounds){return std::any_of(screens.cbegin(),screens.cend(),[&](const QScreen* screen){return screen->availableGeometry().intersects(bounds.adjusted(0,0,0,-std::max(0,bounds.height()-40)));});};
    if(!visible(frameGeometry())){const auto available=QApplication::primaryScreen()->availableGeometry();resize(size().boundedTo(available.size()));move(available.topLeft()+QPoint(12,12));}
    for(auto* dock:findChildren<QDockWidget*>(QString(),Qt::FindDirectChildrenOnly))if(dock->isFloating()&&!visible(dock->frameGeometry())){dock->setFloating(false);addDockWidget(Qt::BottomDockWidgetArea,dock);}
    findChild<QAction*>("lockWorkbenchLayout")->setChecked(preferences.value("docks/locked",false).toBool());
    SetColumnVisible(0,!m_objectDock->isHidden());SetColumnVisible(2,!m_parameterDock->isHidden());
}
void TestWindow::ResetLayout()
{
    if(!m_layoutReady)return;const bool old=m_restoringLayout;m_restoringLayout=true;m_reportOpened=false;m_hadSavedLayout=false;m_grayPreferences.clear();m_lastModule.clear();
    m_objectDock->setProperty("expandedSize",QVariant());m_parameterDock->setProperty("expandedSize",QVariant());
    findChild<QAction*>("lockWorkbenchLayout")->setChecked(false);
    for(auto* dock:findChildren<QDockWidget*>(QString(),Qt::FindDirectChildrenOnly)){dock->setFloating(false);removeDockWidget(dock);}
    // 只在首次/显式重置时收起次要场景栏；恢复已保存布局不应用宽度策略。
    const auto scale=std::max(1.,font().pixelSize()/16.);const bool showObjects=width()>=1280*scale;
    const int objectWidth=260,parameterWidth=400;
    addDockWidget(Qt::LeftDockWidgetArea,m_objectDock);m_objectDock->show();addDockWidget(Qt::RightDockWidgetArea,m_parameterDock);m_parameterDock->show();
    SetViewLayout("FourViews");addDockWidget(Qt::BottomDockWidgetArea,m_grayDock);addDockWidget(Qt::BottomDockWidgetArea,m_resultDock);tabifyDockWidget(m_grayDock,m_resultDock);
    addDockWidget(Qt::BottomDockWidgetArea,m_reportDock);tabifyDockWidget(m_resultDock,m_reportDock);m_reportDock->hide();m_resultDock->hide();m_grayDock->hide();
    resizeDocks({m_objectDock,m_parameterDock},{objectWidth,parameterWidth},Qt::Horizontal);resizeDocks({m_grayDock},{300},Qt::Vertical);
    {const QSignalBlocker left(m_objectToggle),right(m_parameterToggle);m_objectToggle->setChecked(showObjects);m_parameterToggle->setChecked(true);}SetColumnVisible(0,showObjects);SetColumnVisible(2,true);
    m_objectDock->setProperty("expandedSize",QSize(objectWidth,m_objectDock->height()));
    for(auto* bar:findChildren<QToolBar*>(QString(),Qt::FindDirectChildrenOnly))addToolBar(Qt::TopToolBarArea,bar);

    m_restoringLayout=old;if(m_isReady)SyncNavigation();if(!m_restoringLayout)SaveLayout();m_pump.SetVisible();
}
void TestWindow::ShowHistogram()
{
    if(m_isClosing)return;auto* page=dynamic_cast<ModulePanel*>(m_pages->currentWidget());if(!page)return;
    m_grayDock->show();m_grayDock->raise();{const QSignalBlocker blocker(m_histogramAction);m_histogramAction->setChecked(true);}page->Observe();page->RefreshGrayStatistics();
    resizeDocks({m_grayDock},{std::max(280,height()/3)},Qt::Vertical);
}
TestWindow::~TestWindow()
{
    SaveLayout();
    // 正常路径只在 Stop 成功后进入析构；页面先于运行记录/工作流成员销毁。
    m_isClosing = true;
    m_report->CancelTasks(true);m_report->onReadReports={};m_report->getSources={};
    const QSignalBlocker pagesBlocker(m_pages), workspaceBlocker(m_workspaceTabs), featureBlocker(m_featureTabs);
    for (auto* module : m_modules) delete module;
    m_modules.clear();
}
void TestWindow::BuildSession()
{
    QElapsedTimer startup;startup.start();m_operationState->repaint();
    if (m_isReady || m_isClosing) return;
    try {
        // 每一阶段返回事件循环；不在构造途中 processEvents，也不允许 Stop 后续接初始化。
        if(m_buildPhase==0){
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
        m_startupTimings["hostSessionMs"]=startup.elapsed();m_buildPhase=1;QTimer::singleShot(10,this,[this]{BuildSession();});return;
        }
        if(m_buildPhase==1){
        m_modules = BuildModules({m_runtime, m_workflow, m_records}, m_pages);
        m_startupTimings["modulesMs"]=startup.elapsed();m_buildPhase=2;QTimer::singleShot(10,this,[this]{BuildSession();});return;
        }
        if (m_workflow.resources.hasExplicitWorkingLimit)
            AppendLog(QString("显式工作集上限 %1 GiB；伪影单次发布上限 %2 GiB。").arg(m_workflow.resources.workingBytes / (1024.*1024.*1024.), 0, 'f', 2).arg(m_workflow.resources.publishBytes / (1024.*1024.*1024.), 0, 'f', 2));
        else
            AppendLog("未设置算法工作集硬上限；内存快照仅用于诊断和可选缓存保留。");
        for (auto* page : m_modules) {
            m_browsers->addWidget(page->GetBrowser());
            m_statisticsPages->addWidget(page->GetStatisticsWidget());m_grayPages->addWidget(page->GetGrayStatisticsWidget());
            page->GetStatisticsWidget()->SetCompact(false);page->GetGrayStatisticsWidget()->SetCompact(true);m_pages->addWidget(page);
            const QMap<QString,QString> firstTools{{"Data","Load"},{"View","Set"},{"Rotation","Rotate"},{"Artifact","Ring"},{"Roi","Begin"},{"Crop","Start"},{"Part","Start"},{"Surface","GlobalAutomatic"},{"Alignment","ImportReference"},{"Wall","Start"},{"Gap","Start"}};
            if(page->GetActions().contains(firstTools.value(page->GetName())))page->SetInitialAction(firstTools.value(page->GetName()));
            page->onMessage = [this,page](const QString& message) {
                AppendLog(message,page->GetDisplayName());m_feedbackText=message;
                // 选操作时的通用说明已在帮助中；固定反馈区保留业务结果与需要处理的提示。
                const auto action=page->GetCurrentAction();const auto description=GetActionDescription(page->GetName(),action);
                const auto prefix=page->GetDisplayName()+" · ";
                if(message!=prefix+description && message!=prefix+GetActionText(page->GetName(),action)+" ｜ "+description)
                    page->SetFeedback(message);
            };
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
            if(auto* page=GetModule(module)){m_pages->setCurrentWidget(page);m_showStatistics->setChecked(true);m_resultDock->show();m_resultDock->raise();m_resultTabs->setCurrentIndex(0);page->RefreshStatistics();}
        };
        m_workflow.onShowHistogram=[this](const QString& module){if(auto* page=GetModule(module))m_pages->setCurrentWidget(page);ShowHistogram();};
        m_workflow.ensureCropViews=[this]{m_viewsVisible=true;for(const auto& view:m_views)view.dock->show();m_pump.SetVisible();};
        if(m_hadSavedLayout)if(auto* saved=GetModule(m_savedModule))m_pages->setCurrentWidget(saved);
        m_isReady = true; m_openVolume->setEnabled(true);m_startupFeedback->hide(); SyncNavigation();
        if (auto* data = GetModule("Data")) data->SelectAction("Load");
        QStringList enabled;
        for (auto* page : m_modules) if (!page->GetActions().isEmpty()) enabled.append(page->GetDisplayName());
        AppendLog("业务入口：" + enabled.join("、"));
        StartResponseProbe();m_readyAtMs=m_responseClock.isValid()?m_responseClock.elapsed():-1;m_startupTimings["registerAndNavigateMs"]=startup.elapsed();
        if(m_responseClock.isValid()){std::fprintf(stdout,"UI_READY elapsed_ms=%lld\n",static_cast<long long>(m_readyAtMs));std::fflush(stdout);}
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
void TestWindow::SetViewsVisible(bool visible) {m_viewsVisible=visible;for(int i=0;i<static_cast<int>(m_views.size());++i)m_views[i].dock->setVisible(visible&&(m_viewLayoutMode!="ThreeDOnly"||i==0));m_pump.SetVisible();}
bool TestWindow::StopSession()
{
    m_responseStep="Session.Stop";
    if (!m_isClosing) AppendLog("正在关闭测试会话，停止计算并释放视图资源。");
    if(!m_isClosing)SaveLayout();m_layoutTimer.stop();
    m_isClosing = true; m_openVolume->setEnabled(false); m_stop->setEnabled(false); m_workflow.Stop(); m_pump.Stop();
    if(!m_report->CancelTasks(true)) { ShowMessage("正在结束报告任务，资源已保留；请稍候再关闭。"); return false; }
    // 原生报告和分页目录持有正式结果，须先释放才能退休其 OwnerRetirement 作用域。
    for(const auto& id:m_report->GetReportIds())m_report->RemoveReport(id);
    bool readsPending=false;
    for(auto* module:m_modules) {
        module->CancelReadTasks();module->GetStatisticsWidget()->Cancel();module->GetGrayStatisticsWidget()->Cancel();
        if(module->onSessionClosing)module->onSessionClosing();
        readsPending=readsPending || module->property("readTasksBusy").toBool();
    }
    // 读取的所属线程定时器先交付唯一终态，再停止 Host 和其余未完成记录。
    if(readsPending) { ShowMessage("正在结束读取任务，请稍候再关闭。"); return false; }
    const auto session = m_runtime.GetSession();
    const auto current = GetDescriptor(session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{});
    if (!m_runtime.Stop()) { ShowMessage("正在结束当前任务，请稍候再关闭。"); return false; }
    m_records.StopPending(current);
    for (auto& view : m_views) view.widget->setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
    if (m_isReady) AppendLog("测试会话已停止。");
    m_isReady = false; m_renderMode->setEnabled(false);
    SampleResponse();m_responseTimer.stop();
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
void TestWindow::showEvent(QShowEvent* event) { QMainWindow::showEvent(event);m_pump.SetVisible();if(!m_firstShow){m_firstShow=true;if(!m_hadSavedLayout)QMetaObject::invokeMethod(this,[this]{if(!m_isClosing)ResetLayout();},Qt::QueuedConnection);} }
void TestWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);if(!m_steps)return;
    const QStringList full{"数据准备","表面测定","ROI·裁剪","分析测量","结果"},compact{"数据","表面","区域","分析","结果"};
    const auto scale=std::max(1.,m_steps->font().pixelSize()/16.);
    for(int i=0;i<m_steps->count();++i){m_steps->setTabText(i,width()<1100*scale?compact[i]:full[i]);m_steps->setTabToolTip(i,full[i]);}
    if(auto* button=findChild<QToolButton*>("windowLayoutMenu"))button->setText(width()>=1400*scale?"窗口与布局":"窗口");
    if(auto* button=findChild<QToolButton*>("testTools"))button->setText(width()>=1400*scale?"测试工具":"测试");
    if(m_operationState)m_operationState->setFixedWidth(width()<1100*scale?static_cast<int>(52*scale):static_cast<int>(76*scale));
}
bool TestWindow::eventFilter(QObject* watched, QEvent* event)
{
    if(watched->objectName()=="viewHeader"&&(event->type()==QEvent::Resize||event->type()==QEvent::PaletteChange)){
        auto* header=static_cast<QWidget*>(watched);const auto scale=std::max(1.,header->font().pixelSize()/16.);
        // 窄停靠窗保留角色标题与鼠标拖动区；呈现方式仍可从“显示与导航”的同一属性修改。
        if(m_renderMode&&m_renderMode->parentWidget()==header)m_renderMode->setVisible(header->width()>=220*scale);
        if(event->type()==QEvent::PaletteChange)for(auto* button:header->findChildren<QAbstractButton*>())button->setIcon(qobject_cast<QPushButton*>(button)?GetFitViewIcon():GetCloseDockIcon());
    }
    if(m_layoutReady&&!m_restoringLayout&&!m_isClosing&&(event->type()==QEvent::Move||event->type()==QEvent::Resize))m_layoutTimer.start();
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
