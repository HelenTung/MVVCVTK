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
    m_prepare = nullptr;
    m_submit = nullptr;
    m_collect = nullptr;
    m_testFault = nullptr;
    m_profile = nullptr;
    m_close = nullptr;
    m_deviceBytes = 0;
    ++m_epoch;
}
bool SurfaceCudaCompute::Start(const SurfaceCudaVolume &volume, std::size_t releasedBytes,
                               std::size_t deviceLimit) {
    Reset();
#ifdef MVVCVTK_SURFACE_CUDA_ENABLED
    if (std::fegetround() != FE_TONEAREST)
        return false;
    char backend[16]{};
    GetEnvironmentVariableA("MVVCVTK_SURFACE_COMPUTE_BACKEND", backend, sizeof(backend));
    if (std::string_view(backend) == "cpu")
        return false;
    // 实测初始化可能消耗数百 MiB 主机内存；低额度请求在加载 DLL 前直接走 CPU。
    // 主机门限是准入预留，不代表驱动的峰值或常驻内存上界。
    if (releasedBytes < MVVCVTK_SURFACE_CUDA_DEVICE_BYTES ||
        releasedBytes < MVVCVTK_SURFACE_CUDA_HOST_BYTES)
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
    m_prepare = reinterpret_cast<SurfaceCudaPrepareCallback>(GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaPrepare"));
    m_submit = reinterpret_cast<SurfaceCudaSubmitCallback>(GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaSubmit"));
    m_collect = reinterpret_cast<SurfaceCudaCollectCallback>(GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaCollect"));
    m_testFault = reinterpret_cast<SurfaceCudaTestFaultCallback>(GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaTestSetFault"));
    // 诊断入口缺失不影响旧私有 DLL 的计算能力。
    m_profile = reinterpret_cast<SurfaceCudaProfileCallback>(GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaGetProfile"));
    m_close = reinterpret_cast<SurfaceCudaClose>(
        GetProcAddress(static_cast<HMODULE>(m_library), "SurfaceCudaDestroy"));
    if (!open || !m_read || !m_prepare || !m_submit || !m_collect || !m_close ||
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
#ifdef MVVCVTK_SURFACE_CUDA_TESTING
    char fault[4]{};
    if (GetEnvironmentVariableA("MVVCVTK_SURFACE_CUDA_TEST_FAULT",fault,sizeof(fault))==1 &&
        fault[0]>='1' && fault[0]<='7') SetTestFault(unsigned(fault[0]-'0'));
#endif
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
double *SurfaceCudaCompute::Prepare(unsigned slot) {
    double *indices = nullptr;
    return m_session && m_prepare && m_prepare(m_session, slot, &indices) ? indices : nullptr;
}
bool SurfaceCudaCompute::Submit(unsigned slot, std::size_t count, SurfaceCudaTicket &ticket) {
    ticket = {};
    if (!m_session || !m_submit || !m_submit(m_session, slot, count, &ticket)) return false;
    ticket.owner = this;
    ticket.epoch = m_epoch;
    return true;
}
const SurfaceCudaSample *SurfaceCudaCompute::Collect(const SurfaceCudaTicket &ticket) {
    const SurfaceCudaSample *samples = nullptr;
    return ticket.owner == this && ticket.epoch == m_epoch && m_session && m_collect &&
           m_collect(m_session, &ticket, &samples) ? samples : nullptr;
}
bool SurfaceCudaCompute::SetTestFault(unsigned fault) {
    return m_session && m_testFault && m_testFault(m_session,fault);
}
SurfaceCudaProfile SurfaceCudaCompute::GetProfile() const {
    SurfaceCudaProfile profile;
    if (m_session && m_profile) m_profile(m_session,&profile);
    return profile;
}
