// 资源岛 — 设置持久化。
//
// 复用 dsh-pet 的配置文件：%APPDATA%\dsh-pet-standalone-webm-chat\config.json
// 只读写其中 dynamic_island 段的这些键：
//     x, y               位置（绝对虚拟桌面坐标，逻辑像素）
//     dock_edge          none / top / bottom / left / right
//     edge_dock          停靠总开关（dsh-pet 也存在这个键）
//     accent, style, opacity   外观（沿用 dsh-pet 的口径，不写成自己的私货）
//     rtss_autostart     资源岛自己的开关：RTSS 不在时是否自动把它拉起来
// 其它键一律原样保留：写回采用「只改目标键的值 + 原子替换 + 一次备份」的方式，
// 绝不整文件重排，避免破坏 dsh-pet 自己写入的内容。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class DockEdge { None, Top, Bottom, Left, Right };

// 停靠几何参数（逻辑像素，与 dsh-pet 对齐）
constexpr int   kDockThreshold     = 40;   // 距工作区任一边 <= 40 判定停靠
constexpr int   kStripThickness    = 16;   // 细条厚度
constexpr int   kStripSide         = 64;   // 左右停靠时细条长度
constexpr int   kEdgeMarginDefault = 16;   // 首次启动无配置时的边距
constexpr DWORD kDockBackMs        = 800;  // 鼠标离开后收回细条
constexpr DWORD kDockAnimMs        = 180;  // 停靠/滑出动画时长
constexpr DWORD kAnimTickMs        = 16;   // 动画节拍

const wchar_t* DockEdgeName(DockEdge edge);
DockEdge       DockEdgeFromName(const std::string& name);

// 默认（无配置时）的落点：工作区右上角向内 kEdgeMarginDefault。
POINT DefaultIslandPos(const RECT& work);

struct IslandConfig {
    bool     ok         = false;   // 文件存在且解析出 dynamic_island 段
    bool     pathOk     = false;   // 配置路径可确定（仅用于诊断）

    bool     hasPos     = false;   // 文件里有 x 与 y
    bool     hasEdge    = false;   // 文件里有 dock_edge
    POINT    pos{ 0, 0 };          // hasPos 为真时有效
    DockEdge edge       = DockEdge::None;
    bool     edgeDock   = true;    // 停靠总开关，默认开

    // 外观（沿用 dsh-pet 口径；缺省时由调用方保持自己的默认样式）
    std::string accent;            // blue / purple / green / orange / pink / red / cyan / amber
    std::string style;             // dark / light
    float       opacity = -1.0f;   // <0 表示文件里没有该键

    // 资源岛自己的开关，同样住在 dynamic_island 段（dsh-pet 会原样保留不认识的键）
    bool        rtssAutostart = true;   // RTSS 不在时自动拉起，默认开
};

// %APPDATA%\dsh-pet-standalone-webm-chat\config.json，取不到时返回空串。
std::wstring IslandConfigPath();

// 读取（文件不存在 / 无 dynamic_island 段都不是错误，ok 会是 false）。
IslandConfig LoadIslandConfig();

// 指定路径的版本（自检 / 测试用；产品路径走上面的默认实现）。
IslandConfig LoadIslandConfigAt(const std::wstring& path);

// 更新 dynamic_island 段中的字段并原子写回。
// 传 nullptr / 负数表示「不改这一项」。失败返回 false 并填 error。
bool SaveIslandConfig(const IslandConfig& cfg, bool setPos, bool setEdge, bool setAccent,
                      const std::string& accentName, bool setStyle, const std::string& styleName,
                      float opacity, std::string* error);

bool SaveIslandConfigAt(const std::wstring& path, const IslandConfig& cfg, bool setPos,
                        bool setEdge, bool setAccent, const std::string& accentName,
                        bool setStyle, const std::string& styleName, float opacity,
                        std::string* error);

// 只改 dynamic_island 段里指定的几个键：pairs 是「键名（不带引号）→ 原始 JSON 值文本」。
// 供资源岛自己的开关（如 rtss_autostart）使用；键不存在就追加到段尾。
// 值必须是合法 JSON 文本（布尔写 "true"/"false"，字符串要自带引号）。
bool PatchIslandConfigKeys(const std::vector<std::pair<std::string, std::string>>& pairs,
                           std::string* error);

bool PatchIslandConfigKeysAt(const std::wstring& path,
                             const std::vector<std::pair<std::string, std::string>>& pairs,
                             std::string* error);
