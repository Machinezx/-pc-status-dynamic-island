# PC 状态灵动岛 ｜ PC Status Dynamic Island

> 拟定简称 Short name: **芯屿 ｜ ChipIsle**

小白借助 DeepSeek 完成的第一个项目！· A beginner's first project, built with DeepSeek.

它显示 CPU 使用率与频率、内存占用、GPU 使用率 / 显存 / 频率，以及游戏帧率。平时贴在屏幕边缘只留一条 16 像素宽的细条，鼠标移上去展开成完整胶囊，移开 0.8 秒后自动收回；可以拖到任意一条边停靠，也可以停在桌面任意位置。细条状态下是一枚角色头像加一颗状态点。

纯 Win32 + C++20，静态链接成单个 exe：免安装、不依赖 VC++ 运行库、不联网、不打日志、不发遥测，也不需要管理员权限。单一采样线程每秒一次，窗口隐藏时自动暂停。常驻内存约 3 MB；开启 GPU 读取时 NVIDIA 的 `nvml` 接口会额外占用约 19 MB，可用 `--no-gpu` 关掉。

Windows 11 桌面上一块实时显示电脑运转状况的悬浮小岛。
A small floating island for the Windows 11 desktop that shows how the machine is doing, live.

![docked strip](shot-strip-final.png)

---

## 功能 ｜ Features

| 指标 Metric | 内容 What it shows |
| --- | --- |
| CPU | 使用率 + 当前频率 · usage + clock speed |
| 内存 Memory | 使用率 + 已用 / 总量 · usage + used / total |
| GPU | 使用率 + 显存 + 频率，走 NVIDIA NVML · usage + VRAM + clock via NVML |
| FPS | 前台游戏的实时帧率，读 RTSS 共享内存 · live frame rate of the foreground game, read from RTSS shared memory |

- **绝对不含任何温度监测**，也不使用 WMI 查询显卡。
- **No temperature monitoring of any kind**, and no WMI graphics queries.

## 交互 ｜ Behaviour

- 启动后顶部居中，无边框、圆角、半透明、置顶、不抢焦点。
- 可拖拽移动；松手时若离屏幕任意一边 ≤ 40 逻辑像素，就吸附停靠到那条边。
- 停靠后收成 **16 逻辑像素**宽的细条；鼠标移上去滑出完整胶囊，移开后 800 ms 自动收回。
- 细条上画：靠屏侧的强调色亮线 + 角色头像（仅左右停靠时）+ 状态点。
- 托盘菜单可切换鼠标穿透（快捷键 `Ctrl+Alt+I`）、显示/隐藏、立即刷新、RTSS 自动启动、取消贴边、退出。
- 支持 Per-Monitor DPI V2，多显示器各自缩放。

---

- Starts centred at the top; borderless, rounded, translucent, always-on-top, never steals focus.
- Drag to move; on release, if any screen edge is within 40 logical pixels, it snaps and docks there.
- While docked it collapses to a **16-logical-pixel** strip; hover it and it peeks out as the full
  capsule, then retracts by itself 800 ms after the pointer leaves.
- The strip draws an accent line against the screen edge, a character avatar (left/right docks only)
  and a status dot.
- Tray menu toggles click-through (hotkey `Ctrl+Alt+I`), show/hide, refresh now, RTSS auto-start,
  undock and quit.
- Per-Monitor DPI V2 aware; each display scales independently.

## 构建 ｜ Build

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载（MSVC + Windows SDK）。
Requires the *Desktop development with C++* workload of Visual Studio (MSVC + Windows SDK).

```bat
build.bat
```

`build.bat` 会自动定位 `vcvars64.bat`（先问 `vswhere`，再扫常规安装目录）；也可以自己设
`VCVARS64` 环境变量指定。产物是 `build\ResourceIsland.exe`，`/MT` 静态链接，单个 exe 免安装。

`build.bat` locates `vcvars64.bat` on its own (via `vswhere`, then by scanning the usual install
roots); set the `VCVARS64` environment variable to override. Output is
`build\ResourceIsland.exe` — statically linked with `/MT`, a single installer-free exe.

CMake 路线同样可用 / A CMake project is provided as well:

```bat
cmake -B out -A x64
cmake --build out --config Release
```

## 运行 ｜ Usage

```
ResourceIsland.exe [--passthrough] [--no-gpu] [--attach-pet] [--no-restore]
                   [--monitor=N] [--offset-y=N] [--opacity=F] [--x=N] [--y=N]
```

| 开关 Switch | 作用 Effect |
| --- | --- |
| `--passthrough` | 启动即开启鼠标穿透 · start with click-through enabled |
| `--no-gpu` | 完全不加载 `nvml.dll`，GPU 段显示 `--`，省约 19 MB 工作集 · skip NVML entirely |
| `--attach-pet` | 贴在 dsh-pet 灵动岛的正下方 · park right below the dsh-pet island |
| `--no-restore` | 不读也不写配置文件，用默认落点 · neither read nor write the config file |
| `--monitor=N` | 指定显示器（0 起）· pick a monitor, zero-based |
| `--offset-y=N` | 距工作区顶部的逻辑像素，默认 8 · top offset in logical pixels, default 8 |
| `--opacity=F` | 不透明度 0.0~1.0，覆盖配置 · opacity, overrides the config |
| `--x=N` `--y=N` | 指定初始位置（物理像素）并取消停靠 · initial position in physical pixels, undocked |

## 前置依赖 ｜ Requirements

