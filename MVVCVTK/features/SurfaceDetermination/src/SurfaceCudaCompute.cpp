#include "SurfaceCudaCompute.h"
#include <Windows.h>
#include <Psapi.h>
#include <filesystem>
#include <string_view>
#include <cfenv>

namespace {
void SurfaceModuleAnchor() {}
} // namespace

SurfaceCudaCompute::~SurfaceCudaCompute() noexcept { Reset(); }
void SurfaceCudaCompute::Reset() noexcept {
    if (m_session && m_close)
        m_close(m_session);
    if (m_library)
        FreeLibrary(static_cast<HMODULE>(m_library));
    m_session = nullptr;
    m_library = nullptr;
    m_read = nullptr;
    m_close = nullptr;
    m_deviceBytes = 0;
}
bool SurfaceCudaCompute::Start(const SurfaceCudaVolume &volume, std::size_t releasedBytes,
                               std::size_t deviceLimit) {
#ifdef MVVCVTK_SURFACE_CUDA_ENABLED
    if (std::fegetround() != FE_TONEAREST)
        return false;
    char backend[16]{};
    GetEnvironmentVariableA("MVVCVTK_SURFACE_COMPUTE_BACKEND", backend, sizeof(backend));
    if (std::string_view(backend) == "cpu")
        return false;
    // 固定 GPU 工作区也是保守的 Host 准入预留；小任务不初始化设备。
    if (releasedBytes < MVVCVTK_SURFACE_CUDA_DEVICE_BYTES)
        return false;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&SurfaceModuleAnchor), &owner))
        return false;
    wchar_t path[MAX_PATH];
    const auto length = GetModuleFileNameW(owner, path, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;
    PROCESS_MEMORY_COUNTERS_EX before{};
    before.cb = sizeof(before);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&before),
                         sizeof(before));
    const auto dll = std::filesystem::path(path).parent_path() / L"SurfaceCudaBackend.dll";
    m_library = LoadLibraryExW(dll.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m_library)
        return false;
    const auto open = reinterpret_cast<SurfaceCudaOpen>(
        GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaCreate"));
    m_read = reinterpret_cast<SurfaceCudaRead>(
        GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaReadPoints"));
    m_close = reinterpret_cast<SurfaceCudaClose>(
        GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaDestroy"));
    if (!open || !m_read || !m_close ||
        !open(&volume, deviceLimit, releasedBytes, &m_session, &m_deviceBytes)) {
        Reset();
        return false;
    }
    PROCESS_MEMORY_COUNTERS_EX after{};
    after.cb = sizeof(after);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&after),
                         sizeof(after));
    if (!before.PrivateUsage || !after.PrivateUsage ||
        (after.PrivateUsage > before.PrivateUsage &&
         after.PrivateUsage - before.PrivateUsage > releasedBytes)) {
        Reset();
        return false;
    }
    return true;
#else
    (void)volume;
    (void)releasedBytes;
    (void)deviceLimit;
    return false;
#endif
}
bool SurfaceCudaCompute::Read(const double *points, std::size_t count, SurfaceCudaSample *output) {
    return m_session && m_read && m_read(m_session, points, count, output);
}
