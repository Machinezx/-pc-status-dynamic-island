// 资源岛 — 指标数据结构（纯 POD，跨线程按值传递）
#pragma once

#include <cstdint>

struct CpuMetrics {
    float    usagePercent = 0.0f;   // 0..100
    float    freqMHz      = 0.0f;   // 0 表示未知
};

struct MemMetrics {
    float    usagePercent = 0.0f;   // 0..100
    uint64_t usedBytes    = 0;
    uint64_t totalBytes   = 0;
};

// 仅使用率 / 显存 / 频率三类字段；刻意不含任何温度字段。
struct GpuMetrics {
    bool     available    = false;
    float    usagePercent = 0.0f;
    uint64_t usedBytes    = 0;
    uint64_t totalBytes   = 0;
    float    clockMHz     = 0.0f;
};

// 游戏帧率。来源是 RivaTuner Statistics Server 写出的只读共享内存
// RTSSSharedMemoryV2：我们不注入任何进程、不需要管理员权限。
// RTSS 未安装 / 未运行 / 当前没有正在渲染的 3D 应用时 available=false（UI 显示 --）。
struct FpsMetrics {
    bool     available = false;
    float    fps       = 0.0f;   // 1 秒窗口帧率
    float    frameMs   = 0.0f;   // 同窗口平均帧时间
    uint32_t pid       = 0;      // 命中的游戏进程
    char     app[32]   = {};     // 游戏进程名（仅文件名）
};

struct SystemSample {
    CpuMetrics cpu;
    MemMetrics mem;
    GpuMetrics gpu;
    FpsMetrics fps;
};
