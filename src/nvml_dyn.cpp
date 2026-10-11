#include "nvml_dyn.h"

namespace {

constexpr int kClockGraphics = 0;   // NVML_CLOCK_GRAPHICS
constexpr int kClockSm       = 1;   // NVML_CLOCK_SM
constexpr int kSuccess       = 0;   // NVML_SUCCESS

template <typename T>
void Bind(HMODULE mod, const char* name, T& fn) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
}

}  // namespace

NvmlLib::~NvmlLib() {
    Unload();
}

bool NvmlLib::Load() {
    if (m_ready) return true;

    if (!m_mod) {
        m_mod = LoadLibraryW(L"nvml.dll");
        if (!m_mod) {
            m_mod = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
        if (!m_mod) return false;
    }

    Bind(m_mod, "nvmlInit_v2", m_init);
    if (!m_init) Bind(m_mod, "nvmlInit", m_init);
    Bind(m_mod, "nvmlShutdown", m_shutdown);
    Bind(m_mod, "nvmlDeviceGetHandleByIndex_v2", m_handle);
    if (!m_handle) Bind(m_mod, "nvmlDeviceGetHandleByIndex", m_handle);
    Bind(m_mod, "nvmlDeviceGetUtilizationRates", m_util);
    Bind(m_mod, "nvmlDeviceGetMemoryInfo", m_mem);
    Bind(m_mod, "nvmlDeviceGetClockInfo", m_clock);

    if (!m_init || !m_handle || !m_util || !m_mem) return false;
    if (m_init() != kSuccess) return false;
    if (m_handle(0, &m_dev) != kSuccess || !m_dev) return false;

    m_ready = true;
    return true;
}

void NvmlLib::Unload() {
    if (m_ready && m_shutdown) m_shutdown();
    m_ready = false;
    m_dev   = nullptr;
    if (m_mod) {
        FreeLibrary(m_mod);
        m_mod = nullptr;
    }
    m_init = nullptr;
    m_shutdown = nullptr;
    m_handle = nullptr;
    m_util = nullptr;
    m_mem = nullptr;
    m_clock = nullptr;
}

bool NvmlLib::Sample(GpuMetrics& out) {
    out = GpuMetrics{};
    if (!m_ready && !Load()) return false;

    nvmlUtilization_t util{};
    if (m_util(m_dev, &util) != kSuccess) return false;
    out.usagePercent = static_cast<float>(util.gpu);
    out.available    = true;

    nvmlMemory_t mem{};
    if (m_mem(m_dev, &mem) == kSuccess) {
        out.usedBytes  = mem.used;
        out.totalBytes = mem.total;
    }

    if (m_clock) {
        unsigned int mhz = 0;
        if (m_clock(m_dev, kClockGraphics, &mhz) != kSuccess || mhz == 0) {
            mhz = 0;
            m_clock(m_dev, kClockSm, &mhz);
        }
        out.clockMHz = static_cast<float>(mhz);
    }
    return true;
}
