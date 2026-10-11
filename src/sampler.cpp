#include "sampler.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <cmath>

namespace {

// 把数值归到「显示分辨率」的桶里：桶值相同 → 屏幕上的字符串不变 → 不必重绘。
double Bucket(double v, double step) {
    return (step > 0.0) ? std::floor(v / step + 0.5) : v;
}

bool DisplayEqual(const SystemSample& a, const SystemSample& b) {
    constexpr double kGiB = 1073741824.0;

    if (Bucket(a.cpu.usagePercent, 1.0) != Bucket(b.cpu.usagePercent, 1.0)) return false;
    if (Bucket(a.cpu.freqMHz, 100.0) != Bucket(b.cpu.freqMHz, 100.0)) return false;

    if (Bucket(a.mem.usagePercent, 1.0) != Bucket(b.mem.usagePercent, 1.0)) return false;
    if (Bucket(static_cast<double>(a.mem.usedBytes) / kGiB, 0.1) !=
        Bucket(static_cast<double>(b.mem.usedBytes) / kGiB, 0.1)) return false;

    if (a.gpu.available != b.gpu.available) return false;
    if (Bucket(a.gpu.usagePercent, 1.0) != Bucket(b.gpu.usagePercent, 1.0)) return false;
    if (Bucket(static_cast<double>(a.gpu.usedBytes) / kGiB, 0.1) !=
        Bucket(static_cast<double>(b.gpu.usedBytes) / kGiB, 0.1)) return false;
    if (Bucket(a.gpu.clockMHz, 5.0) != Bucket(b.gpu.clockMHz, 5.0)) return false;

    if (a.fps.available != b.fps.available) return false;
    if (Bucket(a.fps.fps, 1.0) != Bucket(b.fps.fps, 1.0)) return false;

    return true;
}

}  // namespace

Sampler::~Sampler() {
    Stop();
    if (m_pdhQuery) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(m_pdhQuery));
        m_pdhQuery = nullptr;
    }
}

bool Sampler::Start(Notify notify, void* ctx) {
    if (m_thread.joinable()) return true;

    m_notify = notify;
    m_ctx    = ctx;
    m_stop.store(false);
    m_paused.store(false);

    if (!m_evtStop) m_evtStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_evtWake) m_evtWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_evtStop || !m_evtWake) return false;

    ResetEvent(m_evtStop);
    m_thread = std::thread(&Sampler::Run, this);
    return true;
}

void Sampler::Stop() {
    if (!m_thread.joinable()) return;
    m_stop.store(true);
    if (m_evtStop) SetEvent(m_evtStop);
    if (m_evtWake) SetEvent(m_evtWake);
    m_thread.join();

    if (m_evtStop) { CloseHandle(m_evtStop); m_evtStop = nullptr; }
    if (m_evtWake) { CloseHandle(m_evtWake); m_evtWake = nullptr; }
}

void Sampler::SetPaused(bool paused) {
    m_paused.store(paused);
    if (!paused && m_evtWake) SetEvent(m_evtWake);
}

void Sampler::Wake() {
    if (m_evtWake) SetEvent(m_evtWake);
}

SystemSample Sampler::Get() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_latest;
}

void Sampler::ReadBaseClockMHz() {
    DWORD mhz = 0;
    DWORD cb  = sizeof(mhz);
    const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE,
                                    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                    L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &cb);
    if (st == ERROR_SUCCESS && mhz >= 100) {
        m_baseMHz = static_cast<double>(mhz);
    }
}

void Sampler::OpenCpuPerfCounter() {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || !query) return;

    // 必须用英文计数器名：中文系统上 PdhAddCounterW 传英文名会失败。
    PDH_HCOUNTER counter = nullptr;
    const PDH_STATUS st = PdhAddEnglishCounterW(
        query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &counter);
    if (st != ERROR_SUCCESS || !counter) {
        PdhCloseQuery(query);
        return;
    }

    PdhCollectQueryData(query);   // 预热一帧，避免首帧 PDH_INVALID_DATA
    m_pdhQuery   = static_cast<HANDLE>(query);
    m_pdhCounter = static_cast<HANDLE>(counter);
    m_pdhReady   = true;
}

