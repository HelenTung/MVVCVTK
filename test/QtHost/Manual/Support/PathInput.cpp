// 测试用途：验证拖拽本地文件时路径保留空格、中文，并正确处理目录输入。
#include "PathInput.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
namespace Manual {
PathInput::PathInput(bool directory, QWidget* parent) : QLineEdit(parent), m_directory(directory)
{
    setAcceptDrops(true); setPlaceholderText(directory ? "拖入目录或文件，或选择路径" : "拖入文件，或选择路径");
}
void PathInput::dragEnterEvent(QDragEnterEvent* event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.front().isLocalFile()) event->acceptProposedAction();
}
void PathInput::dropEvent(QDropEvent* event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.size() != 1 || !urls.front().isLocalFile()) return;
    const QFileInfo file(urls.front().toLocalFile());
    if (!file.exists() || (!m_directory && !file.isFile())) return;
    const auto path = m_directory && file.isFile() ? file.absolutePath() : file.absoluteFilePath();
    setText(path); event->acceptProposedAction();
    if (onPathChanged) onPathChanged(path);
}
}