| 项目 Item | 是否必需 Required | 说明 Notes |
| --- | --- | --- |
| Windows 10 1703+ / Windows 11 x64 | 必需 | 用到 `SetProcessDpiAwarenessContext`（Per-Monitor V2）与 `UpdateLayeredWindow` |
| VC++ 运行库 VC++ runtime | **不需要** | `/MT` 静态链接；`dumpbin /dependents` 只有 `USER32/GDI32/SHELL32/ADVAPI32/pdh/KERNEL32` |
| .NET / Python / Qt | **不需要** | 纯 Win32 + C++20 |
| 管理员权限 Admin rights | **不需要** | 只有自动启动 RTSS 时才会弹一次 UAC |
| NVIDIA 驱动 | 可选 | 缺了 GPU 段显示 `--`；`nvml.dll` 是动态加载的 |
| RTSS | 可选 | 缺了 FPS 段整个隐藏；托盘可让它自动拉起（RTSS 需要管理员，会弹 UAC） |

## 配置文件 ｜ Config file

```
%APPDATA%\dsh-pet-standalone-webm-chat\config.json
```

与 [dsh-pet](https://github.com/MerZlin/dsh-pet-indesktop) 共用同一个文件，但只读写 `dynamic_island` 段，其余内容原样保留（写回走「精确键匹配 + 临时文件 + 原子替换」，并留一份 `.dsh-bak`）。
Shared with dsh-pet; only the `dynamic_island` section is read or written, everything else is left
byte-for-byte intact (atomic temp-file replace, with a `.dsh-bak` backup).

| 键 Key | 取值 Values | 含义 Meaning |
| --- | --- | --- |
| `x` `y` | 物理像素 | 窗口左上角 · window top-left |
| `dock_edge` | `none` `top` `bottom` `left` `right` | 停靠边 · docked edge |
| `edge_dock` | `true` `false` | 是否允许贴边停靠 · allow edge docking |
| `accent` | `blue` `green` `orange` `pink` `purple` `red` `cyan` `amber` | 强调色，默认 `blue` |
| `style` | `dark` `light` | 配色 · palette |
| `opacity` | `0.0`~`1.0` | 不透明度 · opacity |
| `rtss_autostart` | `true` `false` | RTSS 不在时自动启动 · auto-start RTSS |

## 体积与开销 ｜ Footprint

- 单个 exe 约 309 KB，免安装、零外部文件（头像图已编进可执行文件）。
- 常驻内存约 3 MB；开启 GPU 读取后，NVIDIA 的 `nvml` 初始化会额外占约 19 MB——这是「内存 < 20 MB」目标唯一的硬约束，也是 `--no-gpu` 存在的理由。
- 单一采样线程，1 秒一次；窗口隐藏时暂停采样。
- 不联网、不打日志、不发遥测，不注入 explorer。

---

- A single ~309 KB exe, no installer, no external files (the avatar is baked into the binary).
- About 3 MB resident; enabling GPU readings costs roughly 19 MB more because NVIDIA's `nvml`
  initialisation allocates that much — the one hard constraint on the "< 20 MB" goal, and the reason
  `--no-gpu` exists.
- One sampling thread at 1 Hz, paused whenever the window is hidden.
- No network, no logging, no telemetry, no injection into explorer.

## 目录结构 ｜ Layout

```
build.bat            一键 MSVC 构建 · one-shot MSVC build
CMakeLists.txt       CMake 构建 · CMake build
src/
  main.cpp           wWinMain / DPI 感知 / 单实例 / 命令行  · entry point
  island.cpp/.h      窗口、停靠与动画、托盘、热键、几何 · window, docking, animation, tray
  render.cpp/.h      分层窗口渲染（胶囊 / 细条 / 形变）  · layered-window rendering
  sampler.cpp/.h     采样线程（CPU / 内存 / GPU / FPS）· sampling thread
  config.cpp/.h      最小 JSON 读写，只碰 dynamic_island · minimal JSON for the config section
  nvml_dyn.cpp/.h    动态加载 nvml.dll（绝不引用温度接口）· dynamic NVML loader
  fps.cpp/.h         RTSS 共享内存读取 + 拉起 · RTSS shared-memory reader / launcher
  metrics.h          采样结构体（刻意不含温度字段）· sample structs, deliberately temperature-free
  avatar_data.cpp/.h 内嵌头像位图 · baked avatar bitmap
tools/make_avatar.py 从 PNG 生成 avatar_data.cpp · regenerates the baked avatar
probe/               开发期探针与测试脚本（本地，未随仓库发布）· dev probes, kept local
ref/                 dsh-pet 源码快照（本地规格参考，未随仓库发布）· dsh-pet source snapshots, kept local
```

## 素材来源 ｜ Credits

细条里的角色头像取自 [dsh-pet](https://github.com/MerZlin/dsh-pet-indesktop) 的素材，
用 `tools/make_avatar.py` 抠白底、裁切、缩放后烘进 `src/avatar_data.cpp`。
停靠细条的版式（16 px 细条、靠屏侧亮线、头像 + 状态点）参照 dsh-pet 的灵动岛。
The character avatar in the docked strip comes from dsh-pet's assets, keyed/cropped/scaled by
`tools/make_avatar.py` and baked into `src/avatar_data.cpp`. The docked-strip layout follows
dsh-pet's dynamic island.

## 许可 ｜ License

MIT © 2026 Machinezx — 见 [LICENSE](LICENSE)。
MIT © 2026 Machinezx — see [LICENSE](LICENSE).

注意：细条里的角色头像派生自 [dsh-pet](https://github.com/MerZlin/dsh-pet-indesktop) 的素材，
本仓库的 MIT 许可不覆盖该美术资源；如要再分发请先确认上游授权。
Note: the character avatar derives from dsh-pet's assets. This repository's MIT licence does not
cover that artwork — check the upstream licence before redistributing it.
