// 测试用途：自动执行实际页面入口的合成回归或指定用例，验证中文分派、业务日志和生命周期。
#include "TestCaseRunner.h"
#include "BusinessWorkflowTests.h"
#include "WorkbenchV1Tests.h"
#include "App/TestWindow.h"
#include "Support/JsonInput.h"
#include "Support/UiText.h"
#include "Support/ParameterEditor.h"
#include "Support/RoiSelection.h"
#include "Support/SceneGraph.h"
#include "Support/SceneNodes.h"
#include "Support/CatalogNodes.h"
#include "Support/ResultReport.h"
#include "Support/WorkbenchStyle.h"
#include <QTabWidget>
#include <QDialog>
#include <QToolButton>
#include <QMenu>
#include <QTableWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#if defined(MANUAL_ALIGNMENT)
#include "Modules/AlignmentInput.h"
#endif
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QEventLoop>
#include <QAbstractEventDispatcher>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QGridLayout>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QApplication>
#include <QSignalBlocker>
#include <QGroupBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QMenu>
#include <QContextMenuEvent>
#include <iostream>
#include <functional>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <cmath>
#include <vtkCommand.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkMapper.h>
#include <vtkDataSet.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkMatrix4x4.h>
#include <vtkProperty.h>
#include <vtkCamera.h>
#include <vtkLookupTable.h>
#include <vtkImageSlice.h>
#include <vtkImageProperty.h>
#include <vtkImageMapper3D.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkHandleRepresentation.h>
#include <vtkBoxRepresentation.h>
#include <vtkImplicitPlaneRepresentation.h>
#include <vtkPropPicker.h>
#include <vtkVolume.h>
#include <QVTKOpenGLNativeWidget.h>
namespace Manual {
namespace {
bool Wait(const std::function<bool()>& done, int timeoutMs = 30000)
{
    if (done()) return true;
    QEventLoop loop; QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, &loop,
        [&] { if (done()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(timeoutMs); loop.exec();
    return done();
}
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
    std::cout << "PASS: " << message << std::endl;
}
void SaveBusinessCapture(TestWindow& window, const QString& folder, const QJsonObject& step,
    std::uint64_t operationId)
{
    Check(Wait([&] {
        const auto session = window.GetSession();
        const auto input = session ? session->GetImageDescriptor() : std::optional<ImageDescriptor>{};
        const auto scenes = session ? session->GetSceneViewStates() : std::vector<HostSceneViewState>{};
        if (!input || scenes.size() != 4 || scenes.front().sceneEpoch == 0
            || !window.GetWorkflow().getRenderPending || GetId(window.GetDiagnostics()["pendingUpdateFailures"]) != 0) return false;
        if (step["waitForRoiDisplay"].toBool()) {
            auto* roi = window.GetModule("Roi"); roi->Observe();
            const auto state = roi->GetObservedState();
            if (state["isSaving"].toBool() || state["isComputing"].toBool() || state["displayStatus"].toInt() == 0) return false;
            Check(state["displayStatus"].toInt()!=3 && state["displayError"].toInt()==0
                && state["displayFailures"].toArray().isEmpty(),"business ROI display succeeds before capture");
        }
        return std::all_of(scenes.begin(), scenes.end(), [&](const auto& scene) {
            return scene.isAvailable && scene.presentation && scene.presentation->dataRevision == input->dataRevision
                && scene.sceneEpoch == scenes.front().sceneEpoch && scene.renderedEpoch > 0
                && scene.renderedEpoch <= scene.sceneEpoch && !window.GetWorkflow().getRenderPending(scene.id);
        });
    },step["timeoutMs"].toInt(30000)), "business capture waits for committed 2D/3D scenes and actual renders");
    Check(QDir().mkpath(folder), "business capture directory is available");
    QCoreApplication::processEvents();
    QJsonArray files, scenes;
    for (auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>()) if (widget->isVisible()) {
        const auto frame = widget->grabFramebuffer();
        Check(!frame.isNull() && frame.size() == widget->size() * widget->devicePixelRatioF(),
            "business capture reads the actual visible framebuffer");
        const auto name = widget->objectName() + ".png";
        Check(frame.save(QDir(folder).filePath(name)), "business 2D/3D framebuffer saved");
        files.append(QJsonObject{{"view",widget->objectName()},{"file",name},{"width",frame.width()},{"height",frame.height()}});
    }
    Check(files.size() == 4, "business capture contains one 3D and three 2D views");
    Check(window.grab().save(QDir(folder).filePath("window.png")), "business window context saved");
    for (const auto& scene : window.GetSession()->GetSceneViewStates())
        scenes.append(QJsonObject{{"view",QString::fromStdString(scene.id)}, {"sceneEpoch",QString::number(scene.sceneEpoch)},
            {"renderedEpoch",QString::number(scene.renderedEpoch)},
            {"source",scene.presentation ? GetRefText(scene.presentation->dataRevision) : QString()}});
    ExportJson(QDir(folder).filePath("capture.json"),{{"operationId",QString::number(operationId)},
        {"label",step["stageLabel"]},{"module",step["module"]},{"action",step["action"]},
        {"views",files},{"scenes",scenes}});
}
std::uint64_t Send(TestWindow& window, const QString& module, const QString& action, const QJsonObject& patch = {})
{
    auto* panel = window.GetModule(module);
    if (!panel || !panel->GetActions().contains(action)) throw std::invalid_argument("当前构建不含请求的测试入口");
    panel->SetParameterPatch(action, patch);
    return panel->SendAction(action, panel->GetParameters());
}
QJsonObject GetComplete(TestWindow& window, std::uint64_t id, const QString& status = {}, int timeoutMs = 30000)
{
    const bool completed = Wait([&] { return window.GetRecords().GetRecord(id)["isTerminal"].toBool(); }, timeoutMs);
    if (!completed) {
        const auto pending = window.GetRecords().GetRecord(id);
        std::cerr << GetJsonText(pending).toStdString();
        if (const auto* panel = window.GetModule(pending["module"].toString()))
            std::cerr << GetJsonText(panel->GetObservedState()).toStdString();
    }
    Check(completed, "operation reaches terminal without polling Host ticks");
    const auto result = window.GetRecords().GetRecord(id);
    std::cout << GetJsonText(result).toStdString() << std::endl;
    Check(result["completeCount"].toInt() == 1, "exactly one completion");
    if (!status.isEmpty()) Check(result["status"].toString() == status, ("expected status " + status).toStdString().c_str());
    return result;
}
QStringList GetExpectedStatuses(const QJsonObject& step)
{
    QStringList statuses;
    if (step.contains("expectStatuses")) {
        if (step.contains("expectStatus") || !step["expectStatuses"].isArray())
            throw std::invalid_argument("expectStatuses 必须为数组且不能与 expectStatus 同时指定");
        for (const auto value : step["expectStatuses"].toArray()) {
            if (!value.isString() || value.toString().isEmpty() || statuses.contains(value.toString()))
                throw std::invalid_argument("预期终态必须是非空且不重复的字符串");
            statuses.append(value.toString());
        }
    } else statuses.append(GetText(step, "expectStatus"));
    if (statuses.isEmpty() || statuses.contains(QString{})) throw std::invalid_argument("必须指定非空预期终态");
    return statuses;
}
bool GetEnabled(TestWindow& window, const QString& module)
{
    const auto* panel = window.GetModule(module);
    const bool enabled = panel && !panel->GetActions().isEmpty();
    if (!enabled) std::cout << "NOT BUILT: " << module.toStdString() << std::endl;
    return enabled;
}
void ClickNode(QTreeWidget* tree, QTreeWidgetItem* item, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const auto key = item->data(0, Qt::UserRole).toJsonObject()["id"].toString();
    if(auto* statistics=tree->window()->findChild<QToolButton*>("showBusinessStatistics"))statistics->setChecked(true);
    if(auto* results=tree->window()->findChild<QTabWidget*>("workbenchResultTabs"))results->setCurrentIndex(1);
    for (auto* page = tree->parentWidget(); page; page = page->parentWidget()) {
        auto* tabs = qobject_cast<QTabWidget*>(page->parentWidget() ? page->parentWidget()->parentWidget() : nullptr);
        if (tabs && tabs->indexOf(page) >= 0) { tabs->setCurrentWidget(page); break; }
    }
    QCoreApplication::processEvents();
    item = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject()["id"].toString() == key) { item = *it; break; }
    Check(item != nullptr, "scene selection resolves stable ID after pending updates");
    for (auto* parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
    tree->scrollToItem(item); tree->doItemsLayout();
    const auto point = tree->visualItemRect(item).center();
    QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, modifiers);
    QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(tree->viewport(), &press); QApplication::sendEvent(tree->viewport(), &release);
}
std::uint64_t Click(TestWindow& window, const QString& module, const QString& action, const QJsonObject& patch = {})
{
    if ((action == "SetDisplay" && (module == "Wall" || module == "Gap")) ||
        (action == "SetDeviationDisplay" && module == "Alignment")) {
        auto* legend = window.findChild<QToolButton*>("legend_" + module + "_primary-3d");
        Check(Wait([&] { return legend && legend->isVisible() && legend->isEnabled(); }, 2000),
            "actual viewport legend entry is available");
        if (legend->isChecked()) {
            const QSignalBlocker blocker(legend);
            legend->setChecked(false);
        }
        legend->click();
        auto* panel = window.GetModule(module);
        QWidget* form = nullptr;
        QPushButton* apply = nullptr;
        Check(Wait([&] {
            for (auto* widget : panel->findChildren<QWidget*>("colorLegendParameters"))
                if (widget->isVisible()) form = widget;
            for (auto* button : panel->findChildren<QPushButton*>("applyColorLegend"))
                if (button->isVisible()) apply = button;
            return form && apply && apply->isEnabled();
        }, 2000), "floating color editor exposes the pinned result configuration");
        auto* editor = dynamic_cast<ParameterEditor*>(form);
        Check(editor != nullptr, "floating color editor uses the actual parameter form");
        editor->SetPatch(patch);
        const auto id = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()) + 1;
        apply->click();
        Check(Wait([&] { return !window.GetRecords().GetRecord(id).isEmpty(); }, 2000),
            "color editor apply reaches the feature display request");
        return id;
    }
    auto* panel = window.GetModule(module); window.GetWorkflow().onNavigate(module, action, {});
    // 普通模块从会话命令上下文执行；ROI 保存保留当前草稿和对象选择。
    if(module!="Roi") {
        auto* tree=panel->GetCatalogTree();ClickNode(tree,tree->topLevelItem(0));
    }
    panel->SetParameterPatch(action, patch);
    // 导航与数据变更的页面状态异步刷新；仍检查真实按钮，不绕过入口直接发请求。
    const bool ready = Wait([&] {
        const auto* current = panel->findChild<QPushButton*>("action_" + action);
        const auto* menu = panel->findChild<QToolButton*>("moreOperations")->menu();
        const auto* command = menu->findChild<QAction*>("command_" + action);
        const auto* primary=panel->findChild<QPushButton*>("businessPrimary");
        const bool atPrimary=primary && primary->isEnabled() && primary->property("businessAction")==action && !primary->property("usesOutput").toBool();
        const auto* next=panel->findChild<QPushButton*>("next_"+action);
        const auto* editor=panel->GetParameterEditor(action);
        const bool atNext=next && next->isVisible() && next->isEnabled() && (!editor || !editor->GetHasInputs());
        return atPrimary || atNext || (current && current->isEnabled() && (current->isVisible() || (command && command->isEnabled())));
    }, 2000);
    if (!ready) std::cerr << "Button unavailable: " << module.toStdString() << '.' << action.toStdString() << '\n';
    Check(ready, "actual business button or classified menu command is available");
    auto* button = panel->findChild<QPushButton*>("action_" + action);
    auto* primary=panel->findChild<QPushButton*>("businessPrimary");
    const bool atPrimary=primary && primary->isEnabled() && primary->property("businessAction")==action && !primary->property("usesOutput").toBool();
    auto* next=panel->findChild<QPushButton*>("next_"+action);
    const auto* editor=panel->GetParameterEditor(action);
    const bool atNext=next && next->isVisible() && next->isEnabled() && (!editor || !editor->GetHasInputs());
    Check(button && (atPrimary || atNext || button->isEnabled()), "actual business command is enabled");
    const auto id = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()) + 1;
    if(atPrimary)primary->click();
    else if(atNext)next->click();
    else if (button->isVisible()) button->click();
    else {
        auto* command = panel->findChild<QToolButton*>("moreOperations")->menu()->findChild<QAction*>("command_" + action);
        Check(command && command->isEnabled(), "direct command remains available in the classified menu");
        command->trigger();
        Check(Wait([&] { return !window.GetRecords().GetRecord(id).isEmpty(); }, 2000), "menu dispatch reaches the original business path");
    }
    return id;
}
std::uint64_t ClickCurrentPrimary(TestWindow& window,const QString& module,const QString& expected)
{
    auto* panel=window.GetModule(module);Check(panel && panel->isVisible(),"current primary uses the visible page without navigation");panel->Observe();
    auto* button=panel->findChild<QPushButton*>("businessPrimary");
    if(!button || !button->isEnabled() || button->property("businessAction")!=expected)std::cerr<<"CURRENT PRIMARY: expected="<<expected.toStdString()<<" actual="<<(button?button->property("businessAction").toString().toStdString():"missing")<<" parameterAction="<<panel->GetParameterAction().toStdString()<<" state="<<QJsonDocument(panel->GetObservedState()).toJson(QJsonDocument::Compact).toStdString()<<std::endl;
    Check(button && button->isVisible() && button->isEnabled() && button->property("businessAction")==expected,"current primary presents the expected natural phase");
    const auto id=static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size())+1;button->click();return id;
}
std::uint64_t SendUnavailable(TestWindow& window, const QString& module, const QString& action)
{
    auto* panel = window.GetModule(module); panel->Observe();
    auto* button = panel->findChild<QPushButton*>("action_" + action);
    Check(button && !button->isEnabled() && !button->toolTip().isEmpty(),
        "unavailable business commands are disabled with an explanation");
    return Send(window, module, action);
}
void DropFile(QWidget* widget, const QString& path)
{
    QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent enter(QPoint(8,8), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &enter); Check(enter.isAccepted(), "local file drag is accepted");
    QDropEvent drop(QPointF(8,8), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &drop); Check(drop.isAccepted(), "local file drop is handled");
}
void SetBoolean(ParameterEditor* field, bool enabled)
{
    auto* box = field->findChild<QCheckBox*>("value");
    const auto state = enabled ? Qt::Checked : Qt::Unchecked;
    for (int attempt = 0; box->checkState() != state && attempt < 2; ++attempt) box->click();
    Check(field->GetValue() == QJsonValue(enabled), "checkbox value matches explicit Boolean intent");
}
void CheckFourViewGeometry(TestWindow& window)
{
    const auto views = window.findChildren<QVTKOpenGLNativeWidget*>(); const auto original = window.size();
    auto* layoutChoice = window.findChild<QComboBox*>("viewLayout");
    layoutChoice->setCurrentIndex(layoutChoice->findData("FourViews"));QCoreApplication::processEvents();
    const auto sameSize = [&] {
        if (views.size() != 4) return false;
        int minWidth = views.front()->width(), maxWidth = minWidth, minHeight = views.front()->height(), maxHeight = minHeight;
        for (const auto* view : views) {
            minWidth = std::min(minWidth, view->width()); maxWidth = std::max(maxWidth, view->width());
            minHeight = std::min(minHeight, view->height()); maxHeight = std::max(maxHeight, view->height());
        }
        // 奇数像素的等分最多有一个像素的取整差。
        return maxWidth-minWidth <= 1 && maxHeight-minHeight <= 1
            && std::all_of(views.begin(),views.end(),[](const auto* view){return view->width()>=view->minimumWidth() && view->height()>=view->minimumHeight();});
    };
    Check(window.minimumSize()!=window.maximumSize(),"manual window has a resizable default size");
    Check(Wait(sameSize,3000),"four-view layout has equal image regions inside the current window");
    const auto windows = window.GetSession()->GetRenderViewStates();
    auto* primary = window.findChild<QVTKOpenGLNativeWidget*>("primary3D");
    const auto primarySize = primary->size();
    auto* renderWindow = primary->renderWindow();
    layoutChoice->setCurrentIndex(layoutChoice->findData("ThreeDOnly")); QCoreApplication::processEvents();
    Check(Wait([&] { return primary->width() > primarySize.width() && primary->height() > primarySize.height(); },3000),
        "3D-only layout expands the existing primary viewport");
    for (const auto* view : views) if (view != primary) Check(!view->isVisible(), "slice viewports are hidden in 3D-only layout");
    Check(primary->renderWindow() == renderWindow && window.GetSession()->GetRenderViewStates().size() == windows.size(),
        "layout switching preserves the original render windows and host view identities");
    layoutChoice->setCurrentIndex(layoutChoice->findData("FourViews")); QCoreApplication::processEvents();
    Check(Wait(sameSize,3000), "returning to four views restores equal image regions");
    Check(window.size()==original,"view layout changes never resize the complete manual window");
    if(window.height()>=900) {
        layoutChoice->setCurrentIndex(layoutChoice->findData("Inspection"));QCoreApplication::processEvents();
        Check(Wait([&]{return primary->width()>views[1]->width() && primary->height()>views[1]->height();},3000),"inspection layout gives the existing 3D viewport the main canvas");
        Check(primary->renderWindow()==renderWindow && window.size()==original,"inspection layout preserves render identity and current outer size");
        layoutChoice->setCurrentIndex(layoutChoice->findData("FourViews"));QCoreApplication::processEvents();
    }
}
void CheckUiAndRecords(TestWindow& window)
{
    auto* workspaces = window.findChild<QTabBar*>("workspaceTabs");
    auto* tabs = window.findChild<QTabBar*>("featureTabs");
    auto* pages = window.findChild<QStackedWidget*>("parameterPages");
    auto* browsers = window.findChild<QStackedWidget*>("browserStack");
    Check(workspaces && workspaces->count() == 3 && tabs && tabs->shape() == QTabBar::RoundedNorth,
        "task workspaces organize horizontal module navigation");
    QSet<QString> moduleIds;
    QStringList remembered{"", "", ""};
    {
        const QStringList groups[]{ {"Data","View","Rotation"},{"Roi","Crop","Artifact","Part","Surface"},{"Alignment","Wall","Gap"} };
        for(int group=0;group<3;++group) {
            const bool hasModules=std::any_of(groups[group].begin(),groups[group].end(),[&](const QString& id){return GetEnabled(window,id);});
            Check(workspaces->isTabEnabled(group)==hasModules,"workspace availability follows its compiled business modules");
            if(!hasModules)continue;
            workspaces->setCurrentIndex(group);
            for (int i = 0; i < tabs->count(); ++i) {
                tabs->setCurrentIndex(i);
                const auto id = tabs->tabData(i).toString(); moduleIds.insert(id);
                auto* module = window.GetModule(id);
                Check(groups[group].contains(id) && module && !module->GetActions().isEmpty(),"group navigation shows only compiled business pages");
                Check(pages->currentWidget() == module && browsers->currentWidget() == module->GetBrowser(),
                    "module identity keeps its parameter page and source browser synchronized");
            }
            remembered[group]=tabs->tabData(tabs->currentIndex()).toString();
        }
    }
    int enabled=0;for(const auto* id:{"Data","View","Rotation","Roi","Crop","Artifact","Part","Surface","Alignment","Wall","Gap"})if(GetEnabled(window,id))++enabled;
    Check(moduleIds.size() == enabled,"all compiled modules remain reachable");
    if(GetEnabled(window,"Part"))Check(moduleIds.contains("Part")
        && window.GetModule("PartEdit") == window.GetModule("Part"), "segmentation and editing share one compiled page");
    for (int group = 0; group < workspaces->count(); ++group) {
        if(!workspaces->isTabEnabled(group))continue;
        workspaces->setCurrentIndex(group);
        Check(tabs->tabData(tabs->currentIndex()).toString() == remembered[group], "each workspace remembers its last selected module");
    }
    window.GetWorkflow().onNavigate("Data", "Load", {});
    Check(workspaces->currentIndex() == 0 && tabs->tabData(tabs->currentIndex()) == "Data", "cross-workspace navigation returns to input");
    auto* diagnostics = window.findChild<QToolButton*>("toggleDiagnostics");
    auto* details = window.findChild<QWidget*>("diagnosticDetails");
    Check(diagnostics && details && details->isHidden() && window.findChild<QLabel*>("latestMessage")->isVisible(),
        "latest feedback stays visible while detailed diagnostics are collapsed");
    const auto compactSize=window.size();
    diagnostics->click();Check(Wait([&]{return details->isVisible() && window.size()==compactSize;},3000),"diagnostics expand within the unchanged outer window");
    window.grab().save("diagnostics-expanded.png");diagnostics->click();Check(Wait([&]{return !details->isVisible() && window.size()==compactSize;},3000),"closing diagnostics keeps the prior outer size");
    auto* objectToggle=window.findChild<QToolButton*>("toggleObjectColumn");auto* objects=window.findChild<QWidget*>("objectColumn");objectToggle->click();Check(objects->isHidden(),"object sidebar can be collapsed");objectToggle->click();Check(objects->isVisible(),"object sidebar can be restored");
    const auto resized=compactSize+QSize(40,20);window.resize(resized);
    Check(Wait([&]{return window.size()==resized;},3000),"user can resize the main window beyond its default size");
    window.resize(compactSize);QCoreApplication::processEvents();window.grab().save("workbench-default.png");
    window.showMaximized();Check(Wait([&]{return window.isMaximized();},3000),"main window can be maximized");QCoreApplication::processEvents();
    diagnostics->click();
    Check(Wait([&]{return window.isMaximized() && details->isVisible();},3000),"diagnostics keep a maximized window maximized");
    diagnostics->click();Check(Wait([&]{return window.isMaximized();},3000),"closing diagnostics restores the previous maximized state");
    window.showNormal();window.resize(compactSize);QCoreApplication::processEvents();
    Check(window.findChild<QToolButton*>("testTools")->menu()->findChild<QAction*>("performanceCapture")
        && !window.findChild<QPushButton*>("stopCalculation")->isEnabled(), "diagnostic tools are grouped and idle stop is disabled");
    auto* open = window.findChild<QPushButton*>("openVolume");
    Check(open && open->isVisible() && open->isEnabled() && open->shortcut() == QKeySequence(QKeySequence::Open),
        "input can be opened from the top toolbar or Ctrl+O");
    Check(window.GetSession()->GetRenderViewStates().size() == 4 && window.findChildren<QVTKOpenGLNativeWidget*>().size() == 4,
        "manual workspace contains one 3D viewport and three slice viewports");
    Check(!window.findChild<QComboBox*>("renderMode")->isEnabled() && !window.findChild<QPushButton*>("fitView")->isEnabled(),
        "3D toolbar waits for input before offering display operations");
    auto* dataDefaults = window.GetModule("Data")->GetParameterEditor("Load");
    const auto defaults = dataDefaults->GetValue().toObject();
    Check(defaults["datasetId"] == "" && defaults["dimensions"].isNull()
        && defaults["spacingLPS"] == QJsonArray{1,1,1}
        && defaults["filePath"] == "", "manual input defaults are independent of a particular CT file and geometry");
    if (GetEnabled(window, "Wall")) {
        const auto wallDefaults = window.GetModule("Wall")->GetParameterEditor("Start")->GetValue().toObject();
        Check(wallDefaults["maxBoundaryError"].isNull() && wallDefaults["sampleSpacing"].isNull() && wallDefaults["maxDistance"].isNull(),
            "wall default lengths are resolved from the selected input instead of fixed test geometry");
    }
    const QRegularExpression chinese("[\\x{4e00}-\\x{9fff}]");
    for (const QString name : {QString("Data"), QString("View"), QString("Crop"), QString("Gap"), QString("Part"),
            QString("Surface"), QString("Artifact"), QString("Rotation"), QString("Alignment"), QString("Wall")}) {
        auto* panel = window.GetModule(name);
        Check(panel && chinese.match(panel->GetDisplayName()).hasMatch(), "module display name is Chinese and stable ID resolves");
        Check(panel->GetCatalogTree() && panel->GetSceneTree() && panel->GetSceneTree() != panel->GetCatalogTree(), "scene objects and result catalog have distinct widgets");
        Check(!panel->findChild<QComboBox*>("operationPicker") && !panel->findChild<QComboBox*>("operationSelector") && !panel->findChild<QPushButton*>("executeOperation"),
            "commands are buttons and never entries in an operation dropdown");
        for (const auto& action : panel->GetActions()) {
            const auto* button = panel->findChild<QPushButton*>("action_" + action);
            Check(button && chinese.match(button->text()).hasMatch(), "direct action button uses Chinese business name");
            panel->SetParameterPatch(action, {});
            const auto params = panel->GetParameters();
            for (auto it = params.begin(); it != params.end(); ++it) {
                const auto* field = panel->GetParameterEditor(action)->GetField(it.key());
                if (!field || !chinese.match(field->accessibleName()).hasMatch())
                    throw std::runtime_error((name + "." + action + ": untranslated parameter " + it.key()).toStdString());
            }
        }
    }
    for (const auto* name : {"Part", "Artifact", "Surface", "Wall"}) {
        auto* page = window.GetModule(name); if (page->GetActions().isEmpty()) continue;
        const QString initial = QString(name) == "Artifact" ? "Ring" : QString(name) == "Surface" ? "GlobalAutomatic" : "Start";
        window.GetWorkflow().onNavigate(name, initial, {});
        auto* groups = page->findChild<QTabBar*>("parameterTabs"); const auto before = window.GetRecords().GetRecords()["records"].toArray().size();
        for (int i=0; i<groups->count(); ++i) if (groups->isTabEnabled(i)) {
            Check(groups->tabData(i) != "Stop" && groups->tabData(i) != "Cancel" && groups->tabData(i) != "Clear", "parameter tabs exclude stop, cancel and clear commands");
            groups->setCurrentIndex(i);
        }
        Check(window.GetRecords().GetRecords()["records"].toArray().size() == before, "parameter group navigation never submits an operation");
        page->SelectAction(initial);
        const auto stop = page->GetActions().contains("Stop") ? QString("Stop") : QString("Cancel");
        page->SelectAction(stop);
        Check(page->GetParameterEditor(initial)->isVisible() && page->findChild<QPushButton*>("action_" + stop)->isVisible(),
            "stop remains a direct button without replacing the active algorithm parameters");
    }
    auto* view = window.GetModule("View");
    auto* form = view->GetParameterEditor("Set");
    auto* quality = form->GetField("quality");
    Check(view->GetParameterEditor("Cursor") && !view->findChild<QPlainTextEdit*>(), "separate operation forms exist without JSON editors");
    Check(form->GetValue().toObject()["quality"].isNull(), "optional enum starts unspecified");
    quality->findChild<QCheckBox*>("specified")->setChecked(true);
    auto* choices = quality->findChild<QComboBox*>("value"); choices->setCurrentIndex(choices->findData("Low"));
    Check(form->GetValue().toObject()["quality"] == "Low" && chinese.match(choices->currentText()).hasMatch(), "Chinese choice keeps protocol value");
    auto* iso = form->GetField("iso"); iso->findChild<QCheckBox*>("specified")->setChecked(true);
    auto* input = iso->findChild<QLineEdit*>("value"); input->setText("-");
    auto* axes = form->GetField("axes"); SetBoolean(axes, true); SetBoolean(axes, false);
    view->SelectAction("Reset");
    Check(view->GetCurrentAction() == "Reset" && view->GetParameters()["viewId"] == "primary-3d", "unfinished numeric input does not block another operation");
    view->SelectAction("Set"); view->SetParameterPatch("Set", {{"opacity", 0.75}});
    Check(iso->findChild<QLineEdit*>("value") == input && input->text() == "-", "switch and unrelated node patch preserve unfinished field without rebuilding");
    Check(choices->currentData() == "Low" && axes->GetValue() == QJsonValue(false), "independent forms preserve enum and explicit false");
    input->setText("0.5"); quality->findChild<QCheckBox*>("specified")->setChecked(false);
    Check(form->GetValue().toObject()["quality"].isNull(), "optional field returns null when unspecified");
    view->SetParameterPatch("Set", {{"viewId", "primary-3d"}, {"quality", QJsonValue()}, {"axes", QJsonValue()}, {"iso", QJsonValue()}});
    view->SelectAction("Reset");

    QJsonArray bulkIds;
    for (int i=0; i<1024; ++i) bulkIds.append(QString::number(9007199254740992ULL+static_cast<unsigned>(i)));
    ParameterEditor bulk("Alignment","SaveRecipe","vertexIds",bulkIds,bulkIds);
    Check(bulk.findChildren<QPlainTextEdit*>().size()==1 && bulk.GetCount()==1024
        && bulk.GetValue()==QJsonValue(bulkIds), "bulk measurement IDs preserve every exact integer string in one editor");
    auto* bulkInput=bulk.findChild<QPlainTextEdit*>("bulkIndices"); bulkInput->setPlainText("[");
    bool malformedRejected=false;
    try { (void)bulk.GetValue(); } catch(const std::invalid_argument&) { malformedRejected=true; }
    Check(malformedRejected,"malformed bulk indices are rejected instead of being truncated");
    const auto statuses=GetExpectedStatuses({{"expectStatuses",QJsonArray{"FullyDetermined","NotConverged"}}});
    Check(statuses.contains("NotConverged") && !statuses.contains("Rejected"),"real quality outcomes do not accept unrelated failures");

    TestRecordWriter records;
    QStringList events;
    records.onChanged = [&](const auto& record) { events.append(GetFlowText(record)); };
    const auto immediate = records.StartRecord("View", "Reset", {}, {});
    records.SetComplete(immediate, "Succeeded", {}, {});
    records.SetAdmission(immediate, {{"isAccepted", true}});
    Check(events.size() == 2 && events[0].contains("提交操作") && events[1].contains("操作成功"),
        "synchronous completion is logged once before late admission metadata");
    const auto rejected = records.StartRecord("Data", "Load", {}, {});
    records.SetAdmission(rejected, {{"expectedBindingRevision", "1"}});
    records.SetAdmission(rejected, {{"isAccepted", false}});
    Check(records.GetRecord(rejected)["status"] == "Sending", "metadata and rejection never imply accepted work");
    records.SetComplete(rejected, "Rejected", {{"message", "测试拒绝原因"}}, {});
    Check(events.last().contains("测试拒绝原因"), "business failure reason is visible");
    const auto pending = records.StartRecord("Artifact", "Diffusion", {}, {});
    records.SetAdmission(pending, {{"isAccepted", true}});
    const auto eventCount = events.size();
    records.SetProgress(pending, 20); records.SetProgress(pending, 20);
    Check(events.size() == eventCount + 1 && events.last().contains("20%"), "unchanged progress does not flood the business log");
    records.SetComplete(pending, "Ready", {}, {});
    records.SetProgress(pending, 100);
    Check(events.last().contains("发布校正结果"), "terminal log explains next business action and rejects late progress");
    const auto loaded = records.StartRecord("Data", "Load", {}, {});
    records.SetRendered("primary-3d", "old:1", 10);
    records.SetRendered("primary-3d", "loaded:1", 20);
    records.SetComplete(loaded, "Succeeded", {}, {{"dataRevision", "loaded:1"}});
    records.SetRendered("slice-top-down", "loaded:1", 30);
    records.SetRendered("primary-3d", "other:1", 40);
    const auto duplicate = records.StartRecord("Data", "Load", {}, {});
    records.SetComplete(duplicate, "Rejected", {}, {{"dataRevision", "loaded:1"}});
    records.SetRendered("slice-left-right", "loaded:1", 50);
    const auto measured = records.GetRecord(loaded)["firstRenderedViews"].toObject();
    Check(measured.size() == 3 && measured["primary-3d"].toObject()["dataRevision"] == "loaded:1"
        && measured["primary-3d"].toObject()["renderCallMs"] == .02
        && measured["slice-top-down"].toObject()["renderCallMs"] == .03,
        "first rendering survives completion order and ignores later unrelated revisions");
    Check(records.GetMeasurements()["load"].toObject()["operationId"] == QString::number(loaded),
        "rejected duplicate load does not hide measurements for the loaded input");
}
void CheckSceneRefresh(TestWindow& window)
{
    const auto node = [](const char* id, const QJsonArray& parents) { return QJsonObject{{"id", id}, {"parents", parents}}; };
    const auto linear = GetGraphLayout({node("c", {"b"}), node("b", {"a"}), node("a", {})});
    Check(linear.lanes == 1 && linear.rows.size() == 3, "linear history uses exactly one graph lane");
    const auto branched = GetGraphLayout({node("merge", {"left", "right"}), node("left", {"base"}), node("right", {"base"}), node("base", {})});
    Check(branched.lanes > 1 && branched.edges.size()==4 && branched.edges.contains(QJsonObject{{"child","merge"},{"parent","left"}}) && branched.edges.contains(QJsonObject{{"child","merge"},{"parent","right"}})
        && branched.edges.contains(QJsonObject{{"child","left"},{"parent","base"}}) && branched.edges.contains(QJsonObject{{"child","right"},{"parent","base"}}),
        "real split and merge retain both routes and converge on their shared parent");
    const auto ordinary = GetGraphLayout({QJsonObject{{"id", "views"}, {"children", QJsonArray{QJsonObject{{"id", "one"}}, QJsonObject{{"id", "two"}}}}}});
    Check(ordinary.rows.isEmpty(), "ordinary UI hierarchy is never mistaken for a business branch");
    // 合成的两千零件目录只验证 UI 更新成本与节点身份，不冒充算法/真实 CT 验收。
    TestRecordWriter records; TestWorkflow workflow;
    ModulePanel panel({window.GetModule("View")->GetContext().runtime, workflow, records}, "Part");
    panel.AttachAction("Merge", {}, [](auto, const auto&) {});
    QJsonArray parts;
    for (int i = 0; i < 2000; ++i) parts.append(QJsonObject{{"binding", QJsonObject{{"partId", QString::number(i)}}},
        {"labelId", QString::number(i)}, {"name", "零件"}, {"voxelCount", "100"}});
    QJsonObject state{{"hasCurrentParts", true}, {"parts", parts}};
    panel.SetState(state);
    auto* tree = panel.GetCatalogTree();
    QTreeWidgetItem* part = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject().contains("binding")) { part = *it; break; }
    Check(part != nullptr, "large scene catalog contains selectable parts");
    tree->setCurrentItem(part); part->setSelected(true);
    int changes = 0, resets = 0, removals = 0;
    QObject::connect(tree->model(), &QAbstractItemModel::dataChanged, &panel, [&] { ++changes; });
    QObject::connect(tree->model(), &QAbstractItemModel::modelReset, &panel, [&] { ++resets; });
    QObject::connect(tree->model(), &QAbstractItemModel::rowsRemoved, &panel, [&] { ++removals; });
    records.StartTiming();
    for (int i = 0; i < 100; ++i) panel.SetState(state);
    Check(changes == 0 && resets == 0 && removals == 0, "identical large scene states cause zero model changes or resets");
    const auto identical = records.GetTimings(); records.StartTiming();
    auto changed = parts[0].toObject(); changed["name"] = "更新后的零件"; parts[0] = changed; state["parts"] = parts;
    panel.SetState(state);
    Check(resets == 0 && removals == 0 && tree->currentItem() == part && part->isSelected() && part->text(0).startsWith("更新后的零件"),
        "status/name update preserves actual scene item and selection without rebuilding");
    ExportJson("scene-refresh-performance.json", QJsonObject{{"fixture", "synthetic 2000 parts"}, {"repeatedState", identical}, {"changedName", records.GetTimings()}});
}
void CheckIdle(TestWindow& window)
{
    // 单次观测窗口是空闲调度测试的时间边界，不执行状态刷新。
    QEventLoop settle; QTimer::singleShot(150, &settle, &QEventLoop::quit); settle.exec();
    const auto updates = window.GetUpdateCount(), renders = window.GetRenderCount();
    QEventLoop idle; QTimer::singleShot(350, &idle, &QEventLoop::quit); idle.exec();
    Check(window.GetUpdateCount() == updates && window.GetRenderCount() == renders,
        "idle application performs zero Host updates and zero render requests");
}
QTreeWidgetItem* FindNode(QTreeWidget* tree, const QString& id)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject()["id"] == id) return *it;
    return nullptr;
}
void SaveScene(TestWindow& window, QTreeWidget* tree, const QString& path)
{
    Q_UNUSED(window);
    tree->expandAll(); tree->scrollToTop(); QCoreApplication::processEvents();
    tree->grab().save(path);
}
void ClickPartNode(TestWindow& window, int index)
{
    // 必须经过实际树事件；仅调用 SetState 会漏掉节点向无参操作传参导致的退出。
    auto* panel = window.GetModule("Part");
    Check(panel != nullptr, "part page exists for node selection");
    window.GetWorkflow().onNavigate("Part", "Catalog", {}); panel->Observe();
    const auto parts = panel->GetObservedState()["parts"].toArray();
    Check(index >= 0 && index < parts.size(), "part node index belongs to current catalog");
    const auto binding = parts[index].toObject()["binding"].toObject();
    const auto key = "part:" + QString::fromUtf8(QJsonDocument(binding).toJson(QJsonDocument::Compact));
    auto* tree = panel->GetCatalogTree();
    auto* node = FindNode(tree, key); Check(node != nullptr, "current part has a scene node");
    const auto count = window.GetRecords().GetRecords()["records"].toArray().size();
    ClickNode(tree, node);
    Check(panel->GetCurrentAction() == "Highlight"
        && panel->GetParameterEditor("Highlight")->GetField("target")->GetValue() == binding
        && panel->GetParameterEditor("ClearHighlight")->GetField("target")->GetValue() == binding
        && panel->GetParameterEditor("EditSelected")->GetField("target")->GetValue() == binding
        && panel->GetParameterEditor("SetState")->GetField("target")->GetValue() == binding,
        "clicking a part node binds the exact object to editing and state operations");
    Check(window.GetRecords().GetRecords()["records"].toArray().size() == count,
        "selecting a part node does not execute a business operation");
}
void CheckPartHighlightSwitches(TestWindow& window, int count)
{
    auto* panel = window.GetModule("Part");
    window.SetViewsVisible(true);
    std::map<std::string, QImage> previousImages;
    Check(count > 0 && count <= 1024, "highlight switch count is bounded");
    for (int i = 0; i < count; ++i) {
        const int index = i % 2;
        ClickPartNode(window, index);
        const QJsonObject binding = panel->GetObservedState()["parts"].toArray()[index].toObject()["binding"].toObject();
        Check(Wait([&] { return !window.GetWorkflow().getRenderPending("primary-3d"); }),
            "previous view work settles before the next highlight");
        auto* button = panel->findChild<QPushButton*>("action_Highlight");
        Check(button && button->isVisible() && button->isEnabled(), "selected-node state button is actionable");
        const auto before = window.GetRecords().GetRecords()["records"].toArray().size();
        const auto rendersBefore = window.GetRenderCount();
        // 保留所选节点上下文；通用 Click helper 会先切回根节点，不能复现此用户路径。
        button->click();
        Check(window.GetRecords().GetRecords()["records"].toArray().size() == before + 1,
            "highlight button submits exactly one operation from the selected node");
        const auto record = GetComplete(window, static_cast<std::uint64_t>(before + 1), "Succeeded");
        Check(record["parameters"].toObject()["target"] == binding && record["result"].toObject()["isSelected"] == true,
            "highlight request uses the clicked node without requiring a checkbox or resending false");
        panel->Observe(); int selectedCount = 0;
        for (const auto& value : panel->GetObservedState()["parts"].toArray()) {
            const auto part = value.toObject(); if (!part["selected"].toBool()) continue;
            ++selectedCount; Check(part["binding"] == binding, "highlight belongs to the newly clicked part");
        }
        Check(selectedCount == 1, "continuous node highlight switching keeps exactly one selected part");
        Check(Wait([&] { return window.GetRenderCount() > rendersBefore
            && !window.GetWorkflow().getRenderPending("primary-3d"); }, 5000),
            "highlight-only change triggers rendering without cursor or camera changes");
        for (const auto* viewId : {"primary-3d"}) {
            Check(Wait([&] { return !window.GetWorkflow().getRenderPending(viewId); }), "highlight view finishes rendering");
            auto& previousImage = previousImages[viewId];
            for (auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>())
            if (widget->renderWindow() == window.GetSession()->GetRenderViewEndpoint(viewId)->renderWindow) {
                const auto currentImage = widget->grab().toImage();
                currentImage.save(QString("out/part-volume-highlight-%1-%2.png").arg(viewId).arg(i));
                Check(previousImage.isNull() || currentImage != previousImage,
                    "consecutive different highlights change the actual framebuffer");
                previousImage = currentImage;
            }
        }
    }
}
QJsonObject BuildPartSeeds(TestWindow& window, const QJsonObject& catalog)
{
    std::optional<LabelMapDescriptor> descriptor;
    for (const auto& item : window.GetSession()->GetLabelMapDescriptors())
        if (GetRefText(item.dataRevision) == catalog["labelMap"].toString()) descriptor = item;
    Check(descriptor && descriptor->valueType == ImageValueType::UInt32, "formal part labels are available through the public read API");
    QJsonArray seeds; const auto parts = catalog["parts"].toArray();
    Check(!parts.isEmpty(), "real edit seed audit has source parts");
    for (int p = 0; p < std::min(parts.size(), 2); ++p) {
        const auto part = parts[p].toObject(); const auto extent = GetArray<int,6>(part["extent"]);
        const auto wantedLabel = GetId(part["labelId"]);
        ImageReadRegion region;
        for (int a = 0; a < 3; ++a) { region.offset[a] = static_cast<std::size_t>(extent[a*2]-descriptor->extent[a*2]); region.size[a] = static_cast<std::size_t>(extent[a*2+1]-extent[a*2]+1); }
        LabelMapReadRequest request; request.id = descriptor->id; request.expectedRevision = descriptor->dataRevision;
        request.region = region; request.maxBytes = 1024U*1024U;
        std::size_t offset = 0; std::optional<std::array<int,3>> seed;
        while (!seed) {
            const auto chunk = window.GetSession()->GetLabelMapReadChunk(request, offset);
            Check(chunk.error == LabelMapError::None && chunk.state && chunk.state->values, "read actual source labels in bounded chunks");
            Check(chunk.state->voxelCount <= request.maxBytes/sizeof(std::uint32_t)
                && chunk.state->values->size() == chunk.state->voxelCount*sizeof(std::uint32_t), "label chunk byte count matches its typed values");
            for (std::size_t i = 0; i < chunk.state->voxelCount; ++i) {
                std::uint32_t label = 0; std::memcpy(&label, chunk.state->values->data()+i*sizeof(label), sizeof(label));
                if (label != wantedLabel) continue;
                const auto local = chunk.state->voxelOffset+i;
                seed = std::array<int,3>{extent[0]+static_cast<int>(local%region.size[0]),
                    extent[2]+static_cast<int>((local/region.size[0])%region.size[1]),
                    extent[4]+static_cast<int>(local/(static_cast<std::size_t>(region.size[0])*region.size[1]))};
                break;
            }
            if (seed || chunk.isDone) break;
            Check(chunk.nextVoxelOffset > offset, "bounded label reader advances"); offset = chunk.nextVoxelOffset;
        }
        Check(seed.has_value(), "seed belongs to the exact requested part label, not an assumed centroid");
        seeds.append(QJsonObject{{"binding", part["binding"]}, {"labelId", part["labelId"]}, {"seed", GetValues(*seed)}});
    }
    return {{"source", GetRefText(descriptor->sourceRevision)}, {"labelMap", GetRefText(descriptor->dataRevision)},
        {"dimensions", GetValues(descriptor->dims)}, {"voxelCount", QString::number(descriptor->voxelCount)}, {"parts", seeds}};
}
QJsonObject CheckPartDirectories(TestWindow& window)
{
    auto* panel = window.GetModule("Part"); panel->Observe();
    const auto parts = panel->GetObservedState()["parts"].toArray(); Check(parts.size() >= 2, "directory audit has two real parts");
    const auto a = parts[0].toObject()["binding"].toObject(), b = parts[1].toObject()["binding"].toObject();
    auto* tree = panel->GetCatalogTree();
    const auto nodeId = [](const QString& prefix, const QJsonObject& binding) { return prefix+QString::fromUtf8(QJsonDocument(binding).toJson(QJsonDocument::Compact)); };
    auto* all = FindNode(tree, "parts-all"); auto* first = FindNode(tree, nodeId("part:",a));
    int resets = 0; QObject receiver;
    QObject::connect(tree->model(), &QAbstractItemModel::modelReset, &receiver, [&] { ++resets; });
    ClickPartNode(window, 0);
    const auto id = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()+1);
    panel->findChild<QPushButton*>("action_EditSelected")->click(); GetComplete(window, id, "ParametersCopied");
    auto* edit = window.GetModule("Part");
    for (const auto* action : {"Paint","Erase","Fill","Island","Grow","Split"})
        Check(edit->GetParameterEditor(action)->GetField("target")->GetValue() == a, "entering edit binds every tool to the exact chosen scene object");
    ClickPartNode(window, 1);
    const auto highlightId = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()+1);
    panel->findChild<QPushButton*>("action_Highlight")->click(); GetComplete(window, highlightId, "Succeeded"); panel->Observe();
    auto* highlights = FindNode(tree, "part-highlights"); auto* editing = FindNode(tree, "part-edit-targets");
    Check(highlights && highlights->childCount() == 1 && highlights->child(0)->data(0, Qt::UserRole).toJsonObject()["binding"] == b
        && editing && editing->childCount() == 1 && editing->child(0)->data(0, Qt::UserRole).toJsonObject()["binding"] == a,
        "highlight and actual editing targets remain in separate truthful directories");
    Check(FindNode(tree,"parts-all") == all && FindNode(tree,nodeId("part:",a)) == first && resets == 0,
        "highlight projection changes preserve the complete catalog items without model reset");
    auto* search = panel->findChild<QLineEdit*>("nodeSearch"); search->setText(first->text(0));
    Check(!first->isHidden() && FindNode(tree,nodeId("part:",b))->isHidden(), "part name/label search filters scene items");
    panel->findChild<QPushButton*>("focus_part-highlights")->click();
    Check(search->text().isEmpty() && tree->currentItem()->data(0, Qt::UserRole).toJsonObject()["binding"] == b
        && tree->viewport()->rect().intersects(tree->visualItemRect(tree->currentItem())), "highlight locator reveals the actual selected part in the viewport");
    window.GetWorkflow().onNavigate("Part","Paint",{{"target",a}}); edit->Observe();
    auto* editTree = edit->GetCatalogTree();
    ClickNode(editTree,FindNode(editTree,nodeId("editing-part:",a)));
    ClickNode(editTree,FindNode(editTree,nodeId("part:",a)),Qt::ControlModifier);
    Check(edit->GetParameterEditor("Merge")->GetField("parts")->GetValue().toArray().size() == 1,
        "two projections of one part cannot become two merge sources or retain stale merge targets");
    GetComplete(window, Send(window,"Part","ClearHighlight",{{"target",b}}), "Succeeded");
    return {{"partCount",parts.size()},{"modelResets",resets},{"editingTarget",a},{"highlightTarget",b}};
}
QJsonObject CheckPartEditPreview(TestWindow& window, const QJsonObject& spec)
{
    auto* panel = window.GetModule("Part"); panel->Observe(); const auto state = panel->GetObservedState();
    Check(state["hasPreview"].toBool(), "real edit has an unpublished candidate");
    const auto base = spec["base"].toObject(); const auto parts = base["parts"].toArray();
    QMap<QString,QJsonObject> before;
    const auto key = [](QJsonObject binding) { binding.remove("resultRevision"); return QString::fromUtf8(QJsonDocument(binding).toJson(QJsonDocument::Compact)); };
    qint64 total = 0;
    for (const auto value : parts) { const auto p = value.toObject(); before[key(p["binding"].toObject())] = p; total += static_cast<qint64>(GetId(p["voxelCount"])); }
    const auto previousTotal = total; const auto changes = state["previewChanges"].toObject();
    for (const auto value : changes["changed"].toArray()) {
        const auto p = value.toObject(); const auto old = before.value(key(p["binding"].toObject()));
        if (!old.isEmpty()) total -= static_cast<qint64>(GetId(old["voxelCount"]));
        total += static_cast<qint64>(GetId(p["voxelCount"]));
    }
    for (const auto value : changes["removed"].toArray()) total -= static_cast<qint64>(GetId(value.toObject()["voxelCount"]));
    Check(static_cast<int>(GetId(state["candidatePartCount"])) == spec["partCount"].toInt()
        && changes["changed"].toArray().size() == spec["changed"].toInt()
        && changes["removed"].toArray().size() == spec["removed"].toInt()
        && total-previousTotal == static_cast<qint64>(spec["foregroundDelta"].toDouble()),
        "real edit changes exactly the expected parts and foreground voxel ownership");
    window.GetModule("Part")->Observe();
    Check(window.GetModule("Part")->GetObservedState()["labelMap"] == base["labelMap"], "preview preserves formal label revision");
    auto* tree = panel->GetCatalogTree();
    auto* group = FindNode(tree,"edit-preview:"+state["previewId"].toString());
    Check(group && group->childCount() == changes["changed"].toArray().size()+changes["removed"].toArray().size(),
        "candidate directory lists only changed and removed objects");
    for (int i = 0; i < group->childCount(); ++i) {
        const auto node = group->child(i)->data(0, Qt::UserRole).toJsonObject();
        Check(!node.contains("binding") && node["actions"] == QJsonArray{"Commit","Discard"}, "candidate nodes cannot be used as formal edit inputs");
    }
    return {{"formalLabelMap",base["labelMap"]},{"foregroundBefore",QString::number(previousTotal)},
        {"foregroundCandidate",QString::number(total)},{"state",state}};
}
void CheckNodeParameters(TestWindow& window)
{
    for (const auto& name : {"Data", "View", "Crop", "Gap", "Part", "Surface", "Artifact", "Rotation", "Alignment"}) {
        auto* panel = window.GetModule(name); if (!panel) continue;
        auto* tree = panel->GetCatalogTree();
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const auto patches = (*it)->data(0, Qt::UserRole).toJsonObject()["patches"].toObject();
            for (auto action = patches.begin(); action != patches.end(); ++action) {
                if (!panel->GetActions().contains(action.key())) continue;
                auto* form = panel->GetParameterEditor(action.key());
                for (const auto& key : action.value().toObject().keys())
                    if (!form || !form->GetField(key)) throw std::runtime_error((QString(name)+" / "+action.key()+" does not accept scene parameter "+key).toStdString());
            }
        }
    }
    Check(true, "scene node parameters match their actual operation forms");
}
void CheckNodeErrorBoundary(TestWindow& window)
{
    TestRecordWriter records; TestWorkflow workflow;
    ModulePanel panel({window.GetModule("View")->GetContext().runtime, workflow, records}, "Part");
    panel.AttachAction("Catalog", {}, [](auto, const auto&) {});
    panel.AttachAction("AState", {{"target", "original-target"}}, [](auto, const auto&) {});
    const QJsonObject state{{"hasCurrentParts", true}, {"parts", QJsonArray{}}, {"partCount", 0}};
    panel.SetState(state); QString message; panel.onMessage = [&](const QString& value) { message = value; };
    auto* tree = panel.GetCatalogTree(); auto* item = tree->topLevelItem(0);
    auto node = item->data(0, Qt::UserRole).toJsonObject();
    node["patches"] = QJsonObject{{"AState", QJsonObject{{"target", "wrong-target"}}}, {"Catalog", QJsonObject{{"target", "invalid-node-parameter"}}}};
    item->setData(0, Qt::UserRole, node);
    tree->itemClicked(item, 0);
    Check(message.contains("此操作不需要参数") && panel.GetObservedState() == state
        && panel.GetParameterEditor("AState")->GetField("target")->GetValue() == "original-target"
        && records.GetRecords()["records"].toArray().isEmpty(),
        "invalid scene metadata is rejected before changing any target, without losing business state or executing actions");
}
std::uint64_t ClickNodeAction(TestWindow& window, QTreeWidget* tree, QTreeWidgetItem* item, const QString& action)
{
    const auto nodeId=item->data(0,Qt::UserRole).toJsonObject()["id"].toString();
    auto* pages=window.findChild<QStackedWidget*>("parameterPages");
    ModulePanel* owner=nullptr;if(pages)for(int i=0;i<pages->count();++i)if(auto* panel=dynamic_cast<ModulePanel*>(pages->widget(i)))if(panel->GetCatalogTree()==tree){owner=panel;break;}
    Check(owner!=nullptr,"scene context menu has an actual owning business page");
    // 后台 Send 不切页面；右键必须先走真实导航，不能向隐藏历史树伪造点击。
    window.GetWorkflow().onNavigate(owner->GetName(),action,{});QCoreApplication::processEvents();
    // 工作台的来源/历史已位于视图区；先结束“隐藏视图也可提交输入”的独立场景。
    window.SetViewsVisible(true);
    item=FindNode(tree,nodeId);Check(item!=nullptr,"scene context target survives business-page navigation");
    ClickNode(tree, item); tree->scrollToItem(tree->currentItem()); tree->doItemsLayout();
    Check(tree->isVisible(),"scene context menu uses a visible history tree");
    const auto point = tree->visualItemRect(tree->currentItem()).center();
    const auto clicked = std::make_shared<bool>(false);
    const auto id = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()) + 1;
    QMetaObject::invokeMethod(&window, [clicked, action] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto* entry : menu->actions()) if (entry->objectName() == "node_" + action) { *clicked = true; entry->trigger(); break; }
        menu->close();
    }, Qt::QueuedConnection);
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point));
    QApplication::sendEvent(tree->viewport(), &event);
    if(!*clicked) {
        const auto* hit=tree->itemAt(point);
        std::cerr<<"Scene menu diagnostic: visible="<<tree->isVisible()<<" viewport="<<tree->viewport()->width()<<'x'<<tree->viewport()->height()
            <<" point="<<point.x()<<','<<point.y()<<" action="<<action.toStdString()
            <<" hit="<<(hit?GetJsonText(hit->data(0,Qt::UserRole).toJsonObject()).toStdString():"none")<<std::endl;
    }
    Check(*clicked, "actual scene context menu dispatches requested node operation");
    return id;
}
void CheckBusinessParameters(TestWindow& window)
{
    if (auto* undo = window.GetModule("Part")->GetParameterEditor("Undo")) {
        Check(!undo->GetHasInputs() && !undo->GetField("target") && !undo->GetField("extent") && !undo->GetField("protectedParts"), "history undo has no unused editing target or scope controls");
        Check(!window.GetModule("Part")->GetParameterEditor("Merge")->GetField("target"), "merge does not expose an unused single target");
    }
    if (auto* ring = window.GetModule("Artifact")->GetParameterEditor("Ring")) {
        Check(ring->GetField("ring") && !ring->GetField("diffusion") && !ring->GetField("source")->isHidden()
            && ring->GetField("source")->findChild<QComboBox*>("value"), "ring algorithm and selectable published source remain separate inputs");
        auto* diffusion = window.GetModule("Artifact")->GetParameterEditor("Diffusion");
        Check(diffusion->GetField("diffusion") && !diffusion->GetField("ring"), "diffusion card has no ring parameters");
    }
    if (auto* material = window.GetModule("Surface")->GetParameterEditor("GlobalAutomatic")) {
        Check(material->GetField("materialRange") && material->GetField("initialIsoValue") &&
              !material->GetField("profileHalfLengthModel"), "material form has calibration and no retired fitting controls");
        bool rejected=false;
        try {window.GetModule("Surface")->SetParameters("GlobalAutomatic", {{"localFraction", 0.5}});}
        catch(const std::invalid_argument&) {rejected=true;}
        Check(rejected, "retired fitting parameters are rejected without changing the material form");
    }
    if (auto* node = window.GetModule("Crop")->GetParameterEditor("Node")) Check(!node->GetHasInputs(), "crop history position is supplied by selected node without a numeric form");
}
void CheckParameterLayout(TestWindow& window)
{
    window.GetWorkflow().onNavigate("View", "Set", {}); QCoreApplication::processEvents();
    auto* view = window.GetModule("View"); auto* form = view->GetParameterEditor("Set");
    auto* scroll = view->findChild<QScrollArea*>("operationScroll");
    Check(scroll && scroll->height() > 96 && view->GetBrowser()->parentWidget() != view,
        "parameters occupy a separate full-height column beside the scene browser");
    auto* commandArea = view->findChild<QStackedWidget*>("operationCommands");
    auto* execute = view->findChild<QPushButton*>("action_Set");
    auto* primary=view->findChild<QPushButton*>("businessPrimary");
    Check(primary && !scroll->isAncestorOf(primary) && commandArea && commandArea->isAncestorOf(execute),
        "stage primary remains outside the parameter scroll area; advanced command belongs to its form");
    auto* operationTabs = view->findChild<QTabBar*>("parameterTabs");
    auto* more = view->findChild<QToolButton*>("moreOperations");
    Check(operationTabs && operationTabs->usesScrollButtons() && operationTabs->count() <= 5 && more && more->menu(),
        "common operations fit a compact row and all additional operations have a discoverable menu");
    const auto originalSize = window.size(); QCoreApplication::processEvents();
    const auto commandBounds = QRect(primary->mapTo(view,QPoint()),primary->size());
    Check(window.size() == originalSize && primary->isVisible() && view->rect().contains(commandBounds),
        "the stage primary fits inside the fixed parameter column");
    window.grab().save("manual-layout-1280.png");
    QCoreApplication::processEvents();
    const auto canvasSize = window.findChild<QWidget*>("primary3D")->size();
    window.findChild<QPushButton*>("showResultReport")->click(); QCoreApplication::processEvents();
    auto* reportWindow = window.findChild<QDialog*>("operationReports");
    Check(reportWindow && reportWindow->isVisible() && window.findChild<QWidget*>("primary3D")->size() == canvasSize,
        "result report opens independently without reducing the four-view canvas");
    reportWindow->hide();
    Check(form->isVisible() && !view->GetParameterEditor("Cursor")->isVisible(), "only the selected operation form is expanded");
    auto* windowLevel = form->GetField("windowLevel"); windowLevel->findChild<QCheckBox*>("specified")->setChecked(true);
    windowLevel->GetElement(0)->findChild<QLineEdit*>("value")->setText("1200");
    windowLevel->GetElement(1)->findChild<QLineEdit*>("value")->setText("600");
    Check(windowLevel->GetValue() == QJsonValue(QJsonArray{1200,600}), "separate window width and level fields assemble exact request values");
    const auto beforeNavigation = window.GetRecords().GetRecords()["records"].toArray().size();
    auto* resetNavigation = more->menu()->findChild<QAction*>("navigate_Reset");
    Check(resetNavigation && resetNavigation->isEnabled(), "additional parameter operations remain accessible through the more menu");
    resetNavigation->trigger(); QCoreApplication::processEvents();
    Check(view->GetParameterEditor("Reset")->isVisible()
        && window.GetRecords().GetRecords()["records"].toArray().size() == beforeNavigation,
        "more menu parameter navigation opens its form without submitting a command");
    view->SelectAction("Set"); QCoreApplication::processEvents();
    Check(windowLevel->GetValue() == QJsonValue(QJsonArray{1200,600}), "more menu navigation preserves the edited parameter draft");
    auto* visibility = view->GetParameterEditor("Visibility");
    auto* planes = visibility->GetField("planes");
    SetBoolean(planes, true); SetBoolean(planes, false);
    Check(visibility->GetValue().toObject()["planes"] == QJsonValue(false), "auxiliary display card retains explicit false");
    view->SetParameterPatch("Set", {{"windowLevel", QJsonValue()}});
    view->SetParameterPatch("Visibility", {{"planes", QJsonValue()}});
    view->SetParameterPatch("Set", {{"viewId", "primary-3d"}}); view->SelectAction("Set");
    const auto direct = GetComplete(window, SendUnavailable(window, "View", "Set"), "Rejected");
    Check(direct["action"] == "Set" && direct["parameters"].toObject()["viewId"] == "primary-3d",
        "disabled display command still rejects programmatic input before data is loaded");
    view->SetParameterPatch("Set", {{"viewId", "primary-3d"}});

    const auto identity = QJsonArray{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    ParameterEditor poses("Alignment", "Start", "initialPoses", QJsonArray{QJsonValue(identity)}, QJsonArray{QJsonValue(identity)});
    auto* matrix = poses.GetElement(0);
    auto* grid = matrix->findChild<QGridLayout*>("matrixGrid");
    Check(grid && matrix->findChildren<QLineEdit*>("value").size() == 16, "pose is displayed as a complete 4 by 4 matrix");
    for (int i = 0; i < 16; ++i) {
        Check(!matrix->GetElement(i)->findChild<QLabel*>("parameterTitle"), "matrix cells have no repetitive row and column headings");
        Check(grid->itemAtPosition(i/4, i%4+1)->widget() == matrix->GetElement(i), "matrix cells retain their row and column positions");
    }
    ParameterEditor direction("Data", "Load", "directionLPS", QJsonArray{1,0,0,0,1,0,0,0,1}, QJsonArray{1,0,0,0,1,0,0,0,1});
    Check(direction.findChild<QGridLayout*>("matrixGrid") && direction.GetCount() == 9, "direction uses the same compact 3 by 3 matrix");
    ParameterEditor optional("Roi", "Begin", "boxToSource", QJsonValue(), QJsonValue());
    Check(optional.GetValue().isNull(), "unspecified transform matrix keeps the business default");
    optional.findChild<QCheckBox*>("specified")->setChecked(true);
    Check(optional.GetValue() == QJsonValue(identity), "enabling the optional transform supplies the identity matrix");
    const auto invalidMatrix = [&](QJsonArray values) {
        try { (void)GetAffineMatrix(values); return false; } catch (const std::invalid_argument&) { return true; }
    };
    auto singular = identity; singular[10] = 0;
    auto projective = identity; projective[12] = 1;
    Check(invalidMatrix(singular) && invalidMatrix(projective), "manual matrix validation rejects singular and non-affine input before request dispatch");
    auto shear = identity; shear[1] = .25;
    Check(GetAffineMatrix(shear)[1] == .25, "valid affine shear is preserved without forcing a rigid transform");
    bool rigidRejected = false;
    try { (void)GetAffineMatrix(shear,true); } catch (const std::invalid_argument&) { rigidRejected = true; }
    Check(rigidRejected, "alignment pose validation rejects non-rigid input in the manual layer");
    ParameterEditor adaptive("Part","Start","threshold",QJsonValue(),QJsonValue());
    adaptive.SetDefaultValue(.0005);
    Check(adaptive.GetValue().isNull() && adaptive.findChild<QLineEdit*>("value")->text().toDouble() == .0005,
        "automatic threshold is visible while its omission remains distinct from explicit input");
    adaptive.SetValue(.05); adaptive.findChild<QLineEdit*>("value")->setText("-"); adaptive.SetDefaultValue(500.);
    Check(adaptive.findChild<QLineEdit*>("value")->text() == "-", "input changes never overwrite an unfinished explicitly supplied threshold");
    ImageDescriptor largeRange, signedRange;
    largeRange.scalarRange = {0.,1000.}; signedRange.scalarRange = {-.12,.04};
    signedRange.spacing = {.02125,.0425,.085}; signedRange.dims = {10,20,30};
    Check(GetInputNumber({{"threshold",QJsonValue()}},"threshold",GetScalarMidpoint(largeRange)) == 500.
        && std::abs(GetInputNumber({{"threshold",QJsonValue()}},"threshold",GetScalarMidpoint(signedRange)) + .04) < 1e-12
        && GetInputNumber({{"threshold",0.}},"threshold",GetScalarMidpoint(signedRange)) == 0.,
        "default thresholds adapt to unsigned and signed gray ranges while explicit zero stays explicit");
    Check(GetVoxelSpacing(signedRange) == .02125 && GetInputDiagonal(signedRange) > 2.55,
        "default measurement lengths use the input geometry rather than one CT fixture");
    auto* dataForm = window.GetModule("Data")->GetParameterEditor("Load");
    auto* grouped = dataForm->findChild<QWidget*>("parameterGroups");
    Check(grouped && grouped->findChildren<QToolButton*>("sectionToggle").size() >= 2, "data input and geometry use named collapsible sections");
    const auto groupValues = window.GetModule("Data")->GetParameterEditor("Load")->GetValue();
    const auto groupRecordCount = window.GetRecords().GetRecords()["records"].toArray().size();
    int edits = 0; const auto previousCallback = dataForm->onEdited;
    dataForm->onEdited = [&] { ++edits; };
    for (auto* section : grouped->findChildren<QToolButton*>("sectionToggle")) { section->click(); section->click(); }
    dataForm->onEdited = previousCallback;
    Check(edits == 0, "section folding never emits parameter edits");
    Check(window.GetModule("Data")->GetParameterEditor("Load")->GetValue() == groupValues
        && window.GetRecords().GetRecords()["records"].toArray().size() == groupRecordCount,
        "parameter group navigation preserves all values and never dispatches a request");
    // 使用真实字段构造检查窄栏；不依赖整窗分割器把参数列撑宽。
    for (const auto& sample : {std::pair<QString,QString>{"Data", "Load"}, {"View", "Set"}, {"Gap", "SetDisplay"}}) {
        auto* source = window.GetModule(sample.first)->GetParameterEditor(sample.second); if (!source) continue;
        QScrollArea narrow; narrow.setStyleSheet(GetWorkbenchStyle()); narrow.setWidgetResizable(true); narrow.resize(340,600);
        auto* editor = new ParameterEditor(sample.first,sample.second,{},source->GetValue(),source->GetValue()); narrow.setWidget(editor);
        if (auto* segments = editor->GetField("segments")) {
            if (auto* add = segments->findChild<QPushButton*>("addRow")) add->click();
        }
        narrow.show(); QCoreApplication::processEvents();
        Check(Wait([&] { return narrow.horizontalScrollBar()->maximum() == 0; },2000), "narrow parameter samples fit without horizontal scrolling");
        narrow.hide();
    }
    const QJsonObject report{{"operationId","7"},{"module","Roi"},{"action","Begin"},{"status","Succeeded"},
        {"parameters",QJsonObject{{"matrix",identity},{"name","含逗号,引号\"与\n换行"}}},{"result",QJsonObject{{"value",.05}}},{"source",QJsonObject{} },
        {"current",QJsonObject{{"dims",QJsonArray{32,32,32}}}}};
    const auto csv = GetReportCsv(report);
    Check(csv.contains("matrix[4,4]") && csv.contains("current.dims[0]") && csv.contains("\"含逗号,引号\"\"与\n换行\""),
        "CSV report includes post-operation input, matrix row/column indices, and escaped commas, quotes and newlines");
    QTemporaryDir reportDirectory; Check(reportDirectory.isValid(), "report export directory exists");
    const auto reportPath = reportDirectory.filePath("结果报告.csv"); ExportReportCsv(reportPath,report);
    QFile reportFile(reportPath); Check(reportFile.open(QIODevice::ReadOnly), "CSV report can be read back");
    const auto reportBytes = reportFile.readAll();
    Check(reportBytes.startsWith(QByteArray("\xef\xbb\xbf")) && reportBytes.mid(3) == csv.toUtf8(), "CSV export preserves Chinese content and all report rows");
    ResultReport reportView;
    auto firstRecord = report; firstRecord["isTerminal"] = true;
    auto secondRecord = firstRecord; secondRecord["operationId"] = "8";
    QJsonArray largeReport; for (int index = 0; index < 1010; ++index) largeReport.append(index);
    secondRecord["result"] = QJsonObject{{"rows",largeReport}};
    reportView.SetRecord(firstRecord); reportView.SetRecord(secondRecord);
    Check(reportView.findChild<QComboBox*>("reportOperation")->count() == 2
        && reportView.GetSelectedRecord()["operationId"] == "8", "report stores terminal operations and selects the latest completion");
    reportView.show(); QCoreApplication::processEvents();
    bool reportLimited = false;
    for (auto* table : reportView.findChildren<QTableWidget*>("reportTable")) reportLimited = reportLimited || table->rowCount() == 1000;
    Check(reportLimited && GetReportCsv(reportView.GetSelectedRecord()).contains("rows[1009]"), "report preview is bounded while CSV includes the final row");
    reportView.findChild<QComboBox*>("reportOperation")->setCurrentIndex(0);
    Check(reportView.GetSelectedRecord()["operationId"] == "7", "report can select an earlier completed operation without changing business state");
    reportView.hide();
#if defined(MANUAL_ROI)
    window.GetWorkflow().onNavigate("Roi", "Begin", {}); QCoreApplication::processEvents();
    window.GetModule("Roi")->findChild<QWidget*>("card_Begin")->grab().save("parameter-ui-roi-matrix.png");
#endif
    poses.GetElement(0)->GetElement(3)->findChild<QLineEdit*>("value")->setText("12.5");
    Check(poses.GetValue().toArray()[0].toArray()[3].toDouble() == 12.5, "individual matrix cell retains row-major position");
    poses.findChild<QPushButton*>("addRow")->click(); Check(poses.GetCount() == 2, "another initial pose can be added without array text");
    ParameterEditor ids("Alignment", "SaveRecipe", "vertexIds", QJsonArray{"18446744073709551615"}, QJsonArray{});
    Check(ids.GetValue().toArray()[0].toString() == "18446744073709551615", "structured lists preserve full uint64 strings");
    const QJsonObject firstSegment{{"segmentFrom",QJsonValue()},{"segmentTo",1.0},{"palette","Constant"},
        {"lowColor",QJsonArray{.6,.2,.7}},{"highColor",QJsonArray{.2,.3,.8}}};
    const QJsonObject lastSegment{{"segmentFrom",1.0},{"segmentTo",QJsonValue()},{"palette","Constant"},
        {"lowColor",QJsonArray{.2,.3,.8}},{"highColor",QJsonArray{.2,.3,.8}}};
    ParameterEditor segments("Gap","SetDisplay","segments",QJsonArray{firstSegment,lastSegment},QJsonArray{});
    Check(segments.GetElement(0)->GetField("segmentFrom")->GetValue().isNull()
        && segments.GetElement(1)->GetField("segmentTo")->GetValue().isNull()
        && segments.GetElement(0)->GetField("highColor")->isHidden(),
        "segment rows retain infinite endpoints and hide unused constant-color controls");
    segments.GetElement(1)->parentWidget()->findChild<QPushButton*>("moveUp",Qt::FindDirectChildrenOnly)->click();
    Check(segments.GetValue().toArray()[0].toObject()["segmentFrom"]==1,
        "segment reorder is reflected in the submitted value");
    segments.GetElement(0)->parentWidget()->findChild<QPushButton*>("moveDown",Qt::FindDirectChildrenOnly)->click();
    segments.GetElement(1)->parentWidget()->findChild<QPushButton*>("removeRow",Qt::FindDirectChildrenOnly)->click();
    Check(segments.GetCount()==1 && segments.GetValue().toArray()[0].toObject()["segmentTo"]==1,
        "segment removal preserves the remaining physical interval");
    if (auto* split = window.GetModule("Part")->GetParameterEditor("Split")) {
        window.GetWorkflow().onNavigate("Part", "Split", {});
        auto* card = window.GetModule("Part")->findChild<QWidget*>("card_Split");
        auto* splitScroll = window.GetModule("Part")->findChild<QScrollArea*>("operationScroll");
        Check(Wait([&] { return qAbs(card->mapTo(splitScroll->viewport(), QPoint()).y()) < 30; }, 3000),
            "navigation reveals requested card after hidden page layout settles");
        auto* seeds = split->GetField("seeds");
        const auto original = seeds->GetValue(); const int rows = seeds->GetCount(); seeds->findChild<QPushButton*>("addRow")->click();
        auto* added = seeds->GetElement(rows); added->GetField("target")->findChild<QLineEdit*>("value")->setText("3");
        added->GetField("imageIndex")->GetElement(2)->findChild<QLineEdit*>("value")->setText("7");
        Check(seeds->GetValue().toArray().last().toObject()["target"] == "3" && seeds->GetValue().toArray().last().toObject()["imageIndex"].toArray()[2] == 7,
            "split seed row combines independent coordinates and target label");
        seeds->GetElement(0)->parentWidget()->findChild<QPushButton*>("removeRow", Qt::FindDirectChildrenOnly)->click();
        Check(seeds->GetCount() == rows && seeds->GetValue().toArray().last().toObject()["target"] == "3", "deleting a row retains remaining values");
        QCoreApplication::processEvents();
        window.GetWorkflow().onNavigate("Part", "Split", {});
        Check(Wait([&] { return qAbs(card->mapTo(splitScroll->viewport(), QPoint()).y()) < 30; }, 3000), "edited card can be focused again without resetting rows");
        window.grab().save("parameter-ui-split.png");
        QJsonArray longSeeds; for (int index = 0; index < 12; ++index) longSeeds.append(seeds->GetValue().toArray().first());
        seeds->SetValue(longSeeds);
        QCoreApplication::processEvents();
        Check(Wait([&] { return splitScroll->verticalScrollBar()->maximum() > 0; },3000), "long seed lists scroll in a small window");
        auto* splitCommand = window.GetModule("Part")->findChild<QPushButton*>("businessPrimary");
        const auto commandPosition = splitCommand->mapTo(window.GetModule("Part"),QPoint());
        splitScroll->verticalScrollBar()->setValue(splitScroll->verticalScrollBar()->maximum()); QCoreApplication::processEvents();
        Check(splitCommand->isVisible() && splitCommand->mapTo(window.GetModule("Part"),QPoint()) == commandPosition
            && window.GetModule("Part")->rect().contains(QRect(commandPosition,splitCommand->size())),
            "scrolling a long form leaves the stage primary in its fixed position");
        window.grab().save("manual-layout-long-form-1280.png");
        Check(window.size()==originalSize,"long parameter forms never resize the complete window");QCoreApplication::processEvents();
        seeds->SetValue(original);
    }
    window.GetWorkflow().onNavigate("Data", "Load", {}); QCoreApplication::processEvents(); window.grab().save("parameter-ui-data.png");
}
void CheckBooleanControls(TestWindow& window)
{
    int checked = 0;
    for (const QString module : {QString("Data"), QString("View"), QString("Crop"), QString("Gap"), QString("Part"),
            QString("Surface"), QString("Artifact"), QString("Rotation"), QString("Alignment"), QString("Wall")}) {
        const auto boxes = window.GetModule(module)->findChildren<QCheckBox*>("value");
        for (auto* box : boxes) {
            if (!box->isEnabled()) continue;
            auto* field = dynamic_cast<ParameterEditor*>(box->parentWidget()->parentWidget());
            Check(field && !field->findChild<QCheckBox*>("specified"), "business Boolean has one checkbox, never a second specified checkbox");
            const auto before = field->GetValue();
            SetBoolean(field, true);
            box->click(); Check(field->GetValue() == QJsonValue(false), "unchecking a business Boolean always produces false");
            if (auto* unset = field->findChild<QPushButton*>("unsetBoolean")) {
                unset->click(); Check(field->GetValue() == (field->GetIsStateBound() ? field->GetAppliedBoolean() : QJsonValue()), "reset restores applied state for switches or omission for calculation parameters");
                SetBoolean(field, true);
                box->click(); Check(field->GetValue() == QJsonValue(false), "unchecking after unspecified never cycles back into omission");
            }
            field->SetValue(before); ++checked;
        }
    }
    Check(checked >= 6, "Boolean regression covers controls across available modules");
    std::cout << "BOOLEAN CONTROLS: " << checked << std::endl;
    auto* view = window.GetModule("View");
    auto* visibility = view->GetParameterEditor("Visibility");
    Check(!view->GetParameterEditor("Set")->GetField("visibility") && visibility->GetField("viewScope"), "auxiliary switches have an explicit scope and are separate from single-view display settings");
    view->SetParameters("Visibility", {{"viewScope", "all"}, {"planes", false}, {"ruler", true}});
    Check(visibility->GetValue().toObject()["planes"] == QJsonValue(false) && visibility->GetValue().toObject()["ruler"] == QJsonValue(true), "imported Boolean changes survive applied-state synchronization");
    view->SetParameterPatch("Set", {{"axes", false}});
    Check(visibility->GetValue().toObject()["planes"] == QJsonValue(false), "unrelated card edits preserve auxiliary drafts");
    const auto exported = visibility->GetValue().toObject(); view->SetParameters("Visibility", exported);
    Check(visibility->GetValue().toObject() == exported, "Boolean import and export retain exact current and draft values");
    view->SetParameterPatch("Set", {{"axes", QJsonValue()}});
    view->SetParameterPatch("Visibility", {{"planes", QJsonValue()}, {"crosshair", QJsonValue()}, {"ruler", QJsonValue()}});
}
void CheckBooleanRequests(TestWindow& window)
{
    auto* panel = window.GetModule("View");
    auto* form = panel->GetParameterEditor("Set"); auto* auxiliary = panel->GetParameterEditor("Visibility");
    const auto original = form->GetValue().toObject();
    panel->SetParameterPatch("Set", {{"viewId", "primary-3d"}});
    panel->SetParameterPatch("Visibility", {{"viewScope", "all"}});
    auto* axes = form->GetField("axes");
    for (const bool enabled : {true, false}) {
        SetBoolean(axes, enabled);
        const auto display = GetComplete(window, Click(window, "View", "Set"), "Succeeded");
        Check(display["parameters"].toObject()["axes"] == QJsonValue(enabled) && window.GetSession()->GetRenderViewState({"primary-3d"})->isAxesVisible == enabled,
            "axes checkbox and applied Host state agree after execution");
        for (const auto* key : {"planes", "crosshair", "ruler"}) SetBoolean(auxiliary->GetField(key), enabled);
        const auto record = GetComplete(window, Click(window, "View", "Visibility"), "Succeeded");
        Check(record["result"].toObject()["views"].toArray().size() == 4, "auxiliary action completes all four target requests once");
        for (const auto& view : window.GetSession()->GetRenderViewStates()) {
            const bool slice = view.viewMode == HostRenderMode::SliceTopDown || view.viewMode == HostRenderMode::SliceFrontBack || view.viewMode == HostRenderMode::SliceLeftRight;
            for (const auto bit : slice ? std::vector<unsigned>{2U} : std::vector<unsigned>{1U, 4U})
                Check(((view.visibilityMask & bit) != 0) == enabled, "each actual view has the requested applicable auxiliary state");
        }
        for (const auto* key : {"planes", "crosshair", "ruler"}) Check(auxiliary->GetField(key)->GetAppliedBoolean() == QJsonValue(enabled), "auxiliary checkbox reads back its effective scope");
    }
    GetComplete(window, Click(window, "View", "Visibility", {{"viewScope", "slice-top-down"}, {"crosshair", true}}), "Succeeded");
    panel->SetParameterPatch("Visibility", {{"viewScope", "all"}});
    Check(auxiliary->GetField("crosshair")->GetAppliedBoolean().isNull(), "different slice crosshairs are shown as mixed");
    GetComplete(window, Click(window, "View", "Visibility", {{"planes", true}}), "Succeeded");
    for (const auto& view : window.GetSession()->GetRenderViewStates()) if (view.id.find("slice-") == 0)
        Check(((view.visibilityMask & 2U) != 0) == (view.id == "slice-top-down"), "3D plane changes preserve independent slice crosshairs");
    GetComplete(window, Click(window, "View", "Visibility", {{"viewScope", "slices"}, {"crosshair", true}}), "Succeeded");
    for (const auto* key : {"planes", "ruler"}) Check(!auxiliary->GetField(key)->findChild<QCheckBox*>("value")->isEnabled(), "slice scope disables inapplicable 3D controls");
    GetComplete(window, Click(window, "View", "Visibility", {{"viewScope", "all"}, {"planes", false}, {"crosshair", false}, {"ruler", false}}), "Succeeded");
    panel->SetParameterPatch("Set", {{"viewId", "slice-top-down"}, {"windowLevel", QJsonArray{100.,50.}}});
    Check(form->GetField("mode")->isHidden() && form->GetField("quality")->isHidden(), "slice parameters exclude 3D rendering modes and quality");
    GetComplete(window, Click(window, "View", "Set"), "Succeeded");
    Check(window.GetSession()->GetRenderViewState({"slice-top-down"})->viewMode == HostRenderMode::SliceTopDown, "editing slice window level preserves the slice direction");
    panel->SetParameterPatch("Set", {{"viewId", "primary-3d"}, {"windowLevel", QJsonValue()}});
    SetBoolean(auxiliary->GetField("ruler"), true);
    Check(auxiliary->GetField("ruler")->findChild<QLabel*>("booleanState")->text().contains("待应用"), "pending edits are distinguished from applied display state");
    auxiliary->GetField("ruler")->findChild<QPushButton*>("unsetBoolean")->click();
    Check(auxiliary->GetField("ruler")->GetValue() == QJsonValue(false), "retracting a draft restores the applied state");
    panel->SetParameterPatch("Visibility", {{"viewScope", "primary-3d"}});
    Check(auxiliary->GetField("crosshair")->GetValue().isNull() && !auxiliary->GetField("crosshair")->findChild<QCheckBox*>("value")->isEnabled(), "3D view never claims to display a slice crosshair");
    GetComplete(window, Click(window, "View", "Set", {{"viewId", "primary-3d"}, {"mode", "IsoSurface"}}), "Succeeded");
    Check(auxiliary->GetField("planes")->GetValue().isNull() && !auxiliary->GetField("planes")->findChild<QCheckBox*>("value")->isEnabled(), "plain isosurface mode cannot display composite reference planes");
    GetComplete(window, Click(window, "View", "Set", {{"mode", "CompositeIsoSurface"}}), "Succeeded");
    panel->SetParameterPatch("Set", {{"mode", QJsonValue()}});
    panel->SetParameterPatch("Visibility", {{"viewScope", "primary-3d"}});
    window.GetWorkflow().onNavigate("View", "Visibility", {}); QCoreApplication::processEvents(); window.grab().save("applied-state-view.png");
    GetComplete(window, Click(window, "View", "Visibility", {{"viewScope", "all"}, {"planes", true}, {"crosshair", true}, {"ruler", true}}), "Succeeded");
    panel->SetParameters("Set", original);
}
QJsonObject CheckPartDisplay(TestWindow& window);
void CheckPartBooleanRequests(TestWindow& window, const QJsonObject& part)
{
    auto* panel = window.GetModule("Part"); auto* form = panel->GetParameterEditor("SetState");
    const auto original = form->GetValue().toObject();
    panel->SetParameterPatch("SetState", {{"target", part["binding"]}});
    for (const bool enabled : {true, false}) {
        for (const auto* key : {"isVisible", "isSelected", "isReviewed"}) {
            auto* field = form->GetField(key); auto* box = field->findChild<QCheckBox*>("value"); if (field->GetValue() != QJsonValue(enabled)) box->click();
        }
        const auto record = GetComplete(window, Click(window, "Part", "SetState"), "Succeeded");
        for (const auto* key : {"isVisible", "isSelected", "isReviewed"}) Check(record["parameters"].toObject()[key] == QJsonValue(enabled), "part state request preserves explicitly unchecked fields");
        const auto catalog = GetComplete(window, Send(window, "Part", "Catalog"), "Observed")["result"].toObject();
        bool matched = false;
        for (const auto value : catalog["parts"].toArray()) if (value.toObject()["binding"] == part["binding"]) {
            const auto state = value.toObject(); matched = state["visible"] == QJsonValue(enabled) && state["selected"] == QJsonValue(enabled) && state["reviewed"] == QJsonValue(enabled);
        }
        Check(matched, "catalog readback confirms visible, selected and reviewed all follow true-to-false clicks");
        (void)CheckPartDisplay(window);
    }
    GetComplete(window, Click(window, "Part", "SetState", {{"target", part["binding"]}, {"isVisible", part["visible"]}, {"isSelected", part["selected"]}, {"isReviewed", part["reviewed"]}}), "Succeeded");
    panel->SetParameters("SetState", original);
}
void CheckOverlaySwitch(TestWindow& window, const QString& module,
    const std::map<std::string, double>& originalOpacities = {})
{
    auto* panel = window.GetModule(module); panel->Observe();
    auto* field = panel->GetParameterEditor("Visibility")->GetField("isVisible");
    for (const bool enabled : {true, false, true}) {
        SetBoolean(field, enabled);
        const auto record = GetComplete(window, Click(window, module, "Visibility"));
        Check(record["status"] != "Rejected" && record["status"] != "Failed" && record["status"] != "InvalidInput", "overlay switch request is accepted by its real feature");
        Check(Wait([&] { panel->Observe(); return panel->GetObservedState()["isOverlayVisible"] == QJsonValue(enabled) && field->GetAppliedBoolean() == QJsonValue(enabled); }),
            "required overlay checkbox follows real feature state through on-off-on transitions");
        if (module == "Part") (void)CheckPartDisplay(window);
        if (!enabled) for (const auto& original : originalOpacities) {
            const auto state = window.GetSession()->GetRenderViewState({original.first});
            Check(state && state->material.opacity == original.second, "disabling part preview restores each user's original source opacity");
        }
    }
}
void CheckViewCapabilities(TestWindow& window)
{
    auto* panel = window.GetModule("View"); auto* form = panel->GetParameterEditor("Set");
    const auto original = form->GetValue().toObject();
    const auto hasField = [&](const char* key, bool expected) {
        Check(form->GetField(key)->isHidden() != expected
            && form->GetValue().toObject().contains(key) == expected,
            "view fields and submitted parameters follow the effective rendering mode");
    };
    for (const auto* mode : {"CompositeVolume", "CompositeIsoSurface", "Volume", "IsoSurface"}) {
        GetComplete(window, Click(window, "View", "Set", {{"viewId", "primary-3d"}, {"mode", mode}}), "Succeeded");
        panel->Observe(); const bool volume = QString(mode).endsWith("Volume");
        hasField("iso", !volume); hasField("transfer", volume); hasField("windowLevel", false);
        hasField("quality", true); hasField("opacity", true); hasField("mode", true);
        const auto threshold = window.GetSession()->GetRenderViewState({"primary-3d"})->isoThreshold;
        const auto invalid = volume ? QJsonObject{{"viewId", "primary-3d"}, {"iso", 123.}}
            : QJsonObject{{"viewId", "primary-3d"}, {"transfer", QJsonObject{}}};
        GetComplete(window, panel->SendAction("Set", invalid), "InvalidInput");
        Check(window.GetSession()->GetRenderViewState({"primary-3d"})->isoThreshold == threshold,
            "direct requests cannot apply fields unsupported by the current manual view");
    }
    for (const auto* view : {"slice-top-down", "slice-front-back", "slice-left-right"}) {
        panel->SetParameterPatch("Set", {{"viewId", view}}); panel->Observe();
        for (const auto* key : {"mode", "iso", "transfer", "quality", "opacity"}) hasField(key, false);
        hasField("windowLevel", true);
    }
    panel->SetParameters("Set", original);
    // 工具栏直接发请求，不经过参数页；参数显隐仍须立即跟随实际模式。
    panel->SetParameterPatch("Set", {{"mode", "invalid-imported-mode"}});
    GetComplete(window, Send(window, "View", "Set"), "InvalidInput");
    auto* toolbar = window.findChild<QComboBox*>("renderMode");
    for (const int index : {1, 0}) {
        const auto id = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()) + 1;
        toolbar->setCurrentIndex(index); toolbar->activated(index); GetComplete(window, id, "Succeeded");
        panel->Observe(); hasField("iso", index == 0); hasField("transfer", index == 1); hasField("quality", true);
        QCoreApplication::processEvents(); window.grab().save(index ? "view-volume-capabilities.png" : "view-iso-capabilities.png");
    }
}
void SendCropDrag(TestWindow& window, const std::array<double,3>& fraction)
{
    const auto original = *window.GetSession()->GetImageDescriptor();
    const auto* endpoint = window.GetSession()->GetPrimaryEndpoint();
        std::optional<std::array<double,16>> modelToWorld;
        for(const auto value:window.GetWorkflow().getViewTransforms()["views"].toArray())if(value.toObject()["view"]==QString::fromStdString(endpoint->id))modelToWorld=GetAffineMatrix(value.toObject()["modelToWorld"]);
        Check(modelToWorld.has_value(),"crop input has an explicit model-to-world transform");
        std::array<double,6> bounds{std::numeric_limits<double>::max(),std::numeric_limits<double>::lowest(),std::numeric_limits<double>::max(),std::numeric_limits<double>::lowest(),std::numeric_limits<double>::max(),std::numeric_limits<double>::lowest()};
        // 描述符是模型坐标；盒控件位于世界 AABB，先变换八角点再选取世界面中心。
        for(int corner=0;corner<8;++corner) {
            auto model=original.origin;
            for(int row=0;row<3;++row)for(int axis=0;axis<3;++axis)model[row]+=original.direction[row*3+axis]*original.spacing[axis]*original.extent[2*axis+((corner>>axis)&1)];
            for(int row=0;row<3;++row){double point=(*modelToWorld)[row*4+3];for(int axis=0;axis<3;++axis)point+=(*modelToWorld)[row*4+axis]*model[axis];bounds[2*row]=std::min(bounds[2*row],point);bounds[2*row+1]=std::max(bounds[2*row+1],point);}
        }
        std::array<double,3> world{};for(int axis=0;axis<3;++axis)world[axis]=bounds[2*axis]+fraction[axis]*(bounds[2*axis+1]-bounds[2*axis]);
        endpoint->renderer->SetWorldPoint(world[0], world[1], world[2], 1.0); endpoint->renderer->WorldToDisplay();
        const auto* display = endpoint->renderer->GetDisplayPoint();
        auto* widget=window.findChild<QVTKOpenGLNativeWidget*>("primary3D");const auto size=endpoint->renderWindow->GetSize();
        Check(widget && size[0]>0 && size[1]>0,"crop pointer target has an actual rendered viewport");
        const QPointF start(display[0]*widget->width()/size[0],(size[1]-1-display[1])*widget->height()/size[1]);
        const QPointF end=start+QPointF(8,0);
        // 经实际 Qt/VTK 适配器投递，正确转换像素比例、Y 方向与按钮状态。
        QMouseEvent press(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(widget,&press);QCoreApplication::processEvents();
        QMouseEvent move(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(widget,&move);QCoreApplication::processEvents();
        QMouseEvent release(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(widget,&release);QCoreApplication::processEvents();
}
void CheckCropWorkflow(TestWindow& window)
{
    if (!GetEnabled(window, "Crop")) return;
    window.SetViewsVisible(true);
    auto* panel = window.GetModule("Crop");
    Check(Wait([&] { return panel->GetObservedState()["framesReady"].toBool(); }), "all four crop views settle through events");
    GetComplete(window, Click(window,"Crop","CreateDocument"),"Succeeded");
    const auto firstDocument=panel->GetObservedState()["documentId"].toString();
    GetComplete(window, Click(window,"Crop","CreateDocument"),"Succeeded");
    const auto secondDocument=panel->GetObservedState()["documentId"].toString();
    Check(firstDocument!=secondDocument,"each explicit document has a new identity");
    GetComplete(window,Send(window,"Crop","ActivateDocument",{{"documentId",firstDocument}}),"Succeeded");
    GetComplete(window, Send(window, "View", "Set", {{"iso", 1000.0}}), "Succeeded");
    const auto unavailable = GetComplete(window, SendUnavailable(window, "Crop", "Box"), "Rejected");
    Check(unavailable["result"].toObject()["message"].toString().contains("显示等值阈值") && panel->GetObservedState()["operationCount"].toString()=="0" && panel->GetObservedState()["editMode"].toInt()==0, "empty display is rejected before enabling a crop tool or accumulating invisible previews");
    GetComplete(window,Click(window,"Crop","CloseDocument"),"Succeeded");
    Check(panel->GetObservedState()["documents"].toArray().size()==1,"empty iso display still allows document cleanup");
    GetComplete(window, Send(window, "View", "Set", {{"iso", 50.0},{"opacity",1.0}}), "Succeeded");
    GetComplete(window,Send(window,"Crop","ActivateDocument",{{"documentId",secondDocument}}),"Succeeded");
    Check(Wait([&] { panel->Observe(); return panel->GetObservedState()["framesReady"].toBool(); }),
        "reactivated crop document renders before history actions");
    GetComplete(window, SendUnavailable(window, "Crop", "BuildResult"), "Rejected");
    GetComplete(window, Click(window, "Crop", "Box"), "Succeeded");
    auto* cropLayoutChoice=window.findChild<QComboBox*>("viewLayout");cropLayoutChoice->setCurrentIndex(cropLayoutChoice->findData("ThreeDOnly"));QCoreApplication::processEvents();
    const auto cropViews=window.findChildren<QVTKOpenGLNativeWidget*>();
    Check(cropLayoutChoice->currentData()=="FourViews" && std::all_of(cropViews.begin(),cropViews.end(),[](const auto* view){return view->isVisible();}),"crop editing restores visible frame guards when 3D-only layout is requested");
    GetComplete(window, Click(window, "Crop", "RemoveInside"), "Succeeded");
    GetComplete(window, Click(window, "Crop", "KeepInside"), "Succeeded");
    Check(Wait([&]{panel->Observe();return panel->GetObservedState()["framesReady"].toBool();}),"crop mode changes settle before finishing editing");
    GetComplete(window, SendUnavailable(window, "Crop", "FinishEditing"), "Rejected");
    GetComplete(window, Click(window, "Crop", "Plane"), "Succeeded");
    Check(Wait([&] { return panel->GetObservedState()["framesReady"].toBool(); }), "plane tool renders");
    GetComplete(window, Click(window, "Crop", "Box"), "Succeeded");
    Check(Wait([&] { return panel->GetObservedState()["framesReady"].toBool(); }), "box tool renders");
    Check(panel->GetObservedState()["editMode"].toInt() == 1, "changing crop shape preserves KeepInside mode");
    Check(Wait([&] { return panel->GetObservedState()["framesReady"].toBool(); }), "first crop entry frames its controls through events without a separate view reset");
    const auto original = *window.GetSession()->GetImageDescriptor();
    const auto* endpoint = window.GetSession()->GetPrimaryEndpoint();
    Check(endpoint && endpoint->renderer && endpoint->interactor, "public crop input endpoint exists");
    const std::array<std::array<double, 3>, 10> fractions{{
        {1,.5,.5}, {0,.5,.5}, {.5,1,.5}, {.5,0,.5}, {.5,.5,1}, {.5,.5,0}, {1,1,1}, {0,0,0}, {1,0,1}, {0,1,0}}};
    const auto drag=[&](const std::array<double,3>& fraction) { SendCropDrag(window, fraction); };
    for (const auto& fraction : fractions) {
        const auto beforeDrag = panel->GetObservedState()["operationCount"].toString().toULongLong();
        drag(fraction);
        if (Wait([&] { return panel->GetObservedState()["operationCount"].toString().toULongLong() > beforeDrag
                && panel->GetObservedState()["framesReady"].toBool(); }, 1500)
                && panel->GetObservedState()["operationCount"].toString().toULongLong() >= 3) break;
    }
    const auto count = panel->GetObservedState()["operationCount"].toString().toULongLong();
    if (!count) {
        panel->onObserve();
        std::cerr << "Crop diagnostic: " << GetJsonText(panel->GetObservedState()).toStdString()
            << "updates=" << window.GetUpdateCount() << ", renders=" << window.GetRenderCount() << std::endl;
        for (const auto& scene : window.GetSession()->GetSceneViewStates()) std::cerr << scene.id << " epoch " << scene.sceneEpoch
            << "/" << scene.renderedEpoch << " interacting=" << (scene.presentation && scene.presentation->isInteracting) << std::endl;
    }
    Check(count >= 2,"actual VTK drags create immutable crop history nodes");
    const auto history=panel->GetObservedState()["nodes"].toArray();
    const auto currentNode=panel->GetObservedState()["appliedHead"].toString();
    QString otherNode,rootNode;
    for(const auto item:history){const auto node=item.toObject();if(node["parentNodeId"].toString()=="0")rootNode=node["nodeId"].toString();
        else if(node["nodeId"].toString()!=currentNode)otherNode=node["nodeId"].toString();}
    Check(!otherNode.isEmpty()&&!rootNode.isEmpty(),"history exposes stable branch and Root IDs");
    auto* tree=panel->GetCatalogTree();tree->expandAll();QTreeWidgetItem* historyNode=nullptr;
    for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toJsonObject()["patches"].toObject()["Node"].toObject()["nodeId"].toString()==otherNode){historyNode=*it;break;}
    Check(historyNode!=nullptr,"crop tree exposes selectable stable nodes");ClickNode(tree,historyNode);
    Check(panel->GetParameters()["nodeId"].toString()==otherNode,"scene selection supplies stable node identity");
    const auto selectedHistory=static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size())+1;
    window.SetViewsVisible(false);panel->findChild<QPushButton*>("action_Node")->click();CheckIdle(window);
    Check(!window.GetRecords().GetRecord(selectedHistory)["isTerminal"].toBool(),"hidden crop waits for a presented frame without notification spin");
    window.SetViewsVisible(true);GetComplete(window,selectedHistory,"Succeeded");
    Check(panel->GetObservedState()["appliedHead"].toString()==otherNode&&panel->GetObservedState()["renderedHead"].toString()==otherNode,"selected node and presented node agree");
    GetComplete(window,Click(window,"Crop","Previous"),"Succeeded");
    GetComplete(window,Send(window,"Crop","Node",{{"nodeId",currentNode}}),"Succeeded");
    GetComplete(window,ClickNodeAction(window,tree,FindNode(tree,"crop-history"),"ResetPreview"),"Succeeded");
    Check(panel->GetObservedState()["appliedHead"].toString()==rootNode,"Root selection retains the tree");
    int rootChildren=0;for(const auto item:history)rootChildren+=item.toObject()["parentNodeId"].toString()==rootNode;
    GetComplete(window,Click(window,"Crop","Next"),rootChildren==1?"Succeeded":"Rejected");
    GetComplete(window,Send(window,"Crop","Node",{{"nodeId",currentNode}}),"Succeeded");
    GetComplete(window,Send(window,"Crop","Node",{{"nodeId","18446744073709551615"}}),"Rejected");
    // 从 Root 再次开始实际拖动，显式创建分支；连续拖动可以合法地形成单条路径。
    GetComplete(window,ClickNodeAction(window,tree,FindNode(tree,"crop-history"),"ResetPreview"),"Succeeded");
    GetComplete(window,Click(window,"Crop","Box"),"Succeeded");
    Check(Wait([&]{return panel->GetObservedState()["framesReady"].toBool();}),"branch widget renders");
    for(const auto& fraction:fractions){drag(fraction);
        if(Wait([&]{return panel->GetObservedState()["nodes"].toArray().size()>history.size()
            &&panel->GetObservedState()["framesReady"].toBool();},1500))break;}
    const auto pruneNode=panel->GetObservedState()["appliedHead"].toString();
    const auto branchHistory=panel->GetObservedState()["nodes"].toArray();
    Check(pruneNode!=rootNode&&pruneNode!=currentNode&&branchHistory.size()>history.size(),"editing Root creates an independent branch while retaining the original path");
    GetComplete(window,Send(window,"Crop","Node",{{"nodeId",currentNode}}),"Succeeded");
    QTreeWidgetItem* prunedNode=nullptr;for(QTreeWidgetItemIterator it(tree);*it;++it)
        if((*it)->data(0,Qt::UserRole).toJsonObject()["patches"].toObject()["PruneSubtree"].toObject()["nodeId"].toString()==pruneNode){prunedNode=*it;break;}
    Check(prunedNode!=nullptr,"branch context action uses Prune with stable identity");
    GetComplete(window,ClickNodeAction(window,tree,prunedNode,"PruneSubtree"),"Succeeded");
    const auto remaining=panel->GetObservedState()["nodes"].toArray();bool exact=remaining.size()==branchHistory.size()-1;
    for(const auto item:remaining)exact=exact&&item.toObject()["nodeId"].toString()!=pruneNode;
    Check(exact&&panel->GetObservedState()["appliedHead"].toString()==currentNode,"pruning another branch preserves the current node and all other identities");
    GetComplete(window,Send(window,"Crop","PruneSubtree",{{"nodeId",pruneNode}}),"Rejected");
    GetComplete(window, Click(window, "Crop", "FinishEditing"), "PreviewConfirmed");
    Check(Wait([&]{panel->Observe();return panel->GetObservedState()["framesReady"].toBool();}),"widget removal finishes rendering before publication");
    auto published = GetComplete(window, Send(window, "Crop", "BuildResult", {{"nodeId",currentNode}}), "Published")["result"].toObject();
    Check(window.GetSession()->GetImageDescriptor()->dataRevision == original.dataRevision, "crop publish keeps source selected");
    const auto firstOutput=published["output"].toString();
    // 同源视图命中缓存；第二次发布仍须通过工作通知推进旧结果退休和提交。
    GetComplete(window,Click(window,"Crop","Box"),"Succeeded");
    Check(Wait([&]{panel->Observe();return panel->GetObservedState()["framesReady"].toBool();}),"new crop tool settles while the first output is retained");
    QJsonArray secondBox;
    for(int row=0;row<4;++row)for(int column=0;column<4;++column) {
        double value=row==3 && column==3 ? 1.0 : 0.0;
        if(row<3 && column<3)value=original.direction[row*3+column]*original.spacing[column]*(original.extent[column*2+1]-original.extent[column*2])*0.25;
        if(row<3 && column==3){value=original.origin[row];for(int axis=0;axis<3;++axis)value+=original.direction[row*3+axis]*original.spacing[axis]*(original.extent[axis*2]+original.extent[axis*2+1])*0.5;}
        secondBox.append(value);
    }
    GetComplete(window,Send(window,"Crop","AppendExact",{{"matrix",secondBox}}),"Succeeded");
    Check(Wait([&]{panel->Observe();return panel->GetObservedState()["framesReady"].toBool();}),"new preview presents before its confirmation");
    GetComplete(window,Click(window,"Crop","FinishEditing"),"PreviewConfirmed");
    Check(Wait([&]{panel->Observe();return panel->GetObservedState()["framesReady"].toBool();}),"reconfirmed cached source views are ready before the second publication");
    const auto combined=ClickCurrentPrimary(window,"Crop","BuildResult");
    published=GetComplete(window,combined,"Published")["result"].toObject();
    Check(Wait([&]{return !window.GetRecords().GetRecord(combined+1).isEmpty();},2000),"a publication's own source restore still dispatches automatic adoption");
    GetComplete(window,combined+1,"Succeeded");
    Check(published["output"].toString()!=firstOutput && GetRefText(window.GetSession()->GetImageDescriptor()->dataRevision)==published["output"].toString(),
        "repeated publication on cached source views adopts its exact new output through the natural primary");
    GetComplete(window, Click(window, "Crop", "SelectOutput"), "Succeeded");
    Check(GetRefText(window.GetSession()->GetImageDescriptor()->dataRevision) == published["output"].toString(), "select uses exact published crop output");
    GetComplete(window, Click(window, "Crop", "RestoreSource"), "Succeeded");
    Check(window.GetSession()->GetImageDescriptor()->dataRevision == original.dataRevision, "restore returns exact crop source");
    GetComplete(window, Click(window, "Crop", "RestoreSource"), "Succeeded");
    window.GetWorkflow().onNavigate("Crop", "Box", {});
    QCoreApplication::processEvents(); window.grab().save("scene-ui-crop.png");
    CheckIdle(window);
    GetComplete(window,Click(window,"Crop","CloseDocument"),"Succeeded");
    Check(panel->GetObservedState()["documents"].toArray().isEmpty(),"all explicitly created crop documents close");
}
void CheckCropMouseInteraction(TestWindow& window)
{
    if (!GetEnabled(window, "Crop")) return;
    auto* panel = window.GetModule("Crop");
    auto* view = window.findChild<QVTKOpenGLNativeWidget*>("primary3D");
    const auto* endpoint = window.GetSession()->GetPrimaryEndpoint();
    Check(view && endpoint && view->renderWindow()->GetInteractor() == endpoint->interactor,
        "manual QVTK mouse input uses the crop interactor");
    const auto settled = [&] { panel->Observe(); return panel->GetObservedState()["framesReady"].toBool(); };
    GetComplete(window, Click(window, "Crop", "CreateDocument"), "Succeeded");
    for (const auto* renderMode : {"CompositeIsoSurface", "CompositeVolume"}) {
        GetComplete(window, Send(window, "View", "Set", {{"viewId", "primary-3d"}, {"mode", renderMode}}), "Succeeded");
        Check(Wait(settled), "display mode settles before crop input");
        for (const auto* shape : {"Box", "Plane", "Sphere", "Cylinder"}) {
            for (const auto* removal : {"KeepInside", "RemoveInside", "PositionOnly"}) {
                auto* historyTree=panel->GetCatalogTree();
                GetComplete(window,ClickNodeAction(window,historyTree,FindNode(historyTree,"crop-history"),"ResetPreview"),"Succeeded");
                GetComplete(window, Click(window, "Crop", shape), "Succeeded");
                GetComplete(window, Click(window, "Crop", removal), "Succeeded");
                Check(Wait(settled), "crop controls render before mouse input");
                using Point = std::array<double, 3>;
                std::vector<std::function<Point()>> handles;
                std::function<std::vector<double>()> geometry;
                auto* props = endpoint->renderer->GetViewProps(); props->InitTraversal();
                while (auto* prop = props->GetNextProp()) {
                    if (auto* handle = vtkHandleRepresentation::SafeDownCast(prop)) {
                        handles.push_back([handle] { Point point{}; handle->GetWorldPosition(point.data()); return point; });
                    } else if (auto* box = vtkBoxRepresentation::SafeDownCast(prop)) {
                        for (const int index : {14, 8, 10}) handles.push_back([box, index] {
                            vtkNew<vtkPolyData> poly; box->GetPolyData(poly); Point point{}; poly->GetPoint(index, point.data()); return point;
                        });
                        geometry = [box] {
                            vtkNew<vtkPolyData> poly; box->GetPolyData(poly); std::vector<double> values;
                            for (int i = 0; i < 8; ++i) { const auto* p = poly->GetPoint(i); values.insert(values.end(), p, p+3); }
                            return values;
                        };
                    } else if (auto* plane = vtkImplicitPlaneRepresentation::SafeDownCast(prop)) {
                        handles.push_back([plane] { Point point{}; plane->GetOrigin(point.data()); return point; });
                        geometry = [plane] {
                            std::vector<double> values(6); plane->GetOrigin(values.data()); plane->GetNormal(values.data()+3); return values;
                        };
                    }
                }
                if (!geometry) geometry = [handles] {
                    std::vector<double> values;
                    for (const auto& position : handles) { const auto p = position(); values.insert(values.end(), p.begin(), p.end()); }
                    return values;
                };
                const int expected = QString(shape) == "Cylinder" ? 4 : QString(shape) == "Box" ? 3 : QString(shape) == "Plane" ? 1 : 2;
                Check(handles.size() == expected, "crop exposes its controls in the 3D renderer");
                for (const auto& handle : handles) {
                    const auto before = geometry(); const auto point = handle();
                    endpoint->renderer->SetWorldPoint(point[0], point[1], point[2], 1.0);
                    endpoint->renderer->WorldToDisplay();
                    const auto* display = endpoint->renderer->GetDisplayPoint();
                    const auto* size = endpoint->renderWindow->GetSize();
                    const QPointF start(display[0] * view->width() / size[0],
                        (size[1] - 1 - display[1]) * view->height() / size[1]);
                    const auto sendMouse = [&](QEvent::Type type, QPointF position, Qt::MouseButton button, Qt::MouseButtons buttons) {
                        const auto modifiers = QString(shape) == "Box" && &handle == handles.data() ? Qt::ShiftModifier : Qt::NoModifier;
                        QMouseEvent event(type, position, button, buttons, modifiers);
                        QApplication::sendEvent(view, &event);
                    };
                    const auto count = panel->GetObservedState()["operationCount"].toString().toULongLong();
                    const auto cursorBefore = window.GetSession()->GetRenderViewState({"primary-3d"})->cursorWorld;
                    std::array<double, 3> cameraBefore{};
                    endpoint->renderer->GetActiveCamera()->GetPosition(cameraBefore.data());
                    const auto rendersBefore = window.GetRenderCount();
                    sendMouse(QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
                    sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
                    for (int step = 1; step <= 4; ++step)
                        sendMouse(QEvent::MouseMove, start + QPointF(step * 4, step * 3), Qt::NoButton, Qt::LeftButton);
                    panel->Observe();
                    const bool dragging = panel->GetObservedState()["isBusy"].toBool();
                    const bool renderedDuringDrag = Wait([&] { return window.GetRenderCount() > rendersBefore; }, 2000);
                    sendMouse(QEvent::MouseButtonRelease, start + QPointF(16, 12), Qt::LeftButton, Qt::NoButton);
                    const auto after = geometry();
                    std::array<double, 3> cameraAfter{};
                    endpoint->renderer->GetActiveCamera()->GetPosition(cameraAfter.data());
                    const bool positionOnly = QString(removal) == "PositionOnly";
                    std::cout << "Crop mouse: " << renderMode << '/' << shape << '/' << removal << " handle " << (&handle - handles.data()) << std::endl;
                    if (!(positionOnly || dragging) || before == after || !renderedDuringDrag)
                        std::cerr << "Crop drag diagnostic: dragging=" << dragging << " changed=" << (before != after)
                            << " rendered=" << renderedDuringDrag << std::endl;
                Check((positionOnly || dragging) && before != after && renderedDuringDrag, "Qt mouse drag changes and renders crop geometry before release");
                Check(Wait([&] { return settled() && panel->GetObservedState()["operationCount"].toString().toULongLong() == count + (positionOnly ? 0 : 1); }),
                    "crop drag renders all views and records history only in removal modes");
                Check(cursorBefore == window.GetSession()->GetRenderViewState({"primary-3d"})->cursorWorld && cameraBefore == cameraAfter,
                    "crop controls take precedence over reference planes without moving the slices or camera");
                }
            }
        }
    }
    GetComplete(window, Click(window, "Crop", "CloseDocument"), "Succeeded");
    GetComplete(window, Send(window, "View", "Set", {{"viewId", "primary-3d"}, {"mode", "CompositeIsoSurface"}}), "Succeeded");
    Check(Wait(settled), "closing crop removes its controls before reference-plane input");
    // 斜视使切片法线在屏幕上有非零投影，避开正视时无法沿深度拖动的退化情况。
    auto* camera = endpoint->renderer->GetActiveCamera(); camera->Azimuth(30); camera->Elevation(20);
    endpoint->interactor->Render(); Check(Wait(settled), "oblique reference-plane view renders");
    bool movedPlane = false; int planeCandidates = 0, visiblePlanes = 0;
    vtkNew<vtkPropPicker> picker;
    auto* actors = endpoint->renderer->GetActors(); actors->InitTraversal();
    std::vector<vtkSmartPointer<vtkActor>> referenceActors;
    // 硬件拾取会重新遍历 renderer 的 actor collection，先保留本轮候选。
    while (auto* actor = actors->GetNextActor()) referenceActors.emplace_back(actor);
    for (const auto& actor : referenceActors) {
        if (movedPlane || !actor->GetVisibility() || !actor->GetPickable() || !actor->GetMapper()) continue;
        auto* data = actor->GetMapper()->GetInput();
        if (!data || data->GetNumberOfPoints() != 4) continue;
        ++planeCandidates;
        const auto* bounds = actor->GetBounds();
        std::array<double, 3> point{};
        for (int i = 0; i < 3; ++i) point[i] = bounds[2*i] * .96 + bounds[2*i+1] * .04;
        endpoint->renderer->SetWorldPoint(point[0], point[1], point[2], 1.); endpoint->renderer->WorldToDisplay();
        std::array<double, 3> display{}; endpoint->renderer->GetDisplayPoint(display.data());
        if (!picker->Pick(display[0], display[1], 0, endpoint->renderer) || picker->GetActor() != actor) continue;
        ++visiblePlanes;
        const auto* size = endpoint->renderWindow->GetSize();
        const QPointF start(display[0] * view->width() / size[0], (size[1]-1-display[1]) * view->height() / size[1]);
        const auto before = window.GetSession()->GetRenderViewState({"primary-3d"})->cursorWorld;
        QMouseEvent press(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &press);
        QMouseEvent move(QEvent::MouseMove, start+QPointF(16,12), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, start+QPointF(16,12), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &release);
        movedPlane = Wait([&] { return before != window.GetSession()->GetRenderViewState({"primary-3d"})->cursorWorld; }, 1000);
    }
    if (!movedPlane) std::cerr << "Reference-plane diagnostic: candidates=" << planeCandidates << " visible=" << visiblePlanes << std::endl;
    Check(movedPlane && Wait(settled), "reference planes remain draggable after exiting every crop mode");
    GetComplete(window, Click(window, "View", "Reset", {{"viewId", "primary-3d"}}), "Succeeded");
}
void CheckRenderModeSwitch(TestWindow& window)
{
    auto* mode = window.findChild<QComboBox*>("renderMode");
    auto* renderWindow = window.GetSession()->GetRenderViewEndpoint("primary-3d")->renderWindow;
    const auto source = window.GetSession()->GetImageDescriptor()->dataRevision;
    for (const int index : {1, 0}) {
        const auto id = static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size()) + 1;
        mode->setCurrentIndex(index); mode->activated(index);
        GetComplete(window, id, "Succeeded");
        Check(Wait([&] { return !window.GetWorkflow().getRenderPending("primary-3d"); }), "mode switch reaches a rendered frame");
        const auto state = window.GetSession()->GetRenderViewState({"primary-3d"});
        Check(state && state->viewMode == (index ? HostRenderMode::CompositeVolume : HostRenderMode::CompositeIsoSurface)
            && state->dataRevision == source && window.GetSession()->GetRenderViewEndpoint("primary-3d")->renderWindow == renderWindow,
            "toolbar changes mode in the same 3D window and preserves data identity");
        int slices = 0;
        for (const auto& view : window.GetSession()->GetRenderViewStates()) if (view.id != "primary-3d") {
            Check(view.viewMode == HostRenderMode::SliceTopDown || view.viewMode == HostRenderMode::SliceFrontBack || view.viewMode == HostRenderMode::SliceLeftRight,
                "3D mode switch preserves each slice mode"); ++slices;
        }
        Check(slices == 3, "all three slices remain available after 3D mode switching");
        if (window.GetModule("Part")->GetObservedState()["hasCurrentParts"].toBool()) (void)CheckPartDisplay(window);
    }
}
void CheckWallWorkflow(TestWindow& window, const QString& directory)
{
    if (!GetEnabled(window, "Wall") || !GetEnabled(window, "Part") || !GetEnabled(window, "Surface")) return;
    std::vector<float> data(32*32*32);
    for (int z=0; z<32; ++z) for (int y=0; y<32; ++y) for (int x=0; x<32; ++x) {
        const auto distance = std::max({std::abs(x-15.5)-10, std::abs(y-15.5)-10, std::abs(z-15.5)-4});
        data[x+32*(y+32*z)] = static_cast<float>(500*(1-std::tanh(distance/0.7)));
    }
    const auto path = directory + "/wall-slab.raw";
    QFile file(path); Check(file.open(QIODevice::WriteOnly), "wall fixture opens");
    Check(file.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()*sizeof(float))) == static_cast<qint64>(data.size()*sizeof(float)), "wall fixture writes"); file.close();
    GetComplete(window, Click(window, "Data", "Load", {{"filePath", path}, {"datasetId", "wall-manual-slab"},
        {"dimensions", QJsonArray{32,32,32}}, {"spacingLPS", QJsonArray{1,1,1}}, {"originLPS", QJsonArray{-31,-31,0}}, {"sourceDigest", ""}}), "Succeeded");
    GetComplete(window, SendUnavailable(window, "Wall", "Start"), "Rejected");
    const auto parts = GetComplete(window, Click(window, "Part", "Start", {{"threshold", 500.}, {"minPartVoxels", "1"}}), "Succeeded");
    GetComplete(window, Click(window, "Surface", "GlobalAutomatic", {{"componentSelection", "Largest"}, {"initialIsoValue", 500.}, {"materialRange", QJsonArray{0.,1000.}},
        {"analysisRoi", ""}}), "Succeeded");
    RoiRequest evaluation; evaluation.definition.source=window.GetSession()->GetImageDescriptor()->dataRevision;
    const auto catalog=window.GetSession()->GetRoiDescriptors(true);
    evaluation.expectedCatalogRevision=catalog.empty()?0:catalog.front().catalogRevision;evaluation.metadata.name="壁厚评价范围";
    RoiNode region;region.primitive.localToSource={5.5,0,0,15.5,0,5.5,0,15.5,0,0,15.5,15.5,0,0,0,1};evaluation.definition.nodes={region};
    const auto evaluatedRoi=window.GetSession()->SetRoi(evaluation);
    Check(evaluatedRoi.error==RoiError::None && evaluatedRoi.roi,"wall workflow saves an independent evaluation ROI");
    GetComplete(window,Click(window,"View","Set",{{"mode","CompositeVolume"}}),"Succeeded");
    auto* preparation=window.GetModule("Wall");
    auto cancelledParameters=preparation->GetActionParameters("Start");cancelledParameters["directionCount"]=1;
    const auto cancelledStart=preparation->SendAction("Start",cancelledParameters);
    const auto cancelledPreparation=preparation->SendAction("Cancel",{{"targetRequestId","0"}});
    GetComplete(window,cancelledPreparation,"Succeeded");GetComplete(window,cancelledStart,"Cancelled");
    Check(window.GetSession()->GetRenderViewState({"primary-3d"})->viewMode==HostRenderMode::CompositeVolume,
        "cancelling display preparation restores the volume after its queued mode change");
    const auto lateStart=preparation->SendAction("Start",cancelledParameters);
    QCoreApplication::processEvents();
    const auto lateCancel=preparation->SendAction("Cancel",{{"targetRequestId","0"}});
    GetComplete(window,lateCancel,"Succeeded");GetComplete(window,lateStart,"Cancelled");
    Check(window.GetSession()->GetRenderViewState({"primary-3d"})->viewMode==HostRenderMode::CompositeVolume,
        "cancelling an already queued view transition restores the original volume");
    const auto local = GetComplete(window, Click(window, "Wall", "Start", {{"maxBoundaryError", 0.5},
        {"directionCount",1},{"evaluationRoi",GetRefText(evaluatedRoi.roi->revision)}}), "Succeeded");
    Check(window.GetSession()->GetRenderViewState({"primary-3d"})->viewMode==HostRenderMode::CompositeVolume
        && local["result"].toObject()["viewRestoreSucceeded"].toBool(),
        "completed native wall measurement restores its original volume presentation");
    GetComplete(window,Click(window,"Wall","KeepResult",{{"graphRevision",local["result"].toObject()["result"]}}),"Succeeded");
    Check(local["result"].toObject()["validCount"].toString().toULongLong()>0
        && local["result"].toObject()["algorithmVersion"]=="wall-thickness-cuda-gray-ray-field-4",
        "local gray wall measurement accepts component-filtered sampling seeds");
    const auto surface = GetComplete(window, Click(window, "Surface", "GlobalAutomatic", {{"componentSelection", "All"}, {"initialIsoValue", 500.}, {"materialRange", QJsonArray{0.,1000.}},
        {"analysisRoi", ""}}), "Succeeded");
    const auto savedLabels = window.GetWorkflow().getPartLabels;
    const auto savedSurface = window.GetWorkflow().getMeasurementSurface;
    window.GetWorkflow().getPartLabels = [] { return DataRevisionRef{}; };
    window.GetWorkflow().getMeasurementSurface = [] { return std::optional<MeasurementSurface>{}; };
    const auto record = GetComplete(window, Click(window, "Wall", "Start", {{"maxDistance", 24.}, {"sampleSpacing", 1.},
        {"maxBoundaryError", 0.5}, {"directionCount", 1}, {"evaluationRoi", GetRefText(evaluatedRoi.roi->revision)},
        {"labels",parts["result"].toObject()["labelMap"]},{"mesh",surface["result"].toObject()["mesh"]},{"materialThreshold",500.}}), "Succeeded");
    window.GetWorkflow().getPartLabels = savedLabels;
    window.GetWorkflow().getMeasurementSurface = savedSurface;
    Check(record["parameters"].toObject()["labels"] == parts["result"].toObject()["labelMap"]
        && record["parameters"].toObject()["mesh"] == surface["result"].toObject()["mesh"],
        "wall reuses explicitly selected published inputs without latest-operation workflow aliases");
    const auto result = record["result"].toObject();
    Check(result["isDisplayReady"].toBool() && result["coverage"].toDouble() >= 0.5
        && !result["minimum"].isNull() && std::abs(result["minimum"].toDouble()-8.) <= 0.2,
        "manual wall entry measures the known 8 mm slab and creates its 3D display");
    const auto queried = GetComplete(window, Click(window, "Wall", "Result"), "Observed")["result"].toObject();
    Check(queried["result"] == result["result"] && queried["sampleCount"] == result["sampleCount"], "wall query retains exact published result identity");
    const auto inputBeforeHistory=window.GetSession()->GetImageDescriptor()->dataRevision;
    const auto historical=local["result"].toObject()["result"].toString();GetComplete(window,Click(window,"Wall","Result",{{"result",historical}}),"Observed");
    auto* historicalPage=window.GetModule("Wall");historicalPage->Observe();Check(historicalPage->GetObservedState()["viewedResult"]==historical,"history detail binds the selected result");
    GetComplete(window,ClickCurrentPrimary(window,"Wall","ActivateViewed"),"Succeeded");
    Check(window.GetSession()->GetImageDescriptor()->dataRevision==inputBeforeHistory,"activating a wall history result preserves the global image input");
    GetComplete(window, Click(window, "Wall", "SetEvaluation", {{"lower", 7.}, {"upper", 9.}, {"histogramRange", QJsonArray{0,24}}}), "Succeeded");
    historicalPage->Observe();Check(historicalPage->GetObservedState()["viewedResult"]!=historical && historicalPage->GetObservedState()["viewedIsActive"].toBool(),"successful history evaluation selects its new result revision");
    for (const bool visible : {false, true}) GetComplete(window, Click(window, "Wall", "SetDisplay", {{"mode", "Tolerance"}, {"range", QJsonArray{0,24}}, {"isVisible", visible}}), "Succeeded");
    GetComplete(window, Click(window, "Wall", "SelectSample", {{"sampleIndex", "0"}}), "Succeeded");
    auto* wall = window.GetModule("Wall"); wall->Observe();
    Check(FindNode(wall->GetSceneTree(), "wall-result:" + wall->GetObservedState()["result"].toString()) != nullptr,
        "wall scene contains the current visible result");
    Check(!FindNode(wall->GetSceneTree(), "published-graph") && FindNode(wall->GetCatalogTree(), "published-graph"),
        "wall scene objects exclude the independent published-result catalog");
    window.GetWorkflow().onNavigate("Wall", "SetDisplay", {}); QCoreApplication::processEvents(); window.grab().save("wall-manual-ui.png");
    GetComplete(window, Click(window, "Wall", "Clear"), "Succeeded");
    wall->Observe(); Check(wall->GetObservedState()["activeResult"].toString().isEmpty() && !wall->GetObservedState()["viewedIsActive"].toBool(), "wall clear removes activation while retained historical results remain readable");
}
void CheckFeatureSwitchDuringSegmentation(TestWindow& window, const QString& directory)
{
    if (!GetEnabled(window, "Part")) return;
    constexpr int n = 96;
    std::vector<float> data(n*n*n, 0.f);
    for (int z=12; z<84; ++z) for (int y=12; y<84; ++y) for (int x=12; x<84; ++x)
        if (x < 42 || x > 54) data[x+n*(y+n*z)] = 100.f;
    const auto path = directory + "/switch-during-part.raw";
    QFile file(path); Check(file.open(QIODevice::WriteOnly), "switch regression fixture opens");
    Check(file.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()*sizeof(float))) == static_cast<qint64>(data.size()*sizeof(float)), "switch regression fixture writes"); file.close();
    GetComplete(window, Click(window, "Data", "Load", {{"filePath", path}, {"datasetId", "feature-switch-regression"},
        {"dimensions", QJsonArray{n,n,n}}, {"spacingLPS", QJsonArray{1,1,1}}, {"originLPS", QJsonArray{0,0,0}}, {"sourceDigest", ""}}), "Succeeded");
    auto* tabs = window.findChild<QTabBar*>("featureTabs");
    auto* workspaces = window.findChild<QTabBar*>("workspaceTabs");
    auto* part = window.GetModule("Part"); auto* form = part->GetParameterEditor("Start");
    const auto threshold = form->GetField("threshold");
    for (int round = 0; round < 3; ++round) {
        const auto id = Click(window, "Part", "Start", {{"threshold", 50.}, {"minPartVoxels", "1"}});
        Check(!window.GetRecords().GetRecord(id)["isTerminal"].toBool(), "feature switching begins while segmentation is pending");
        int switches = 0;
        const auto change = [&] {
            if (tabs->currentIndex()+1 < tabs->count()) tabs->setCurrentIndex(tabs->currentIndex()+1);
            else { workspaces->setCurrentIndex((workspaces->currentIndex()+1) % workspaces->count()); tabs->setCurrentIndex(0); }
            ++switches;
        };
        for (int i = 0; i < (window.GetModule("Roi") ? 11 : 10); ++i) change();
        QTimer switching; switching.setInterval(1); QObject::connect(&switching, &QTimer::timeout, &window, [&] {
            if (window.GetRecords().GetRecord(id)["isTerminal"].toBool()) switching.stop(); else change();
        }); switching.start();
        GetComplete(window, id, "Succeeded"); switching.stop();
        window.GetWorkflow().onNavigate("Part", "Start", {}); part->Observe();
        Check(switches >= tabs->count() && form == part->GetParameterEditor("Start") && threshold == form->GetField("threshold"),
            "pending segmentation survives all feature tabs and preserves parameter widgets");
        Check(!part->GetObservedState()["isBusy"].toBool() && part->GetObservedState()["hasCurrentParts"].toBool(),
            "returning to Part displays the completed result exactly once");
    }
    CheckFourViewGeometry(window);
    QCoreApplication::processEvents(); window.grab().save("four-view-part-ui.png");
    if (GetEnabled(window, "Artifact")) {
        window.GetWorkflow().onNavigate("Artifact", "Ring", {}); QCoreApplication::processEvents(); window.grab().save("artifact-buttons-ui.png");
    }
}
void CheckSharedRoi(TestWindow& window)
{
#if defined(MANUAL_ROI)
    const auto source=window.GetSession()->GetImageDescriptor()->dataRevision;
    GetComplete(window,Send(window,"Roi","Begin",{{"name","共享区域"}}),"Succeeded");
    const auto record=GetComplete(window,Send(window,"Roi","Commit"),"Succeeded");
    const auto ref=GetRef(record["result"].toObject()["roi"]);
    const auto defaultImage=window.GetSession()->GetImageDescriptor();
    const auto region=window.GetSession()->GetRoiDescriptor(ref);
    Check(defaultImage && region && region->definition.nodes.size()==1,
        "default region preserves a single source-relative primitive");
    const auto& box=region->definition.nodes.front().primitive.localToSource;
    for(int row=0;row<3;++row) {
        double center=defaultImage->origin[row];
        for(int axis=0;axis<3;++axis) {
            const auto expected=defaultImage->direction[row*3+axis]*defaultImage->spacing[axis]
                *(defaultImage->extent[2*axis+1]-defaultImage->extent[2*axis]+1)*0.25;
            Check(std::abs(box[row*4+axis]-expected)<=1e-10,
                "default region affine axes follow half the physical source domain");
            center+=defaultImage->direction[row*3+axis]*defaultImage->spacing[axis]
                *(defaultImage->extent[2*axis]+defaultImage->extent[2*axis+1])*0.5;
        }
        Check(std::abs(box[row*4+3]-center)<=1e-10,
            "default region remains centered in the source coordinate frame");
    }
    Check(window.GetSession()->GetImageDescriptor()->dataRevision==source,"saving a region preserves the business input");
    // 不同工具形状切换后直接保存运算候选，不能把适用字段变化当作几何编辑。
    {
    const std::array<double,3> center{box[3],box[7],box[11]};
    const auto radius=1.2*std::sqrt(box[0]*box[0]+box[4]*box[4]+box[8]*box[8]);
    GetComplete(window,Click(window,"Roi","Begin",{{"name","运算另一形状"},{"shape","Sphere"},{"centerMM",GetValues(center)},{"radius",radius}}),"Succeeded");
    const auto other=GetComplete(window,Click(window,"Roi","Commit"),"Succeeded").value("result").toObject().value("roi");
    GetComplete(window,Click(window,"Roi","Union",{{"roi",GetRefText(ref)},{"operands",QJsonArray{other}},{"outputMode","CreateNew"}}),"PreviewReady");
    const auto combined=GetComplete(window,Click(window,"Roi","Commit"),"Succeeded").value("result").toObject().value("roi");
    const auto combinedRoi=window.GetSession()->GetRoiDescriptor(GetRef(combined));
    Check(combinedRoi && combinedRoi->definition.nodes.back().kind==RoiNodeKind::Union,
        "direct business save preserves the cross-shape Boolean candidate instead of its primitive input");
    if(combinedRoi && combinedRoi->metadata.name!=region->metadata.name)
        std::cerr<<"Boolean metadata mismatch: expected="<<region->metadata.name<<" actual="<<combinedRoi->metadata.name<<std::endl;
    Check(combinedRoi && combinedRoi->metadata.name==region->metadata.name,
        "Boolean save inherits the selected input name instead of the previously edited region");
    GetComplete(window,Send(window,"Roi","Remove",{{"roi",combined}}),"Succeeded");
    GetComplete(window,Send(window,"Roi","Remove",{{"roi",other}}),"Succeeded");
    }
    Check(GetSelectedRoi(*window.GetSession(),GetRefText(ref),source)==ref,"selection freezes the exact saved region revision");
    for(const auto& use:std::vector<std::array<QString,3>>{{"Surface","GlobalAutomatic","analysisRoi"},
        {"Artifact","Diffusion","processingRoi"},{"Part","Paint","editRoi"},{"Alignment","SaveRecipe","inputRoi"}}) {
        if(!GetEnabled(window,use[0]))continue;
        auto* page=window.GetModule(use[0]);page->Observe();
        if(auto* form=page->GetParameterEditor(use[1]))if(auto* field=form->GetField(use[2])) {
            const auto* choices=field->findChild<QComboBox*>("value");
            Check(choices && choices->findData(GetRefText(ref))>=0,"saved ROI is selectable in the consumer form");
        }
    }
    auto* surface=window.GetModule("Surface");
    const auto createRegion=[&] {
        surface->Observe();
        auto* button=surface->GetParameterEditor("GlobalAutomatic")->GetField("analysisRoi")->findChild<QPushButton*>("createRegion");
        Check(button!=nullptr,"business range exposes the shared region editor");button->click();
    };
    if(GetEnabled(window,"Surface")) {
    createRegion();
    GetComplete(window,Send(window,"Roi","Begin",{{"name","业务内新区域"}}),"Succeeded");
    const auto created=GetComplete(window,Send(window,"Roi","Commit"),"Succeeded").value("result").toObject().value("roi");
    if(surface->GetActionParameters("GlobalAutomatic")["analysisRoi"]!=created || !window.GetWorkflow().roiConsumer.isEmpty())
        std::cerr<<GetJsonText(QJsonObject{{"expectedRoi",created},
            {"actualRoi",surface->GetActionParameters("GlobalAutomatic")["analysisRoi"]},
            {"consumer",window.GetWorkflow().roiConsumer}}).toStdString();
    Check(surface->GetActionParameters("GlobalAutomatic")["analysisRoi"]==created
        && window.GetWorkflow().roiConsumer.isEmpty(),"saving a new region returns its exact revision to the requesting business");
    surface->SetParameterPatch("GlobalAutomatic",{{"analysisRoi",""}});
    createRegion();
    GetComplete(window,Send(window,"Roi","Begin"),"Succeeded");
    window.GetModule("Roi")->onStop();
    Check(window.GetWorkflow().roiConsumer.isEmpty(),"stopping the shared editor clears the old return destination");
    }
#if defined(MANUAL_CROP)
    window.SetViewsVisible(true);window.GetWorkflow().onNavigate("Crop","PrepareRoi",{});
    auto* crop=window.GetModule("Crop");
    const auto framesReady=[&]{crop->Observe();const auto state=crop->GetObservedState();return state["framesReady"].toBool() && !state["isBusy"].toBool() && state["appliedHead"]==state["renderedHead"];};
    GetComplete(window,Send(window,"Crop","PrepareRoi",{{"inputRoi",GetRefText(ref)}}),"ParametersCopied");
    Check(Wait(framesReady),"formal ROI confirmation waits for actual crop frames");
    GetComplete(window,Send(window,"Crop","FinishEditing"),"PreviewConfirmed");
    Check(window.GetSession()->GetImageDescriptor()->dataRevision==source,"preparing a saved ROI for crop preserves the source input");
    crop->SetParameterPatch("BuildResult",{{"inputRoi",""}});
    const auto document=crop->GetObservedState().value("documentId");
    const auto root=crop->GetObservedState().value("appliedHead");
    GetComplete(window,Send(window,"Crop","AppendExact",{{"matrix",GetValues(window.GetSession()->GetRoiDescriptor(ref)->definition.nodes.front().primitive.localToSource)}}),"Succeeded");
    Check(Wait(framesReady),"interactive history is presented before switching to a formal ROI");
    const auto historyOperations=crop->GetObservedState().value("operationCount");
    const auto historyNodes=crop->GetObservedState().value("nodes");
    Check(crop->GetObservedState()["appliedHead"]!=root,"history fixture selects a non-root crop node");
    GetComplete(window,Send(window,"Crop","PrepareRoi",{{"inputRoi",GetRefText(ref)}}),"ParametersCopied");
    Check(Wait(framesReady),"formal ROI root selection reaches the displayed preview");
    Check(crop->GetObservedState()["documentId"]==document && crop->GetObservedState()["operationCount"]==historyOperations && crop->GetObservedState()["nodes"]==historyNodes
        && crop->GetObservedState()["appliedHead"]==root,"formal ROI selection preserves the document and history while selecting the build root");
    GetComplete(window,Send(window,"Crop","FinishEditing"),"PreviewConfirmed");
    const auto scope=crop->getGrayScope();
    Check(scope.roi==ref && !scope.filter,"formal ROI statistics use the region without intersecting old crop history");
    crop->SetParameterPatch("BuildResult",{{"inputRoi",""}});
    GetComplete(window,Send(window,"Crop","CloseDocument"),"Succeeded");
    window.SetViewsVisible(false);
#endif
    const auto selectableBeforeArchive=GetRoiChoices(*window.GetSession(),source).size();
    const auto catalogBeforeArchive=GetRoiChoices(*window.GetSession(),source,true).size();
    GetComplete(window,Send(window,"Roi","SetMetadata",{{"roi",GetRefText(ref)},{"isArchived",true}}),"Succeeded");
    Check(GetRoiChoices(*window.GetSession(),source).size()+1==selectableBeforeArchive
        && GetRoiChoices(*window.GetSession(),source,true).size()==catalogBeforeArchive,"archived regions remain manageable and leave consumer choices");
    bool rejected=false;try{(void)GetSelectedRoi(*window.GetSession(),GetRefText(ref),source);}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"an archived region cannot silently run as a selected analysis range");
    GetComplete(window,Send(window,"Roi","SetMetadata",{{"roi",GetRefText(ref)},{"isArchived",false}}),"Succeeded");
    auto other=source;++other.generation;rejected=false;
    try{(void)GetSelectedRoi(*window.GetSession(),GetRefText(ref),other);}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"a saved ROI from another source revision is explicitly rejected");
    window.GetModule("Roi")->Observe();
    Check(FindNode(window.GetModule("Roi")->GetCatalogTree(),"roi:"+GetRefText(ref))!=nullptr,"region management exposes the saved catalog object");
    const auto image=window.GetSession()->GetImageDescriptor();std::array<double,3> center=image->origin;
    for(int row=0;row<3;++row)for(int axis=0;axis<3;++axis)
        center[row]+=image->direction[row*3+axis]*image->spacing[axis]*(image->extent[axis*2]+image->extent[axis*2+1])*.5;
    for(const auto* shape:{"Sphere","Cylinder"}) {
        GetComplete(window,Send(window,"Roi","Begin",{{"shape",shape},{"centerMM",GetValues(center)},
            {"radius",2.0},{"height",4.0}}),"Succeeded");
        auto* roiPanel=window.GetModule("Roi");roiPanel->SetParameterPatch("SetDraft",{{"radius",3.0},{"group","曲面测试"}});roiPanel->Observe();
        Check(roiPanel->GetObservedState()["draftParametersChanged"].toBool(),"curve geometry and metadata edits require applying the draft before save");
        GetComplete(window,Send(window,"Roi","SetDraft"),"Succeeded");roiPanel->Observe();
        Check(!roiPanel->GetObservedState()["draftParametersChanged"].toBool(),"applying curve geometry and metadata clears the dirty draft state");
        const auto curve=GetComplete(window,Send(window,"Roi","Commit"),"Succeeded").value("result").toObject().value("roi");
        const auto descriptor=window.GetSession()->GetRoiDescriptor(GetRef(curve));
        Check(descriptor && descriptor->definition.nodes.front().primitive.origin==center
            && descriptor->definition.nodes.front().primitive.radius==3.0 && descriptor->metadata.group=="曲面测试","curve editor fields save the intended source-space geometry");
    }
    std::array<double,3> voxelCenter=image->origin;
    for(int row=0;row<3;++row)for(int axis=0;axis<3;++axis)
        voxelCenter[row]+=image->direction[row*3+axis]*image->spacing[axis]*((image->extent[axis*2]+image->extent[axis*2+1])/2);
    const double angle=2*std::acos(-1.)/180.,half=.3;
    const std::array<double,16> rotated{half*std::cos(angle),-half*std::sin(angle),0,voxelCenter[0],
        half*std::sin(angle),half*std::cos(angle),0,voxelCenter[1],0,0,half,voxelCenter[2],0,0,0,1};
    for(const bool apply:{false,true}){
        GetComplete(window,Send(window,"Roi","Begin",{{"name","旋转盒候选"},{"shape","Box"},{"boxToSource",GetValues(rotated)}}),"Succeeded");
        auto* roiPanel=window.GetModule("Roi");roiPanel->Observe();
        Check(!roiPanel->GetObservedState()["draftParametersChanged"].toBool(),"an untouched rotated box is not an edited parameter draft");
        GetComplete(window,Send(window,"Roi","Dilate",{{"radius",2.0},{"radiusUnit","Voxels"}}),"PreviewReady");
        if(apply)GetComplete(window,Send(window,"Roi","ApplyOperation"),"Succeeded");
        const auto candidate=roiPanel->getGrayScope().key;
        auto* name=roiPanel->GetParameterEditor("SetDraft")->GetField("name")->findChild<QLineEdit*>("value");
        Check(name!=nullptr,"ROI metadata field is available");name->setText("仅改名称的旋转结果");
        Check(roiPanel->getGrayScope().key==candidate,"metadata editing preserves the operation candidate version");
        const auto saved=GetComplete(window,Send(window,"Roi","Commit"),"Succeeded").value("result").toObject().value("roi");
        const auto descriptor=window.GetSession()->GetRoiDescriptor(GetRef(saved));
        Check(descriptor && descriptor->definition.nodes.front().primitive.shape==RoiShape::VoxelSet
            && descriptor->metadata.name=="仅改名称的旋转结果","saving an unchanged rotated tool preserves the computed region before and after applying");
    }
    if(GetEnabled(window,"Surface")) {
    surface->SetParameterPatch("GlobalAutomatic",{{"analysisRoi",""}});createRegion();
    GetComplete(window,Send(window,"Roi","Begin",{{"name","异步业务回填"},{"shape","Box"},{"boxToSource",GetValues(rotated)}}),"Succeeded");
    const auto operation=Send(window,"Roi","Dilate",{{"radius",2.0},{"radiusUnit","Voxels"}});
    const auto pending=Send(window,"Roi","Commit");
    Check(!window.GetRecords().GetRecord(pending)["isTerminal"].toBool(),"saving during the operation uses the pending completion path");
    const auto asynchronous=GetComplete(window,pending,"Succeeded").value("result").toObject().value("roi");
    GetComplete(window,operation,"PreviewReady");
    Check(surface->GetActionParameters("GlobalAutomatic")["analysisRoi"]==asynchronous
        && window.GetWorkflow().roiConsumer.isEmpty(),"pending ROI save returns its exact result to the requesting business once");
    }
