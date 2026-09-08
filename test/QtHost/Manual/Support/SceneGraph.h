// 测试用途：将真实父修订关系排成提交记录式轨道，支持单线、分叉与汇合的场景节点展示。
#pragma once
#include <QJsonArray>
#include <QHash>
#include <QTreeWidget>
#include <vector>
namespace Manual {
struct GraphStroke { int from = 0, to = 0; bool incoming = false; };
struct GraphRow { int lane = 0; std::vector<GraphStroke> strokes; bool current = false, pending = false; };
struct GraphLayout { QHash<QString, GraphRow> rows; int lanes = 1; };
GraphLayout GetGraphLayout(const QJsonArray& nodes);
class SceneGraphTree final : public QTreeWidget {
public:
    explicit SceneGraphTree(QWidget* parent = nullptr);
    void SetGraph(const QJsonArray& nodes);
    const GraphLayout& GetGraph() const { return m_graph; }
private:
    GraphLayout m_graph;
};
}
