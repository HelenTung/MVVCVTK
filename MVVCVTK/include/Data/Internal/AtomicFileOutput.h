#pragma once

#include "Platform/TaskStopToken.h"
#include <filesystem>

// 每个导出独占同目录临时文件；只有完整文件才能原子替换最终路径。
class AtomicFileOutput final {
public:
    explicit AtomicFileOutput(std::filesystem::path target);
    ~AtomicFileOutput();
    AtomicFileOutput(const AtomicFileOutput&) = delete;
    AtomicFileOutput& operator=(const AtomicFileOutput&) = delete;
    bool GetIsOpen() const noexcept;
    bool Write(const void* bytes, std::size_t size) noexcept;
    bool Commit(const TaskStopToken& stopToken) noexcept;
private:
    std::filesystem::path m_target;
    std::filesystem::path m_temporary;
    void* m_handle = nullptr;
};
