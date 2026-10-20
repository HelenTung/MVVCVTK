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
#include <QSplitter>
#include <QSettings>
#include <QApplication>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QAbstractItemModel>
#include <QAbstractButton>
#include <stdexcept>
#include <cmath>
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
                modules->setCurrentIndex(module);QCoreApplication::processEvents();
                CheckWorkbenchText(window);
                for(auto* combo:{groups,modules}) {
                    QStyleOptionComboBox option;option.initFrom(combo);option.currentText=combo->currentText();option.frame=true;
                    const auto field=combo->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,combo);
                    Require(combo->fontMetrics().horizontalAdvance(combo->currentText())<=field.width(),"every current category and module name fits its actual text field at all three window sizes");
                    Require(window.rect().contains(QRect(combo->mapTo(&window,QPoint()),combo->size())),"navigation selectors remain inside the minimum-width window");
                }
            }
        }
    }
    window.resize(originalSize);window.GetWorkflow().onNavigate("Data","Load",{});QCoreApplication::processEvents();
}
}
void CheckWorkbenchText(TestWindow& window)
{
    for(auto* button:window.findChildren<QAbstractButton*>()) {
        const auto visible=button->visibleRegion().boundingRect();
        if(visible.isEmpty() || button->text().isEmpty())continue;
        const auto message=QString("visible command text fits: %1 [%2], width %3, required %4")
            .arg(button->objectName(),button->text()).arg(button->width()).arg(button->sizeHint().width());
        Require(button->width()>=button->sizeHint().width() && visible.width()==button->width(),message.toStdString().c_str());
    }
    for(auto* combo:window.findChildren<QComboBox*>()) {
        if(combo->visibleRegion().isEmpty() || combo->isEditable())continue;
        QStyleOptionComboBox option;option.initFrom(combo);option.currentText=combo->currentText();option.frame=true;
        const auto field=combo->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,combo);
        const auto message=QString("visible selector text fits: %1 [%2], field %3, required %4")
            .arg(combo->objectName(),combo->currentText()).arg(field.width()).arg(combo->fontMetrics().horizontalAdvance(combo->currentText()));
        Require(combo->fontMetrics().horizontalAdvance(combo->currentText())<=field.width(),message.toStdString().c_str());
    }
}
void CheckWorkbenchV1(TestWindow& window)
{
    CheckNavigationWidth(window);
    auto* split=window.findChild<QSplitter*>("workbenchSplit");
    auto* objects=window.findChild<QToolButton*>("toggleObjectColumn");
    auto* parameters=window.findChild<QToolButton*>("toggleParameterColumn");
    auto* viewArea=window.findChild<QWidget*>("viewArea");
    objects->setChecked(true);parameters->setChecked(true);QCoreApplication::processEvents();
    const auto originalSizes=split->sizes();const int originalCanvas=viewArea->width();
    for(const auto& entry:{std::pair<int,QToolButton*>{0,objects},{2,parameters}}) {
        entry.second->setFocus();
        QKeyEvent press(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier),release(QEvent::KeyRelease,Qt::Key_Space,Qt::NoModifier);
        QApplication::sendEvent(entry.second,&press);QApplication::sendEvent(entry.second,&release);QCoreApplication::processEvents();
        Require(split->widget(entry.first)->isHidden() && viewArea->width()>originalCanvas,"keyboard collapse gives the released sidebar width to the views");
        Require(window.focusWidget()==entry.second,"collapsed sidebar leaves logical keyboard focus on its expand button");
        QSettings preferences(QCoreApplication::applicationDirPath()+"/QtFeatureWorkbench.ini",QSettings::IniFormat);
        Require(!preferences.value(entry.first==0?"layout/objectsVisible":"layout/parametersVisible").toBool(),"collapsed state is written to persistent preferences");
        QApplication::sendEvent(entry.second,&press);QApplication::sendEvent(entry.second,&release);QCoreApplication::processEvents();
        Require(split->widget(entry.first)->isVisible() && std::abs(split->sizes()[entry.first]-originalSizes[entry.first])<=2,"keyboard expansion restores the last sidebar width");
    }
    ParameterEditor optional("Roi","SetDraft","boxToSource",QJsonValue(),QJsonValue());
    int optionalEdits=0;optional.onEdited=[&]{++optionalEdits;};
    QJsonArray identity;for(int i=0;i<16;++i)identity.append(i%5==0?1:0);
    optional.SetValue(identity);optional.SetValue(QJsonValue());
    Require(optionalEdits==0,"programmatic optional geometry updates never notify an intermediate edit");
    optional.findChild<QCheckBox*>("specified")->click();
    Require(optionalEdits==1,"explicit optional-parameter toggle still publishes one edit");
    Require(optional.GetValue()==identity,"optional matrix toggle restores its actual default matrix");
    const auto count=window.GetRecords().GetRecords()["records"].toArray().size();
    if(auto* roi=window.GetModule("Roi"))if(!roi->GetActions().isEmpty()) {
        auto* tabs=roi->findChild<QComboBox*>("businessTaskTabs");Require(tabs && tabs->count()==4,"ROI has creation, edit, operation and management tasks");
        window.GetWorkflow().onNavigate("Roi","Begin",{});
        for(int i=0;i<4;++i){tabs->setCurrentIndex(i);QCoreApplication::processEvents();Require(tabs->currentIndex()==i,"ROI task selection is stable after workflow refresh");}
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
        Require(advanced && expert && advanced->isAncestorOf(expert),"surface expert parameters are nested inside advanced settings");
        form->SetPatch({{"initialIsoValue",QJsonValue()},{"materialRange",QJsonValue()}});
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
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));Require(actions==0,"ordinary directory selection is read-only");use->click();Require(actions==1,"adoption is a distinct explicit command");browser.hide();
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
    statistics.hide();
    window.GetWorkflow().onNavigate("Data","Load",{});
}
}
