#include "MemMappedFile.h"
#include <cstring>
#include <limits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool MemMappedFile::Load(const std::string& path, size_t length) {
    Clear();
#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0
        || static_cast<unsigned long long>(fileSize.QuadPart)
            > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)())) {
        CloseHandle(hFile); return false;
    }
    const size_t fileBytes = static_cast<size_t>(fileSize.QuadPart);
    const size_t mapBytes = length == 0 ? fileBytes : length;
    if (mapBytes == 0 || mapBytes > fileBytes) {
        CloseHandle(hFile); return false;
    }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) { CloseHandle(hFile); return false; }

    const void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, mapBytes);
    if (!ptr) { CloseHandle(hMap); CloseHandle(hFile); return false; }

    m_hFile = hFile;
    m_hMap = hMap;
    m_data = ptr;
    m_size = mapBytes;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st {};
    if (::fstat(fd, &st) < 0 || st.st_size <= 0
        || static_cast<unsigned long long>(st.st_size)
            > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)())) {
        ::close(fd); return false;
    }
    const size_t fileBytes = static_cast<size_t>(st.st_size);
    const size_t mapBytes = length == 0 ? fileBytes : length;
    if (mapBytes == 0 || mapBytes > fileBytes) {
        ::close(fd); return false;
    }

#ifdef __linux__
    int flags = MAP_PRIVATE | MAP_POPULATE;  // 预读所有页
#else
    int flags = MAP_PRIVATE;                 // macOS / BSD 兼容
#endif
    void* ptr = ::mmap(nullptr, mapBytes, PROT_READ, flags, fd, 0);
    if (ptr == MAP_FAILED) { ::close(fd); return false; }

#if defined(MADV_SEQUENTIAL)
    ::madvise(ptr, mapBytes, MADV_SEQUENTIAL);
#endif

    m_fd = fd;
    m_data = ptr;
    m_size = mapBytes;
#endif
    return true;
}

void MemMappedFile::Clear() {
#ifdef _WIN32
    if (m_data) UnmapViewOfFile(m_data);
    if (m_hMap) CloseHandle(static_cast<HANDLE>(m_hMap));
    if (m_hFile) CloseHandle(static_cast<HANDLE>(m_hFile));
    m_hMap = m_hFile = nullptr;
#else
    if (m_data) ::munmap(const_cast<void*>(m_data), m_size);
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
#endif
    m_data = nullptr;
    m_size = 0;
}