#endif
}
void StartSelfTest(TestWindow& window)
{
    QCoreApplication::processEvents();
    CheckFourViewGeometry(window);
    CheckUiAndRecords(window);
    CheckBusinessWorkflow(window);
    CheckWorkbenchV1(window);
    CheckSceneRefresh(window);
    CheckParameterLayout(window);
    CheckBusinessParameters(window);
    CheckBooleanControls(window);
    auto* view = window.GetModule("View");
    view->SetParameterPatch("Reset", {{"viewId", "missing"}});
    GetComplete(window, SendUnavailable(window, "View", "Reset"), "Rejected");
    const auto* log = window.findChild<QPlainTextEdit*>("businessLog");
    Check(log && log->toPlainText().contains("显示与导航 · 重置视图") && log->toPlainText().contains("操作被拒绝"),
        "unavailable display commands retain Chinese rejection details for programmatic requests");
    Check(log->toPlainText().contains(QRegularExpression("\\[[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}\\]")),
        "business timestamps use readable ASCII digits regardless of system locale");
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary fixture directory");
    std::vector<float> voxels(24 * 24 * 24, 0.0f);
    for (int z = 3; z < 20; ++z) for (int y = 3; y < 20; ++y) for (int x = 3; x < 20; ++x)
        voxels[x + 24 * (y + 24 * z)] = x < 10 || x > 12 ? 100.0f : 0.0f;
    // 封闭内孔使单个3D目标也有可显示的孔隙网格，不能依靠空切片标签冒充显示成功。
    for (int z = 8; z < 11; ++z) for (int y = 8; y < 11; ++y) for (int x = 5; x < 8; ++x)
        voxels[x + 24 * (y + 24 * z)] = 0.f;
    const auto path = directory.filePath("拖拽 数据-float32.raw");
    QFile file(path); Check(file.open(QIODevice::WriteOnly), "fixture opens");
    const QByteArray bytes(reinterpret_cast<const char*>(voxels.data()), static_cast<int>(voxels.size() * sizeof(float)));
    Check(file.write(bytes) == bytes.size(), "fixture writes"); file.close();
    Check(GetInputDimensions(path,QJsonValue()) == std::array<int,3>{24,24,24}, "unprovided RAW dimensions are resolved only when file length identifies a cube");
    window.SetViewsVisible(false);
    const QJsonObject load{{"filePath", path}, {"datasetId", "synthetic-manual-contract-test"}, {"dimensions", QJsonArray{24, 24, 24}}, {"spacingLPS", QJsonArray{1,1,1}},
        {"evidenceKind", "synthetic-regression"}, {"sourceDigest", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
    auto* dataPanel = window.GetModule("Data"); dataPanel->SetParameterPatch("Load", load);
    const auto loadDraft = dataPanel->GetParameterEditor("Load")->GetValue();
    const auto recordCount = window.GetRecords().GetRecords()["records"].toArray().size();
    window.GetWorkflow().onNavigate("View", "Set", {});
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QTimer dialogDeadline; dialogDeadline.setSingleShot(true);
    QObject::connect(&dialogDeadline, &QTimer::timeout, &window, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    bool opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>("inputFileDialog");
        if (!dialog) return;
        opened = true; dialog->reject();
    });
    dialogDeadline.start(5000); window.findChild<QPushButton*>("openVolume")->click(); dialogDeadline.stop();
    Check(opened && dataPanel->GetParameterEditor("Load")->GetValue() == loadDraft,
        "cancelling the top-level file picker preserves the loading parameters");
    opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>("inputFileDialog");
        if (!dialog) return;
        opened = true; dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    });
    dialogDeadline.start(5000); window.findChild<QPushButton*>("openVolume")->click(); dialogDeadline.stop();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);
    Check(opened && dataPanel->isVisible() && dataPanel->GetCurrentAction() == "Load"
        && dataPanel->GetParameterEditor("Load")->GetValue() == loadDraft
        && window.GetRecords().GetRecords()["records"].toArray().size() == recordCount,
        "opening a file reveals the loading card and preserves geometry without submitting a request");
    dataPanel->GetParameterEditor("Load")->GetField("filePath")->findChild<QLineEdit*>("value")->clear(); DropFile(dataPanel->GetParameterEditor("Load")->GetField("filePath")->findChild<QLineEdit*>("value"), path);
    Check(dataPanel->GetParameters()["filePath"].toString() == path, "drop preserves Unicode and spaces in file path");
    DropFile(&window, path);
    Check(dataPanel->GetParameters()["filePath"].toString() == path, "dropping onto window fills data path without loading");
    Check(!window.GetSession()->GetImageDescriptor(), "file drop never silently loads an unconfirmed geometry");
    Check(dataPanel->GetActions().contains("ExportSlices"), "slice export is available alongside the four views");
    const auto firstLoad = Click(window, "Data", "Load", load);
    GetComplete(window, Send(window, "Data", "Load", load), "Rejected");
    GetComplete(window, firstLoad, "Succeeded");
    Check(window.GetSession()->GetImageDescriptor().has_value(), "hidden windows do not prevent input commit");
    const auto original = *window.GetSession()->GetImageDescriptor();
