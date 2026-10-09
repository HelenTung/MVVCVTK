#include "Data/Internal/AtomicFileOutput.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <string>

AtomicFileOutput::AtomicFileOutput(std::filesystem::path target)
    : m_target(std::move(target))
{
    static std::atomic<unsigned long long> sequence{0};
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        auto candidate = m_target;
        candidate += L".export-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp";
        const auto handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            m_temporary = std::move(candidate);
            m_handle = handle;
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
}

AtomicFileOutput::~AtomicFileOutput()
{
    if (m_handle) CloseHandle(m_handle);
    if (!m_temporary.empty()) {
        std::error_code error;
        std::filesystem::remove(m_temporary, error);
    }
}

bool AtomicFileOutput::GetIsOpen() const noexcept { return m_handle != nullptr; }

bool AtomicFileOutput::Write(const void* bytes, std::size_t size) noexcept
{
    if (!m_handle || (!bytes && size)) return false;
    auto* position = static_cast<const unsigned char*>(bytes);
    while (size) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(m_handle, position, count, &written, nullptr) || written != count) return false;
        position += written;
        size -= written;
    }
    return true;
}

bool AtomicFileOutput::Commit(const TaskStopToken& stopToken) noexcept
{
    if (!m_handle || stopToken.GetIsStopped() || !FlushFileBuffers(m_handle)) return false;
    const bool closed = CloseHandle(m_handle) != FALSE;
    if (!closed) return false;
    m_handle = nullptr;
    if (stopToken.GetIsStopped()) return false;
    // 同卷 rename 由文件系统串行提交：同名并发导出只会留下某次完整结果。
    if (!MoveFileExW(m_temporary.c_str(), m_target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    m_temporary.clear();
    return true;
}