void Sampler::CollectCpu(SystemSample& s) {
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto toU64 = [](const FILETIME& ft) -> unsigned long long {
            return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
                   static_cast<unsigned long long>(ft.dwLowDateTime);
        };
        const unsigned long long i = toU64(idle);
        const unsigned long long k = toU64(kernel);
        const unsigned long long u = toU64(user);

        if (m_hasPrevTimes) {
            const unsigned long long di = i - m_prevIdle;
            const unsigned long long dk = k - m_prevKernel;
            const unsigned long long du = u - m_prevUser;
            const unsigned long long total = dk + du;   // kernel 已包含 idle
            if (total > 0) {
                const unsigned long long busy = (di < total) ? (total - di) : 0ULL;
                float pct = static_cast<float>(static_cast<double>(busy) * 100.0 /
                                               static_cast<double>(total));
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 100.0f) pct = 100.0f;
                s.cpu.usagePercent = pct;
            }
        }
        m_prevIdle   = i;
        m_prevKernel = k;
        m_prevUser   = u;
        m_hasPrevTimes = true;
    }

    if (m_pdhReady) {
        if (PdhCollectQueryData(static_cast<PDH_HQUERY>(m_pdhQuery)) == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE value{};
            if (PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(m_pdhCounter),
                                            PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS) {
                if (value.CStatus == PDH_CSTATUS_VALID_DATA ||
                    value.CStatus == PDH_CSTATUS_NEW_DATA) {
                    double mhz = m_baseMHz * value.doubleValue / 100.0;   // 可 >100%（睿频）
                    if (mhz < 0.0) mhz = 0.0;
                    if (mhz > 8000.0) mhz = 8000.0;
                    s.cpu.freqMHz = static_cast<float>(mhz);
                }
            }
        }
    }
}

void Sampler::CollectMemory(SystemSample& s) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;
    s.mem.usagePercent = static_cast<float>(ms.dwMemoryLoad);
    s.mem.totalBytes   = ms.ullTotalPhys;
    s.mem.usedBytes    = ms.ullTotalPhys - ms.ullAvailPhys;
}

void Sampler::CollectFps(SystemSample& s) {
    // 只读 RTSS 共享内存；RTSS 不在或没有活跃 3D 应用时 available 保持 false。
    m_fps.Collect(s.fps);
}

void Sampler::TryStartRtss() {
    // 注意：开关关着的时候不要置 m_rtssLaunchTried，这样用户中途打开还能立刻生效。
    if (m_rtssLaunchTried || !m_rtssAutostart.load()) return;
    // 启动 3 秒后才试：开机瞬间不弹 UAC，也不在游戏加载时弹。
    if (GetTickCount() - m_startTick < 3000) return;
    m_rtssLaunchTried = true;

    const RtssLaunch r = LaunchRtss();
    if (r == RtssLaunch::Declined && m_rtssDeclined) m_rtssDeclined(m_rtssDeclinedCtx);
}

void Sampler::Collect(SystemSample& s) {
    CollectCpu(s);
    CollectMemory(s);
    if (m_gpuEnabled) m_nvml.Sample(s.gpu);
    CollectFps(s);
}

void Sampler::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    if (m_gpuEnabled) m_nvml.Load();
    ReadBaseClockMHz();
    OpenCpuPerfCounter();
    m_startTick = GetTickCount();

    for (;;) {
        if (m_stop.load(std::memory_order_relaxed)) break;

        if (m_paused.load(std::memory_order_relaxed)) {
            // 隐藏时彻底停采：只等「恢复」或「退出」
            const HANDLE waits[2] = { m_evtStop, m_evtWake };
            WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            continue;
        }

        TryStartRtss();

        SystemSample next;
        Collect(next);

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            changed = !DisplayEqual(m_latest, next);
            m_latest = next;
        }
        if (changed && m_notify) m_notify(m_ctx);

        const HANDLE waits[2] = { m_evtStop, m_evtWake };
        WaitForMultipleObjects(2, waits, FALSE, 1000);
    }

    m_nvml.Unload();
}