#if defined(MANUAL_ALIGNMENT)
    if (GetEnabled(window, "Alignment")) {
        const QJsonValue volume = GetComplete(window, Send(window, "Alignment", "EvaluateVoxelVolume", {
            {"source", GetRefText(original.dataRevision)}, {"extent", QJsonArray{0,1,0,1,0,1}},
            {"storedValueRange", QJsonArray{0,1000}}, {"requireEnclosedSelection", false}}), "FullyDetermined").value("result").toObject().value("voxelVolume");
        const auto stats = GetComplete(window, Send(window, "Alignment", "VoxelVolumeResult", {{"voxelVolume", volume}}), "Observed")["result"].toObject();
        Check(stats["total"]=="8" && stats["selected"]=="8" && stats["volumeMm3"].toDouble()==8.,
            "independent voxel result preserves the known eight unit cells before any alignment");
        const auto path = directory.filePath("独立体积报告.json");
        GetComplete(window, Click(window, "Alignment", "ExportReport", {{"outputPath",path},{"format","Json"},
            {"result","current"},{"inspection",QJsonValue()},{"deviation",QJsonValue()},
            {"voxelVolume",volume},{"captureCurrentDisplay",false}}), "ReportExported");
        const auto report = LoadJson(path);
        Check(report["alignments"].toArray().isEmpty() && report["voxelVolumes"].toArray().size()==1,
            "volume-only report is not blocked by an unavailable alignment and invents no alignment result");
    }
