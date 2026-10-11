// 动态加载 nvml.dll。
// 只绑定使用率 / 显存 / 频率接口；绝不引用 nvmlDeviceGetTemperature 等任何温度接口，
// 也不使用 WMI 查询显卡。
#pragma once

#include <windows.h>

#include "metrics.h"

class NvmlLib {
public:
    NvmlLib() = default;
    ~NvmlLib();

    NvmlLib(const NvmlLib&) = delete;
    NvmlLib& operator=(const NvmlLib&) = delete;

    bool Load();                    // 在采样线程内首次调用
    void Unload();
    bool Sample(GpuMetrics& out);   // 未就绪时自动重试 Load

private:
    using nvmlReturn_t = int;
    using nvmlDevice_t = void*;

    struct nvmlUtilization_t {
        unsigned int gpu;
        unsigned int memory;
    };
    struct nvmlMemory_t {
        unsigned long long total;
        unsigned long long free;
        unsigned long long used;
    };

    using FnInit         = nvmlReturn_t (*)();
    using FnShutdown     = nvmlReturn_t (*)();
    using FnGetHandle    = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
    using FnGetUtil      = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
    using FnGetMemory    = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);
    using FnGetClock     = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);

    HMODULE          m_mod      = nullptr;
    FnInit           m_init     = nullptr;
    FnShutdown       m_shutdown = nullptr;
    FnGetHandle      m_handle   = nullptr;
    FnGetUtil        m_util     = nullptr;
    FnGetMemory      m_mem      = nullptr;
    FnGetClock       m_clock    = nullptr;
    nvmlDevice_t     m_dev      = nullptr;
    bool             m_ready    = false;
};
