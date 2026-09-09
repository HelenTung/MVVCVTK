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
    // 每组由 CatalogNodes 保证子修订在前、父修订在后；不同组不建立连线。
    GraphLayout result;
    const std::function<void(const QJsonArray&)> visit = [&](const QJsonArray& values) {
        QSet<QString> remaining;
        for (const auto value : values) if (value.toObject().contains("parents")) remaining.insert(value.toObject()["id"].toString());
        QStringList lanes;
        auto freeLane = [&] { int lane = lanes.indexOf(QString()); if (lane < 0) { lane = lanes.size(); lanes.append(QString()); } return lane; };
        for (const auto value : values) {
            const auto node = value.toObject(); const auto id = node["id"].toString();
            if (node.contains("parents")) {
                GraphRow row; row.current = node["current"].toBool(); row.pending = node["pending"].toBool();
                row.lane = lanes.indexOf(id); if (row.lane < 0) row.lane = freeLane();
                for (int lane = 0; lane < lanes.size(); ++lane) if (!lanes[lane].isEmpty()) row.strokes.push_back({lane, lane, true});
                lanes[row.lane].clear(); remaining.remove(id);
                for (int lane = 0; lane < lanes.size(); ++lane) if (!lanes[lane].isEmpty()) row.strokes.push_back({lane, lane, false});
                for (const auto parent : node["parents"].toArray()) {
                    const auto ref = parent.toString(); if (!remaining.contains(ref)) continue;
                    int lane = lanes.indexOf(ref); if (lane < 0) { lane = freeLane(); lanes[lane] = ref; }
                    row.strokes.push_back({row.lane, lane, false});
                }
                result.lanes = std::max(result.lanes, lanes.size()); result.rows.insert(id, std::move(row));
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
