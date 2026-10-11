// 单采样线程：1s 周期，窗口隐藏时彻底停采。
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "fps.h"
#include "metrics.h"
#include "nvml_dyn.h"

// 采样线程 → UI 线程：数据已变化，需要重绘
inline constexpr UINT kMsgSampleReady = WM_APP + 1;
// 采样线程 → UI 线程：用户在 UAC 上拒绝了自动拉起 RTSS，请把开关落盘关掉
inline constexpr UINT kMsgRtssDeclined = WM_APP + 2;

class Sampler {
public:
    using Notify = void (*)(void* ctx);

    Sampler() = default;
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    bool Start(Notify notify, void* ctx);
    void Stop();
    void SetPaused(bool paused);
    void Wake();                    // 立即重新采样一次
    void SetGpuEnabled(bool on) { m_gpuEnabled = on; }   // 必须在 Start 之前调用
    // RTSS 不在时自动拉起（默认关，由岛按配置打开）。可在任意时刻调用。
    void SetRtssAutostart(bool on) { m_rtssAutostart.store(on); }
    // 用户拒绝 UAC 时回调（在采样线程上，实现应只做线程安全的事）。
    void SetRtssDeclinedHandler(Notify cb, void* ctx) {
        m_rtssDeclined    = cb;
        m_rtssDeclinedCtx = ctx;
    }

    SystemSample Get() const;

private:
    void Run();
    void Collect(SystemSample& s);
    void CollectCpu(SystemSample& s);
    void CollectMemory(SystemSample& s);
    void CollectFps(SystemSample& s);
    void TryStartRtss();
    void OpenCpuPerfCounter();
    void ReadBaseClockMHz();

    std::thread       m_thread;
    HANDLE            m_evtWake = nullptr;
    HANDLE            m_evtStop = nullptr;
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_stop{false};
    bool              m_gpuEnabled = true;
    Notify            m_notify = nullptr;
    void*             m_ctx    = nullptr;

    // RTSS 自动拉起：只在启动 3 秒后尝试一次，避免开机即弹 UAC。
    std::atomic<bool> m_rtssAutostart{false};
    bool   m_rtssLaunchTried  = false;
    DWORD  m_startTick        = 0;
    Notify m_rtssDeclined     = nullptr;
    void*  m_rtssDeclinedCtx  = nullptr;

    mutable std::mutex m_mu;
    SystemSample       m_latest;

    // CPU 使用率（GetSystemTimes 差分）
    unsigned long long m_prevIdle   = 0;
    unsigned long long m_prevKernel = 0;
    unsigned long long m_prevUser   = 0;
    bool               m_hasPrevTimes = false;

    // CPU 频率（PDH \Processor Information(_Total)\% Processor Performance）
    HANDLE m_pdhQuery   = nullptr;
    HANDLE m_pdhCounter = nullptr;
    bool   m_pdhReady   = false;
    double m_baseMHz    = 2400.0;

    NvmlLib m_nvml;
    FpsReader m_fps;
};