#endif
    const auto initialIso = original.scalarRange[0] * 0.5 + original.scalarRange[1] * 0.5;
    Check(Wait([&] {
        const auto state = window.GetSession()->GetRenderViewState({"primary-3d"});
        if (!state || state->isoThreshold != initialIso) return false;
        return true;
    }), "initial isosurface commits the native scalar-range midpoint with the data load");
    const auto maskA = GetComplete(window, Send(window, "Data", "CreateMask", {{"purpose", "分支来源 A"}}), "MaskPublished").value("result").toObject().value("mask");
    GetComplete(window,Send(window,"Data","KeepResult",{{"graphRevision",maskA}}),"Succeeded");
    const auto maskB = GetComplete(window, Send(window, "Data", "CreateMask", {{"purpose", "分支来源 B"}}), "MaskPublished").value("result").toObject().value("mask");
    const auto graph = window.GetWorkflow().getPublishedGraph();
    for (const auto ref : {maskA, maskB}) {
        bool matched = false;
        for (const auto item : graph["nodes"].toArray()) if (item.toObject()["ref"] == ref)
            matched = item.toObject()["parents"].toArray().contains(GetRefText(original.dataRevision));
        Check(matched, "separate real publications retain the same source parent in the data graph");
    }
    auto* dataTree = dataPanel->GetCatalogTree(); dataPanel->Observe();
    Check(static_cast<SceneGraphTree*>(dataTree)->GetGraph().lanes >= 2, "published sibling data uses real graph branches");
    auto* sourceNode = FindNode(dataTree, "published:" + GetRefText(original.dataRevision));
    Check(sourceNode != nullptr, "published input is available as a scene node");
    GetComplete(window, ClickNodeAction(window, dataTree, sourceNode, "UseData"), "Succeeded");
    Check(window.GetSession()->GetRenderViewState({"primary-3d"})->isoThreshold == initialIso,
        "reselecting the same data preserves the initial ISO");
    window.grab().save("scene-ui-data-graph.png");
    GetComplete(window, Click(window, "View", "Reset", {{"viewId", "missing"}}), "Failed");
    GetComplete(window, Send(window, "Data", "Select", {{"revision", GetRefText(original.dataRevision)}, {"expectedBindingRevision", "0"}}), "Failed");
    GetComplete(window, Send(window, "View", "Set", {{"viewId", "primary-3d"}, {"iso", 50.0}}), "Succeeded");
    CheckBooleanRequests(window);
    CheckSharedRoi(window);
    CheckViewCapabilities(window);
    const auto burstStart = window.GetUpdateCount();
    std::vector<std::uint64_t> burst;
    for (int index = 0; index < 10; ++index) burst.push_back(Send(window, "View", "Set", {{"iso", 41.0 + index}}));
    // Host 的显示完成检查绑定精确 presentation revision：被后续设置替代的请求返回失败，不能伪报成功。
    for (const auto id : burst) GetComplete(window, id, id == burst.back() ? "Succeeded" : "Failed");
    Check(window.GetUpdateCount() - burstStart < burst.size(), "burst notifications merge and every command retains its exact completion");
    Check(window.GetSession()->GetRenderViewState({"primary-3d"})->isoThreshold == 50.0, "burst keeps final requested view state");
    GetComplete(window, Click(window, "View", "Set", {{"mode", "CompositeVolume"},{"opacity",0.8}}), "Succeeded");
    Check(Wait([&]{view->Observe();return !view->GetObservedState()["parametersChanged"].toBool()
        && view->GetObservedState()["appliedDisplay"].toObject()["mode"]=="CompositeVolume";}),
        "applied volume settings stay clean when inapplicable ISO and window fields are omitted");
    view->SetParameterPatch("Set",{{"opacity",0.6}});view->Observe();
    Check(view->GetObservedState()["parametersChanged"].toBool(),"editing volume opacity marks the visible draft as unapplied");
    GetComplete(window,ClickCurrentPrimary(window,"View","Set"),"Succeeded");
    Check(Wait([&]{view->Observe();return !view->GetObservedState()["parametersChanged"].toBool();}),
        "the natural primary clears the volume draft only after applying it");
    GetComplete(window, Send(window, "View", "Set", {{"mode", "CompositeIsoSurface"}}), "Succeeded");
    Check(window.GetSession()->GetRenderViewState({"primary-3d"})->isoThreshold == 50.0,
        "returning to isosurface preserves the explicitly supplied ISO");
    CheckCropWorkflow(window);
    CheckCropMouseInteraction(window);
    window.SetViewsVisible(false);
    if (GetEnabled(window, "Gap")) {
        GetComplete(window, Send(window, "Gap", "Overlay"), "Rejected");
        GetComplete(window, Click(window, "Gap", "Start", {{"absoluteIsoValue", 50.0}}), "Succeeded");
        GetComplete(window, Send(window, "Gap", "Overlay"), "Succeeded");
        GetComplete(window, Send(window, "Gap", "Exit"), "Exited");
    }
    if (GetEnabled(window, "Part")) {
        std::map<std::string, double> originalOpacities;
        for (const auto* id : {"primary-3d"})
            originalOpacities[id] = window.GetSession()->GetRenderViewState({id})->material.opacity;
        GetComplete(window, Click(window, "Part", "Start", {{"threshold", 50.0}}), "Succeeded");
        const auto catalog = GetComplete(window, Send(window, "Part", "Catalog"), "Observed")["result"].toObject();
        const auto parts = catalog["parts"].toArray(); Check(parts.size() >= 2, "two parts from actual segmentation entry");
        for (const auto& part : parts) Check(part.toObject()["opacity"].toDouble() == 1
            && part.toObject()["colorRGBA"].toArray() == QJsonArray{0.72,0.72,0.72,1.0},
            "new parts use an opaque neutral preview without translucent inner-shell overlap");
        (void)CheckPartDisplay(window);
        window.SetViewsVisible(true); CheckRenderModeSwitch(window); window.SetViewsVisible(false);
        ClickPartNode(window, 0); ClickPartNode(window, 1); ClickPartNode(window, 0);
        CheckNodeParameters(window); CheckNodeErrorBoundary(window);
        const auto target = parts[0].toObject();
        CheckPartBooleanRequests(window, target);
        CheckOverlaySwitch(window, "Part", originalOpacities);
        auto* editPanel = window.GetModule("Part");
        window.GetWorkflow().onNavigate("Part", "Merge", {});
        auto* editTree = editPanel->GetCatalogTree();
        const auto partNode = [&](const QJsonObject& binding) -> QTreeWidgetItem* {
            for (QTreeWidgetItemIterator it(editTree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toJsonObject()["binding"].toObject() == binding) return *it;
            return nullptr;
        };
        Check(Wait([&] { return partNode(target["binding"].toObject()) != nullptr; }), "editable part nodes exist");
        editTree->expandAll(); ClickNode(editTree, partNode(target["binding"].toObject()));
        ClickNode(editTree, partNode(parts[1].toObject()["binding"].toObject()), Qt::ControlModifier);
        Check(editTree->selectedItems().size() == 2 && editPanel->GetCurrentAction() == "Merge" && editPanel->GetParameters()["parts"].toArray().size() == 2,
            "multi-select prepares exact part bindings for merge");
        GetComplete(window, Send(window, "Part", "SetState", {{"target", target["binding"]}, {"name", "节点刷新测试零件"}}), "Succeeded");
        Check(Wait([&] { const auto* item = partNode(target["binding"].toObject()); return item && item->text(0).contains("节点刷新测试零件") && editTree->selectedItems().size() == 2; }),
            "scene rebuild preserves multi-selection by stable part identity");
        QCoreApplication::processEvents(); window.grab().save("scene-ui-parts.png");
        GetComplete(window, SendUnavailable(window, "Part", "Commit"), "Rejected");
        GetComplete(window, Click(window, "Part", "EditSelected", {{"target", target["binding"]}}), "ParametersCopied");
        Check(editPanel == window.GetModule("Part") && editPanel->GetCurrentAction() == "Paint"
            && editPanel->GetParameterEditor("Paint")->isVisible(), "editing a selected part reveals the paint tool in the same workspace");
        GetComplete(window, Send(window, "Part", "SetState", {{"target", target["binding"]}}), "Succeeded");
        const auto formal = window.GetSession()->GetLabelMapDescriptors(); Check(!formal.empty(), "formal label descriptor exists");
        const auto formalRef = formal.front().dataRevision;
        const auto edit = QJsonObject{{"target", target["binding"]}, {"sourcePointsMM", QJsonArray{target["centroidSourceMM"]}}, {"radiusMM", 1.5}};
        GetComplete(window, Click(window, "Part", "Erase", edit), "PreviewReady");
        Check(window.GetSession()->GetLabelMapDescriptors().front().dataRevision == formalRef, "preview does not publish formal labels");
        GetComplete(window, Send(window, "Part", "Discard"), "Discarded");
        GetComplete(window, Click(window, "Part", "Erase", edit), "PreviewReady");
        GetComplete(window, Click(window, "Part", "Commit"), "Succeeded");
        Check(window.GetSession()->GetLabelMapDescriptors().front().dataRevision != formalRef, "commit changes formal labels");
        GetComplete(window, Send(window, "Part", "SetState", {{"target", target["binding"]}, {"expectedCatalogRevision", catalog["catalogRevision"]}}), "Failed");
        GetComplete(window, Send(window, "Part", "Undo"), "PreviewReady");
        GetComplete(window, Click(window, "Part", "Commit"), "Succeeded");
        GetComplete(window, Send(window, "Part", "Redo"), "PreviewReady");
        GetComplete(window, Send(window, "Part", "Discard"), "Discarded");
        const auto restored = GetComplete(window, Send(window, "Part", "Catalog"), "Observed")["result"].toObject()["parts"].toArray();
        QJsonArray mergeParts; for (const auto part : restored) mergeParts.append(part.toObject()["binding"]);
        GetComplete(window, Click(window, "Part", "Merge", {{"parts", mergeParts}}), "PreviewReady");
        GetComplete(window, Click(window, "Part", "Commit"), "Succeeded");
        const auto merged = GetComplete(window, Send(window, "Part", "Catalog"), "Observed")["result"].toObject();
        Check(merged["parts"].toArray().size() == 1 && merged["relations"].toArray().size() >= 2, "real merge exposes both previous part bindings");
        editPanel->Observe();
        bool joined = false;
        for (QTreeWidgetItemIterator it(editTree); *it; ++it) {
            const auto node = (*it)->data(0, Qt::UserRole).toJsonObject();
            if (node.contains("binding")) joined = joined || node["parents"].toArray().size() >= 2;
        }
        Check(joined, "merged scene part has both real parents, not just a UI category parent");
        SaveScene(window, editTree, "scene-ui-merge.png");
        const QJsonArray splitSeeds{QJsonObject{{"imageIndex", QJsonArray{6, 11, 11}}, {"target", "1"}}, QJsonObject{{"imageIndex", QJsonArray{16, 11, 11}}, {"target", "2"}}};
        GetComplete(window, Click(window, "Part", "Split", {{"target", merged["parts"].toArray().first().toObject()["binding"]}, {"seeds", splitSeeds}}), "PreviewReady");
        GetComplete(window, Click(window, "Part", "Commit"), "Succeeded");
        const auto split = GetComplete(window, Send(window, "Part", "Catalog"), "Observed")["result"].toObject();
        Check(split["parts"].toArray().size() == 2, "actual split restores two independently actionable scene parts");
        for (const auto& part : split["parts"].toArray()) Check(part.toObject()["opacity"].toDouble() == 1
            && part.toObject()["colorRGBA"].toArray() == QJsonArray{0.72,0.72,0.72,1.0},
            "split children retain the opaque neutral default presentation");
        (void)CheckPartDisplay(window);
        QJsonArray splitParents;
        for (const auto relation : split["relations"].toArray()) if (relation.toObject()["kind"] == "拆分") splitParents.append(relation.toObject()["previous"]);
        Check(splitParents.size() == 2 && splitParents[0] == splitParents[1], "split children share their real previous part binding");
        editPanel->Observe(); SaveScene(window, editTree, "scene-ui-split-graph.png");
    }
    if (GetEnabled(window, "Artifact")) {
        GetComplete(window, SendUnavailable(window, "Artifact", "Commit"), "Rejected");
        GetComplete(window, Send(window, "Artifact", "Diffusion"), "Ready");
        GetComplete(window, Send(window, "Artifact", "Diffusion"), "Rejected");
        GetComplete(window, Click(window, "Artifact", "Commit"), "Published");
        Check(window.GetSession()->GetImageDescriptor()->dataRevision == original.dataRevision, "artifact publication does not select input");
        GetComplete(window, Send(window, "Artifact", "SelectOutput"), "Succeeded");
        Check(window.GetSession()->GetImageDescriptor()->dataRevision != original.dataRevision, "explicit select changes input");
        const auto firstOutput = GetRefText(window.GetSession()->GetImageDescriptor()->dataRevision);
        GetComplete(window, Send(window, "Artifact", "RestoreSource"), "Succeeded");
        auto selectPublished = [&](const QString& ref) {
            window.GetWorkflow().onNavigate("Data", "Descriptor", {}); dataPanel->Observe();
            auto* item = FindNode(dataTree, "published:" + ref); Check(item != nullptr, "historical volume remains selectable in the data graph");
            GetComplete(window, ClickNodeAction(window, dataTree, item, "UseData"), "Succeeded");
            Check(GetRefText(window.GetSession()->GetImageDescriptor()->dataRevision) == ref, "scene context action selects the exact published revision");
        };
        selectPublished(firstOutput);
        GetComplete(window, Send(window, "Artifact", "Diffusion"), "Ready");
        auto* artifact=window.GetModule("Artifact");artifact->Observe();
        auto* primary=artifact->findChild<QPushButton*>("businessPrimary");
        Check(primary && primary->isEnabled() && primary->text().contains("发布并使用"),"ready candidate presents a concrete publish-and-use primary");
        const auto publishId=static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size())+1;
        primary->click();
        const auto child = GetComplete(window,publishId,"Published").value("result").toObject().value("correctedVolume");
        GetComplete(window,publishId+1,"Succeeded");
        Check(GetRefText(window.GetSession()->GetImageDescriptor()->dataRevision)==child.toString(),"actual stage primary adopts the exact published candidate and advances the input");
        // 历史分支比较显式保留；默认重复发布不再无限持有未使用的整卷结果。
        GetComplete(window, Send(window,"Data","KeepResult",{{"graphRevision",child}}),"Succeeded");
        selectPublished(GetRefText(original.dataRevision));
        GetComplete(window, Send(window, "Artifact", "Diffusion"), "Ready");
        const auto sibling = GetComplete(window, Send(window, "Artifact", "Commit"), "Published").value("result").toObject().value("correctedVolume");
        const auto published = window.GetWorkflow().getPublishedGraph()["nodes"].toArray();
        for (const auto expected : {std::make_pair(child, QJsonValue(firstOutput)), std::make_pair(sibling, QJsonValue(GetRefText(original.dataRevision)))}) {
            bool found = false;
            for (const auto value : published) if (value.toObject()["ref"] == expected.first) found = value.toObject()["parents"].toArray().contains(expected.second);
            Check(found, "continuing from historical input publishes a real branch with the exact selected parent");
        }
        selectPublished(GetRefText(original.dataRevision));
        GetComplete(window,Send(window,"Data","UnloadResult",{{"graphRevision",child}}),"Failed");
        GetComplete(window,Send(window,"Data","ReleaseResult",{{"graphRevision",child}}),"Succeeded");
        const auto afterRelease=window.GetWorkflow().getPublishedGraph()["nodes"].toArray();
        for (const auto value : afterRelease)
            Check(value.toObject()["ref"]!=child && value.toObject()["ref"]!=firstOutput,
                "unpinning an obsolete branch releases it and its unused upstream result");
        SaveScene(window, dataTree, "scene-ui-published-branches.png");
    }
    if (GetEnabled(window, "Surface")) {
        GetComplete(window, Send(window, "Surface", "GlobalAutomatic"), "Succeeded");
        window.GetModule("Surface")->Observe();
        Check(!window.GetModule("Surface")->GetObservedState()["parametersChanged"].toBool(),"a formal surface does not treat an inapplicable seed field as a changed parameter");
        GetComplete(window, window.GetModule("View")->SendAction("Set", {{"viewId", "primary-3d"}, {"mode", "CompositeVolume"}}), "Succeeded");
        GetComplete(window, SendUnavailable(window, "Surface", "CopyIsoToDisplay"), "Rejected");
        GetComplete(window, window.GetModule("View")->SendAction("Set", {{"viewId", "primary-3d"}, {"mode", "CompositeIsoSurface"}}), "Succeeded");
        window.GetModule("Surface")->Observe();
        Check(window.GetModule("Surface")->findChild<QPushButton*>("action_CopyIsoToDisplay")->isEnabled(),
            "estimated ISO can be applied again after returning to isosurface mode");
        GetComplete(window, Send(window, "Surface", "GlobalAutomatic", {{"initialIsoValue", 50.0}}), "Succeeded");
        CheckOverlaySwitch(window, "Surface");
        Check((window.GetWorkflow().GetMeasurementSurface() && GetDataRevisionRefValid(window.GetWorkflow().GetMeasurementSurface()->mesh)), "material surface is a formal metrology input");
#if defined(MANUAL_ALIGNMENT)
        if (GetEnabled(window, "Alignment")) {
            GetComplete(window, Send(window, "Surface", "GlobalAutomatic", {{"initialIsoValue", 50.0}}), "Succeeded");
            const auto samples = GetComplete(window, Send(window, "Surface", "SamplePoints"), "Observed")["result"].toObject()["samples"].toArray();
            GetComplete(window, Click(window, "Surface", "OpenAlignment"), "ParametersCopied");
            QJsonArray points;
            for (const auto sample : samples) if (sample.toObject()["flags"].toInt() == 0 && sample.toObject()["validSupportRatio"].toDouble() > 0)
                points.append(sample);
            Check(points.size() >= 3, "surface exposes bounded exact vertex samples");
            const auto origin = GetArray<double, 3>(points[0].toObject()["sourcePoint"]);
            const auto delta = [&](int i) { const auto p = GetArray<double, 3>(points[i].toObject()["sourcePoint"]); return std::array<double, 3>{p[0]-origin[0], p[1]-origin[1], p[2]-origin[2]}; };
            const auto squared = [](const auto& p) { return p[0]*p[0] + p[1]*p[1] + p[2]*p[2]; };
            int second = 1, third = 2;
            for (int i = 1; i < points.size(); ++i) if (squared(delta(i)) > squared(delta(second))) second = i;
            const auto axis = delta(second);
            const auto area = [&](int i) { const auto p = delta(i); return squared(std::array<double, 3>{axis[1]*p[2]-axis[2]*p[1], axis[2]*p[0]-axis[0]*p[2], axis[0]*p[1]-axis[1]*p[0]}); };
            for (int i = 1; i < points.size(); ++i) if (area(i) > area(third)) third = i;
            Check(area(third) > 1e-8, "reference samples are not collinear");
            auto reference = GetReferenceTemplate(AlignmentMethod::Rps);
            reference["evidenceKind"] = "synthetic-regression";
            reference["provenance"] = "synthetic fixture identity alignment; not real CT acceptance";
            auto recipe = reference["recipe"].toObject();
            auto geometries = recipe["geometries"].toArray(); auto constraints = recipe["constraints"].toArray();
            const int selected[]{0, second, third};
            for (int i = 0; i < 3; ++i) {
                const auto sample = points[selected[i]].toObject(); auto geometry = geometries[i].toObject();
                auto region = geometry["region"].toObject(); region["vertexIds"] = QJsonArray{sample["vertexId"]}; geometry["region"] = region; geometries[i] = geometry;
                for (int axisIndex = 0; axisIndex < 3; ++axisIndex) { auto c = constraints[i*3+axisIndex].toObject(); c["nominalPoint"] = sample["sourcePoint"]; constraints[i*3+axisIndex] = c; }
            }
            recipe["geometries"] = geometries; recipe["constraints"] = constraints; reference["recipe"] = recipe;
            const auto refPath = directory.filePath("synthetic-reference.json"); ExportJson(refPath, reference);
            window.GetWorkflow().onNavigate("Alignment", "ImportReference", {});
            DropFile(window.GetModule("Alignment")->GetParameterEditor("ImportReference")->GetField("filePath")->findChild<QLineEdit*>("value"), refPath);
            GetComplete(window, Click(window, "Alignment", "ImportReference"), "ReferencePublished");
            GetComplete(window, Send(window, "Alignment", "SaveRecipe"), "FullyDetermined");
            GetComplete(window, Send(window, "Alignment", "Start"), "FullyDetermined");
            const auto firstPoint=points[0].toObject();const auto secondPoint=points[second].toObject();
            GetComplete(window,Click(window,"Alignment","InspectGeometry",{{"kind","Point"},{"vertexIds",QJsonArray{firstPoint["vertexId"]}},{"measurementMetric","CenterX"},{"nominal",origin[0]},{"nominalSource","合成夹具解析坐标"},{"deviationLimits",QJsonArray{-.0001,.0001}}}),"FullyDetermined");
            const auto singleInspection=GetComplete(window,ClickCurrentPrimary(window,"Alignment","InspectionResult"),"Observed");Check(!singleInspection["result"].toObject()["items"].toArray().isEmpty(),"single geometry natural next action reads its own result");
            const auto firstInspection=singleInspection["result"].toObject()["inspection"].toString();
            GetComplete(window,Click(window,"Alignment","KeepResult",{{"graphRevision",firstInspection}}),"Succeeded");
            const QJsonObject firstGeometry{{"kind","Point"},{"vertexIds",QJsonArray{firstPoint["vertexId"]}},{"targetBounds",QJsonValue()},{"targetDirection",QJsonArray{0,0,1}}};
            auto secondGeometry=firstGeometry;secondGeometry["vertexIds"]=QJsonArray{secondPoint["vertexId"]};
            GetComplete(window,Click(window,"Alignment","InspectPair",{{"first",firstGeometry},{"second",secondGeometry},{"measurementMetric","CenterDistance"},{"nominal",std::sqrt(squared(delta(second)))},{"nominalSource","合成夹具两点解析距离"},{"deviationLimits",QJsonArray{-.0001,.0001}}}),"FullyDetermined");
            const auto pairInspection=GetComplete(window,ClickCurrentPrimary(window,"Alignment","InspectionResult"),"Observed");
            const auto measuredItems=pairInspection["result"].toObject()["items"].toArray();Check(measuredItems.size()==1 && measuredItems[0].toObject()["verdict"]=="Pass","pair measurement reports the analytical nominal and tolerance in alignment length units");
            const auto historyInspection=GetComplete(window,Click(window,"Alignment","InspectionResult",{{"inspection",firstInspection}}),"Observed");
            Check(historyInspection["result"].toObject()["inspection"]==firstInspection && historyInspection["result"].toObject()["items"].toArray().size()==2,"historical inspection detail preserves I1 after I2 succeeds");
            auto* inspectionPage=window.GetModule("Alignment");window.SetViewsVisible(true);QCoreApplication::processEvents();window.GetWorkflow().onShowStatistics("Alignment");inspectionPage->RefreshStatistics();
            Check(inspectionPage->GetStatisticsWidget()->GetHistogram()["description"].toString().contains(firstInspection),"historical chart uses the same I1 as its detail");
            const auto historyReportPath=directory.filePath("history-inspection-report.json");
            GetComplete(window,Click(window,"Alignment","ExportReport",{{"outputPath",historyReportPath},{"inspection",firstInspection},{"deviation",QJsonValue()},{"voxelVolume",QJsonValue()},{"captureCurrentDisplay",false}}),"ReportExported");
            const auto historyReports=LoadJson(historyReportPath)["inspections"].toArray();Check(historyReports.size()==1 && historyReports[0].toObject()["result"]==firstInspection,"report form exports the explicitly selected historical I1");
            window.SetViewsVisible(false);
            GetComplete(window, Click(window, "Alignment", "Result"), "Observed");
            auto* resultTree = window.GetModule("Alignment")->GetCatalogTree();
            bool hasResidual = false;
            for (QTreeWidgetItemIterator it(resultTree); *it; ++it) hasResidual = hasResidual || (*it)->data(0, Qt::UserRole).toJsonObject()["id"].toString().startsWith("residual:");
            Check(hasResidual, "alignment residuals are visible as result nodes");
            QCoreApplication::processEvents(); window.grab().save("scene-ui-alignment.png");
            GetComplete(window, Send(window, "Alignment", "Activate"), "FullyDetermined");
            CheckOverlaySwitch(window, "Alignment");
            GetComplete(window, Send(window, "Alignment", "Start"), "Rejected");
            GetComplete(window, Send(window, "Alignment", "Deactivate"), "FullyDetermined");
            window.GetModule("Alignment")->Observe();
            auto* inactiveOverlay = window.GetModule("Alignment")->GetParameterEditor("Visibility")->GetField("isVisible");
            Check(inactiveOverlay->GetValue().isNull() && !inactiveOverlay->findChild<QCheckBox*>("value")->isEnabled(),
                "deactivated alignment never presents a retained display preference as a visible overlay");
            const auto archivePath = directory.filePath("alignment-archive.json");
            GetComplete(window, Send(window, "Alignment", "ExportArchive", {{"outputPath", archivePath}}), "ArchiveExported");
            GetComplete(window, Send(window, "Alignment", "Restore", {{"archivePath", archivePath}}), "FullyDetermined");
            GetComplete(window, SendUnavailable(window, "Alignment", "Result"), "Rejected");
            GetComplete(window, Send(window, "Alignment", "Start"), "FullyDetermined");
        }
#endif
        // 材料表面单独验证，不依赖可选对齐模块替它生成正式结果。
        GetComplete(window, Send(window, "Surface", "GlobalAutomatic", {{"initialIsoValue", 50.0},
            {"materialRange", QJsonArray{0.,100.}}}), "Succeeded");
#if defined(MANUAL_ALIGNMENT)
        if(GetEnabled(window,"Alignment") && window.GetModule("Alignment")->GetObservedState()["hasReference"].toBool()) {
            GetComplete(window,Click(window,"Surface","OpenAlignment"),"ParametersCopied");auto* alignmentPage=window.GetModule("Alignment");alignmentPage->Observe();
            Check(!alignmentPage->GetObservedState()["isReferenceCurrent"].toBool() && !alignmentPage->GetObservedState()["hasRecipe"].toBool() && !alignmentPage->GetObservedState()["hasResult"].toBool(),"new formal mesh from the same image cannot inherit the old alignment recipe and result");
        }
#endif
        Check((window.GetWorkflow().GetMeasurementSurface() && GetDataRevisionRefValid(window.GetWorkflow().GetMeasurementSurface()->mesh)),
            "material ISO publishes an independent formal measurement surface");
    }
    if (GetEnabled(window, "Rotation")) {
        auto* rotation = window.GetModule("Rotation");
        for (const bool enabled : {true, false}) {
            SetBoolean(rotation->GetParameterEditor("SetEnabled")->GetField("isEnabled"), enabled);
            GetComplete(window, Click(window, "Rotation", "SetEnabled"), "Succeeded");
            Check(Wait([&] { rotation->Observe(); return rotation->GetObservedState()["isEnabled"] == QJsonValue(enabled); }), "rotation enabled checkbox follows the real feature through true and false");
        }
        GetComplete(window, Click(window, "Rotation", "Rotate", {{"angleDeg", 5.0}}), "Succeeded");
        GetComplete(window, Send(window, "Rotation", "Undo"), "Succeeded");
    }
    for (const auto* name : {"Crop", "Gap", "Part", "Surface", "Artifact", "Alignment"}) {
        if (!GetEnabled(window, name)) continue;
        const auto* panel = window.GetModule(name);
        const auto nodes = GetCatalogNodes(name, panel->GetObservedState(), GetDescriptor(window.GetSession()->GetImageDescriptor()), panel->GetActions(), {}, window.GetWorkflow().getPublishedGraph());
        bool found = false;
        for (const auto child : nodes.first().toObject()["children"].toArray()) if (child.toObject()["id"] == "published-graph")
            for (const auto row : child.toObject()["children"].toArray()) found = found || row.toObject()["title"].toString().startsWith(GetModuleText(name));
        if(QString(name)=="Crop")
            Check(!found&&panel->GetObservedState()["documents"].toArray().isEmpty(),"closed crop documents release producer records from the live graph");
        else Check(found, "each feature graph includes its real producer records with Chinese business names");
    }
    window.SetViewsVisible(true);
    Check(Wait([&] {
        const auto scene = window.GetSession()->GetSceneViewState({"primary-3d"});
        return scene && !window.GetWorkflow().getRenderPending("primary-3d");
    }), "restored visible view renders latest scene");
    CheckRenderModeSwitch(window);
    CheckWallWorkflow(window, directory.path());
    CheckFeatureSwitchDuringSegmentation(window, directory.path());
    Check(Wait([&] {
        const auto* part = window.GetModule("Part");
        if (part && part->GetObservedState()["isDisplayPreparing"].toBool()) return false;
        for (const auto& view : window.GetSession()->GetSceneViewStates())
            if (window.GetWorkflow().getRenderPending(view.id)) return false;
        return true;
    }), "optional part display work and its real frames finish before the idle observation window");
    CheckIdle(window);
    // 关闭时保留页面和接收对象，Session 完成/取消尚未交付的业务回调。
    const auto pendingLoad = Send(window, "Data", "Load", load);
    Check(Wait([&] { return window.StopSession(); }), "owner Stop completes with operation in flight");
    Check(window.GetRecords().GetRecord(pendingLoad)["completeCount"].toInt() == 1, "Stop delivers late completion once while receiver lives");
}
QJsonValue GetResolved(const QJsonValue& value, const QJsonObject& results)
{
    if (value.isString() && value.toString().startsWith("$")) {
        const auto path = value.toString().mid(1).split('.');
        QJsonValue current = results;
        for (const auto& key : path) current = current.isArray() ? current.toArray().at(key.toInt()) : current.toObject().value(key);
        if (current.isUndefined()) throw std::invalid_argument("用例结果引用不存在");
        return current;
    }
    if (value.isObject()) { QJsonObject result; const auto object = value.toObject(); for (auto it = object.begin(); it != object.end(); ++it) result[it.key()] = GetResolved(it.value(), results); return result; }
    if (value.isArray()) { QJsonArray result; for (const auto entry : value.toArray()) result.append(GetResolved(entry, results)); return result; }
    return value;
}
#if defined(MANUAL_ALIGNMENT)
QJsonObject BuildKnownTransformReference(const QJsonObject& parameters)
{
    // 真实网格上的已知变换回归；不冒充外部名义 CAD 或计量认证。
    const auto transform = GetArray<double, 16>(parameters["transform"]);
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        double dot = 0; for (int axis = 0; axis < 3; ++axis) dot += transform[row*4+axis]*transform[column*4+axis];
        Check(std::abs(dot-(row == column ? 1. : 0.)) < 1e-8, "known reference transform has an orthonormal rotation");
    }
    Check(std::abs(transform[12])+std::abs(transform[13])+std::abs(transform[14])+std::abs(transform[15]-1.) < 1e-8, "known reference transform is affine");
    const double determinant = transform[0]*(transform[5]*transform[10]-transform[6]*transform[9])-transform[1]*(transform[4]*transform[10]-transform[6]*transform[8])+transform[2]*(transform[4]*transform[9]-transform[5]*transform[8]);
    Check(std::abs(determinant-1.) < 1e-8, "known reference transform preserves handedness");
    QJsonArray points;
    for (const auto value : parameters["samples"].toArray()) {
        const auto sample = value.toObject();
        if (sample["flags"].toInt() == 0 && sample["validSupportRatio"].toDouble() > 0) points.append(sample);
    }
    Check(points.size() >= 3, "real measured mesh provides valid reference samples");
    const auto origin = GetArray<double, 3>(points[0].toObject()["sourcePoint"]);
    const auto delta = [&](int i) { const auto point = GetArray<double, 3>(points[i].toObject()["sourcePoint"]); return std::array<double, 3>{point[0]-origin[0], point[1]-origin[1], point[2]-origin[2]}; };
    const auto squared = [](const auto& point) { return point[0]*point[0]+point[1]*point[1]+point[2]*point[2]; };
    int second = 1, third = 2;
    for (int i = 1; i < points.size(); ++i) if (squared(delta(i)) > squared(delta(second))) second = i;
    const auto axis = delta(second);
    const auto area = [&](int i) { const auto point = delta(i); return squared(std::array<double, 3>{axis[1]*point[2]-axis[2]*point[1], axis[2]*point[0]-axis[0]*point[2], axis[0]*point[1]-axis[1]*point[0]}); };
    for (int i = 1; i < points.size(); ++i) if (area(i) > area(third)) third = i;
    Check(area(third) > 1e-8, "real reference samples are not collinear");
    auto reference = GetReferenceTemplate(AlignmentMethod::Rps);
    reference["evidenceKind"] = "real-ct-known-transform-regression";
    reference["provenance"] = GetText(parameters, "provenance") + "; known rigid transform generated from measured CT vertices; not nominal CAD metrology acceptance";
    reference["expectedTransform"] = GetValues(transform);
    auto recipe = reference["recipe"].toObject(); auto geometries = recipe["geometries"].toArray(), constraints = recipe["constraints"].toArray();
    for (const auto& selection : {std::make_pair(0, 0), std::make_pair(1, second), std::make_pair(2, third)}) {
        const auto sample = points[selection.second].toObject(); auto geometry = geometries[selection.first].toObject();
        auto region = geometry["region"].toObject(); region["vertexIds"] = QJsonArray{sample["vertexId"]}; geometry["region"] = region; geometries[selection.first] = geometry;
        const auto source = GetArray<double, 3>(sample["sourcePoint"]); QJsonArray target;
        for (int row = 0; row < 3; ++row) target.append(transform[row*4]*source[0]+transform[row*4+1]*source[1]+transform[row*4+2]*source[2]+transform[row*4+3]);
        for (int component = 0; component < 3; ++component) { auto constraint = constraints[selection.first*3+component].toObject(); constraint["nominalPoint"] = target; constraints[selection.first*3+component] = constraint; }
    }
    recipe["geometries"] = geometries; recipe["constraints"] = constraints; reference["recipe"] = recipe;
    return reference;
}
#endif
int SaveRendererAudit(TestWindow& window, const QString& path)
{
    const auto* endpoint = window.GetSession()->GetPrimaryEndpoint();
    Check(endpoint && endpoint->renderer, "primary renderer is available for display audit");
    auto* renderer = endpoint->renderer; QJsonArray actors;
    auto* collection = renderer->GetActors(); collection->InitTraversal();
    while (auto* actor = collection->GetNextActor()) {
        QJsonArray bounds, matrix, color;
        for (int i = 0; i < 6; ++i) bounds.append(actor->GetBounds()[i]);
        for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) matrix.append(actor->GetMatrix()->GetElement(row, col));
        for (int i = 0; i < 3; ++i) color.append(actor->GetProperty()->GetColor()[i]);
        auto* data = actor->GetMapper() ? actor->GetMapper()->GetInput() : nullptr;
        actors.append(QJsonObject{{"type", actor->GetClassName()}, {"visible", actor->GetVisibility() != 0}, {"opacity", actor->GetProperty()->GetOpacity()},
            {"bounds", bounds}, {"matrix", matrix}, {"color", color}, {"points", data ? QString::number(data->GetNumberOfPoints()) : "0"}, {"cells", data ? QString::number(data->GetNumberOfCells()) : "0"}});
    }
    auto* camera = renderer->GetActiveCamera(); QJsonArray position, focus, clipping;
    for (int i = 0; i < 3; ++i) { position.append(camera->GetPosition()[i]); focus.append(camera->GetFocalPoint()[i]); }
    for (int i = 0; i < 2; ++i) clipping.append(camera->GetClippingRange()[i]);
    int orangePixels = 0;
    for (auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>()) if (widget->renderWindow() == endpoint->renderWindow) {
        const auto pixels = widget->grab().toImage();
        for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
            const auto color = pixels.pixelColor(x, y);
            if (color.red() > 100 && color.green() > 40 && color.green() < color.red()*.8 && color.blue() < color.green()*.6) ++orangePixels;
        }
    }
    ExportJson(path, {{"actors", actors}, {"cameraPosition", position}, {"cameraFocus", focus}, {"clipping", clipping},
        {"depthPeeling", renderer->GetUseDepthPeeling() != 0}, {"usedDepthPeeling", renderer->GetLastRenderingUsedDepthPeeling() != 0},
        {"multiSamples", endpoint->renderWindow->GetMultiSamples()}, {"alphaBitPlanes", endpoint->renderWindow->GetAlphaBitPlanes()},
        {"orangePixels", orangePixels}, {"hostDiagnostics", window.GetDiagnostics()}});
    return orangePixels;
}
QJsonObject CheckPartDisplay(TestWindow& window)
{
    auto* panel = window.GetModule("Part");
    Check(Wait([&] { panel->Observe();
        const auto error = panel->GetObservedState()["sourcePreviewError"].toString();
        if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        return panel->GetObservedState()["sourcePreviewReady"].toBool(); }),
        "source display switches through owner events before checking part preview");
    const auto catalog = panel->GetObservedState();
    const auto parts = catalog["parts"].toArray();
    const bool enabled = catalog["hasCurrentParts"].toBool() && catalog["isOverlayVisible"].toBool();
    // Visibility 的 UI 接收先于 owner scene delta，等待门铃消费后的正式场景状态。
    Check(Wait([&] {
        for (const auto& id : GetPartViews().viewIds) {
            const auto scene = window.GetSession()->GetSceneViewState({id});
            if (!scene) return false;
            const auto count = std::count_if(scene->displays.begin(), scene->displays.end(),
                [](const auto& display) { return display.featureId == "part-segmentation"; });
            if (count != (enabled ? 1 : 0)) return false;
        }
        return true;
    }), "owner scene delta settles part preview visibility in the current 3D view");
    vtkDataSet* sharedSurface = nullptr;
    QJsonArray views;
    for (const auto& id : GetPartViews().viewIds) {
        const auto* endpoint = window.GetSession()->GetRenderViewEndpoint(id);
        Check(endpoint && endpoint->renderer, "part display endpoint exists");
        const auto scene = window.GetSession()->GetSceneViewState({id});
        Check(scene.has_value(), "part display scene exists");
        int displayCount = 0;
        for (const auto& display : scene->displays) if (display.featureId == "part-segmentation") {
            ++displayCount;
            Check(GetRefText(display.data) == catalog["labelMap"].toString(), "view displays the current formal label revision");
        }
        Check(displayCount == (enabled ? 1 : 0), "segmentation display registration is synchronized in the current 3D view");
        const auto presentation = window.GetSession()->GetRenderViewState({id});
        Check(presentation.has_value(), "part preview has an applied source presentation");
        if (!enabled) { views.append(QJsonObject{{"view", QString::fromStdString(id)}, {"displayCount", displayCount},
            {"sourceOpacity", presentation->material.opacity}}); continue; }
        const bool isSlice = presentation->viewMode == HostRenderMode::SliceTopDown || presentation->viewMode == HostRenderMode::SliceFrontBack || presentation->viewMode == HostRenderMode::SliceLeftRight;
        const bool isVolume = presentation->viewMode == HostRenderMode::Volume || presentation->viewMode == HostRenderMode::CompositeVolume;
        vtkLookupTable* table = nullptr; vtkDataSet* input = nullptr;
        int matches = 0, volumes = 0;
        auto* props = endpoint->renderer->GetViewProps(); props->InitTraversal();
        while (auto* prop = props->GetNextProp()) {
            if (vtkVolume::SafeDownCast(prop)) ++volumes;
            vtkLookupTable* candidate = nullptr; vtkDataSet* data = nullptr;
            if (auto* slice = vtkImageSlice::SafeDownCast(prop)) {
                candidate = vtkLookupTable::SafeDownCast(slice->GetProperty()->GetLookupTable());
                data = slice->GetMapper()->GetInput();
            }
            if (auto* actor = vtkActor::SafeDownCast(prop)) {
                auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
                if (mapper && mapper->GetScalarVisibility()) {
                    candidate = vtkLookupTable::SafeDownCast(mapper->GetLookupTable()); data = mapper->GetInput();
                }
            }
            if (candidate && candidate->GetNumberOfTableValues() == parts.size() + 1) {
                ++matches; table = candidate; input = data;
            }
        }
        Check(matches == 1 && table && input, "view has exactly one current part preview with all labels");
        if (!isSlice) {
            const auto state = window.GetSession()->GetRenderViewState({id});
            Check(state && (isVolume ? state->material.opacity > 0 : state->material.opacity == 0),
                "only primary shell is hidden; native DVR opacity stays positive");
            Check(!sharedSurface || sharedSurface == input, "primary and volume previews share the exact same surface product");
            sharedSurface = input;
        }
        if (isVolume) Check(volumes > 0, "volume background remains a real volume renderer");
        QJsonArray colors;
        for (const auto& value : parts) {
            const auto part = value.toObject(); const auto label = GetId(part["labelId"]);
            Check(label > 0 && label < static_cast<std::uint64_t>(table->GetNumberOfTableValues()), "part label indexes the view lookup table");
            double actual[4]{}; table->GetTableValue(static_cast<vtkIdType>(label), actual);
            auto expected = GetArray<double, 4>(part["colorRGBA"]);
            expected[3] *= part["visible"].toBool() ? part["opacity"].toDouble() : 0;
            if (isSlice) expected[3] *= part["selected"].toBool() ? 0.8 : 0.18;
            if (scene->role == HostRenderViewRole::Composite3D) expected[3] *= part["selected"].toBool() ? 0.35 : 0;
            for (int c = 0; c < 4; ++c) if (std::abs(expected[c] - actual[c]) > 1.0/255.0)
                throw std::runtime_error("零件显示色、显隐或选中状态与目录不一致：" + id);
            colors.append(QJsonObject{{"label", QString::number(label)}, {"rgba", QJsonArray{actual[0],actual[1],actual[2],actual[3]}}});
        }
        views.append(QJsonObject{{"view", QString::fromStdString(id)}, {"displayCount", displayCount},
            {"sourceOpacity", presentation->material.opacity}, {"volumeCount", volumes}, {"points", QString::number(input->GetNumberOfPoints())}, {"colors", colors}});
    }
    Check(true, "part colours, visibility and selection match the catalog in the current 3D mode");
    return {{"catalog", catalog}, {"views", views}};
}
void StartSequence(TestWindow& window, const QString& path)
{
    const auto script = LoadJson(path);
    if(script.contains("windowSize")) {
        const auto size=script["windowSize"].toArray();
        Check(size.size()==2 && size[0].toInt()>=1000 && size[1].toInt()>=800,
            "scenario declares a usable business screenshot window");
        window.resize(size[0].toInt(),size[1].toInt());QCoreApplication::processEvents();
    }
    const auto initialWindowSize=window.size();
    if(script["viewLayout"].isString()) {
        auto* choice=window.findChild<QComboBox*>("viewLayout");const auto index=choice->findData(script["viewLayout"].toString());
        if(index<0)throw std::invalid_argument("未知内部视图布局");choice->setCurrentIndex(index);QCoreApplication::processEvents();
    }
    QJsonObject results;
    QMap<QString, QImage> volumeFrames;
    QTimer previewWatch;
    int preparingSamples = 0;
    bool hasHiddenSource = false, hasPreparationCapture = false;
    if (script["checkPartPreparation"].toBool()) {
        QObject::connect(&previewWatch, &QTimer::timeout, &window, [&] {
            const auto* part = window.GetModule("Part");
            const auto state = part ? part->GetObservedState() : QJsonObject{};
            const auto session = window.GetSession();
            if (!session) return;
            const auto view = session->GetRenderViewState({"primary-3d"});
            if (!view || !state["isDisplayPreparing"].toBool()
                || window.GetWorkflow().getRenderPending("primary-3d")) return;
            ++preparingSamples;
            const bool iso = view->viewMode == HostRenderMode::IsoSurface || view->viewMode == HostRenderMode::CompositeIsoSurface;
            hasHiddenSource = hasHiddenSource || (iso && view->material.opacity <= 0);
            if (!hasPreparationCapture && script.contains("partPreparationScreenshot")) {
                hasPreparationCapture = window.grab().save(GetText(script, "partPreparationScreenshot"));
            }
        });
        previewWatch.start(100);
    }
    if (!script["steps"].isArray() || script["steps"].toArray().isEmpty()) throw std::invalid_argument("用例必须包含非空 steps");
    // 执行加载/计算前校验全部等待参数；范围来自 Qt 的 int 毫秒计时接口，不设业务时限。
    const auto getWaitMilliseconds=[](const QJsonObject& step){
        const auto timeout=step.contains("timeoutMs")?GetNumber(step,"timeoutMs"):30000.;
        if(timeout<1 || timeout>std::numeric_limits<int>::max() || timeout!=std::trunc(timeout))
            throw std::invalid_argument("用例 timeoutMs 必须为 1..2147483647 毫秒整数");
        return static_cast<int>(timeout);
    };
    for(const auto value:script["steps"].toArray())
        if(!value.toObject()["generateKnownTransformReference"].toBool() && value.toObject()["control"]!="checkPage" && value.toObject()["control"]!="editParameters") {
            (void)getWaitMilliseconds(value.toObject());
            (void)GetExpectedStatuses(value.toObject());
        }
    if (script["viewsVisible"].isBool()) window.SetViewsVisible(script["viewsVisible"].toBool());
    const auto checkObserved=[&](const QJsonObject& step) {
        auto* panel=window.GetModule(GetText(step,"module"));
        const auto expected=GetResolved(step["expectState"],results).toObject();QJsonObject actual;
        const auto observedValue=[&](const QString& key){
            QJsonValue value=actual;for(const auto& part:key.split('.'))value=value.toObject().value(part);return value;
        };
        // 请求终态可以早于 Host 提交场景；等待页面观察到实际提交值，保持原断言。
        const bool settled=Wait([&] {
            panel->Observe();actual=panel->GetObservedState();
            for(auto it=expected.begin();it!=expected.end();++it)if(observedValue(it.key())!=it.value())return false;
            return true;
        },std::min(getWaitMilliseconds(step),30000));
        if(!settled)std::cerr<<"STATE MISMATCH: expected="<<GetJsonText(expected).toStdString()<<" actual="<<GetJsonText(actual).toStdString()<<std::endl;
        for(auto it=expected.begin();it!=expected.end();++it)Check(observedValue(it.key())==it.value(),("observed business state matches "+it.key()).toStdString().c_str());
        if(step.contains("stateAudit"))ExportJson(GetText(step,"stateAudit"),{{"module",panel->GetName()},{"parameterAction",panel->GetParameterAction()},{"primary",panel->findChild<QPushButton*>("businessPrimary")->property("businessAction").toString()},{"state",actual}});
    };
    for (const auto value : script["steps"].toArray()) {
        const auto step = value.toObject();
        const auto params = GetResolved(step["parameters"], results).toObject();
        if(step["control"]=="checkPage") {
            const auto module=GetText(step,"module");auto* tabs=window.findChild<QTabBar*>("featureTabs");int index=-1;
            const int group=QStringList{"Data","View","Rotation"}.contains(module)?0:QStringList{"Roi","Crop","Artifact","Part","Surface"}.contains(module)?1:2;
            auto* workspaces=window.findChild<QTabBar*>("workspaceTabs");Check(workspaces->isTabEnabled(group),"planned business group is compiled");
            const auto groupPoint=workspaces->tabRect(group).center();
            QMouseEvent groupPress(QEvent::MouseButtonPress,groupPoint,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(workspaces,&groupPress);
            QMouseEvent groupRelease(QEvent::MouseButtonRelease,groupPoint,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(workspaces,&groupRelease);QCoreApplication::processEvents();
            for(int i=0;i<tabs->count();++i)if(tabs->tabData(i)==module)index=i;
            Check(index>=0,"planned business tab is present");
            const auto point=tabs->tabRect(index).center();
            QMouseEvent press(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(tabs,&press);QMouseEvent release(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(tabs,&release);
            auto* page=window.GetModule(module);QCoreApplication::processEvents();page->Observe();
            Check(page->isVisible() && page->findChild<QPushButton*>("businessPrimary")->isVisible(),"Qt tab mouse event reveals the planned stage and primary");
            Check(window.size()==initialWindowSize,"tab clicks preserve the current outer window size");
            checkObserved(step);if(step.contains("expectPrimaryAction"))Check(page->findChild<QPushButton*>("businessPrimary")->property("businessAction")==step["expectPrimaryAction"].toString(),"current page presents the expected phase without selecting a form");
            if(step.contains("screenshot"))Check(window.grab().save(GetText(step,"screenshot")),"business page screenshot saved");
            continue;
        }
        if(step["control"]=="editParameters") {
            auto* panel=window.GetModule(GetText(step,"module"));Check(panel && panel->isVisible(),"editing targets the already visible business page");
            const auto action=panel->GetParameterAction();if(step.contains("action"))Check(action==GetText(step,"action"),"editing preserves the currently selected form");
            panel->SetParameterPatch(action,params);QCoreApplication::processEvents();panel->Observe();
            if(step.contains("expectPrimaryAction"))Check(panel->findChild<QPushButton*>("businessPrimary")->property("businessAction")==step["expectPrimaryAction"].toString(),"edited current form changes its natural next action");
            if(step.contains("screenshot"))Check(window.grab().save(GetText(step,"screenshot")),"edited business page screenshot saved");
            checkObserved(step);
            continue;
        }
        if (step["generateKnownTransformReference"].toBool()) {
#if defined(MANUAL_ALIGNMENT)
            ExportJson(GetText(params, "outputPath"), BuildKnownTransformReference(params));
            std::cout << "REFERENCE EVIDENCE: real CT measured vertices with known rigid transform; not external nominal CAD acceptance" << std::endl;
            continue;
#else
            throw std::invalid_argument("当前构建未启用计量对齐");
#endif
        }
        const auto expected = GetExpectedStatuses(step);
        const auto timeout=getWaitMilliseconds(step);
        if (step["viewsVisible"].isBool()) window.SetViewsVisible(step["viewsVisible"].toBool());
        if (step["waitAvailable"].toBool()) {
            auto* panel = window.GetModule(GetText(step, "module"));
            const auto action = GetText(step, "action");
            Check(panel && Wait([&] {
                panel->Observe();
                auto* button = panel->findChild<QPushButton*>("action_" + action);
                return button && button->isEnabled();
            }, static_cast<int>(timeout)), "public business action becomes available after its actual frame guards");
        }
        if (step["waitForCropFrames"].toBool()) {
            auto* crop = window.GetModule("Crop");
            Check(crop && Wait([&] { crop->Observe(); return crop->GetObservedState()["framesReady"].toBool(); },
                static_cast<int>(timeout)), "crop business frames are ready before the next operation");
        }
        const auto checkpointPath = step["performanceCheckpoint"].toString();
        const auto timingsBefore = checkpointPath.isEmpty() ? QJsonObject{} : window.GetRecords().GetTimings();
        const auto measurementsBefore = checkpointPath.isEmpty() ? QJsonObject{} : window.GetRecords().GetMeasurements();
        const auto requestedUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QElapsedTimer checkpointClock; checkpointClock.start();
        if (!checkpointPath.isEmpty()) std::cout << "phase=" << GetText(step,"module").toStdString() << "."
            << GetText(step,"action").toStdString() << " event=start utc=" << requestedUtc.toStdString() << std::endl;
        const auto control=step["control"].toString();
        std::uint64_t id=0;
        if(control=="businessPrimary" || control=="nextButton" || control=="currentPrimary") {
            const auto module=GetText(step,"module"),action=GetText(step,"action");auto* panel=window.GetModule(module);
            if(!panel)throw std::invalid_argument("脚本模块不存在");
            const auto parameterAction=step["parameterAction"].isString()?GetText(step,"parameterAction"):action;
            if(control=="currentPrimary") {Check(panel->isVisible(),"primary click targets the already visible page");Check(params.isEmpty(),"currentPrimary never injects parameters or navigation");}
            else window.GetWorkflow().onNavigate(module,parameterAction,params);
            QCoreApplication::processEvents();panel->Observe();
            QPointer<QPushButton> button;
            const bool available=Wait([&]{panel->Observe();button=panel->findChild<QPushButton*>(control=="nextButton" ? "next_"+action : "businessPrimary");return button && button->isVisible() && button->isEnabled();},std::min(timeout,30000));
            if(!available)std::cout << "CONTROL BLOCKED: " << module.toStdString() << "." << action.toStdString()
                << " tooltip=" << (button ? button->toolTip().toStdString() : "missing control") << " state="
                << QJsonDocument(panel->GetObservedState()).toJson(QJsonDocument::Compact).toStdString() << std::endl;
            Check(available,"actual stage primary is visible and enabled");
            if(step["expectPrimaryAction"].isString())Check(button->property("businessAction").toString()==step["expectPrimaryAction"].toString(),"stage primary matches the planned business phase");
            id=static_cast<std::uint64_t>(window.GetRecords().GetRecords()["records"].toArray().size())+1;
            std::cout<<"PRIMARY CLICK: "<<module.toStdString()<<"."<<action.toStdString()<<" record="<<id<<std::endl;
            button->click();
            std::cout<<"PRIMARY RETURN: "<<module.toStdString()<<"."<<action.toStdString()<<std::endl;
            const bool dispatched=Wait([&]{return !window.GetRecords().GetRecord(id).isEmpty();},2000);
            if(!dispatched)std::cerr<<"PRIMARY DISPATCH MISSING: "<<module.toStdString()<<"."<<action.toStdString()
                <<" primary="<<button->property("businessAction").toString().toStdString()<<" enabled="<<button->isEnabled()
                <<" message="<<window.findChild<QLabel*>("latestMessage")->text().toStdString()
                <<" state="<<GetJsonText(panel->GetObservedState()).toStdString()<<std::endl;
            Check(dispatched,"primary click creates the actual operation record");
        } else if(control=="catalogAction") {
            const auto module=GetText(step,"module"),action=GetText(step,"action");
            auto* panel=window.GetModule(module);
            window.GetWorkflow().onNavigate(module,action,{});QCoreApplication::processEvents();panel->Observe();
            auto* tree=panel->GetCatalogTree();QTreeWidgetItem* target=nullptr;
            Check(Wait([&] {
                panel->Observe();
                for(QTreeWidgetItemIterator it(tree);*it;++it) {
                    const auto patch=(*it)->data(0,Qt::UserRole).toJsonObject()["patches"].toObject()[action].toObject();
                    if(patch.isEmpty())continue;
                    bool matches=true;for(auto p=params.begin();p!=params.end();++p)matches=matches && patch[p.key()]==p.value();
                    if(matches){target=*it;return true;}
                }
                return false;
            }),"catalog exposes the exact requested history identity");
            id=ClickNodeAction(window,tree,target,action);
        } else if(control=="unavailableAction") {
            const auto module=GetText(step,"module"),action=GetText(step,"action");
            window.GetWorkflow().onNavigate(module,action,params);
            id=SendUnavailable(window,module,action);
        } else if(control=="actionButton")id=Click(window,GetText(step,"module"),GetText(step,"action"),params);
        else if(control.isEmpty())id=Send(window,GetText(step,"module"),GetText(step,"action"),params);
        else throw std::invalid_argument("control 仅支持 currentPrimary、editParameters、businessPrimary、nextButton、actionButton、catalogAction、unavailableAction、checkPage");
        QTimer switching, cancellation;
        int switchCount = 0;
        if (step["switchFeaturesWhilePending"].toBool()) {
            auto* tabs = window.findChild<QTabBar*>("featureTabs");
            QObject::connect(&switching, &QTimer::timeout, &window, [&, tabs] {
                if (window.GetRecords().GetRecord(id)["isTerminal"].toBool()) { switching.stop(); return; }
                auto* workspaces = window.findChild<QTabBar*>("workspaceTabs");
                if (tabs->currentIndex()+1 < tabs->count()) tabs->setCurrentIndex(tabs->currentIndex()+1);
                else { workspaces->setCurrentIndex((workspaces->currentIndex()+1) % workspaces->count()); tabs->setCurrentIndex(0); }
                ++switchCount;
            });
            switching.start(20);
        }
        if (step.contains("cancelAfterMs")) {
            const auto delay = GetNumber(step, "cancelAfterMs");
            auto* module = window.GetModule(GetText(step, "module"));
            if (delay < 1 || delay >= timeout || std::trunc(delay) != delay || !module->onStop)
                throw std::invalid_argument("取消审计需要有效的取消入口和小于等待时限的延迟");
            QString exactTarget;
            if(step["cancelExactRequestId"].toBool()) {
                Check(module->GetName()=="Surface","exact cancellation targets the supported surface request family");
                exactTarget=window.GetRecords().GetRecord(id)["admission"].toObject()["requestId"].toString();
                Check(GetId(QJsonValue(exactTarget))!=0,"exact cancellation freezes the admitted request ID");
            }
            cancellation.setSingleShot(true);
            QObject::connect(&cancellation, &QTimer::timeout, &window, [&, module,exactTarget] {
                if (!window.GetRecords().GetRecord(id)["isTerminal"].toBool()) {
                    if(exactTarget.isEmpty())module->onStop();
                    else module->SendAction("Stop",{{"targetRequestId",exactTarget}});
                }
            });
            cancellation.start(static_cast<int>(delay));
        }
        const auto record = GetComplete(window, id, {}, static_cast<int>(timeout));
        // 合法终态集合只描述可观测结局；实际质量状态仍完整保留在 record 中。
        if (!expected.contains(record["status"].toString()) && step.contains("businessCapture"))
            SaveBusinessCapture(window,GetText(step,"businessCapture"),step,id);
        Check(expected.contains(record["status"].toString()), ("expected terminal status " + expected.join(" / ")).toStdString().c_str());
        if (step.contains("expectMessageContains"))
            Check(record["result"].toObject()["message"].toString().contains(GetText(step,"expectMessageContains")),
                "negative request reports its declared input error");
        const auto expectedResult=GetResolved(step["expectResult"],results).toObject();
        for(auto field=expectedResult.begin();field!=expectedResult.end();++field)
            Check(record["result"].toObject()[field.key()]==field.value(),("business result matches "+field.key()).toStdString().c_str());
        const auto metrics=GetResolved(step["expectMetrics"],results).toObject();
        for(auto field=metrics.begin();field!=metrics.end();++field) {
            QJsonValue actual=record["result"];
            for(const auto& key:field.key().split('.'))actual=actual.toObject().value(key);
            bool valid=actual.isDouble();const auto number=actual.isString()?actual.toString().toDouble(&valid):actual.toDouble();
            Check(valid && std::isfinite(number),("business metric is finite: "+field.key()).toStdString().c_str());
            const auto bounds=field.value().toObject();
            if(bounds.contains("min"))Check(number>=GetNumber(bounds,"min"),("business metric meets minimum: "+field.key()).toStdString().c_str());
            if(bounds.contains("max"))Check(number<=GetNumber(bounds,"max"),("business metric meets maximum: "+field.key()).toStdString().c_str());
        }
        if(step.contains("expectCurrentInput")) {
            const auto current=window.GetSession()->GetImageDescriptor();
            Check(current && GetRefText(current->dataRevision)==GetResolved(step["expectCurrentInput"],results).toString(),"business step retains the expected exact input revision");
        }
        checkObserved(step);
        if(step.contains("expectFollowupStatus")) {
            const bool followed=Wait([&]{return !window.GetRecords().GetRecord(id+1).isEmpty();},2000);
            if(!followed)std::cerr<<"FOLLOWUP MISSING: "<<window.findChild<QLabel*>("latestMessage")->text().toStdString()
                <<" state="<<GetJsonText(window.GetModule(GetText(step,"module"))->GetObservedState()).toStdString()<<std::endl;
            Check(followed,"combined primary dispatches its real follow-up transaction");
            const auto followup=GetComplete(window,id+1,GetText(step,"expectFollowupStatus"),timeout);
            Check(followup["action"]=="UseData","combined primary uses the real input adoption request");
            if(step["expectAdoptPublished"].toBool()) {
                const auto result=record["result"].toObject();const auto output=result["output"].isString()?result["output"].toString():result["correctedVolume"].toString();
                Check(!output.isEmpty() && GetRefText(window.GetSession()->GetImageDescriptor()->dataRevision)==output,"script adopts the precise result published by this primary");
            }
        }
        if(script["checkWindowStable"].toBool() || script["checkWindowLocked"].toBool())Check(window.size()==initialWindowSize,"business operations preserve the user-selected outer window size");
        if(step["showStatistics"].toBool()) {
            auto* panel=window.GetModule(GetText(step,"module"));window.GetWorkflow().onShowStatistics(panel->GetName());panel->RefreshStatistics();
            if(step["waitHistogram"].toBool() || step["waitStatistics"].toBool()) {
                Check(Wait([&]{
                    const auto data=panel->GetStatisticsWidget()->GetHistogram();
                    const bool directReady=step["waitHistogram"].toBool()
                        && !data["cacheKey"].isString() && !data["counts"].toArray().isEmpty()
                        && (data["status"].isUndefined() || data["status"]=="Ready");
                    return directReady || (data["cacheKey"].isString()
                        && QStringList{"Ready","Empty","Failed"}.contains(data["status"].toString()));
                },timeout),"real statistics reaches a terminal state within the bounded read");
                const auto data=panel->GetStatisticsWidget()->GetHistogram();
                const bool directReady=step["waitHistogram"].toBool()
                    && !data["cacheKey"].isString() && !data["counts"].toArray().isEmpty()
                    && (data["status"].isUndefined() || data["status"]=="Ready");
                Check(directReady || data["status"]=="Ready" || data["status"]=="Empty",
                    "real statistics succeeds including a valid empty result");
                if(step["waitHistogram"].toBool() && data["status"]=="Ready")
                    Check(!data["counts"].toArray().isEmpty(),"ready histogram contains its declared bins");
            }
            if(step.contains("statisticsAudit"))ExportJson(GetText(step,"statisticsAudit"),panel->GetStatisticsWidget()->GetHistogram());
            if(step.contains("statisticsScreenshot"))Check(panel->GetStatisticsWidget()->grab().save(GetText(step,"statisticsScreenshot")),"actual embedded statistics controls are captured");
            // 统计在主窗口内部，不再有遮挡裁切视窗的浮动统计窗口。
            QCoreApplication::processEvents();
        }
        switching.stop(); cancellation.stop();
        if (GetText(step, "module") == "View" && GetText(step, "action") == "Set"
            && params["mode"].isString() && record["status"] == "Succeeded") {
            Check(record["parameters"].toObject()["mode"] == params["mode"], "explicit view mode survives editor context refresh");
            const auto expectedMode = GetEnum<HostRenderMode>(params, "mode", {
                {"Volume", HostRenderMode::Volume}, {"CompositeVolume", HostRenderMode::CompositeVolume},
                {"IsoSurface", HostRenderMode::IsoSurface}, {"CompositeIsoSurface", HostRenderMode::CompositeIsoSurface},
                {"SliceTopDown", HostRenderMode::SliceTopDown}, {"SliceFrontBack", HostRenderMode::SliceFrontBack},
                {"SliceLeftRight", HostRenderMode::SliceLeftRight}});
            const auto view = window.GetSession()->GetRenderViewState({(params.contains("viewId") ? GetText(params, "viewId") : QString("primary-3d")).toStdString()});
            Check(view && view->viewMode == expectedMode, "requested view mode is actually applied");
        }
        if (script["checkPartPreparation"].toBool()) Check(!hasHiddenSource, "source stays visible while part surface is preparing");
        if (step["checkDefaultImageRead"].toBool()) {
            const auto read = window.GetSession()->GetImageReadResult();
            Check(read.error == ImageReadError::None && read.state && read.state->values
                && read.state->dims == std::array<int,3>{1000,1000,1000}
                && read.requiredBytes == 4000000000ULL && read.state->values->size() == 4000000000ULL,
                "default image read copies the complete native 1000-cube");
        }
        if (step["checkDefaultLabelRead"].toBool()) {
            const auto descriptors = window.GetSession()->GetLabelMapDescriptors();
            Check(descriptors.size() == 1 && descriptors.front().dims == std::array<int,3>{1000,1000,1000},
                "the full-resolution segmentation label map is available");
            LabelMapReadRequest request; request.id = descriptors.front().id;
            const auto read = window.GetSession()->GetLabelMapReadResult(request);
            Check(read.error == LabelMapError::None && read.state && read.state->values
                && read.requiredBytes == 4000000000ULL && read.state->values->size() == 4000000000ULL,
                "default label read copies the complete native 1000-cube");
        }
        if (step["checkFullMaskArchive"].toBool()) {
            const auto descriptors=window.GetSession()->GetLabelMapDescriptors();
            const auto source=window.GetSession()->GetImageDescriptor();
            Check(descriptors.size()==1 && source && descriptors.front().sourceRevision==source->dataRevision,
                "full-grid label mask and current source have the same geometry");
            RoiRequest request;request.definition.source=source->dataRevision;
            request.metadata.name="full-grid-mask";
            RoiNode mask;mask.kind=RoiNodeKind::Primitive;
            mask.primitive.shape=RoiShape::MaskReference;
            mask.primitive.mask=descriptors.front().dataRevision;
            request.definition.nodes={mask};
            const auto created=window.GetSession()->SetRoi(request);
            Check(created.error==RoiError::None && created.roi,"full-grid mask ROI is created");
            const auto key=descriptors.front().datasetId;
            const auto saved=window.GetSession()->GetRoiArchiveReader({{created.roi->revision,key}});
            Check(saved.error==RoiError::None&&saved.reader&&saved.reader->GetMetadata().masks.size()==1
                &&saved.reader->GetMaskCount(0)==1000000000ULL,"ROI archive describes the entire mask without aggregating its values");
            RoiArchiveChunkRequest chunk;chunk.elementOffset=999999999;chunk.chunkElements=1;
            const auto tail=saved.reader->GetMaskChunk(chunk);
            Check(tail.error==RoiError::None&&tail.values.size()==1&&tail.isComplete,"full-grid archive exposes a bounded tail chunk");
            const auto restored=window.GetSession()->LoadRoiArchiveReader(*saved.reader,{{key,source->dataRevision}},{created.roi->catalogRevision});
            Check(restored.error==RoiError::None&&restored.rois.size()==1
                &&restored.rois[0].revision.entityId!=created.roi->revision.entityId,
                "full-grid ROI reader restores an independent identity in one publication");
        }
        if (step["cropDrag"].toBool()) {
            // 复用已有公共 interactor 鼠标路径，真实产生裁剪历史节点，禁止空发布冒充裁切。
            Check(GetText(step, "module") == "Crop", "cropDrag belongs to a crop operation");
            auto* panel = window.GetModule("Crop");
            Check(Wait([&] { panel->Observe(); return panel->GetObservedState()["framesReady"].toBool(); }, timeout),
                "crop input views settle before dragging");
            const auto before = panel->GetObservedState()["operationCount"].toString().toULongLong();
            bool hasDragged = false;
            QJsonArray probes;
            for (const std::array<double,3> fraction : {std::array<double,3>{1,.5,.5}, {0,.5,.5},
                    {.5,1,.5}, {.5,0,.5}, {.5,.5,1}, {.5,.5,0}, {1,1,1}, {0,0,0}, {1,0,1}, {0,1,0}}) {
                QElapsedTimer probeClock; probeClock.start();
                SendCropDrag(window, fraction);
                if (Wait([&] { panel->Observe(); return panel->GetObservedState()["operationCount"].toString().toULongLong() > before;
                    }, 1500)) hasDragged = true;
                probes.append(QJsonObject{{"fraction", GetValues(fraction)}, {"elapsedMs", QString::number(probeClock.elapsed())},
                    {"state", panel->GetObservedState()}});
                if (hasDragged) break;
            }
            // 无变化的拾取探测保持短等待；已经提交的节点按本步骤时限等待大数据真实帧。
            const bool framesReady = hasDragged && Wait([&] {
                panel->Observe(); const auto state = panel->GetObservedState();
                return state["framesReady"].toBool() && state["appliedHead"] == state["renderedHead"];
            }, timeout);
            const QJsonObject audit{{"operationCountBefore", QString::number(before)}, {"probes", probes},{"viewTransforms",window.GetWorkflow().getViewTransforms()},
                {"hasCommittedDrag", hasDragged}, {"framesReady", framesReady}, {"finalState", panel->GetObservedState()}};
            if (step.contains("cropDragAudit")) ExportJson(GetText(step, "cropDragAudit"), audit);
            if (!hasDragged || !framesReady) std::cerr << "Crop drag diagnostic: " << GetJsonText(audit).toStdString() << std::endl;
            Check(hasDragged, "real mouse drag commits a new crop history node");
            Check(framesReady, "all crop views present the exact committed history node within the step timeout");
        }
        if (step["switchFeaturesWhilePending"].toBool()) {
            Check(switchCount >= 11, "real pending operation survives switching across every feature page");
            std::cout << "FEATURE SWITCH COUNT: " << switchCount << std::endl;
        }
        if (step.contains("savePartSeeds")) {
            const auto seedKey = GetText(step,"savePartSeeds"); results[seedKey] = BuildPartSeeds(window,record["result"].toObject());
            if (step.contains("partSeedAudit")) ExportJson(GetText(step,"partSeedAudit"),results[seedKey].toObject());
        }
        if (step.contains("partDirectoryAudit")) ExportJson(GetText(step,"partDirectoryAudit"),CheckPartDirectories(window));
        if (step.contains("partEditAudit")) {
            const auto spec = GetResolved(step["partEditAudit"],results).toObject();
            ExportJson(GetText(spec,"path"),CheckPartEditPreview(window,spec));
        }
        if (step.contains("expectPartCount")) {
            const QJsonValue count = record["result"].toObject()["partCount"];
            Check((count.isString() ? GetId(count) : static_cast<std::uint64_t>(count.toInt())) == static_cast<std::uint64_t>(step["expectPartCount"].toInt()),
                "formal catalog has the expected part count");
        }
        if(step.contains("expectRoiCount")) {
            const auto regions=window.GetSession()->GetRoiDescriptors(true);
            Check(regions.size()==static_cast<std::size_t>(step["expectRoiCount"].toInt()),"formal ROI count matches the requested output mode");
            if(step["expectRoiShape"].isString()) {
                const auto roi=window.GetSession()->GetRoiDescriptor(GetRef(record["result"].toObject()["roi"]));
                const auto shape=GetEnum<RoiShape>(step,"expectRoiShape",{{"Box",RoiShape::Box},{"Sphere",RoiShape::Sphere},{"Cylinder",RoiShape::Cylinder},{"Ellipsoid",RoiShape::Ellipsoid},{"HalfSpace",RoiShape::HalfSpace},{"Contour",RoiShape::Contour},{"EllipticCylinder",RoiShape::EllipticCylinder}});
                if(roi && !(roi->definition.nodes.size()==1 && roi->definition.nodes.front().kind==RoiNodeKind::Primitive && roi->definition.nodes.front().primitive.shape==shape)) {
                    QJsonArray nodes;for(const auto& node:roi->definition.nodes)nodes.append(QJsonObject{{"kind",int(node.kind)},{"shape",int(node.primitive.shape)},{"radius",node.primitive.radius}});
                    std::cerr<<"ROI saved geometry diagnostic: "<<GetJsonText(QJsonObject{{"nodes",nodes},{"form",window.GetModule("Roi")->GetActionParameters("SetDraft")}}).toStdString()<<std::endl;
                }
                Check(roi && roi->definition.nodes.size()==1 && roi->definition.nodes.front().kind==RoiNodeKind::Primitive && roi->definition.nodes.front().primitive.shape==shape,"formal saved ROI preserves the requested primitive shape");
                if(step.contains("expectRoiSource"))Check(GetRefText(roi->definition.source)==GetResolved(step["expectRoiSource"],results).toString(),"formal ROI binds its exact declared source");
                const auto& p=roi->definition.nodes.front().primitive;
                const QJsonObject geometry{{"origin",GetValues(p.origin)},{"matrix",GetValues(p.localToSource)},{"radius",p.radius},{"height",p.height},{"axis",GetValues(p.normal)}};
                const auto expectedGeometry=GetResolved(step["expectRoiGeometry"],results).toObject();
                for(auto field=expectedGeometry.begin();field!=expectedGeometry.end();++field) {
                    const auto geometryMatches=[](double a,double b){return std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=1e-10*std::max({1.,std::abs(a),std::abs(b)});};
                    const auto actual=geometry.value(field.key());
                    if(field.value().isArray()) {
                        const auto a=actual.toArray(),b=field.value().toArray();Check(actual.isArray() && a.size()==b.size(),"ROI geometry array shape matches the declared primitive");
                        for(int i=0;i<a.size();++i)Check(geometryMatches(a[i].toDouble(),b[i].toDouble()),"ROI geometry matches declared model coordinates");
                    } else Check(actual.isDouble() && geometryMatches(actual.toDouble(),field.value().toDouble()),"ROI geometric value matches the declared primitive");
                }
                if(step.contains("roiAudit")) {
                    auto audit=geometry;audit["roi"]=GetRefText(roi->revision);audit["source"]=GetRefText(roi->definition.source);audit["shape"]=int(p.shape);
                    ExportJson(GetText(step,"roiAudit"),audit);
                }
            }
            if(step.contains("expectRetainedRoi")) {
                const auto original=GetRef(GetResolved(step["expectRetainedRoi"],results));
                Check(window.GetSession()->GetRoiDescriptor(original).has_value() && GetRef(record["result"].toObject()["roi"])!=original,"save as new ROI preserves the original exact revision");
            }
        }
        if (step.contains("expectEditingCount")) {
            auto* panel = window.GetModule("Part"); panel->Observe();
            Check(panel->GetObservedState()["editingParts"].toArray().size() == step["expectEditingCount"].toInt(), "editing directory follows committed split/merge/history outputs");
        }
        if (step.contains("expectLabelDimensions")) {
            const auto dimensions = GetArray<int,3>(step["expectLabelDimensions"]); bool matched = false;
            for (const auto& descriptor : window.GetSession()->GetLabelMapDescriptors())
                if (GetRefText(descriptor.dataRevision) == record["result"].toObject()["labelMap"].toString()) matched = descriptor.dims == dimensions;
            Check(matched,"edited labels retain the complete original grid dimensions");
        }
        if (step.contains("directoryScreenshot")) {
            auto* panel = window.GetModule(GetText(step,"module"));
            window.GetWorkflow().onNavigate(GetText(step,"module"),GetText(step,"action"),{});
            panel->Observe(); QCoreApplication::processEvents();
            Check(panel->grab().save(GetText(step,"directoryScreenshot")), "actual directory and operation panel screenshot saved");
        }
        if (step.contains("selectPartNode")) ClickPartNode(window, step["selectPartNode"].toInt(-1));
        if (step.contains("highlightSwitches")) CheckPartHighlightSwitches(window, step["highlightSwitches"].toInt());
        if (step["selectAllPartNodes"].toBool()) {
            auto* panel = window.GetModule("Part"); panel->Observe();
            const auto count = panel->GetObservedState()["parts"].toArray().size();
            for (int index = 0; index < count; ++index) ClickPartNode(window, index);
            CheckNodeParameters(window);
        }
        if (step.contains("saveAs")) results[GetText(step, "saveAs")] = record;
        if (step.contains("expectedTransform")) {
            const auto expectedTransform = GetArray<double, 16>(step["expectedTransform"]);
            const auto actual = GetArray<double, 16>(record["result"].toObject()["sourceToTarget"]);
            double error = 0; for (int i = 0; i < 16; ++i) error = std::max(error, std::abs(expectedTransform[i]-actual[i]));
            std::cout << "KNOWN TRANSFORM MAX ERROR: " << error << std::endl;
            Check(error < 1e-5, "alignment recovers the known rigid transform from real measured points");
        }
        if (step.contains("partDisplayAudit")) ExportJson(GetText(step, "partDisplayAudit"), CheckPartDisplay(window));
        if (step.contains("expectVolumeOpacity")) {
            const auto state = window.GetSession()->GetRenderViewState({"primary-3d"});
            Check(state && std::abs(state->material.opacity - step["expectVolumeOpacity"].toDouble()) < 1e-12,
                "segmentation preserves the exact user-selected DVR opacity");
        }
        if (step.contains("volumeFrame")) {
            const auto spec = step["volumeFrame"].toObject();
            Check(Wait([&] { return !window.GetWorkflow().getRenderPending("primary-3d"); }), "native DVR render settles");
            QImage pixels;
            for (auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>())
                if (widget->renderWindow() == window.GetSession()->GetRenderViewEndpoint("primary-3d")->renderWindow)
                    pixels = widget->grab().toImage();
            Check(!pixels.isNull(), "native DVR framebuffer exists");
            if (spec.contains("save")) volumeFrames[GetText(spec, "save")] = pixels;
            if (spec.contains("equals")) Check(volumeFrames.contains(GetText(spec, "equals"))
                && pixels == volumeFrames[GetText(spec, "equals")], "unselected segmentation leaves the native DVR framebuffer unchanged");
        }
        if (step.contains("expectIso")) {
            const auto expectedIso = GetNumber(step, "expectIso");
            Check(Wait([&] {
                const auto state = window.GetSession()->GetRenderViewState({"primary-3d"});
                if (!state || state->isoThreshold != expectedIso) return false;
                return true;
            }, static_cast<int>(timeout)), "primary 3D uses the expected ISO");
        }
        if (step.contains("screenshot")) {
            window.GetWorkflow().onNavigate(GetText(step, "module"), GetText(step, "action"), {});
            std::cout<<"CAPTURE: waiting for visible views"<<std::endl;
            Check(Wait([&] { for (const auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>()) if(widget->isVisible()) {
                const auto id=widget->objectName()=="primary3D"?QString("primary-3d"):widget->objectName();
                if (window.GetWorkflow().getViewRenderPending && window.GetWorkflow().getViewRenderPending(id.toStdString())) return false;
                }
                return true; }, static_cast<int>(timeout)), "visible real-data views finish requested rendering before capture");
            QCoreApplication::processEvents();
            // 直接读取已完成的视图帧；截图不依赖顶层窗口再触发交换信号。
            for (auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>()) if (widget->isVisible()) {
                std::cout<<"CAPTURE: reading "<<widget->objectName().toStdString()<<std::endl;
                const auto frame = widget->grabFramebuffer();
                Check(!frame.isNull() && frame.size() == widget->size() * widget->devicePixelRatioF(),
                    "visible view framebuffer is readable at its current size");
            }
            std::cout<<"CAPTURE: saving window"<<std::endl;
            Check(window.grab().save(GetText(step, "screenshot")), "case screenshot saved");
        }
        if (step["checkViewLayout"].toBool()) CheckFourViewGeometry(window);
        if (step["checkSceneConsistency"].toBool()) {
            const bool consistent = Wait([&] {
                const auto descriptor = window.GetSession()->GetImageDescriptor();
                const auto scenes = window.GetSession()->GetSceneViewStates();
                if (!descriptor || scenes.size() != 4 || scenes.front().sceneEpoch == 0
                    || !window.GetWorkflow().getRenderPending
                    || GetId(window.GetDiagnostics()["pendingUpdateFailures"]) != 0) return false;
                return std::all_of(scenes.begin(), scenes.end(), [&](const auto& scene) {
                    return scene.isAvailable && scene.presentation
                        && scene.presentation->dataRevision == descriptor->dataRevision
                        && scene.sceneEpoch == scenes.front().sceneEpoch
                        // HostDriven 无 dirty 的视图保留最后实际绘制的 epoch；完成以 owner/render 队列为空为准。
                        && scene.renderedEpoch > 0 && scene.renderedEpoch <= scene.sceneEpoch
                        && !window.GetWorkflow().getRenderPending(scene.id);
                });
            }, static_cast<int>(timeout));
            if (!consistent) {
                QJsonArray scenes;
                const auto descriptor = window.GetSession()->GetImageDescriptor();
                for (const auto& scene : window.GetSession()->GetSceneViewStates())
                    scenes.append(QJsonObject{{"id", QString::fromStdString(scene.id)}, {"isAvailable", scene.isAvailable},
                        {"sceneEpoch", QString::number(scene.sceneEpoch)}, {"renderedEpoch", QString::number(scene.renderedEpoch)},
                        {"dataRevision", scene.presentation ? GetRefText(scene.presentation->dataRevision) : QString()}});
                std::cerr << GetJsonText(QJsonObject{{"scenes", scenes},
                    {"selectedInput", descriptor ? GetRefText(descriptor->dataRevision) : QString()},
                    {"hostDiagnostics", window.GetDiagnostics()}}).toStdString();
            }
            Check(consistent, "all four committed scenes use the selected input and epoch with no pending updates or renders");
            if (GetText(step, "module") == "Data" && GetText(step, "action") == "Load") {
                const auto loaded = window.GetRecords().GetRecord(id);
                const auto rendered = loaded["firstRenderedViews"].toObject();
                const auto current = loaded["current"].toObject();
                const auto revision = current["dataRevision"];
                Check(rendered.size() == 4, "successful real load records first actual renders for all four views");
                for (const auto* viewId : {"primary-3d", "slice-top-down", "slice-front-back", "slice-left-right"}) {
                    const auto measured = rendered[viewId].toObject();
                    Check(measured["dataRevision"] == revision && measured["requestToRenderReturnMs"].toString().toLongLong() >= 0
                        && measured["renderCallMs"].isDouble(), "first render measurements use the exact view and loaded revision");
                }
            }
        }
        if (step.contains("businessCapture")) SaveBusinessCapture(window,GetText(step,"businessCapture"),step,id);
        if (!checkpointPath.isEmpty()) {
            QJsonArray viewports;
            for (const auto* widget : window.findChildren<QVTKOpenGLNativeWidget*>()) viewports.append(QJsonObject{
                {"id",widget->objectName()},{"width",widget->width()},{"height",widget->height()},
                {"devicePixelRatio",widget->devicePixelRatioF()},{"isVisible",widget->isVisible()}});
            const auto state = window.GetSession()->GetRenderViewState({"primary-3d"});
            // 仅在测试宿主保存阶段累计统计；绘制计时仍来自 Host 的实际 Render 返回。
            ExportJson(checkpointPath,{{"operationId",QString::number(id)},
                {"module",GetText(step,"module")},{"action",GetText(step,"action")},{"status",record["status"]},
                {"requestedUtc",requestedUtc},{"checkpointUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {"elapsedToCheckpointMs",QString::number(checkpointClock.elapsed())},{"businessElapsedMs",record["elapsedMs"]},
                {"timingsBefore",timingsBefore},{"timingsAfter",window.GetRecords().GetTimings()},
                {"measurementsBefore",measurementsBefore},{"measurementsAfter",window.GetRecords().GetMeasurements()},
                {"viewports",viewports},{"isoThreshold",state ? QJsonValue(state->isoThreshold) : QJsonValue()},
                {"frameGuardChecked",step["checkSceneConsistency"].toBool()}});
            std::cout << "phase=" << GetText(step,"module").toStdString() << "." << GetText(step,"action").toStdString()
                << " event=complete businessMs=" << record["elapsedMs"].toString().toStdString() << std::endl;
        }
        if (step.contains("rendererAudit")) {
            const auto orangePixels = SaveRendererAudit(window, GetText(step, "rendererAudit"));
            if (step.contains("minimumOrangePixels")) Check(orangePixels >= GetNumber(step, "minimumOrangePixels"), "real surface overlay contributes visible orange pixels through the Host render path");
        }
    }
    previewWatch.stop();
    if (script["checkPartPreparation"].toBool()) {
        Check(preparingSamples > 0 && !hasHiddenSource, "observed pending part surface without hiding the source model");
        if (script.contains("partPreparationScreenshot")) Check(hasPreparationCapture, "pending part surface screenshot captured");
    }
    if (script["checkIdle"].toBool()) CheckIdle(window);
    if (script.contains("maximumUpdateFailures")) {
        const auto diagnostics = window.GetDiagnostics();
        Check(GetId(diagnostics["updateFailures"]) <= GetId(script["maximumUpdateFailures"]), "input transitions have bounded update failures without self-waking");
        Check(GetId(diagnostics["pendingUpdateFailures"]) == 0, "input transition finishes without pending update failures");
    }
}
}
int StartTestCase(TestWindow& window, const QString& path, const QString& recordPath, bool selfTest)
{
    int result = 0;
    try {
        Check(Wait([&] { return window.GetIsReady() || !window.GetFailure().isEmpty(); }), "window initializes");
        Check(window.GetIsReady(), window.GetFailure().toStdString().c_str());
        Check(window.findChild<QLabel*>("businessMeasurements") != nullptr, "manual business measurements panel exists");
        Check(window.GetRecords().GetMeasurements()["memory"].toObject()["available"].toBool(), "OS process resource measurement is available");
        if (selfTest) StartSelfTest(window); else StartSequence(window, path);
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << std::endl; window.grab().save("scene-ui-failure.png"); result = 1; }
    const auto businessDiagnostics = window.GetDiagnostics();
    QElapsedTimer stopClock; stopClock.start(); int stopAttempts = 0;
    const bool stopped = Wait([&] { ++stopAttempts; return window.StopSession(); });
    if (!stopped) { std::cerr << "FAIL: Stop remains pending" << std::endl; result = 1; }
    try { if (!recordPath.isEmpty()) { auto records = window.GetRecords().GetRecords(); records["resources"] = window.GetWorkflow().resources.GetJson(); records["hostDiagnostics"] = window.GetDiagnostics();
        records["businessDiagnostics"] = businessDiagnostics;
        records["shutdown"] = QJsonObject{{"isStopped", stopped}, {"elapsedMs", QString::number(stopClock.elapsed())}, {"attempts", stopAttempts}};
        ExportJson(recordPath, records); } }
    catch (const std::exception& error) { std::cerr << error.what() << std::endl; result = 1; }
    return result;
}
}
