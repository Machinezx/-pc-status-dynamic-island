// 资源岛 — 顶层入口与命令行选项
#pragma once

#include <windows.h>

struct Options {
    bool  passthrough  = false;   // 启动即开启鼠标穿透
    bool  attachPet    = false;   // 吸附到 dsh-pet 灵动岛正下方（读其 config.json，只读）
    int   monitorIndex = -1;      // -1 = 窗口所在显示器
    int   offsetY      = 8;       // 距工作区顶部的逻辑像素
    bool  hasX         = false;   // --x / --y 同给时使用绝对坐标
    bool  hasY         = false;
    int   x            = 0;
    int   y            = 0;
    float opacity      = -1.0f;   // <0 表示沿用 dsh-pet 配置或默认值
    bool  noGpu        = false;   // 不加载 nvml.dll（无 N 卡或极简内存占用时）
    bool  noPersist    = false;   // --no-restore：不读也不写 dsh-pet 的配置，用默认落点
};

// 解析单个命令行参数；识别成功返回 true。
bool ParseIslandOption(const wchar_t* arg, Options& opt);

// 创建窗口并进入消息循环，返回进程退出码。
int RunResourceIsland(HINSTANCE hInst, const Options& opt);
