// 测试用途：提供中文结构化参数控件；多值逐格输入，多组数据逐行增删，不要求手写 JSON。
#include "ParameterEditor.h"
#include <QTabWidget>
#include "UiText.h"
#include "PathInput.h"
#if defined(MANUAL_ALIGNMENT)
#include "Modules/AlignmentInput.h"
#endif
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFocusEvent>
#include <QLocale>
#include <QResizeEvent>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QVBoxLayout>
#include <QSet>
#include <QSignalBlocker>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Manual {
namespace {
class NumberInput final : public QLineEdit {
public:
    NumberInput(double value, QWidget* parent) : QLineEdit(parent)
    {
        setProperty("numericValue",true);
        SetNumber(value);
        const auto remember = [this](const QString& text) {
            if (!m_formatting) m_exactText = text;
        };
        // textEdited 可早于 textChanged；先保存新文本，再由表单回调读取实际值。
        connect(this, &QLineEdit::textEdited, this, remember);
        connect(this, &QLineEdit::textChanged, this, remember);
    }
    void SetNumber(double value)
    {
        m_exactText = QString::number(value, 'g', 17);
        SetPresentation();
    }
    double GetNumber(bool* valid) const { return m_exactText.toDouble(valid); }
protected:
    void focusInEvent(QFocusEvent* event) override
    {
        QLineEdit::focusInEvent(event);
        m_formatting = true; setText(m_exactText); m_formatting = false;
        if (event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason) selectAll();
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        QLineEdit::focusOutEvent(event); SetPresentation();
    }
private:
    void SetPresentation()
    {
        bool valid = false; const auto number = GetNumber(&valid);
        auto display = m_exactText;
        if (!hasFocus() && valid && std::isfinite(number)) {
            auto locale = QLocale();
            // 与统计刻度一致：Qt 5 会把不连续的本地数字（例如〇）错误地映射成括号。
            if(!locale.zeroDigit().isDigit())locale=QLocale::c();
            locale.setNumberOptions(QLocale::OmitGroupSeparator);
            display = locale.toString(number, 'g', 6);
        }
        // 显示舍入与实际参数分开；聚焦、失焦和程序回填均不发布 textEdited。
        m_formatting = true; setText(display); m_formatting = false; setToolTip(m_exactText);
    }
    QString m_exactText;
    bool m_formatting = false;
};
QString ShortTitle(const QString& key, const QString& title)
{
    if(title.startsWith("第 "))return title;
    static const QHash<QString, QString> names{
        {"labels","材料标签"}, {"materialLabels","材料标签集"}, {"maxDistance","搜索上限"},
        {"sampleSpacing","采样间距"}, {"materialThreshold","材料阈值"}, {"materialRange","背景/材料"},
        {"coneAngleDegrees","搜索半角"}, {"minRegionArea","最小区域面积"}, {"filePath","输入文件"},
        {"outputPath","输出文件"}, {"archivePath","归档文件"}, {"maskPath","掩码输出"},
        {"parentNodeId","父裁剪节点"}, {"matrix","坐标矩阵"}, {"center","中心"},
        {"centerMM","中心"}, {"sizeMM","完整尺寸"}, {"direction3D","区域方向"},
        {"dimensions","体素数量"}, {"spacingLPS","体素间距"}, {"originLPS","原点坐标"},
        {"directionLPS","方向矩阵"}, {"expectedBindingRevision","输入绑定版本"},
        {"angleDeg","旋转角度"}, {"offset","起始偏移"}, {"legendPosition","色标位置"},
        {"segmentFrom","从值"}, {"segmentTo","至值"}, {"lowColor","低端颜色"}, {"highColor","高端颜色"},
        {"toleranceBelowColor","公差下侧颜色"}, {"toleranceAboveColor","公差上侧颜色"},
        {"belowColor","下侧颜色"}, {"aboveColor","上侧颜色"}, {"transfer","传递函数"},
        {"inputRoi","输入区域"}, {"minPartVoxels","最小体素数"}, {"boxToSource","区域变换"},
        {"expectedCatalogRevision","零件目录版本"}, {"expectedLabelMap","预期标签图"},
        {"sourcePointsMM","笔刷点"}, {"overwriteParts","可覆盖零件"}, {"seedModelPoint","种子点"},
        {"profileHalfLengthModel","剖面半长"}, {"profileSampleStepModel","剖面采样间距"},
        {"maximumOffsetModel","最大定位偏移"}, {"profileSmoothingSigmaModel","剖面平滑尺度"},
        {"maximumPlateauNoiseRatio","平台噪声比"}, {"maximumNormalizedResidual","拟合残差上限"},
        {"minimumEdgeWidthModel","最小边缘宽度"}, {"maximumEdgeWidthModel","最大边缘宽度"},
        {"minimumEdgeSeparationModel","最小边缘间距"}, {"targetRequestId","目标请求编号"},
        {"worldAxis","世界旋转轴"}, {"worldCenter","世界旋转中心"}, {"reference","名义参考"},
        {"mesh","测量网格"}, {"selectionMode","作用方式"}, {"outputMode","输出方式"},
        {"planeToSource","轮廓平面变换"}, {"sourceToView","源到视图变换"}, {"viewToClip","投影变换"},
        {"rasterize","目标网格重采样"}, {"sourceToTarget","源到目标变换"}, {"meshToSource","网格到源变换"},
        {"meshToRoi","网格到区域变换"}, {"hasOrientation","区分内外侧"},
        {"maxParallelAngleDegrees","平行角偏差"}, {"captureCurrentDisplay","捕获显示配置"},
        {"includeDeviationSamples","包含逐点偏差"}, {"deviationLimits","偏差允许区间"},
        {"useSectionPlane","使用截面平面"}, {"fileUnit","STL 单位"},
        {"requiresMeasurementQuality","要求测量质量"}, {"minPairNormalCosine","法向余弦下限"},
        {"maxPairDistance","点对距离上限"}, {"minPairCoverage","点对覆盖下限"}};
    return names.value(key, title.section(QChar(0xff08), 0, 0));
}
QString ValueUnit(const QString& key, const QString& title)
{
    if (key == "centerMM") return "源模型";
    if (key == "sizeMM" || title.contains("模型单位") || title.contains("模型长度单位")) return "模型单位";
    if (title.contains("（mm³）") || title.contains("立方毫米")) return "mm³";
    if (title.contains("（mm）") || title.contains("毫米")) return "mm";
    if (title.contains("对齐长度单位")) return "对齐单位";
    if (title.contains("（度）")) return "°";
    if (title.contains("（毫秒）")) return "ms";
    if (title.contains("（体素）")) return "体素";
    if (title.contains("（0～1）")) return "0～1";
    return {};
}
class BooleanCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    // 半选只表示“不修改”；正常点击始终在 true/false 之间切换，不进入省略状态。
    void nextCheckState() override { setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked); }
};
bool IsList(const QString& key)
{
    static const QSet<QString> keys{"sourcePointsMM", "seeds", "barriers", "parts", "protectedParts", "overwriteParts",
        "indexBoxes", "initialPoses", "colorNodes", "opacityNodes", "geometries", "constraints", "fitPairs", "vertexIds", "segments", "triangles", "sourceTriangles", "referenceTriangles", "scales"};
    return keys.contains(key);
}
QJsonValue Shape(const QString& key)
{
    if (key == "range" || key == "tolerance" || key == "deviationLimits" || key == "storedValueRange") return QJsonArray{0.,1.};
    if (key == "selectionPoint") return QJsonArray{0.,0.,0.};
    if (key == "nominal" || key == "minNormalCosine") return 0.;
    if (key == "worldCenter" || key == "seedModelPoint" || key == "targetNormal") return QJsonArray{0,0,0};
    if(key=="segmentFrom" || key=="segmentTo")return 0.0;
    if (key == "extent" || key == "targetBounds" || key=="sectionPlaneBounds") return QJsonArray{0,0,0,0,0,0};
    if (key == "windowLevel" || key == "radiusRange") return QJsonArray{0,1};
    if (key == "centerIndex") return QJsonArray{0,0};
    if (key == "dimensions") return QJsonArray{0,0,0};
    if (key == "colorRGBA") return QJsonArray{1,1,1,1};
    if (key == "constantColor" || key == "lowColor" || key == "highColor"
        || key == "belowColor" || key == "aboveColor") return QJsonArray{0.7,0.7,0.7};
    if (key == "boxToSource") return QJsonArray{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (key == "centerMM") return QJsonArray{0,0,0};
    if (key == "sizeMM") return QJsonArray{1,1,1};
    if (key == "direction3D") return QJsonArray{1,0,0,0,1,0,0,0,1};
    if (key == "matrix") return QJsonArray{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (key == "directionLPS") return QJsonArray{1,0,0,0,1,0,0,0,1};
    if (key == "materialRange") return QJsonArray{0,1};
    if (key == "histogramRange") return QJsonArray{0,1};
    if (key == "transfer") return QJsonObject{{"colorNodes", QJsonArray{}}, {"opacityNodes", QJsonArray{}}};
    if (key == "grayPair") return QJsonObject{{"sideA", QJsonArray{-1.0, 0.0}}, {"sideB", QJsonArray{1.0, 2.0}}};
    if (key == "slice") return QJsonObject{{"origin", QJsonArray{0,0,0}}, {"normal", QJsonArray{0,0,1}}, {"thicknessMM", 1.0}};
    if (key == "axes" || (key.startsWith("is") && key.size() > 2 && key[2].isUpper()) || key == "planes" || key == "crosshair" || key == "ruler") return false;
    if (key == "iso" || key == "threshold" || key == "absoluteIsoValue" || key == "backgroundMean" || key == "materialMean" || key == "materialThreshold"
        || key == "sampleSpacing" || key == "maxBoundaryError" || key == "maxDistance"
        || key == "lower" || key == "upper"
        || key == "opacity" || key == "angleDeg" || key == "minimumContrast" || key == "initialIsoValue"
        || key.startsWith("profile") || key == "maximumOffsetModel" || key == "minPairNormalCosine"
        || key == "minimumEdgeWidthModel" || key == "maximumEdgeWidthModel" || key == "minimumEdgeSeparationModel") return 0.0;
#if defined(MANUAL_ALIGNMENT)
    if (key == "sourceRegion") return QJsonObject{{"pinnedMesh", "$mesh"}, {"vertexIds", QJsonArray{}}, {"targetBounds", QJsonValue()}};
    if (key == "surfaceFit") return QJsonObject{{"sourceRegion", QJsonValue()}, {"referenceTriangles", QJsonArray{}}, {"iterationLimit", "40"}, {"isPointToPlane", true}, {"scales", QJsonArray{}}};
    if (key == "prealignment") return QJsonObject{{"sampleSpacing", 0.}, {"normalRadius", 0.}, {"featureRadius", 0.}, {"maxPairDistance", 0.},
        {"minCoverage", .25}, {"edgeLengthRatio", .9}, {"candidateSeparation", 0.}, {"neighborLimit", "64"}, {"matchCount", "4"},
        {"iterationLimit", "10000"}, {"candidateLimit", "8"}, {"randomSeed", "0"}, {"sourceTriangles", QJsonArray{}}, {"referenceTriangles", QJsonArray{}}};
    if (key == "recipe") return GetReferenceTemplate(AlignmentMethod::Rps)["recipe"];
#endif
    return QString();
}
QJsonValue RowTemplate(const QString& action, const QString& key, const QJsonArray& values)
{
    if (key == "scales") return QJsonObject{{"voxelSize", 0.}, {"maxPairDistance", 1.}, {"iterationLimit", "40"}};
    if (!values.isEmpty()) return values.first();
    if(key=="segments")return QJsonObject{{"segmentFrom",QJsonValue()},{"segmentTo",QJsonValue()},
        {"palette","Constant"},{"lowColor",QJsonArray{.7,.7,.7}},{"highColor",QJsonArray{.7,.7,.7}}};
    if (key == "seeds") return action == "Split" ? QJsonValue(QJsonObject{{"imageIndex", QJsonArray{0,0,0}}, {"target", "1"}}) : QJsonValue(QJsonArray{0,0,0});
    if (key == "sourcePointsMM") return QJsonArray{0,0,0};
    if (key == "barriers") return QJsonObject{{"imageIndex", QJsonArray{0,0,0}}, {"axis", 0}};
    if (key == "indexBoxes") return QJsonArray{0,0,0,0,0,0};
    if (key == "colorNodes") return QJsonArray{0,1,1,1};
    if (key == "opacityNodes") return QJsonArray{0,1};
    if (key == "initialPoses") return QJsonArray{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (key == "parts" || key == "protectedParts" || key == "overwriteParts") return QString("selected");
    if (key == "vertexIds" || key == "triangles" || key == "sourceTriangles" || key == "referenceTriangles") return QString("0");
#if defined(MANUAL_ALIGNMENT)
    if (key == "geometries" || key == "constraints") return GetReferenceTemplate(AlignmentMethod::Rps)["recipe"].toObject()[key].toArray().first();
    if (key == "fitPairs") return GetReferenceTemplate(AlignmentMethod::ConstrainedBestFit)["recipe"].toObject()[key].toArray().first();
#endif
    return QString();
}
QString Component(const QString& key, int index, int size)
{
    if (size == 9 || size == 16) { const int columns = size == 9 ? 3 : 4; return QString("%1 行 %2 列").arg(index/columns+1).arg(index%columns+1); }
    if (key == "windowLevel") return index == 0 ? "窗宽" : "窗位";
    if (key == "colorNodes") return QStringList{"灰度", "红", "绿", "蓝"}.value(index);
    if (key == "opacityNodes") return index == 0 ? "灰度" : "透明度";
    if (key == "colorRGBA") return QStringList{"红", "绿", "蓝", "透明度"}.value(index);
    if (key == "constantColor" || key == "lowColor" || key == "highColor"
        || key == "belowColor" || key == "aboveColor") return QStringList{"红（0～1）", "绿（0～1）", "蓝（0～1）"}.value(index);
    if (key == "centerIndex") return QString("截面轴 %1").arg(index+1);
    if (size == 6) return QStringList{"X 最小", "X 最大", "Y 最小", "Y 最大", "Z 最小", "Z 最大"}.value(index);
    if (size == 3) return QStringList{"X", "Y", "Z"}.value(index);
    if (size == 2) return index == 0 ? "最小值" : "最大值";
    return QString("值 %1").arg(index+1);
}
bool IsPath(const QString& key)
{
    return key == "filePath" || key == "plyPath" || key == "outputPath" || key == "outputDir" || key == "archivePath";
}
}
ParameterEditor::ParameterEditor(QString module, QString action, QString key, QJsonValue value, QJsonValue schema,
    QWidget* parent, bool listItem, QString title, bool showTitle, bool deferFields)
    : QWidget(parent), m_module(std::move(module)), m_action(std::move(action)), m_key(std::move(key)),
      m_title(title.isEmpty() ? GetParameterText(m_key) : std::move(title)), m_schema(std::move(schema)),
      m_listItem(listItem), m_optional(m_schema.isNull() && !m_key.isEmpty())
{
    if(m_module=="Roi") {
        if(m_key=="name")m_title="区域名称";
        else if(m_key=="shape")m_title="区域形状";
        else if(m_key=="axis")m_title="柱轴（源模型坐标）";
        else if(m_key=="centerMM")m_title="区域中心（源模型坐标）";
    }
    m_boolean = m_schema.isBool() || (m_schema.isNull() && Shape(m_key).isBool());
    setObjectName(m_key); setAccessibleName(ShortTitle(m_key,m_title)); setAccessibleDescription(m_title+"\n"+GetParameterHelp(m_key));
    auto effective = value;
    if (effective.isNull()) effective = m_schema.isNull() ? Shape(m_key) : m_schema;
    const auto effectiveArray = effective.toArray();
    m_isPropertyRow = !m_key.isEmpty() && !effective.isObject()
        && (!effective.isArray() || (effectiveArray.size() != 9 && effectiveArray.size() != 16
            && (m_listItem || !(IsList(m_key) || effectiveArray.isEmpty()
                || (!effectiveArray.first().isDouble() && !effectiveArray.first().isString())))));
    setProperty("propertyRow", m_isPropertyRow);
    m_isVector = m_isPropertyRow && effective.isArray();
    setMinimumWidth(0); setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_layout = new QGridLayout(this);
    m_layout->setContentsMargins(0,0,0,0); m_layout->setSpacing(6);m_layout->setColumnStretch(2,1);
    const bool duplicateHeading=m_module=="Artifact" && (m_key=="ring" || m_key=="diffusion") && effective.isObject() && !m_optional;
    if (!m_key.isEmpty() && showTitle && !duplicateHeading) {
        auto* label = new QLabel(ShortTitle(m_key,m_title), this); m_titleLabel=label;
        label->setObjectName("parameterTitle"); label->setAccessibleName(label->text());
        label->setAccessibleDescription(accessibleDescription());label->setToolTip(accessibleDescription());
        label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
        const auto unit=ValueUnit(m_key,m_title);
        if(!unit.isEmpty()) {m_unitLabel=new QLabel(unit,this);m_unitLabel->setObjectName("parameterUnit");}
        if (m_isPropertyRow) {
            m_layout->addWidget(label,0,0);
        } else {
            auto* heading = new QHBoxLayout; heading->addWidget(label, 1);
            if(m_unitLabel)heading->addWidget(m_unitLabel);
            if (m_optional && !m_boolean) {
                m_specified = new QCheckBox("指定", this); m_specified->setObjectName("specified");
                m_specified->setAccessibleName("提供参数：" + m_title);
                m_specified->setToolTip("取消仅表示不提供此参数，由业务使用默认值或保留原值；不会清零或设置 false。"); heading->addWidget(m_specified);
            }
            m_layout->addLayout(heading,0,0,1,4);m_bodyRow=1;
        }
        if (m_isPropertyRow && m_optional && !m_boolean) {
            m_specified = new QCheckBox(this); m_specified->setObjectName("specified");
            m_specified->setAccessibleName("提供参数：" + m_title);
            m_specified->setToolTip("勾选以指定此参数；取消由业务使用默认值或保留原值，不会清零或设置 false。"); m_layout->addWidget(m_specified,0,1);
        }
        if (m_specified) {
            connect(m_specified, &QCheckBox::toggled, this, [this](bool enabled) {
                if (m_body) { m_body->setEnabled(enabled); if (m_type == QJsonValue::Object) m_body->setVisible(enabled); }
                SetInheritedGeometry();SetPropertyLayout();NotifyEdited();
            });
        }
    }
    m_fieldsDeferred=deferFields&&m_key.isEmpty()&&value.isObject();SetValue(value);
}
void ParameterEditor::BuildFields()
{
    if(!m_fieldsDeferred)return;
    const auto value=m_deferredValue;m_fieldsDeferred=false;m_deferredValue={};SetValue(value);
}
void ParameterEditor::showEvent(QShowEvent* event)
{
    BuildFields();QWidget::showEvent(event);
}
void ParameterEditor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);SetPropertyLayout();
}
void ParameterEditor::SetPropertyLayout()
{
    if(!m_isPropertyRow) {
        int nameWidth=0;
        for(const auto& field:m_fields)if(!field.second->isHidden() && field.second->m_isPropertyRow && field.second->m_titleLabel)
            nameWidth=std::max(nameWidth,field.second->m_titleLabel->fontMetrics().horizontalAdvance(field.second->m_titleLabel->text()));
        const int limit=fontMetrics().horizontalAdvance(QString(9,QChar(0x540d)));
        for(const auto& field:m_fields) {
            field.second->m_nameColumnWidth=std::min(nameWidth,limit);field.second->SetPropertyLayout();
        }
        return;
    }
    if(!m_isPropertyRow || !m_titleLabel || !m_body)return;
    const int preferred=m_nameColumnWidth>0 ? m_nameColumnWidth : m_titleLabel->sizeHint().width();
    const int nameWidth=m_listItem ? m_titleLabel->sizeHint().width() : std::min(preferred,std::max(1,width()*2/5));
    const int unitWidth=m_unitLabel?m_unitLabel->sizeHint().width()+m_layout->spacing():0;
    const int optionalWidth=m_specified?m_specified->sizeHint().width()+m_layout->spacing():0;
    // 名称、枚举或数组空间不足时值整行下移，保持完整文字与原始量值。
    const int componentWidth=fontMetrics().horizontalAdvance("-8888.88")+fontMetrics().horizontalAdvance("X")+24;
    const auto* choice=qobject_cast<QComboBox*>(m_input);
    const int regionButtonWidth=m_createRegion&&!m_createRegion->isHidden()?m_createRegion->sizeHint().width()+m_body->layout()->spacing():0;
    const int choiceWidth=choice?choice->fontMetrics().horizontalAdvance(choice->currentText())+40+regionButtonWidth:0;
    const int valueWidth=width()-nameWidth-unitWidth-optionalWidth-2*m_layout->spacing();
    const bool inherited=m_inheritedGeometry&&!m_inheritedGeometry->isHidden();
    const bool below=!inherited&&(m_titleLabel->fontMetrics().horizontalAdvance(m_titleLabel->text())>nameWidth
        || (choice && valueWidth<choiceWidth) || (m_isVector && valueWidth<GetCount()*componentWidth));
    if(m_appliedNameWidth==nameWidth && m_vectorBelow==below)return;
    m_appliedNameWidth=nameWidth;m_vectorBelow=below;setProperty("vectorBelow",below);
    m_layout->removeWidget(m_titleLabel);m_layout->removeWidget(m_body);
    if(m_specified)m_layout->removeWidget(m_specified);
    if(m_unitLabel)m_layout->removeWidget(m_unitLabel);
    m_layout->setColumnMinimumWidth(0,below?0:nameWidth);
    m_titleLabel->setMinimumWidth(below?0:nameWidth);m_titleLabel->setMaximumWidth(below?QWIDGETSIZE_MAX:nameWidth);
    m_layout->addWidget(m_titleLabel,0,0);
    if(m_specified)m_layout->addWidget(m_specified,0,1);
    if(below) {
        if(m_unitLabel)m_layout->addWidget(m_unitLabel,0,2,1,2,Qt::AlignRight);
        m_layout->addWidget(m_body,1,0,1,4);
    } else {
        m_layout->addWidget(m_body,0,m_specified?2:1,1,m_specified?1:2);
        if(m_unitLabel)m_layout->addWidget(m_unitLabel,0,3);
    }
}
bool ParameterEditor::GetFieldApplicable(const QString& key) const
{
    if (m_applicableFields.contains(key) && !m_applicableFields[key].toBool()) return false;
    if(m_key.isEmpty() && m_module=="Roi" && (m_action=="Begin" || m_action=="SetDraft")) {
        const auto shape=m_fields.find("shape");
        const auto selected=shape==m_fields.end()?QString("Box"):shape->second->GetValue().toString();
        const bool affine=selected=="Box" || selected=="Ellipsoid";
        if(key=="boxToSource" || key=="sizeMM" || key=="direction3D")return affine;
        if(key=="radius")return selected=="Sphere" || selected=="Cylinder" || selected=="Brush";
        if(key=="axis")return selected=="Cylinder" || selected=="HalfSpace";
        if(key=="height")return selected=="Cylinder";
        if(key=="contours")return selected=="Contour" || selected=="ProjectedContour";
        if(key=="planeToSource")return selected=="Contour" || selected=="Brush" || selected=="EllipticCylinder";
        if(key=="depthMM")return selected=="Contour" || selected=="ProjectedContour" || selected=="Brush" || selected=="EllipticCylinder";
        if(key=="sourceToView" || key=="viewToClip")return selected=="ProjectedContour";
        if(key=="pathMM" || key=="isSliceBrush")return selected=="Brush";
        if(key=="mask" || key=="labels")return selected=="MaskReference";
    }
    if(m_key.isEmpty() && m_module=="Surface" && m_action=="GlobalAutomatic" && (key=="initialIsoValue" || key=="materialRange"))return m_isManualSource;
    if(m_key.isEmpty() && m_module=="Gap") {
        if(m_action=="Evaluate" && QStringList{"isoMode","dataRangeRatio","absoluteIsoValue","backgroundMean","materialMean","detectionRoi"}.contains(key))return false;
        if(m_action=="Start" && key=="parentResult")return false;
        const auto mode=m_fields.find("isoMode");
        if(mode!=m_fields.end() && key=="dataRangeRatio")return mode->second->GetValue()=="DataRangeRatio";
        if(mode!=m_fields.end() && key=="absoluteIsoValue")return mode->second->GetValue()=="AbsoluteValue";
    }
    if(m_key.isEmpty() && m_module=="Roi" && m_schema.toObject().contains("outputMode")) {
        const auto action=m_action;
        if(key=="roi" || key=="outputMode")return true;
        if(key=="operands")return QStringList{"Union","Intersection","Difference","Merge"}.contains(action);
        if(key=="radius" || key=="radiusUnit")return QStringList{"Dilate","Erode","Open","Close"}.contains(action);
        if(key=="connectivity")return QStringList{"RemoveSmall","KeepLargest","KeepSelected","Split","FillHoles","Grow"}.contains(action);
        if(key=="minimumVolumeMM3")return action=="RemoveSmall";
        if(key=="maximumHoleVolumeMM3")return action=="FillHoles";
        if(key=="componentLabels")return action=="KeepSelected";
        if(key=="keepInput")return action=="Split";
        if(key=="grayRange")return action=="Threshold" || action=="Grow";
        if(key=="seedsMM")return action=="Grow";
        if(key=="domainRoi")return action=="Invert";
        if(key=="sourceToTarget" || key=="rasterize" || key=="targetSource")return action=="Transform";
        if(key=="mesh" || key=="meshToSource")return action=="SurfaceInterior" || action=="SurfaceLayer";
        if(key=="inwardMM" || key=="outwardMM" || key=="hasOrientation")return action=="SurfaceLayer";
    }
    if (m_key.isEmpty() && m_module == "Surface" && key == "seedModelPoint") {
        const auto selection = m_fields.find("componentSelection");
        return selection != m_fields.end() && selection->second->GetValue() == "Seeded";
    }
    if(m_key.isEmpty() && ((m_action=="SetDisplay"&&(m_module=="Wall"||m_module=="Gap"))
        || (m_module=="Alignment"&&m_action=="SetDeviationDisplay"))) {
        const auto field=[&](const QString& name){const auto i=m_fields.find(name);return i==m_fields.end()?QJsonValue():i->second->GetValue();};
        const bool tolerance=m_module=="Alignment"?field("useToleranceColors").toBool():field("mode").toString()=="Tolerance";
        const auto segments=m_fields.find("segments");const bool segmented=segments!=m_fields.end()&&segments->second->GetCount()>0;
        if(key=="belowColor"||key=="aboveColor")return (m_module=="Alignment"&&tolerance)||(!tolerance&&segmented);
        if(m_module=="Alignment") {
            if(key=="range")return field("rangeMode").toString()=="Manual";
            if(key=="palette"||key=="constantColor"||key=="lowColor"||key=="highColor"||key=="isReversed"||key=="segments") {
                if(tolerance)return false;
                if(segmented&&key!="segments"&&key!="isReversed")return false;
                if(key=="constantColor")return field("palette").toString()=="Constant";
                if(key=="lowColor"||key=="highColor")return field("palette").toString()=="Gradient";
            }
        }
    }
    if ((m_key.isEmpty() || m_key=="segments") && (m_module == "Wall" || m_module == "Gap") && m_action == "SetDisplay") {
        const auto field = [&](const QString& name) {
            const auto found = m_fields.find(name);
            return found == m_fields.end() ? QString() : found->second->GetValue().toString();
        };
        if(m_fields.find("segmentFrom")!=m_fields.end()) {
            if(key=="lowColor")return field("palette")=="Constant" || field("palette")=="Gradient";
            if(key=="highColor")return field("palette")=="Gradient";
            return true;
        }
        if(key.startsWith("tolerance") && key.endsWith("Color"))return m_module=="Wall" && field("mode")=="Tolerance";
        if (m_module == "Wall" && field("mode") == "Tolerance"
            && (key == "palette" || key == "constantColor" || key == "lowColor" || key == "highColor"
                || key == "belowColor" || key == "aboveColor" || key=="segments")) return false;
        if(key=="opacityRange")return field("displayStyle")=="Inclined" || field("displayStyle")=="InverseInclined";
        if (key == "range") return field("rangeMode") == "Manual" || field("rangeMode") == "SelectedInterval";
        const auto segments=m_fields.find("segments");
        if(segments!=m_fields.end() && segments->second->GetCount()>0
            && (key=="palette" || key=="constantColor" || key=="lowColor" || key=="highColor"))return false;
        if (key == "constantColor") return field("palette") == "Constant";
        if (key == "lowColor" || key == "highColor") return field("palette") == "Gradient";
    }
    return true;
}
void ParameterEditor::SetFieldApplicability(QJsonObject fields)
{
    if (m_applicableFields == fields) return;
    m_applicableFields = std::move(fields); SetFieldVisibility();
}
void ParameterEditor::SetFieldVisibility()
{
    if (!m_key.isEmpty() && m_key!="segments") return;
    const auto bound = m_key.isEmpty() ? GetBoundParameters(m_module, m_action) : QStringList{};
    for (const auto& field : m_fields) field.second->setVisible(!bound.contains(field.first) && GetFieldApplicable(field.first));
    for (const auto& section : m_groupSections) {
        auto* page = m_groupPages.at(section.first);
        bool populated = std::any_of(m_fields.begin(), m_fields.end(), [&](const auto& field) {
            return field.second->parentWidget() == page && !field.second->isHidden();
        });
        for(auto* nested:page->findChildren<QWidget*>("parameterSection"))populated=populated || !nested->isHidden();
        section.second->setVisible(populated);
    }
    SetPropertyLayout();
}
void ParameterEditor::NotifyEdited() { SetFieldVisibility(); if (onEdited) onEdited(); }
void ParameterEditor::UpdateBooleanState()
{
    if (!m_booleanState) return;
    auto* input = qobject_cast<QCheckBox*>(m_input);
    if (m_stateBound) {
        if (!m_booleanEdited) {
            const QSignalBlocker blocker(input);
            input->setCheckState(m_appliedBoolean.isNull() ? Qt::PartiallyChecked : m_appliedBoolean.toBool() ? Qt::Checked : Qt::Unchecked);
        }
        input->setEnabled(m_booleanUnavailable.isEmpty());
        const QString applied = !m_booleanUnavailable.isEmpty() ? m_booleanUnavailable : m_appliedBoolean.isNull() ? "各对象状态不同" : m_appliedBoolean.toBool() ? "当前开启" : "当前关闭";
        m_booleanState->setText(m_booleanEdited ? QString("待应用：%1 · %2").arg(input->isChecked() ? "开启" : "关闭", applied) : applied);
        m_unsetBoolean->setText("撤回修改"); m_unsetBoolean->setEnabled(m_booleanEdited);
    } else {
        m_booleanState->setText(!m_booleanSpecified ? "不修改" : input->isChecked() ? "开启" : "关闭");
        m_unsetBoolean->setText("不修改"); m_unsetBoolean->setEnabled(m_booleanSpecified);
    }
}
void ParameterEditor::CreateBooleanStateControls()
{
    if (m_booleanState) return;
    auto* layout = static_cast<QHBoxLayout*>(m_body->layout());
    m_booleanState = new QLabel(m_body); m_booleanState->setObjectName("booleanState"); m_booleanState->setWordWrap(true);
    m_unsetBoolean = new QPushButton("不修改", m_body); m_unsetBoolean->setObjectName("unsetBoolean");
    layout->addWidget(m_booleanState); layout->addStretch(); layout->addWidget(m_unsetBoolean);
    connect(m_unsetBoolean, &QPushButton::clicked, this, [this] {
        auto* input = qobject_cast<QCheckBox*>(m_input); const QSignalBlocker blocker(input);
        if (!m_stateBound) input->setCheckState(Qt::PartiallyChecked);
        m_booleanEdited = false; m_booleanSpecified = false; UpdateBooleanState(); NotifyEdited();
    });
}
void ParameterEditor::SetAppliedBoolean(QJsonValue value, const QString& context, const QString& unavailableReason)
{
    if (!m_boolean || !m_input) return;
    if (!value.isBool()) value = QJsonValue();
    if (!m_stateBound || m_stateContext != context || !unavailableReason.isEmpty()) m_booleanEdited = false;
    m_booleanUnavailable = unavailableReason;
    m_stateBound = true; m_stateContext = context; m_appliedBoolean = value;
    if (m_booleanEdited && value.isBool() && qobject_cast<QCheckBox*>(m_input)->isChecked() == value.toBool()) m_booleanEdited = false;
    CreateBooleanStateControls(); UpdateBooleanState();
}
void ParameterEditor::SetDefaultValue(const QJsonValue& value)
{
    if (!m_specified || m_specified->isChecked()) return;
    if (m_type == QJsonValue::Double && value.isDouble()) {
        static_cast<NumberInput*>(m_input)->SetNumber(value.toDouble());
    } else if(m_type==QJsonValue::String && value.isString()) {
        if(auto* choice=qobject_cast<QComboBox*>(m_input)){const QSignalBlocker blocker(choice);const auto index=choice->findData(value.toString());if(index>=0)choice->setCurrentIndex(index);}
    } else if (m_type == QJsonValue::Array && value.isArray() && value.toArray().size() == GetCount()) {
        const auto values = value.toArray();
        for (int index = 0; index < values.size(); ++index) {
            auto* field=m_rows[index].second;
            if(field->m_type==QJsonValue::Double && values[index].isDouble())
                static_cast<NumberInput*>(field->m_input)->SetNumber(values[index].toDouble());
        }
    }
}
void ParameterEditor::SetReferenceChoices(const ParameterChoices& choices)
{
    if(m_type==QJsonValue::Array){for(const auto& row:m_rows)row.second->SetReferenceChoices(choices);return;}
    auto* input = qobject_cast<QComboBox*>(m_input);
    if (!input || m_type != QJsonValue::String) return;
    auto next = GetParameterChoices(m_module, m_key);
    for (const auto& choice : choices)
        if (std::none_of(next.cbegin(), next.cend(), [&](const auto& entry) { return entry.first == choice.first; })) next.append(choice);
    const auto current = input->currentData().toString();
    if (std::none_of(next.cbegin(), next.cend(), [&](const auto& entry) { return entry.first == current; }))
        next.append({current, "不可用或自定义引用：" + current});
    bool same = input->count() == next.size();
    for (int index = 0; same && index < next.size(); ++index)
        same = input->itemData(index).toString() == next[index].first && input->itemText(index) == next[index].second;
    if (same) return;
    const QSignalBlocker blocker(input);
    input->clear();
    for (const auto& choice : next) input->addItem(choice.second, choice.first);
    input->setCurrentIndex(input->findData(current));
}
void ParameterEditor::SetRegionCreator(std::function<void()> onCreate)
{
    m_onCreateRegion = std::move(onCreate);
    if (!m_createRegion && qobject_cast<QComboBox*>(m_input)) {
        m_createRegion = new QPushButton("新建", m_body);m_createRegion->setAccessibleName("新建区域");m_createRegion->setToolTip("新建区域并用于当前参数");
        m_createRegion->setObjectName("createRegion");
        static_cast<QHBoxLayout*>(m_body->layout())->addWidget(m_createRegion);
        connect(m_createRegion, &QPushButton::clicked, this, [this] {
            if (m_onCreateRegion) m_onCreateRegion();
        });
        SetPropertyLayout();
    }
}
void ParameterEditor::SetAllowedChoices(const ParameterChoices& choices)
{
    auto* input=qobject_cast<QComboBox*>(m_input);if(!input || m_type!=QJsonValue::String)return;
    const auto current=input->currentData().toString();auto next=choices;
    if(std::none_of(next.begin(),next.end(),[&](const auto& entry){return entry.first==current;}))next.append({current,"当前组合不支持："+current});
    bool same=input->count()==next.size();for(int i=0;same && i<next.size();++i)same=input->itemData(i)==next[i].first && input->itemText(i)==next[i].second;
    if(same)return;const QSignalBlocker blocker(input);input->clear();for(const auto& entry:next)input->addItem(entry.second,entry.first);input->setCurrentIndex(input->findData(current));
}
void ParameterEditor::SetValue(const QJsonValue& value)
{
    if(m_fieldsDeferred){m_deferredValue=value;return;}
    if (m_boolean && !value.isNull() && !value.isBool()) throw std::invalid_argument((m_title + "：请输入布尔值").toStdString());
    auto effective = value;
    if (effective.isNull()) effective = m_schema.isNull() ? Shape(m_key) : m_schema;
    if (m_body) { m_layout->removeWidget(m_body); delete m_body; }
    m_inheritedGeometry=nullptr;
    m_fields.clear(); m_rows.clear(); m_input = nullptr; m_rowLayout = nullptr; m_createRegion = nullptr;m_parameterSource=nullptr;
    m_groupPages.clear(); m_groupSections.clear();
    m_booleanState = nullptr; m_unsetBoolean = nullptr; m_booleanSpecified = !value.isNull();
    if (m_stateBound) m_booleanEdited = value.isBool();
    m_body = new QWidget(this);
    m_body->setProperty("parameterValue",true);m_appliedNameWidth=-1;
    if (m_isPropertyRow) m_layout->addWidget(m_body,0,m_specified ? 2 : 1,1,m_specified ? 1 : 2);
    else m_layout->addWidget(m_body,m_bodyRow,0,1,4);
    m_type = effective.type();
    BuildValue(effective);
    UpdateBooleanState();
    SetFieldVisibility();
    if (m_specified) {
        // 程序回填开关不能发布编辑事件，否则整组补丁未完成时会提交旧几何。
        const QSignalBlocker blocker(m_specified);
        m_specified->setChecked(!value.isNull()); m_body->setEnabled(!value.isNull());
        if (m_type == QJsonValue::Object) m_body->setVisible(!value.isNull());
    }
    SetInheritedGeometry();SetPropertyLayout();
}
void ParameterEditor::SetInheritedGeometry()
{
    if(m_module!="Roi"||!m_specified||!m_body||!QStringList{"centerMM","sizeMM","radius","height","axis","direction3D","boxToSource","depthMM"}.contains(m_key))return;
    const bool provided=m_specified->isChecked();
    if(!m_inheritedGeometry){
        m_inheritedGeometry=new QLabel("按当前数据生成",m_body);m_inheritedGeometry->setMinimumHeight(32);m_inheritedGeometry->setObjectName("inheritedGeometryHint");m_inheritedGeometry->setToolTip("未指定几何；开始或编辑时由当前数据生成，不表示已采用零坐标或单位尺寸。");
        if(auto* grid=qobject_cast<QGridLayout*>(m_body->layout()))grid->addWidget(m_inheritedGeometry,0,0,1,std::max(1,grid->columnCount()));
        else if(auto* row=qobject_cast<QHBoxLayout*>(m_body->layout()))row->addWidget(m_inheritedGeometry);
    }
    for(auto* child:m_body->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(child!=m_inheritedGeometry)child->setVisible(provided);
    m_inheritedGeometry->setVisible(!provided);m_body->setVisible(true);
}
void ParameterEditor::BuildValue(const QJsonValue& value)
{
    if (value.isObject()) {
        const auto object = value.toObject(); auto keys = object.keys();
        const QStringList first{"filePath","datasetId","source","mesh","labels","reference","target","roi","operands","analysisRoi","evaluationRoi","processingRoi","viewScope","viewId","mode","iso","windowLevel","transfer","opacity","quality","shape","selectionMode","centerMM","sizeMM","radius","height","grayRange","threshold","minPartVoxels","dimensions","spacingLPS","originLPS","directionLPS","worldAxis","worldCenter","angleDeg","ring","diffusion","componentSelection","initialIsoValue","materialRange","seedModelPoint","seed","seeds","sourcePointsMM","world"};
        for (auto i = first.crbegin(); i != first.crend(); ++i) if (keys.removeOne(*i)) keys.prepend(*i);
        if(m_module=="Roi" && keys.removeOne("shape"))keys.prepend("shape");
        const bool seedRow = m_listItem && object.contains("imageIndex") && object.size() == 2;
        QSet<QString> groups;
        if (m_key.isEmpty() || (m_module=="Artifact" && (m_key=="ring" || m_key=="diffusion"))) for (const auto& key : keys)
            if (!GetBoundParameters(m_module, m_action).contains(key)) groups.insert(GetParameterGroup(m_module, key));
        const bool grouped = groups.size() > 1;
        std::map<QString, QGridLayout*> grids;
        std::map<QString, int> rows;
        QVBoxLayout* sections = nullptr;
        const auto advanced=[](const QString& name){return name=="高级与诊断"||name=="高级几何"||name=="精确参数"||name=="高级设置"||name=="专家设置"||name=="更多信息";};
        if (grouped) {
            m_body->setObjectName("parameterGroups");
            sections=new QVBoxLayout(m_body);sections->setContentsMargins(0,0,0,0);sections->setSpacing(8);sections->setAlignment(Qt::AlignTop);
        } else {
            auto* grid = new QGridLayout(m_body); grid->setContentsMargins(0,0,0,0); grid->setHorizontalSpacing(12); grid->setVerticalSpacing(4);
            grids[QString()] = grid;
        }
        if(m_module=="Surface" && m_key.isEmpty() && m_action=="GlobalAutomatic") {
            m_isManualSource=!object["initialIsoValue"].isNull() || !object["materialRange"].isNull();
            auto* sourceRow=new QWidget(m_body);auto* row=new QHBoxLayout(sourceRow);row->setContentsMargins(0,0,0,0);row->addWidget(new QLabel("参数来源",sourceRow));
            m_parameterSource=new QComboBox(sourceRow);m_parameterSource->setObjectName("surfaceParameterSource");
            m_parameterSource->addItem("自动估计",false);m_parameterSource->addItem("手动输入",true);m_parameterSource->setAccessibleName("表面参数来源");
            m_parameterSource->setItemData(0,"自动估计背景、材料与等值阈值",Qt::ToolTipRole);
            m_parameterSource->setCurrentIndex(m_isManualSource?1:0);row->addWidget(m_parameterSource,1);
            if(sections)sections->addWidget(sourceRow);else grids.at(QString())->addWidget(sourceRow,rows[QString()]++,0);
            // 参数来源只控制提交值，切回自动不会销毁手动草稿。
            connect(m_parameterSource,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){m_isManualSource=index==1;NotifyEdited();});
        }
        for (const auto& key : keys) {
            const auto group = grouped ? GetParameterGroup(m_module, key) : QString();
            QWidget* page = m_body;
            if (grouped) {
                if (!m_groupPages.count(group)) {
                    auto* section = new QWidget(m_body); section->setObjectName("parameterSection");
                    section->setProperty("sectionName", group);
                    auto* sectionLayout = new QVBoxLayout(section); sectionLayout->setContentsMargins(0,0,0,0); sectionLayout->setSpacing(4);
                    const bool expanded=!advanced(group);
                    const bool duplicateGroup=m_module=="Artifact" && (m_key=="ring" || m_key=="diffusion") && group=="算法参数";
                    page = new QWidget(section); page->setObjectName("sectionFields"); page->setVisible(expanded);
                    if(!duplicateGroup) {
                        auto* toggle = new QToolButton(section); toggle->setObjectName("sectionToggle"); toggle->setText(group);
                        if(advanced(group))toggle->setText("高级");
                        if(m_module=="Artifact" && group=="高级设置" && (m_key=="ring" || m_key=="diffusion"))
                            toggle->setText(m_key=="ring"?"环形高级设置":"扩散高级设置");
                        toggle->setProperty("parameterSection", true); toggle->setCheckable(true);
                        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                        toggle->setChecked(expanded); toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                        sectionLayout->addWidget(toggle);
                        // 折叠仅改变可见性，不读取输入、不重建字段，也不通知业务编辑。
                        connect(toggle, &QToolButton::toggled, page, [page, toggle](bool isExpanded) {
                            page->setVisible(isExpanded); toggle->setArrowType(isExpanded ? Qt::DownArrow : Qt::RightArrow);
                        });
                    }
                    sectionLayout->addWidget(page);sections->addWidget(section);
                    m_groupPages[group] = page; m_groupSections[group] = section;
                    auto* grid = new QGridLayout(page); grid->setContentsMargins(4,2,4,4); grid->setVerticalSpacing(4);
                    grids[group] = grid;
                } else page = m_groupPages.at(group);
            }
            auto* grid = grids.at(group); auto& row = rows[group];
            const auto v = object[key]; const auto prototype = m_schema.isObject() && m_schema.toObject().contains(key) ? m_schema.toObject()[key] : v;
            auto* field = new ParameterEditor(m_module, m_action, key, v, prototype, page, false,
                seedRow && key == "target" ? "新标签编号" : QString());
            field->onEdited = [this] { NotifyEdited(); }; m_fields[key] = field;
            if (m_key.isEmpty() && GetBoundParameters(m_module, m_action).contains(key)) { field->hide(); continue; }
            grid->addWidget(field, row++, 0);
        }
        for (const auto& entry : grids) {
            auto* grid = entry.second;
            grid->setColumnStretch(0, 1);
        }
        if (sections) {
            for (const auto& name : {"测量输入","输入与输出","输入与结果","目标视图","呈现模式","作用范围","评价范围","区域工具","区域信息","区域几何","尺寸与间距","空间与矩阵","旋转轴","旋转中心","旋转角度","初始阈值","环状校正","扩散校正","算法参数","阈值与灰度","孔隙筛选","零件属性","输出命名","显示质量","显示设置","显示辅助","导出与诊断","更多信息","高级几何","高级设置","高级与诊断","精确参数","专家设置"}) {
                const auto section = m_groupSections.find(name);
                if(section!=m_groupSections.end()){sections->removeWidget(section->second);sections->addWidget(section->second);}
            }
            if(m_module=="Surface" && m_groupSections.count("高级设置") && m_groupSections.count("专家设置")) {
                auto* expert=m_groupSections.at("专家设置");sections->removeWidget(expert);
                auto* advanced=m_groupPages.at("高级设置");auto* grid=static_cast<QGridLayout*>(advanced->layout());grid->addWidget(expert,grid->rowCount(),0);
            }
            if(m_module=="Roi" && m_groupSections.count("精确参数") && m_groupSections.count("更多信息")) {
                auto* details=m_groupSections.at("更多信息");sections->removeWidget(details);
                auto* advanced=m_groupPages.at("精确参数");auto* grid=static_cast<QGridLayout*>(advanced->layout());grid->addWidget(details,grid->rowCount(),0);
                m_groupSections.at("精确参数")->findChild<QToolButton*>("sectionToggle")->setText("高级参数");
            }
        }
        return;
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        const bool indexList = m_key == "vertexIds" || m_key == "sourceTriangles" || m_key == "referenceTriangles" || m_key == "triangleIds";
        if (indexList && array.size()>128) {
            auto* layout = new QVBoxLayout(m_body); layout->setContentsMargins(0,0,0,0);
            auto* count = new QLabel(QString("批量索引：%1 项；完整数组保留，可整体编辑。").arg(array.size()),m_body);
            auto* input = new QPlainTextEdit(m_body); input->setObjectName("bulkIndices");
            input->setLineWrapMode(QPlainTextEdit::NoWrap); input->setMaximumHeight(120);
            input->setPlainText(QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
            layout->addWidget(count); layout->addWidget(input); m_input=input;
            connect(input,&QPlainTextEdit::textChanged,this,[this]{NotifyEdited();});
            return;
        }
        m_isList = !m_listItem && (IsList(m_key) || array.isEmpty() || (!array.first().isDouble() && !array.first().isString()));
        if (m_isList) {
            auto* layout = new QVBoxLayout(m_body); layout->setContentsMargins(0,0,0,0); layout->setSpacing(6);
            auto* rows = new QWidget(m_body); m_rowLayout = new QVBoxLayout(rows); m_rowLayout->setContentsMargins(0,0,0,0); m_rowLayout->setSpacing(8); layout->addWidget(rows);
            const auto prototype = m_schema.isArray() ? m_schema.toArray() : array;
            m_itemTemplate = RowTemplate(m_action, m_key, prototype);
            for (const auto item : array) AddRow(item);
            auto* add = new QPushButton("添加一行", m_body); add->setObjectName("addRow"); layout->addWidget(add, 0, Qt::AlignLeft);
            connect(add, &QPushButton::clicked, this, [this] { AddRow(m_itemTemplate); NotifyEdited(); });
        } else {
            auto* grid = new QGridLayout(m_body); grid->setContentsMargins(0,0,0,0); grid->setSpacing(8);
            const bool matrix = array.size() == 9 || array.size() == 16;
            const int columns = array.size() == 16 || array.size() == 4 ? 4 : array.size() == 2 || array.size() == 6 ? 2 : 3;
            const int offset = matrix ? 1 : 0;
            if (matrix) {
                grid->setObjectName("matrixGrid"); m_body->setObjectName("matrix");
                m_body->setProperty("matrixDimension", columns);
                if (auto* title = findChild<QLabel*>("parameterTitle", Qt::FindDirectChildrenOnly))
                    {title->setText(ShortTitle(m_key,m_title)+QString("（%1 × %1）").arg(columns));title->setAccessibleName(title->text());}
                for (const bool left : {true, false}) {
                    auto* bracket = new QFrame(m_body); bracket->setFixedWidth(6);
                    bracket->setStyleSheet(QString("border-top: 2px solid #718294; border-bottom: 2px solid #718294; border-%1: 2px solid #718294;").arg(left ? "left" : "right"));
                    grid->addWidget(bracket, 0, left ? 0 : columns+1, columns, 1);
                }
            }
            for (int i = 0; i < array.size(); ++i) {
                const auto component = Component(m_key, i, array.size());
                auto* field = new ParameterEditor(m_module, m_action, QString::number(i), array[i], array[i], m_body, true, component, !matrix);
                if (matrix) {
                    field->setToolTip(component);
                    if (auto* input = field->findChild<QLineEdit*>("value")) input->setAlignment(Qt::AlignCenter);
                }
                field->onEdited = [this] { NotifyEdited(); }; grid->addWidget(field, i/columns, i%columns+offset);
                m_rows.emplace_back(field, field); grid->setColumnStretch(i%columns+offset, 1);
            }
        }
        return;
    }
    auto* layout = new QHBoxLayout(m_body); layout->setContentsMargins(0,0,0,0);
    auto choices = GetParameterChoices(m_module, m_key);
    if (m_module == "Alignment" && m_key == "kind") {
        if (m_action == "InspectGeometry" || m_action=="InspectPair") choices = {{"Point","点"},{"Line","直线"},{"Plane","平面"},{"Circle", "圆"}, {"Sphere", "球"}, {"Cylinder", "圆柱"}};
        else if (value == "Hard" || value == "Soft" || value == "Check") choices = {{"Hard", "硬约束"}, {"Soft", "软约束"}, {"Check", "校验约束"}};
        else choices = {{"Point", "点"}, {"Line", "直线"}, {"Plane", "平面"}, {"Circle", "圆"}, {"Sphere", "球"}, {"Cylinder", "圆柱"}};
    }
    if (!choices.isEmpty() && value.isString()) {
        auto* input = new QComboBox(m_body); input->setObjectName("value");
        input->setMinimumWidth(0); input->setMinimumContentsLength(1);
        input->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        for (const auto& choice : choices) input->addItem(choice.second, choice.first);
        int index = input->findData(value.toString());
        if (index < 0) { input->addItem(value.toString().isEmpty() ? "请选择" : "自定义：" + value.toString(), value.toString()); index = input->count()-1; }
        input->setCurrentIndex(index); m_input = input;
        connect(input, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { NotifyEdited(); });
    } else if(m_module=="Artifact" && m_key=="axis" && value.isDouble()) {
        auto* input=new QComboBox(m_body);input->setObjectName("value");input->addItem("X 轴",0);input->addItem("Y 轴",1);input->addItem("Z 轴",2);
        const auto index=input->findData(value.toInt());if(index<0){input->addItem(QString::number(value.toDouble()),value.toDouble());input->setCurrentIndex(input->count()-1);}else input->setCurrentIndex(index);
        m_input=input;connect(input,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{NotifyEdited();});
    } else if (value.isBool()) {
        auto* input = new BooleanCheckBox("启用", m_body); input->setObjectName("value");
        input->setCheckState(m_optional && !m_booleanSpecified ? Qt::PartiallyChecked : value.toBool() ? Qt::Checked : Qt::Unchecked); m_input = input;
        input->setToolTip("勾选发送 true；取消勾选发送 false。");
        connect(input, &QCheckBox::stateChanged, this, [this](int state) { m_booleanSpecified = state != Qt::PartiallyChecked; m_booleanEdited = true; UpdateBooleanState(); NotifyEdited(); });
        if (m_optional || m_stateBound) {
            layout->addWidget(input); CreateBooleanStateControls();
            input->setAccessibleName(m_titleLabel?m_titleLabel->text():ShortTitle(m_key,m_title));input->setAccessibleDescription(accessibleDescription());
            return;
        }
    } else {
        QLineEdit* input = value.isDouble() ? static_cast<QLineEdit*>(new NumberInput(value.toDouble(),m_body))
            : IsPath(m_key) ? static_cast<QLineEdit*>(new PathInput(m_key == "outputDir", m_body)) : new QLineEdit(m_body);
        input->setObjectName("value"); input->setMinimumWidth(35);
        if(value.isString())input->setText(value.toString());
        m_input = input; input->setAccessibleName(m_title); connect(input, &QLineEdit::textEdited, this, [this] { NotifyEdited(); });
        if (IsPath(m_key)) {
            static_cast<PathInput*>(input)->onPathChanged = [this](const QString&) { NotifyEdited(); };
            auto* browse = new QPushButton("选择…", m_body); layout->addWidget(input, 1); layout->addWidget(browse);
            connect(browse, &QPushButton::clicked, this, [this, input] {
                const auto path = m_key == "outputDir" ? QFileDialog::getExistingDirectory(this, "选择目录", input->text())
                    : m_key == "outputPath" ? QFileDialog::getSaveFileName(this, "选择输出文件", input->text()) : QFileDialog::getOpenFileName(this, "选择文件", input->text());
                if (!path.isEmpty()) { input->setText(path); NotifyEdited(); }
            });
            return;
        }
    }
    layout->addWidget(m_input, 1);
    m_input->setAccessibleName(m_titleLabel?m_titleLabel->text():ShortTitle(m_key,m_title));m_input->setAccessibleDescription(accessibleDescription());
}
void ParameterEditor::AddRow(const QJsonValue& value)
{
    auto* row = new QWidget(m_body); auto* layout = new QHBoxLayout(row); layout->setContentsMargins(0,0,0,0);
    auto* editor = new ParameterEditor(m_module, m_action, m_key, value, m_itemTemplate, row, true, "第 " + QString::number(m_rows.size()+1) + " 组");
    editor->onEdited = [this] { NotifyEdited(); };
    auto* remove = new QPushButton("删除", row); remove->setObjectName("removeRow"); remove->setAccessibleName(m_title + "：删除此行");
    auto* commands = new QVBoxLayout; commands->setSpacing(4); commands->addWidget(remove);
    layout->addWidget(editor, 1); layout->addLayout(commands); commands->setAlignment(Qt::AlignTop);
    m_rowLayout->addWidget(row); m_rows.emplace_back(row, editor);
    if(m_key=="segments")for(const int direction:{-1,1}) {
        auto* move=new QPushButton(direction<0 ? "上移" : "下移",row);
        move->setObjectName(direction<0 ? "moveUp" : "moveDown"); move->setAccessibleName(m_title + (direction<0 ? "：上移此行" : "：下移此行")); commands->addWidget(move);
        connect(move,&QPushButton::clicked,this,[this,row,direction] {
            const auto found=std::find_if(m_rows.begin(),m_rows.end(),[row](const auto& item){return item.first==row;});
            if(found==m_rows.end())return;
            const int index=static_cast<int>(found-m_rows.begin()),target=index+direction;
            if(target<0 || target>=static_cast<int>(m_rows.size()))return;
            std::swap(m_rows[index],m_rows[target]);
            for(std::size_t i=0;i<m_rows.size();++i) {
                m_rowLayout->removeWidget(m_rows[i].first);m_rowLayout->insertWidget(static_cast<int>(i),m_rows[i].first);
                auto* input=m_rows[i].second;input->m_title="第 "+QString::number(i+1)+" 组";
                input->setAccessibleName(input->m_title);
                auto* title=input->findChild<QLabel*>("parameterTitle",Qt::FindDirectChildrenOnly);title->setText(input->m_title);title->setAccessibleName(title->text());
            }
            NotifyEdited();
        });
    }
    connect(remove, &QPushButton::clicked, this, [this, row] {
        m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(), [row](const auto& item) { return item.first == row; }), m_rows.end());
        m_rowLayout->removeWidget(row); row->hide(); row->deleteLater();
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            auto* editor = m_rows[i].second; editor->m_title = "第 " + QString::number(i+1) + " 组";
            editor->setAccessibleName(editor->m_title);
            auto* title=editor->findChild<QLabel*>("parameterTitle", Qt::FindDirectChildrenOnly);title->setText(editor->m_title);title->setAccessibleName(title->text());
        }
        NotifyEdited();
    });
}
QJsonValue ParameterEditor::GetValue() const
{
    const_cast<ParameterEditor*>(this)->BuildFields();
    if (m_boolean && m_stateBound && !m_booleanEdited) return m_appliedBoolean;
    if (m_boolean && m_optional && !m_booleanSpecified) return {};
    if (m_specified && !m_specified->isChecked()) return {};
    if (m_type == QJsonValue::Object) {
        QJsonObject value;
        for (const auto& field : m_fields) {
            const bool colorEditor=(m_module=="Wall" || m_module=="Gap" || m_module=="Alignment")
                && (m_action=="SetDisplay" || m_action=="SetDeviationDisplay");
            if (!colorEditor && !GetFieldApplicable(field.first)) {
                if(m_parameterSource && (field.first=="initialIsoValue" || field.first=="materialRange"))value[field.first]=QJsonValue();
                else if(m_module=="Gap" && m_key.isEmpty())value[field.first]=m_schema.toObject()[field.first];
                continue;
            }
            const auto v = field.second->GetValue();
            value[field.first] = v;
        }
        return value;
    }
    if (m_type == QJsonValue::Array) {
        if (auto* input=qobject_cast<QPlainTextEdit*>(m_input)) {
            QJsonParseError error; const auto document=QJsonDocument::fromJson(input->toPlainText().toUtf8(),&error);
            if (error.error!=QJsonParseError::NoError || !document.isArray()) throw std::invalid_argument("批量索引必须为完整 JSON 数组");
            return document.array();
        }
        QJsonArray value; for (const auto& row : m_rows) value.append(row.second->GetValue()); return value;
    }
    if(auto* input=qobject_cast<QComboBox*>(m_input))return m_type==QJsonValue::Double?QJsonValue(input->currentData().toDouble()):QJsonValue(input->currentData().toString());
    if (auto* input = qobject_cast<QCheckBox*>(m_input)) return input->isChecked();
    const auto text = static_cast<QLineEdit*>(m_input)->text();
    if (m_type == QJsonValue::String) return text;
    bool valid = false; const double number = static_cast<NumberInput*>(m_input)->GetNumber(&valid);
    if (!valid || !std::isfinite(number)) { m_input->setFocus(); throw std::invalid_argument((m_title + "：请输入有效数字").toStdString()); }
    return number;
}
void ParameterEditor::SetPatch(const QJsonObject& patch)
{
    BuildFields();
    if(m_parameterSource && (patch.contains("initialIsoValue") || patch.contains("materialRange"))) {
        const auto iso=patch.contains("initialIsoValue")?patch["initialIsoValue"]:m_fields.at("initialIsoValue")->GetValue();
        const auto range=patch.contains("materialRange")?patch["materialRange"]:m_fields.at("materialRange")->GetValue();
        m_isManualSource=!iso.isNull() || !range.isNull();
        const QSignalBlocker blocker(m_parameterSource);m_parameterSource->setCurrentIndex(m_isManualSource?1:0);
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto field = m_fields.find(it.key());
        if (field == m_fields.end()) throw std::invalid_argument(("未知参数：" + it.key()).toStdString());
        field->second->SetValue(it.value());
    }
    SetFieldVisibility();
}
ParameterEditor* ParameterEditor::GetField(const QString& key) const { const_cast<ParameterEditor*>(this)->BuildFields();const auto it = m_fields.find(key); return it == m_fields.end() ? nullptr : it->second; }
ParameterEditor* ParameterEditor::GetElement(int index) const { return index >= 0 && index < static_cast<int>(m_rows.size()) ? m_rows[index].second : nullptr; }
int ParameterEditor::GetCount() const { return qobject_cast<QPlainTextEdit*>(m_input) ? GetValue().toArray().size() : static_cast<int>(m_rows.size()); }
bool ParameterEditor::GetHasInputs() const
{
    const auto bound = GetBoundParameters(m_module, m_action);
    if(m_fieldsDeferred){const auto keys=m_schema.toObject().keys();return std::any_of(keys.cbegin(),keys.cend(),[&bound](const QString& key){return !bound.contains(key);});}
    return std::any_of(m_fields.begin(), m_fields.end(), [&bound](const auto& field) { return !bound.contains(field.first); });
}
}
