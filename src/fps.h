// 游戏帧率来源：只读 RivaTuner Statistics Server 的共享内存。
//
// 机制（本机已实测）：RTSS 把自己 hook 到的每个 3D 应用的帧计数写进命名共享内存
// "RTSSSharedMemoryV2"，结构定义见 RTSS 安装包自带的官方头文件
//     <RTSS>\SDK\Include\RTSSSharedMemory.h
// 帧率公式同官方注释：
//     1000.0f * dwFrames / (dwTime1 - dwTime0)     ← 每秒窗口（本实现用这条）
//     1000000.0f / dwFrameTime                     ← 逐帧
//
// 我们只做三件事：OpenFileMapping(FILE_MAP_READ) → 按偏移取应用条目 → 读计数。
// 不注入、不提权、不写共享内存、不碰 nvml 之外任何驱动接口。
//
// 为什么不能只看「有条目」：RTSS 退出或应用结束后，条目会以陈旧数据留在内存里
// （实测 frames/frametime 冻结不变）。因此必须按「两次采样之间计数有没有增长」
// 判活，增长不了就是陈旧，一律视为不可用。
//
// 另注：RTSS 的统计口径是应用的 Present 调用，开启 DLSS 帧生成时插入帧未必计入，
// 与 NVIDIA App / RTSS 自身 OSD 显示的数字可能有小幅差异。
#pragma once

#include <windows.h>

#include <string>

#include "metrics.h"

// ---------------------------------------------------------------- RTSS 进程
//
// 帧率只有 RTSS 一个来源，而 RTSS 没在跑的时候共享内存根本不存在。RTSS 安装时
// 会把安装路径写进 HKLM\SOFTWARE\WOW6432Node\Unwinder\RTSS\InstallPath，我们用
// 它做权威定位。
//
// 关键约束：RTSS.exe 的嵌入式清单是 <requestedExecutionLevel level=
// "requireAdministrator"/>，所以普通权限下 CreateProcessW 会以
// ERROR_ELEVATION_REQUIRED(740) 失败，只能退到 ShellExecuteExW("runas") ——
// 那会弹一次 UAC。我们绝不要求岛自己提权。

enum class RtssLaunch {
    AlreadyRunning,   // 共享内存已存在，RTSS 本来就在跑
    Started,          // 已成功拉起（或已把 UAC 请求递出去）
    Declined,         // 用户在 UAC 上点了「否」，或提示超时被取消
    NotFound,         // 找不到 RTSS.exe
    Failed,           // 其它失败
};

// RTSS 共享内存是否已存在 —— 等价于「RTSS 正在运行」，只读探测。
bool RtssSharedMemoryPresent();

// 权威安装路径：先查注册表 InstallPath，再按默认安装目录兜底。
bool FindRtssPath(std::wstring& out);

// 拉起 RTSS。已在运行或成功启动返回 Started/AlreadyRunning。
RtssLaunch LaunchRtss();

class FpsReader {
public:
    FpsReader() = default;
    ~FpsReader();

    FpsReader(const FpsReader&) = delete;
    FpsReader& operator=(const FpsReader&) = delete;

    // 采一帧。每次调用间隔应约 1 秒（与采样线程周期一致）。
    void Collect(FpsMetrics& out);

private:
    struct Last {
        unsigned long long frames = 0;
        unsigned long  time1      = 0;
        unsigned long  frameTime  = 0;
    };

    Last m_last[256]{};          // 按条目槽位记录上一次计数
    bool m_lastValid = false;
    unsigned long m_lookupTick = 0;
    unsigned long m_foregroundPid = 0;
};
