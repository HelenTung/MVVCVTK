// 测试用途：绘制真实历史轨道；绘制只读取缓存的轻量布局，不查询业务或复制数据。
#include "SceneGraph.h"
#include <QJsonObject>
#include <QHeaderView>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QStyledItemDelegate>
#include <algorithm>
#include <functional>
namespace Manual {
GraphLayout GetGraphLayout(const QJsonArray& nodes)
{
    // 按真实父关系连接每组已显示的节点，列表可为父先子后或子先父后。
    GraphLayout result;
    const std::function<void(const QJsonArray&)> visit = [&](const QJsonArray& values) {
        QHash<QString,int> positions;QVector<QString> ids;
        for(const auto value:values)if(value.toObject().contains("parents")){const auto id=value.toObject()["id"].toString();positions[id]=ids.size();ids.append(id);}
        for (const auto value : values) {
            const auto node = value.toObject(); const auto id = node["id"].toString();
            if (node.contains("parents")) {
                GraphRow row; row.current = node["current"].toBool(); row.pending = node["pending"].toBool();
                result.rows.insert(id,std::move(row));
            }
        }
        QVector<int> laneEnds;
        for(const auto value:values) {
            const auto node=value.toObject();const auto id=node["id"].toString();
            if(node.contains("parents")) {
                for (const auto parent : node["parents"].toArray()) {
                    const auto ref=parent.toString();if(!positions.contains(ref) || ref==id)continue;
                    const auto first=std::min(positions[id],positions[ref]),last=std::max(positions[id],positions[ref]);
                    result.edges.append(QJsonObject{{"child",id},{"parent",ref}});
                    if(last==first+1){result.rows[ids[first]].strokes.push_back({0,0,false});result.rows[ids[last]].strokes.push_back({0,0,true});continue;}
                    int index=0;while(index<laneEnds.size() && laneEnds[index]>=first)++index;
                    if(index==laneEnds.size())laneEnds.append(last);else laneEnds[index]=last;
                    const int lane=index+1;result.lanes=std::max(result.lanes,lane+1);
                    result.rows[ids[first]].strokes.push_back({0,lane,false});
                    for(int row=first+1;row<last;++row){result.rows[ids[row]].strokes.push_back({lane,lane,true});result.rows[ids[row]].strokes.push_back({lane,lane,false});}
                    result.rows[ids[last]].strokes.push_back({lane,0,true});
                }
            }
            visit(node["children"].toArray());
        }
    };
    visit(nodes); return result;
}
namespace {
QColor Color(int lane) { static const QColor colors[]{"#3464d9", "#d48000", "#8b4bc3", "#089e91", "#d04a70", "#318747"}; return colors[lane % 6]; }
class GraphDelegate final : public QStyledItemDelegate {
public:
    explicit GraphDelegate(SceneGraphTree* tree) : QStyledItemDelegate(tree), m_tree(tree) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const auto node = index.sibling(index.row(), 0).data(Qt::UserRole).toJsonObject();
        const auto found = m_tree->GetGraph().rows.constFind(node["id"].toString());
        painter->save(); painter->setClipRect(option.rect); painter->setRenderHint(QPainter::Antialiasing);
        const double step = std::min(18., (option.rect.width()-12.) / std::max(1, m_tree->GetGraph().lanes));
        const auto x = [&](int lane) { return option.rect.left()+10.+lane*step; };
        const double mid = option.rect.center().y(), top = option.rect.top(), bottom = option.rect.bottom()+1.;
        if (found != m_tree->GetGraph().rows.cend()) {
            for (const auto& stroke : found->strokes) {
                painter->setPen(QPen(Color(stroke.to), 1.8));
                QPainterPath path; path.moveTo(x(stroke.from), stroke.incoming ? top : mid);
                if (stroke.incoming || stroke.from == stroke.to) path.lineTo(x(stroke.to), stroke.incoming ? mid : bottom);
                else path.cubicTo(x(stroke.from), bottom, x(stroke.to), mid, x(stroke.to), bottom);
                painter->drawPath(path);
            }
            const QPointF center(x(found->lane), mid); const auto color = Color(found->lane);
            painter->setPen(QPen(color, found->current ? 2.2 : 1.7));
            painter->setBrush(option.palette.base()); painter->drawEllipse(center, found->current ? 6. : 4., found->current ? 6. : 4.);
            if (!found->pending) { painter->setPen(Qt::NoPen); painter->setBrush(color); painter->drawEllipse(center, 2.6, 2.6); }

        }
        painter->restore();
    }
private:
    SceneGraphTree* m_tree;
};
}
SceneGraphTree::SceneGraphTree(QWidget* parent) : QTreeWidget(parent)
{
    setColumnCount(3); setHeaderLabels({"目录条目", "状态", "来源"});
    setItemDelegateForColumn(2, new GraphDelegate(this));
    header()->setMinimumSectionSize(28);
    header()->moveSection(2, 0); header()->setSectionResizeMode(2, QHeaderView::Interactive); header()->resizeSection(2, 36);
    setIndentation(14); setStyleSheet("QTreeView::item { min-height: 26px; } QHeaderView::section { padding: 5px; }");
}
void SceneGraphTree::SetGraph(const QJsonArray& nodes)
{
    const auto previous = m_graph.lanes; m_graph = GetGraphLayout(nodes);
    if (previous != m_graph.lanes) header()->resizeSection(2, std::min(120, std::max(36, m_graph.lanes*14+12)));
    viewport()->update();
}
}
