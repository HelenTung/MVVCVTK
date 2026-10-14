#pragma once
#include "JsonInput.h"
#include <vector>
#include <initializer_list>

// 外部测试宿主只转发显示区间；颜色映射和业务单位仍由所属 Feature 解释。
namespace Manual {
template<class Segment,class Mode>
std::vector<Segment> GetColorSegments(const QJsonObject& params,
    std::initializer_list<std::pair<const char*,Mode>> modes)
{
    std::vector<Segment> segments;
    if(!params.contains("segments"))return segments;
    if(!params["segments"].isArray() || params["segments"].toArray().size()>1024)
        throw std::invalid_argument("分段列表必须为至多 1024 行的区间数组");
    for(const auto value:params["segments"].toArray()) {
        if(!value.isObject())throw std::invalid_argument("每个色带分段需要独立的上下界、类型和颜色");
        const auto row=value.toObject();Segment segment;
        if(row.contains("segmentFrom") && !row["segmentFrom"].isNull())segment.lower=GetNumber(row,"segmentFrom");
        if(row.contains("segmentTo") && !row["segmentTo"].isNull())segment.upper=GetNumber(row,"segmentTo");
        segment.mode=GetEnum<Mode>(row,"palette",modes);
        if(row.contains("lowColor") && !row["lowColor"].isNull())segment.lowColor=GetArray<double,3>(row["lowColor"]);
        if(row.contains("highColor") && !row["highColor"].isNull())segment.highColor=GetArray<double,3>(row["highColor"]);
        segments.push_back(std::move(segment));
    }
    return segments;
}
template<class Mode,class Segment>
QJsonObject GetColorBandJson(Mode mode,const std::array<double,3>& constant,
    const std::array<double,3>& low,const std::array<double,3>& high,
    const std::array<double,3>& below,const std::array<double,3>& above,
    const std::vector<Segment>& segments,bool reversed)
{
    const char* names[]{"Constant","Gradient","Rainbow","InverseRainbow","HueLoop","Diverging"};
    QJsonArray rows;for(const auto& segment:segments)if(segment.lower && segment.upper)rows.append(QJsonObject{
        {"segmentFrom",segment.lower ? QJsonValue(*segment.lower):QJsonValue()},
        {"segmentTo",segment.upper ? QJsonValue(*segment.upper):QJsonValue()},
        {"palette",names[static_cast<unsigned>(segment.mode)]},{"lowColor",GetValues(segment.lowColor)},{"highColor",GetValues(segment.highColor)}});
    const auto tailLow=!segments.empty() && !segments.front().lower ? segments.front().lowColor:below;
    const auto tailHigh=!segments.empty() && !segments.back().upper ? segments.back().lowColor:above;
    const bool uniform=segments.size()==1 && !segments.front().lower && !segments.front().upper;
    return {{"palette",uniform ? "Constant":names[static_cast<unsigned>(mode)]},{"constantColor",GetValues(uniform ? segments.front().lowColor:constant)},
        {"lowColor",GetValues(low)},{"highColor",GetValues(high)},{"belowColor",GetValues(tailLow)},
        {"aboveColor",GetValues(tailHigh)},{"segments",rows},{"isReversed",reversed}};
}

}
