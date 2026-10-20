#include "WorkbenchV1Tests.h"
#include "App/TestWindow.h"
#include "Support/ParameterEditor.h"
#include "Support/ObjectBrowser.h"
#include <QCoreApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QTabBar>
#include <QTreeWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QToolButton>
#include <QDockWidget>
#include <QVTKOpenGLNativeWidget.h>
#include <QToolBar>
#include <QSettings>
#include <QApplication>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QAbstractItemModel>
#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QFocusEvent>
#include <QShortcutEvent>
#include <QLocale>
#include <QMenu>
#include <QTemporaryDir>
#include <QLayout>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <algorithm>
namespace Manual {
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void CheckNavigationWidth(TestWindow& window)
{
    const auto originalSize=window.size();
    auto* groups=window.findChild<QComboBox*>("workspaceTabs");auto* modules=window.findChild<QComboBox*>("featureTabs");
    for(const auto& size:{QSize(1560,960),QSize(1180,820),QSize(1000,720)}) {
        window.resize(size);
        for(int group=0;group<groups->count();++group) {
            if(!groups->model()->flags(groups->model()->index(group,0)).testFlag(Qt::ItemIsEnabled))continue;
            groups->setCurrentIndex(group);
            for(int module=0;module<modules->count();++module) {
                modules->setCurrentIndex(module);QCoreApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);window.layout()->activate();window.findChild<QToolBar*>("quickToolbar")->layout()->activate();QCoreApplication::processEvents();
                CheckViewToolbarText(window);
                auto* steps=window.findChild<QTabBar*>("workflowSteps");Require(steps&&steps->isVisible(),"work steps remain visible at each supported width");
                Require(window.rect().contains(QRect(steps->mapTo(&window,QPoint()),steps->size())),"work steps remain inside the minimum-width window");
                auto* context=window.findChild<QToolBar*>("contextToolbar");Require(context && !context->actions().isEmpty(),"the current step exposes one layer of tools");
                auto* toolbar=window.findChild<QToolBar*>("quickToolbar");
                for(auto* entry:toolbar->actions())if(entry->isVisible())if(auto* widget=toolbar->widgetForAction(entry)){
                    if(widget->sizePolicy().horizontalPolicy()==QSizePolicy::Expanding&&widget->objectName().isEmpty())continue;
                    const auto message=QString("toolbar entry fits: window %1, group %2, name [%3], text [%4], visible %5, width %6, x %7, bar %8").arg(window.width()).arg(group).arg(widget->objectName(),entry->text()).arg(widget->isVisible()).arg(widget->width()).arg(widget->mapTo(toolbar,QPoint()).x()).arg(toolbar->width());Require(widget->isVisible()&&toolbar->rect().contains(QRect(widget->mapTo(toolbar,QPoint()),widget->size())),message.toUtf8().constData());}
                for(int i=0;i<steps->count();++i){const auto message=QString("workflow stage fits: window %1, group %2, tab %3 [%4,%5 %6x%7], bar %8x%9").arg(window.width()).arg(group).arg(i).arg(steps->tabRect(i).x()).arg(steps->tabRect(i).y()).arg(steps->tabRect(i).width()).arg(steps->tabRect(i).height()).arg(steps->width()).arg(steps->height());Require(steps->rect().contains(steps->tabRect(i)),message.toUtf8().constData());}
            }
        }
    }
    window.resize(originalSize);window.GetWorkflow().onNavigate("Data","Load",{});QCoreApplication::processEvents();
}
}
void CheckViewToolbarText(TestWindow& window)
{
    for(auto* button:window.findChildren<QAbstractButton*>()) {
        const auto name=button->objectName();
        if(name!="showBusinessStatistics" && name!="expandBusinessStatistics" && name!="fitView" && !name.startsWith("fit_slice-"))continue;
        const auto visible=button->visibleRegion().boundingRect();
        if(visible.isEmpty() || button->text().isEmpty())continue;
        const auto message=QString("visible command text fits: %1 [%2], width %3, required %4")
            .arg(button->objectName(),button->text()).arg(button->width()).arg(button->sizeHint().width());
        Require(button->width()>=button->sizeHint().width() && visible.width()==button->width(),message.toStdString().c_str());
    }
    for(auto* combo:window.findChildren<QComboBox*>()) {
        if(combo->objectName()!="viewLayout" && combo->objectName()!="renderMode")continue;
        if(combo->visibleRegion().isEmpty() || combo->isEditable())continue;
        QStyleOptionComboBox option;option.initFrom(combo);option.currentText=combo->currentText();option.frame=true;
        const auto field=combo->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,combo);
        const auto message=QString("visible selector text fits: %1 [%2], field %3, required %4")
            .arg(combo->objectName(),combo->currentText()).arg(field.width()).arg(combo->fontMetrics().horizontalAdvance(combo->currentText()));
        Require(combo->fontMetrics().horizontalAdvance(combo->currentText())<=field.width(),message.toStdString().c_str());
    }
}
void CheckModulePanelText(ModulePanel& panel)
{
    if(!panel.isVisible())return;
    for(auto* button:panel.findChildren<QAbstractButton*>()) {
        if(button->visibleRegion().isEmpty() || button->text().isEmpty())continue;
        const auto message=QString("panel command fits: %1 [%2], width %3, required %4")
            .arg(button->objectName(),button->text()).arg(button->width()).arg(button->sizeHint().width());
        Require(button->width()>=button->sizeHint().width() && button->visibleRegion().boundingRect().width()==button->width(),message.toStdString().c_str());
    }
    for(auto* combo:panel.findChildren<QComboBox*>()) {
        if(combo->visibleRegion().isEmpty() || combo->isEditable())continue;
        QStyleOptionComboBox option;option.initFrom(combo);option.currentText=combo->currentText();option.frame=true;
        const auto field=combo->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,combo);
        const auto message=QString("panel selector fits: %1 [%2], field %3, required %4")
            .arg(combo->objectName(),combo->currentText()).arg(field.width()).arg(combo->fontMetrics().horizontalAdvance(combo->currentText()));
        Require(combo->fontMetrics().horizontalAdvance(combo->currentText())<=field.width(),message.toStdString().c_str());
    }
    for(auto* label:panel.findChildren<QLabel*>("parameterTitle")) {
        auto* field=label->parentWidget();
        if(label->visibleRegion().isEmpty() || !field->property("propertyRow").toBool())continue;
        const auto font=label->font();Require((font.pixelSize()>0?font.pixelSize():font.pointSizeF()*label->logicalDpiY()/72.0)>=16,"property typography meets the 16 px desktop baseline");
        QWidget* body=nullptr;
        for(auto* child:field->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(child->property("parameterValue").toBool())body=child;
        const auto below=field->property("vectorBelow").toBool();
        Require(body && (below ? body->y()>=label->geometry().bottom()+1 : body->x()>=label->geometry().right()+1),"property names and values occupy separate columns or complete vector rows without overlap");
        Require(field->rect().contains(body->geometry()),"property values remain inside their row");
        const auto message=QString("property name fits one line: %1 [%2], width %3, required %4")
            .arg(field->objectName(),label->text()).arg(label->width()).arg(label->fontMetrics().horizontalAdvance(label->text()));
        Require(!label->wordWrap() && label->fontMetrics().horizontalAdvance(label->text())<=label->width(),message.toStdString().c_str());
        Require(label->accessibleName()==label->text() && !label->accessibleDescription().isEmpty(),"visible property name is the accessible name and full explanation remains available");
    }
    for(auto* input:panel.findChildren<QLineEdit*>("value")) {
        if(!input->property("numericValue").toBool() || input->hasFocus() || input->visibleRegion().isEmpty())continue;
        int digits=0;bool significant=false;
        for(const auto character:input->text().toLower().section('e',0,0))if(character.isDigit()) {
            significant=significant || character.digitValue()!=0;if(significant)++digits;
        }
        const auto message=QString("idle numeric value has at most six significant digits: %1 [%2]").arg(input->accessibleName(),input->text());
        Require(digits<=6,message.toStdString().c_str());
    }
    auto* scroll=panel.findChild<QScrollArea*>("operationScroll");
    Require(scroll && scroll->horizontalScrollBarPolicy()==Qt::ScrollBarAlwaysOff,"properties use one vertical scroller");
    auto* tools=panel.findChild<QToolButton*>("moreOperations");
    Require(tools && !scroll->isAncestorOf(tools),"tools and business commands stay outside the property scroller");
    auto* footer=panel.findChild<QWidget*>("businessCommandFooter");int occupied=0;
    for(auto* child:footer->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(child->isVisible())occupied=std::max(occupied,child->geometry().bottom()+1);
    const int gap=footer->height()-occupied,allowed=footer->layout()->contentsMargins().bottom()+8;
    const auto footerMessage=QString("command footer gap %1 px, height %2, occupied %3, allowed %4").arg(gap).arg(footer->height()).arg(occupied).arg(allowed);
    Require(gap<=allowed,footerMessage.toStdString().c_str());
}
void CheckWorkbenchV1(TestWindow& window)
{
    {
        QTemporaryDir directory;Require(directory.isValid(),"startup Stop regression has isolated layout settings");const auto saved=qApp->property("workbenchLayoutPath");qApp->setProperty("workbenchLayoutPath",directory.filePath("startup.ini"));
        {TestWindow initializing;initializing.BuildSession();Require(!initializing.GetIsReady()&&initializing.GetSession(),"phased initialization returns after the owner Session stage");Require(initializing.StopSession(),"Stop can finish between initialization phases");QCoreApplication::processEvents();Require(!initializing.GetIsReady(),"queued initialization cannot resume after Stop");}
        qApp->setProperty("workbenchLayoutPath",saved);
    }
    CheckNavigationWidth(window);
    auto* objects=window.findChild<QToolButton*>("toggleObjectColumn");auto* parameters=window.findChild<QToolButton*>("toggleParameterColumn");
    window.ResetLayout();objects->setChecked(true);parameters->setChecked(true);QCoreApplication::processEvents();
    auto* primary=window.findChild<QWidget*>("primary3D");
    for(const auto& entry:{std::pair<QString,QToolButton*>{"sceneDock",objects},{"parameterDock",parameters}}){
        auto* dock=window.findChild<QDockWidget*>(entry.first);const auto width=dock->width(),canvas=primary->width();auto* action=window.findChild<QAction*>(entry.first=="sceneDock"?"toggleObjectDock":"toggleParameterDock");Require(action&&!action->shortcut().isEmpty(),"window menu exposes a bound keyboard action");
        QShortcutEvent shortcut(action->shortcut(),0);QApplication::sendEvent(action,&shortcut);QCoreApplication::processEvents();
        Require(dock->isHidden()&&primary->width()>canvas,"keyboard collapse gives the released dock width to the views");Require(window.focusWidget()==window.findChild<QToolButton*>("windowLayoutMenu"),"collapse leaves focus on the visible window menu");
        QSettings preferences(window.GetLayoutPath(),QSettings::IniFormat);Require(!preferences.value("docks/state").toByteArray().isEmpty()&&!preferences.contains("layout/objectWidth"),"one native dock state replaces legacy column preferences");
        QApplication::sendEvent(action,&shortcut);QCoreApplication::processEvents();const auto widthMessage=QString("expansion restores dock %1: expected %2, actual %3, cached %4, window %5").arg(entry.first).arg(width).arg(dock->width()).arg(dock->property("expandedSize").toSize().width()).arg(window.width());Require(dock->isVisible()&&std::abs(dock->width()-width)<=2,widthMessage.toUtf8().constData());
    }
    const auto layout=window.saveState(1);struct RestoreLayoutOnExit { TestWindow& window; QByteArray state; ~RestoreLayoutOnExit(){window.restoreState(state,1);} } restoreLayout{window,layout};auto* gray=window.findChild<QDockWidget*>("grayHistogramDock");gray->show();gray->setFloating(true);gray->resize(640,400);window.SaveLayout();gray->setFloating(false);gray->hide();window.RestoreLayout();QCoreApplication::processEvents();Require(gray->isFloating()&&!gray->isHidden(),"saved native layout restores floating visibility");
    gray->setFloating(false);window.addDockWidget(Qt::TopDockWidgetArea,gray);window.SaveLayout();window.addDockWidget(Qt::BottomDockWidgetArea,gray);window.RestoreLayout();Require(window.dockWidgetArea(gray)==Qt::TopDockWidgetArea,"native layout restores a moved edge");
    window.ResetLayout();Require(!gray->isFloating()&&gray->isHidden()&&!window.findChild<QDockWidget*>("parameterDock")->isHidden(),"reset restores the default windows without reconstructing the Session");
    Require(window.restoreState(layout,1),"the original test layout remains restorable");
    const auto sessionBefore=window.GetSession();Require(static_cast<bool>(sessionBefore),"view dock checks start with the existing Session");
    const auto statesBefore=sessionBefore->GetRenderViewStates();Require(statesBefore.size()==4,"the existing Session exposes four render view states");
    const std::array<QString,4> viewIds{"primary-3d","slice-top-down","slice-front-back","slice-left-right"};
    std::array<QVTKOpenGLNativeWidget*,4> viewWidgets{};std::array<vtkRenderWindow*,4> renderWindows{};
    for(std::size_t i=0;i<viewIds.size();++i){const auto objectName=i==0?QString("primary3D"):viewIds[i];viewWidgets[i]=window.findChild<QVTKOpenGLNativeWidget*>(objectName);Require(viewWidgets[i],"each existing view widget remains discoverable");renderWindows[i]=viewWidgets[i]->renderWindow();Require(renderWindows[i],"each existing view retains its render window");}
    for(const auto area:{Qt::LeftDockWidgetArea,Qt::RightDockWidgetArea,Qt::TopDockWidgetArea,Qt::BottomDockWidgetArea}){
        for(const auto& id:viewIds){auto* dock=window.findChild<QDockWidget*>("viewDock_"+id);Require(dock,"each view has its native dock");dock->setFloating(true);QCoreApplication::processEvents();Require(dock->isFloating(),"view dock can enter floating mode");dock->setFloating(false);QCoreApplication::processEvents();Require(!dock->isFloating(),"view dock can leave floating mode");
            Require((dock->allowedAreas()&Qt::AllDockWidgetAreas)==Qt::AllDockWidgetAreas,"view docks allow all four native edges");window.addDockWidget(area,dock);QCoreApplication::processEvents();Require(window.dockWidgetArea(dock)==area,"view dock moves to each requested native edge");}
    }
    Require(window.GetSession()==sessionBefore,"view dock movement preserves the shared Session pointer");
    for(std::size_t i=0;i<viewIds.size();++i)Require(viewWidgets[i]->renderWindow()==renderWindows[i],"view dock movement preserves each QVTK render window");
    const auto statesAfter=sessionBefore->GetRenderViewStates();Require(statesAfter.size()==statesBefore.size(),"view dock movement preserves render view state count");
    for(const auto& before:statesBefore){const auto after=std::find_if(statesAfter.begin(),statesAfter.end(),[&](const auto& value){return value.id==before.id;});Require(after!=statesAfter.end()&&after->role==before.role,"view dock movement preserves every render view ID and role");}
    Require(window.restoreState(layout,1),"view dock movement leaves the original layout restorable");QCoreApplication::processEvents();
    ParameterEditor optional("Roi","SetDraft","boxToSource",QJsonValue(),QJsonValue());
    const QJsonObject deferredValues{{"name","按需构建"},{"shape","Sphere"},{"centerMM",QJsonValue()},{"radius",1.23456789012345},{"sizeMM",QJsonValue()},{"direction3D",QJsonArray{1,0,0,0,1,0,0,0,1}},{"boxToSource",QJsonValue()}};
    ParameterEditor deferred("Roi","Begin",{},deferredValues,deferredValues,nullptr,false,{},true,true);int deferredEdits=0;deferred.onEdited=[&]{++deferredEdits;};
    Require(!deferred.GetFieldsBuilt()&&deferred.GetHasInputs()&&deferred.findChildren<QLineEdit*>().isEmpty(),"hidden action metadata does not construct any property inputs");
    const QJsonObject expectedDeferred{{"name","按需构建"},{"shape","Sphere"},{"centerMM",QJsonValue()},{"radius",1.23456789012345}};
    Require(deferred.GetValue()==expectedDeferred&&deferred.GetFieldsBuilt()&&deferredEdits==0,"first demand preserves exact values, applicable fields and the no-edit construction contract");
    int optionalEdits=0;optional.onEdited=[&]{++optionalEdits;};
    QJsonArray identity;for(int i=0;i<16;++i)identity.append(i%5==0?1:0);
    optional.SetValue(identity);optional.SetValue(QJsonValue());
    Require(optionalEdits==0,"programmatic optional geometry updates never notify an intermediate edit");
    optional.findChild<QCheckBox*>("specified")->click();
    Require(optionalEdits==1,"explicit optional-parameter toggle still publishes one edit");
    Require(optional.GetValue()==identity,"optional matrix toggle restores its actual default matrix");
    const double originalNumber=8.5189186919467659;
    ParameterEditor number("Wall","Start","maxDistance",originalNumber,originalNumber);
    auto* numberInput=number.findChild<QLineEdit*>("value");int numberEdits=0;double editedNumber=0;
    number.onEdited=[&]{++numberEdits;editedNumber=number.GetValue().toDouble();};
    auto numberLocale=QLocale();if(!numberLocale.zeroDigit().isDigit())numberLocale=QLocale::c();
    bool displayValid=false;const auto displayNumber=numberLocale.toDouble(numberInput->text(),&displayValid);
    Require(displayValid && displayNumber==8.51892,"idle numeric display uses a readable precision");
    Require(number.GetValue().toDouble()==originalNumber,"rounded numeric display preserves the exact parameter value");
    QFocusEvent focusIn(QEvent::FocusIn,Qt::TabFocusReason),focusOut(QEvent::FocusOut,Qt::TabFocusReason);
    QApplication::sendEvent(numberInput,&focusIn);
    Require(numberInput->text()==QString::number(originalNumber,'g',17),"numeric focus restores the full precision for editing");
    QApplication::sendEvent(numberInput,&focusOut);
    Require(number.GetValue().toDouble()==originalNumber && numberEdits==0,"focus alone cannot round the parameter or publish an edit");
    QApplication::sendEvent(numberInput,&focusIn);numberInput->selectAll();
    QKeyEvent numericEdit(QEvent::KeyPress,Qt::Key_2,Qt::NoModifier,"2.75");QApplication::sendEvent(numberInput,&numericEdit);
    Require(number.GetValue().toDouble()==2.75 && numberEdits==1 && editedNumber==2.75,"keyboard input and its edit callback see the new parameter exactly once");
    number.SetValue(originalNumber);Require(number.GetValue().toDouble()==originalNumber && numberEdits==1,"programmatic numeric replacement preserves precision without an edit");
    ParameterEditor defaultNumber("Wall","Start","maxDistance",QJsonValue(),QJsonValue());
    defaultNumber.SetDefaultValue(originalNumber);Require(defaultNumber.GetValue().isNull(),"a numeric default does not specify an optional parameter");
    defaultNumber.findChild<QCheckBox*>("specified")->click();Require(defaultNumber.GetValue().toDouble()==originalNumber,"specifying a numeric default retains its original precision");
    ParameterEditor defaultVector("Roi","Begin","centerMM",QJsonValue(),QJsonValue());
    const QJsonArray preciseVector{originalNumber,-originalNumber,std::nextafter(1.0,2.0)};
    defaultVector.SetDefaultValue(preciseVector);defaultVector.findChild<QCheckBox*>("specified")->click();
    Require(defaultVector.GetValue()==preciseVector,"vector defaults retain every component beyond display precision");
    const auto count=window.GetRecords().GetRecords()["records"].toArray().size();
    if(auto* roi=window.GetModule("Roi"))if(!roi->GetActions().isEmpty()) {
        auto* tabs=roi->findChild<QComboBox*>("businessTaskTabs");Require(tabs && tabs->count()==4,"ROI has creation, edit, operation and management tasks");
        window.GetWorkflow().onNavigate("Roi","Begin",{});
        for(int i=0;i<4;++i){tabs->setCurrentIndex(i);QCoreApplication::processEvents();Require(tabs->currentIndex()==i,"ROI task selection is stable after workflow refresh");}
        roi->SelectAction("Begin");auto* begin=roi->GetParameterEditor("Begin");const auto saved=begin->GetValue();
        begin->SetPatch({{"shape","Sphere"}});QCoreApplication::processEvents();QWidget* advancedRegion=nullptr;QWidget* regionDetails=nullptr;
        for(auto* section:begin->findChildren<QWidget*>("parameterSection")) {
            if(section->property("sectionName")=="精确参数")advancedRegion=section;
            if(section->property("sectionName")=="更多信息")regionDetails=section;
        }
        Require(advancedRegion && !regionDetails && !advancedRegion->isHidden() && advancedRegion->isAncestorOf(begin->GetField("group")) && advancedRegion->isAncestorOf(begin->GetField("description")),"sphere ROI retains all metadata in its single advanced group when matrix fields are inapplicable");
        advancedRegion->findChild<QToolButton*>("sectionToggle")->click();QCoreApplication::processEvents();
        Require(begin->GetField("group")->isVisible() && begin->GetField("description")->isVisible(),"sphere ROI metadata can still be expanded and edited");
        advancedRegion->findChild<QToolButton*>("sectionToggle")->setChecked(false);
        begin->SetPatch({{"shape",saved.toObject()["shape"]}});
        Require(begin->GetValue()==saved && begin->GetField("radius"),"restoring the tested shape retains all visible and hidden parameter editors");
        roi->SelectAction("Difference");auto* form=roi->GetParameterEditor("Difference");
        Require(!form->GetField("operands")->isHidden() && form->GetField("radius")->isHidden(),"difference offers operands without unrelated morphology parameters");
        auto* primary=roi->findChild<QPushButton*>("businessPrimary");auto* scroll=roi->findChild<QScrollArea*>("operationScroll");
        Require(primary->parentWidget()->objectName()=="businessCommandFooter" && !scroll->isAncestorOf(primary),"ROI save remains outside the parameter scroller");
        const auto footerHeight=primary->parentWidget()->height();auto* hidden=roi->findChild<QWidget*>("commands_SurfaceInterior");
        Require(hidden && hidden->isHidden(),"the command height regression uses an existing hidden mode");
        hidden->setMinimumHeight(1200);QCoreApplication::processEvents();
        Require(primary->parentWidget()->height()==footerHeight && scroll->height()>=100,"a hidden command page cannot reserve footer height or crush the parameter area");
        hidden->setMinimumHeight(0);
        for(auto* button:roi->findChildren<QPushButton*>())if(button->objectName().startsWith("next_"))
            Require(button->accessibleName()==button->text(),"ROI accessible button names follow their visible labels");
        auto* tools=roi->findChild<QToolButton*>("moreOperations");
        Require(tools->menu()->findChild<QAction*>("command_Cancel"),"ROI cancel remains discoverable in the shared tools menu");
    }
    if(auto* surface=window.GetModule("Surface"))if(!surface->GetActions().isEmpty()) {
        window.GetWorkflow().onNavigate("Surface","GlobalAutomatic",{});
        auto* form=surface->GetParameterEditor("GlobalAutomatic");form->SetPatch({{"initialIsoValue",12.5},{"materialRange",QJsonValue()}});
        auto* source=form->findChild<QComboBox*>("surfaceParameterSource");Require(source && source->currentData().toBool(),"explicit initial ISO selects manual parameter source");
        source->setCurrentIndex(0);Require(form->GetValue().toObject()["initialIsoValue"].isNull(),"automatic mode omits manual ISO without mutating formal data");
        source->setCurrentIndex(1);Require(form->GetValue().toObject()["initialIsoValue"].toDouble()==12.5,"switching back to manual restores the parameter draft");
        QWidget* advanced=nullptr;QWidget* expert=nullptr;
        for(auto* section:form->findChildren<QWidget*>("parameterSection")) {
            if(section->property("sectionName")=="高级设置")advanced=section;
            if(section->property("sectionName")=="专家设置")expert=section;
        }
        Require(advanced && !expert && advanced->isAncestorOf(form->GetField("minimumObjectVoxels")) && advanced->isAncestorOf(form->GetField("parentGeneration")),"surface retains its expert parameters in one advanced group");
        form->SetPatch({{"initialIsoValue",QJsonValue()},{"materialRange",QJsonValue()}});
    }
    if(auto* alignment=window.GetModule("Alignment"))if(!alignment->GetActions().isEmpty()) {
        window.GetWorkflow().onNavigate("Alignment","ImportReference",{});auto* operations=alignment->findChild<QComboBox*>("parameterTabs");
        for(const auto* action:{"ImportReference","EvaluateVoxelVolume","InspectGeometry","InspectSection","EvaluateSurface","ExportReport"})Require(operations->findData(action)>=0,"the single operation selector retains every measurement task family");
        for(int index=0;index<operations->count();++index) {
            operations->setCurrentIndex(index);QCoreApplication::processEvents();
            for(auto* combo:{operations}) {
                QStyleOptionComboBox option;option.initFrom(combo);option.currentText=combo->currentText();option.frame=true;
                const auto field=combo->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,combo);
                Require(combo->fontMetrics().horizontalAdvance(combo->currentText())<=field.width(),"all measurement operations fit the actual single selector field");
            }
        }
    }
    if(auto* wall=window.GetModule("Wall"))if(!wall->GetActions().isEmpty()) {
        window.GetWorkflow().onNavigate("Wall","Start",{});QCoreApplication::processEvents();
        auto* toggle=wall->findChild<QToolButton*>("showOperationPrerequisites");auto* area=wall->findChild<QWidget*>("operationPrerequisites");
        Require(toggle && area->isVisible() && !toggle->isChecked(),"missing wall inputs have a compact collapsed prerequisite strip");
        const auto before=wall->GetParameterEditor("Start")->GetValue();
        QKeyEvent press(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier),release(QEvent::KeyRelease,Qt::Key_Space,Qt::NoModifier);
        QApplication::sendEvent(toggle,&press);QApplication::sendEvent(toggle,&release);QCoreApplication::processEvents();
        auto* detail=area->findChild<QLabel*>("commandRequirement");
        Require(toggle->isChecked() && detail && detail->isVisible() && detail->text()==toggle->accessibleDescription(),"keyboard expands the complete prerequisite reason");
        QApplication::sendEvent(toggle,&press);QApplication::sendEvent(toggle,&release);
        Require(!detail->isVisible() && wall->GetParameterEditor("Start")->GetValue()==before,"collapsing prerequisites preserves all parameter values");
    }
    Require(window.GetRecords().GetRecords()["records"].toArray().size()==count,"navigation and parameter-source switching never submit requests");
    ObjectBrowser browser;browser.resize(400,450);browser.show();
    const QJsonObject graph{{"nodes",QJsonArray{
        QJsonObject{{"ref","volume:1"},{"isVolume",true},{"order","1"},{"producer","org.mvvcvtk.host"}},
        QJsonObject{{"ref","mesh:1"},{"isVolume",false},{"type","org.mvvcvtk.surface-mesh"},{"order","2"}}}}};
    browser.SetObjects(graph,{}, {},"volume:1",{});QCoreApplication::processEvents();
    auto* tree=browser.findChild<QTreeWidget*>("workbenchObjects");auto* use=browser.findChild<QPushButton*>("useSelectedObject");
    Require(tree->topLevelItemCount()==4,"shared directory has four user object categories");
    auto* mesh=tree->topLevelItem(2)->child(0);tree->setCurrentItem(mesh);Require(!use->isEnabled(),"selecting a surface cannot adopt it as grayscale input");
    browser.SetObjects(graph,{}, {},"volume:1",{});Require(tree->currentItem()==mesh,"unchanged directory preserves selection and row identity");
    int actions=0;browser.onAction=[&](const QString&,const QString& action,const QJsonObject& patch){Require(action=="UseData" && patch["graphRevision"]=="volume:1","adoption uses the selected volume revision");++actions;};
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));Require(actions==0,"ordinary directory selection is read-only");use->click();Require(actions==1,"adoption is a distinct explicit command");
    auto* volume=tree->topLevelItem(0)->child(0);
    browser.SetObjects(graph,{}, {},"volume:1",{},1.0);Require(volume->checkState(0)==Qt::Checked,"known visible input has a checked visibility indicator");
    browser.SetObjects(graph,{}, {},"volume:1",{});Require(!volume->data(0,Qt::CheckStateRole).isValid() && !(volume->flags()&Qt::ItemIsUserCheckable),"unknown visibility clears the old indicator instead of displaying a stale checked state");
    browser.SetObjects(graph,{}, {},"volume:1",{},0.0);Require(volume->data(0,Qt::CheckStateRole).isValid() && volume->checkState(0)==Qt::Unchecked,"known hidden input has an explicit unchecked indicator");
    browser.SetObjects(graph,{}, {},"",{});Require(!volume->data(0,Qt::CheckStateRole).isValid() && actions==1,"historical data has no inherited visibility indicator and refresh never submits a command");browser.hide();
    BusinessStatistics statistics;statistics.resize(650,400);statistics.show();
    const QJsonObject histogram{{"title","known range"},{"description","fixed source R1"},{"counts",QJsonArray{2,3}},{"edges",QJsonArray{0,50,100}},
        {"markers",QJsonArray{QJsonObject{{"key","iso"},{"label","ISO"},{"value",50},{"editable",true}}}}};
    statistics.SetHistogram(histogram);statistics.SetInteractionModes({"编辑 ISO"});QCoreApplication::processEvents();
    auto* plot=statistics.findChild<QWidget*>("businessHistogramPlot");auto* modes=statistics.findChild<QComboBox*>("histogramInteraction");
    int edits=0;double latest=0;statistics.onMarker=[&](const QString& key,double value){Require(key=="iso","marker drag keeps parameter identity");latest=value;++edits;};
    const QPointF start(62+(plot->width()-82)*0.5,40),end(62+(plot->width()-82)*0.75,40);
    const auto drag=[&]{QMouseEvent press(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&press);
        QMouseEvent move(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&move);
        QMouseEvent release(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&release);};
    drag();Require(edits==0,"view mode cannot modify a histogram parameter");modes->setCurrentIndex(1);drag();
    // 鼠标事件按整数逻辑像素取整；容差来自一个绘图区像素对应的量值。
    const auto pixelValue=100.0/std::max(1,plot->width()-82);
    Require(edits==1 && std::abs(latest-75)<=pixelValue && statistics.GetHistogram()["counts"]==histogram["counts"],"marker release commits one draft value without rebucketing measurements");
    QMouseEvent press(QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&press);
    QMouseEvent move(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&move);Require(edits==1,"marker motion only previews a value");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QCoreApplication::sendEvent(plot,&escape);Require(edits==1,"Escape discards an uncommitted marker drag");
    auto first=histogram;first["cacheKey"]="same-source:preview:1";auto second=first;second["cacheKey"]="same-source:preview:2";
    statistics.SetHistogram(first);statistics.onMarker=[&](const QString&,double){++edits;};
    QCoreApplication::sendEvent(plot,&press);QCoreApplication::sendEvent(plot,&move);
    statistics.SetHistogram(second);statistics.onMarker=[&](const QString&,double){++edits;};
    QMouseEvent release(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(plot,&release);
    Require(edits==1,"equal descriptions and distributions cannot apply an old marker to a new ROI preview");
    int intervals=0;const QPointF intervalStart(62+(plot->width()-82)*0.25,40);
    QMouseEvent intervalPress(QEvent::MouseButtonPress,intervalStart,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    statistics.SetHistogram(first);statistics.onInterval=[&](double,double){++intervals;};QCoreApplication::sendEvent(plot,&intervalPress);
    statistics.SetHistogram(second);statistics.onInterval=[&](double,double){++intervals;};QCoreApplication::sendEvent(plot,&release);
    Require(intervals==0,"old histogram interval selection cannot edit a new ROI preview");
    QCoreApplication::sendEvent(plot,&intervalPress);QCoreApplication::sendEvent(plot,&release);
    Require(intervals==1,"unchanged histogram source still accepts one interval selection");
    const auto intervalCount=intervals;
    const auto invalidatedDrag=[&](const QString& label,const std::function<void()>& invalidate){
        statistics.SetHistogram(histogram);statistics.SetMarkers(histogram["markers"].toArray());statistics.SetInteractionModes({"编辑 ISO","另一用途"});modes->setCurrentIndex(1);QCoreApplication::processEvents();
        statistics.onMarker=[&](const QString&,double){++edits;};statistics.onInterval=[&](double,double){++intervals;};
        QCoreApplication::sendEvent(plot,&press);QCoreApplication::sendEvent(plot,&move);invalidate();QCoreApplication::sendEvent(plot,&release);
        Require(edits==1 && intervals==intervalCount,label.toUtf8().constData());
    };
    invalidatedDrag("context changes discard a pending marker and interval gesture",[&]{statistics.SetInteractionContext("changed");});
    invalidatedDrag("mode changes discard a pending marker and interval gesture",[&]{modes->setCurrentIndex(2);modes->setCurrentIndex(1);});
    invalidatedDrag("Cancel discards a pending marker and interval gesture",[&]{statistics.Cancel();});
    invalidatedDrag("hide and show discard a pending marker and interval gesture",[&]{statistics.hide();statistics.show();});
    invalidatedDrag("range reset discards a pending marker and interval gesture",[&]{statistics.findChild<QToolButton*>("resetHistogramRange")->click();});
    invalidatedDrag("focus out discards a pending marker and interval gesture",[&]{QFocusEvent lost(QEvent::FocusOut,Qt::OtherFocusReason);QApplication::sendEvent(&statistics,&lost);});
    invalidatedDrag("removing marker editability discards a pending marker and interval gesture",[&]{statistics.SetMarkers({QJsonObject{{"key","iso"},{"label","ISO"},{"value",50},{"editable",false}}});});
    statistics.SetHistogram(histogram);statistics.SetMarkers(histogram["markers"].toArray());statistics.SetInteractionModes({"编辑 ISO","另一用途"});modes->setCurrentIndex(1);statistics.SetInteractionContext("keyboard");
    statistics.onMarker=[&](const QString&,double value){latest=value;++edits;};statistics.onInterval=[&](double,double){++intervals;};
    QKeyEvent right(QEvent::KeyPress,Qt::Key_Right,Qt::NoModifier);QApplication::sendEvent(plot,&right);Require(edits==1,"Right only previews a marker candidate");
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(plot,&enter);Require(edits==2 && latest==51,"Return commits the one-unit marker step exactly once");
    QApplication::sendEvent(plot,&enter);Require(edits==2,"a second Return without a candidate does not recommit");
    QCoreApplication::sendEvent(plot,&press);QCoreApplication::sendEvent(plot,&move);QApplication::sendEvent(plot,&escape);QApplication::sendEvent(plot,&enter);Require(edits==2,"Escape cancels a keyboard candidate before Return");
    modes->setCurrentIndex(0);QApplication::sendEvent(plot,&right);QApplication::sendEvent(plot,&enter);Require(edits==2,"view mode arrows and Return never commit a marker");
    statistics.SetInteractionContext("numeric");modes->setCurrentIndex(1);statistics.onMarker=[&](const QString&,double value){latest=value;++edits;};
    auto* markerValue=statistics.findChild<QLineEdit*>("histogramMarkerValue");auto* applyMarker=statistics.findChild<QPushButton*>("applyHistogramMarker");
    const QString preciseText("12.345678901234567");const double preciseValue=preciseText.toDouble();
    markerValue->setFocus();markerValue->selectAll();QKeyEvent exactText(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,preciseText);QApplication::sendEvent(markerValue,&exactText);
    QKeyEvent numericEnter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(markerValue,&numericEnter);
    Require(edits==3 && latest==preciseValue,"textEdited numeric input submits the exact double once on Enter");
    const auto savedLocale=QLocale();QLocale::setDefault(QLocale(QLocale::German,QLocale::Germany));
    statistics.SetMarkers({QJsonObject{{"key","iso"},{"label","ISO"},{"value",1.234},{"editable",true}}});
    markerValue->selectAll();QKeyEvent localeText(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,"1.234");QApplication::sendEvent(markerValue,&localeText);QApplication::sendEvent(markerValue,&numericEnter);
    QLocale::setDefault(savedLocale);Require(edits==4&&latest==1.234,"canonical precise decimal cannot become a localized thousands group");
    markerValue->selectAll();QKeyEvent staleText(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,preciseText);QApplication::sendEvent(markerValue,&staleText);
    statistics.SetInteractionContext("numeric-changed");applyMarker->click();Require(edits==4,"a context change invalidates a pending numeric marker submission");
    statistics.SetInteractionContext("numeric-view");modes->setCurrentIndex(0);markerValue->selectAll();QKeyEvent viewText(QEvent::KeyPress,Qt::Key_1,Qt::NoModifier,preciseText);QApplication::sendEvent(markerValue,&viewText);applyMarker->click();
    Require(edits==4,"view mode cannot submit a numeric marker value");
    statistics.hide();
    window.GetWorkflow().onNavigate("Data","Load",{});
}
}
