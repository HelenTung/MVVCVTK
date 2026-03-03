#include "MemMappedFile.h"
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool MemMappedFile::open(const std::string& path, size_t length) {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile); return false;
    }
    m_size = (length == 0) ? static_cast<size_t>(fileSize.QuadPart) : length;

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) { CloseHandle(hFile); return false; }

    const void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, m_size);
    if (!ptr) { CloseHandle(hMap); CloseHandle(hFile); return false; }

    m_hFile = hFile;
    m_hMap = hMap;
    m_data = ptr;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    if (length == 0) {
        struct stat st {};
        if (::fstat(fd, &st) < 0) { ::close(fd); return false; }
        m_size = static_cast<size_t>(st.st_size);
    }
    else {
        m_size = length;
    }

    if (m_size == 0) { ::close(fd); return false; }  // 空文件保护

#ifdef __linux__
    int flags = MAP_PRIVATE | MAP_POPULATE;  // 预读所有页
#else
    int flags = MAP_PRIVATE;                 // macOS / BSD 兼容
#endif
    void* ptr = ::mmap(nullptr, m_size, PROT_READ, flags, fd, 0);
    if (ptr == MAP_FAILED) { ::close(fd); return false; }

#if defined(MADV_SEQUENTIAL)
    ::madvise(ptr, m_size, MADV_SEQUENTIAL);
#endif

    m_fd = fd;
    m_data = ptr;
#endif
    return true;
}

void MemMappedFile::close() {
    if (!m_data) return;
#ifdef _WIN32
    UnmapViewOfFile(m_data);
    CloseHandle(static_cast<HANDLE>(m_hMap));
    CloseHandle(static_cast<HANDLE>(m_hFile));
    m_hMap = m_hFile = nullptr;
#else
    ::munmap(const_cast<void*>(m_data), m_size);
    ::close(m_fd);
    m_fd = -1;
#endif
    m_data = nullptr;
    m_size = 0;
}