// 测试用途：接收本地文件拖拽并填入路径，不隐式启动加载或计算。
#pragma once
#include <QLineEdit>
#include <functional>
namespace Manual {
class PathInput final : public QLineEdit {
public:
    explicit PathInput(bool directory, QWidget* parent = nullptr);
    std::function<void(const QString&)> onPathChanged;
protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
private:
    bool m_directory;
};
}
