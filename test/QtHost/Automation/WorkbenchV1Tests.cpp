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
#include <stdexcept>
#include <cmath>
namespace Manual {
namespace { void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);} }
void CheckWorkbenchV1(TestWindow& window)
{
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
        auto* tabs=roi->findChild<QTabBar*>("businessTaskTabs");Require(tabs && tabs->count()==4,"ROI has creation, edit, operation and management tasks");
        window.GetWorkflow().onNavigate("Roi","Begin",{});
        for(int i=0;i<4;++i){tabs->setCurrentIndex(i);QCoreApplication::processEvents();Require(tabs->currentIndex()==i,"ROI task selection is stable after workflow refresh");}
        roi->SelectAction("Difference");auto* form=roi->GetParameterEditor("Difference");
        Require(!form->GetField("operands")->isHidden() && form->GetField("radius")->isHidden(),"difference offers operands without unrelated morphology parameters");
        auto* primary=roi->findChild<QPushButton*>("businessPrimary");auto* scroll=roi->findChild<QScrollArea*>("operationScroll");
        Require(primary->parentWidget()->objectName()=="businessCommandFooter" && !scroll->isAncestorOf(primary),"ROI save remains outside the parameter scroller");
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
