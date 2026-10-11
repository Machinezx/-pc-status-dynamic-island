#include "island.h"

#include "config.h"
#include "render.h"
#include "sampler.h"

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace {

constexpr UINT    kTrayCallback    = WM_APP + 10;
constexpr UINT    kTrayId          = 1;
constexpr int     kHotkeyId        = 1;
constexpr UINT    kMenuPassthrough = 1001;
constexpr UINT    kMenuShowHide    = 1002;
constexpr UINT    kMenuRefresh     = 1003;
constexpr UINT    kMenuQuit        = 1004;
constexpr UINT    kMenuUndock      = 1005;
constexpr UINT    kMenuRtss        = 1006;
constexpr wchar_t kWndClass[]      = L"DSHResourceIslandWnd";

// 定时器
constexpr UINT_PTR kAnimTimerId     = 1;   // 停靠/滑出动画，16ms 节拍
constexpr UINT_PTR kDockBackTimerId = 2;   // 鼠标离开后收回细条（单发 800ms）

// dsh-pet 灵动岛在 config.json 中的位置所对应的胶囊尺寸（逻辑像素），
// 仅用于 --attach-pet 时的相对摆放。
constexpr int kPetIslandWLogical = 300;
constexpr int kPetIslandHLogical = 40;

float DpiScaleOf(HWND hwnd) {
    using FnGetDpi = UINT(WINAPI*)(HWND);
    UINT dpi = 96;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto fn = reinterpret_cast<FnGetDpi>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (fn && hwnd) dpi = fn(hwnd);
    }
    if (dpi == 0) dpi = 96;
    return static_cast<float>(dpi) / 96.0f;
}

struct MonitorSearch {
    int      index;
    int      current;
    HMONITOR found;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* s = reinterpret_cast<MonitorSearch*>(lp);
    if (s->current == s->index) {
        s->found = mon;
        return FALSE;
    }
    ++s->current;
    return TRUE;
}

HMONITOR MonitorByIndex(int index) {
    MonitorSearch search{ index, 0, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&search));
    if (search.found) return search.found;
    return MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
}

COLORREF AccentFromName(const std::string& name) {
    if (name == "purple" || name == "violet") return RGB(0xA7, 0x8B, 0xFA);
    if (name == "green")                        return RGB(0x34, 0xD3, 0x99);
    if (name == "orange")                       return RGB(0xFB, 0x92, 0x3C);
    if (name == "pink")                         return RGB(0xF4, 0x72, 0xB6);
    if (name == "red")                          return RGB(0xEF, 0x44, 0x44);
    if (name == "cyan")                         return RGB(0x22, 0xD3, 0xEE);
    if (name == "amber" || name == "yellow")    return RGB(0xFA, 0xCC, 0x15);
    return RGB(0x4C, 0x8D, 0xF6);   // blue / 默认
}

bool JsonFindString(const std::string& src, size_t from, const char* key, std::string& out,
                    size_t& keyPos) {
    const size_t k = src.find(key, from);
    if (k == std::string::npos) return false;
    const size_t q = src.find('"', k + std::strlen(key));
    if (q == std::string::npos) return false;
    const size_t e = src.find('"', q + 1);
    if (e == std::string::npos) return false;
    out.assign(src, q + 1, e - q - 1);
    keyPos = k;
    return true;
}

bool JsonFindNumber(const std::string& src, size_t from, const char* key, double& out,
                    size_t& keyPos) {
    const size_t k = src.find(key, from);
    if (k == std::string::npos) return false;
    const size_t c = src.find(':', k + std::strlen(key));
    if (c == std::string::npos) return false;
    out = std::strtod(src.c_str() + c + 1, nullptr);
    keyPos = k;
    return true;
}

// dsh-pet 端 config.json 中 dynamic_island 段的可读子集（只读，绝不写回）
struct PetIslandInfo {
    bool     valid    = false;
    int      x        = 0;
    int      y        = 0;
    bool     hasAccent = false;
    COLORREF accent   = RGB(0x4C, 0x8D, 0xF6);
    bool     light    = false;
    float    opacity  = -1.0f;
};

bool ReadPetIslandInfo(PetIslandInfo& info) {
    wchar_t appdata[MAX_PATH * 2]{};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, static_cast<DWORD>(std::size(appdata)))) {
        return false;
    }

    const std::wstring path = std::wstring(appdata) +
                              L"\\dsh-pet-standalone-webm-chat\\config.json";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    constexpr DWORD kMaxRead = 256 * 1024;
    std::string raw;
    raw.resize(kMaxRead);
    DWORD read = 0;
    const BOOL ok = ReadFile(file, raw.data(), kMaxRead, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return false;
    raw.resize(read);

    const size_t root = raw.find("\"dynamic_island\"");
    if (root == std::string::npos) return false;
    const size_t limit = std::min(raw.size(), root + 1500);

    size_t pos = 0;
    std::string text;
    if (JsonFindString(raw, root, "\"accent\"", text, pos) && pos < limit) {
        info.accent    = AccentFromName(text);
        info.hasAccent = true;
    }
    if (JsonFindString(raw, root, "\"style\"", text, pos) && pos < limit) {
        info.light = (text == "light");
    }
    double number = 0.0;
    if (JsonFindNumber(raw, root, "\"opacity\"", number, pos) && pos < limit &&
        number > 0.05 && number <= 1.0) {
        info.opacity = static_cast<float>(number);
    }

    double dx = 0.0;
    double dy = 0.0;
    const bool hasX = JsonFindNumber(raw, root, "\"x\"", dx, pos) && pos < limit;
    const bool hasY = JsonFindNumber(raw, root, "\"y\"", dy, pos) && pos < limit;
    if (hasX && hasY) {
        info.x     = static_cast<int>(std::lround(dx));
        info.y     = static_cast<int>(std::lround(dy));
        info.valid = true;
    }
    return true;
}

void OnSampleReady(void* ctx) {
    PostMessageW(reinterpret_cast<HWND>(ctx), kMsgSampleReady, 0, 0);
}

void OnRtssDeclined(void* ctx) {
    PostMessageW(reinterpret_cast<HWND>(ctx), kMsgRtssDeclined, 0, 0);
}

// 视觉形态：与 dsh-pet 的 _mode(_hover_peek) 对应。
enum class ViewMode { Normal, Docked, Peek };
// 几何动画的目标形态（Docked=细条，Peek/Normal=完整胶囊）。
enum class GeoState { Normal, Docked, Peek };

// dsh-pet 的动画口径：0.18s、cubic ease-out、整数像素。
double EaseOutCubic(double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return 1.0 - std::pow(1.0 - t, 3.0);
}

int ClampInt(int v, int lo, int hi) {
    if (hi < lo) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

class IslandWindow {
public:
    int Run(HINSTANCE hInst, const Options& opt);

private:
    static LRESULT CALLBACK Thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void ReloadStyle();
    void Reposition();
    void LoadStyleAndConfig();
    void ApplyInitialGeometry();
    void ApplyLayoutSize();
    RECT WorkAreaOfWindow() const;
    RECT WorkAreaOfPoint(POINT screenPt) const;
    RECT CurrentWindowRect() const;
    void RectForState(GeoState st, RECT& out) const;
    RECT StateRect(GeoState st) const;
    void SetWindowRect(const RECT& r, UINT flags);
    void StartAnimTo(GeoState st, bool animate);
    void StepAnimation();
    void FinishAnimation();
    void RequestState(GeoState st);
    bool WantsStrip() const;
    void SnapReleaseToEdge(POINT screenPt);
    void BeginDrag();
    void StopDrag();
    void SaveConfig(bool position, bool edge);
    void DrawCurrent();
    void OnSample();
    void TogglePassthrough();
    void SetPassthrough(bool on);
    void ToggleVisible();
    void ShowTrayMenu();
    void InitTray();
    void InitHotkey();
    void Shutdown();

    HWND             m_hwnd   = nullptr;
    HINSTANCE        m_inst   = nullptr;
    Options          m_opt{};
    IslandRenderer   m_render;
    IslandStyle      m_style;
    Sampler          m_sampler;
    NOTIFYICONDATAW  m_nid{};
    SystemSample     m_sample;
    IslandConfig     m_cfg;

    float m_scale = 1.0f;
    int   m_floatW = 0;   // 胶囊正常形态尺寸（由排版决定）
    int   m_floatH = 0;

    // 停靠 / 形态
    ViewMode m_view      = ViewMode::Normal;
    DockEdge m_dockEdge  = DockEdge::None;
    bool     m_hoverPeek = false;

    // 几何动画
    RECT      m_animFrom{};
    // 位置与尺寸的唯一来源 = 窗口矩形本身（与 dsh-pet 的 self.pos()/self.height() 同口径，
    // 这样细条↔胶囊来回切换不会累计偏移）。非动画时取自 CurrentWindowRect()，
    // 动画期间取 m_targetRect，避免用中间态当基准。
    RECT      m_targetRect{};
    GeoState  m_toState   = GeoState::Normal;
    bool      m_animating = false;
    ULONGLONG m_animStart = 0;

    // 拖动
    bool   m_pressed   = false;
    bool   m_dragging  = false;
    POINT  m_pressPt{};                // 按下点（客户区坐标）
    POINT  m_dragOffset{};             // 光标 − 窗口左上角（本窗口无边框，客户区即窗口区）
    int    m_dragThreshold = 4;
    bool   m_tracking      = false;    // TrackMouseEvent 是否已注册

    bool m_passthrough   = false;
    bool m_rtssAutostart = true;
};

LRESULT CALLBACK IslandWindow::Thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    IslandWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<IslandWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<IslandWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void IslandWindow::ReloadStyle() { LoadStyleAndConfig(); }

// 样式与设置一起读：优先用 dsh-pet config.json 的 dynamic_island 段
// （accent / style / opacity / x / y / dock_edge），命令行参数可覆盖外观。
void IslandWindow::LoadStyleAndConfig() {
    m_cfg = LoadIslandConfig();

    IslandStyle style;
    if (m_cfg.ok && m_cfg.style == "light") {
        style.fill     = RGB(0xFA, 0xFA, 0xFA);
        style.edge     = RGB(0xD5, 0xDB, 0xE3);
        style.textMain = RGB(0x17, 0x17, 0x17);
        style.textDim  = RGB(0x6B, 0x72, 0x80);
    }
    if (m_cfg.ok && !m_cfg.accent.empty()) style.accent = AccentFromName(m_cfg.accent);
    if (m_cfg.ok && m_cfg.opacity > 0.0f)  style.opacity = m_cfg.opacity;
    if (m_opt.opacity > 0.0f)              style.opacity = m_opt.opacity;
    m_style = style;
}

// 初始落点：--x/--y > 配置里的 x/y > 主屏工作区顶部居中。
// 这里只往 m_targetRect.left/top 里写"期望落点"，真正的落位由 ApplyInitialGeometry 完成。
void IslandWindow::Reposition() {
    if (m_opt.hasX && m_opt.hasY) {
        m_targetRect.left = m_opt.x;
        m_targetRect.top  = m_opt.y;
        m_dockEdge        = DockEdge::None;
        return;
    }

    if (m_opt.attachPet) {
        PetIslandInfo pet;
        if (ReadPetIslandInfo(pet) && pet.valid) {
            const int petW = static_cast<int>(std::lround(kPetIslandWLogical * m_scale));
            const int petH = static_cast<int>(std::lround(kPetIslandHLogical * m_scale));
            m_targetRect.left = pet.x + (petW - m_floatW) / 2;
            m_targetRect.top  = pet.y + petH + static_cast<int>(std::lround(6.0f * m_scale));
            m_dockEdge        = DockEdge::None;
            return;
        }
    }

    if (!m_opt.noPersist && m_cfg.ok && m_cfg.hasPos) {
        m_targetRect.left = m_cfg.pos.x;
        m_targetRect.top  = m_cfg.pos.y;
        if (m_cfg.edgeDock && m_cfg.hasEdge) m_dockEdge = m_cfg.edge;
        return;
    }

    HMONITOR mon = (m_opt.monitorIndex >= 0)
                       ? MonitorByIndex(m_opt.monitorIndex)
                       : MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    m_targetRect.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - m_floatW) / 2;
    m_targetRect.top  = mi.rcWork.top + static_cast<int>(std::lround(m_opt.offsetY * m_scale));
    m_dockEdge        = DockEdge::None;
}

RECT IslandWindow::WorkAreaOfWindow() const {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    RECT r{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    return r;
}

RECT IslandWindow::WorkAreaOfPoint(POINT screenPt) const {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    return WorkAreaOfWindow();
}

RECT IslandWindow::CurrentWindowRect() const {
    RECT r{};
    GetWindowRect(m_hwnd, &r);
    return r;
}

// 各形态的窗口矩形。位置与尺寸的基准一律取「当前窗口矩形」（动画期间取动画目标），
// 与 dsh-pet 的 _target_rect / strip_rect_for 同口径：
//   · 停靠静置 → 细条：厚度 16，左右停靠长度 64，贴工作区对应的边
//   · 已贴边但滑出/自由 → 完整胶囊，沿贴边方向锚定，另一方向按当前窗口中心对齐
//   · 完全自由 → 胶囊原地，再夹回工作区
// 关键：细条(64) 与胶囊(44) 转换时的"中心"必须用当前窗口高度算，
// 否则每次滑出/收回都会累计漂移一截（dsh-pet 的注释里记过这个坑）。
void IslandWindow::RectForState(GeoState st, RECT& out) const {
    const RECT work  = WorkAreaOfWindow();
    const int  wl = static_cast<int>(work.left);
    const int  wt = static_cast<int>(work.top);
    const int  wr = static_cast<int>(work.right);
    const int  wb = static_cast<int>(work.bottom);

    const int capW  = std::max(m_floatW, 1);
    const int capH  = std::max(m_floatH, 1);
    const int thick = std::max(1, static_cast<int>(std::lround(kStripThickness * m_scale)));
    const int side  = std::max(1, static_cast<int>(std::lround(kStripSide * m_scale)));

    const RECT base  = m_animating ? m_targetRect : CurrentWindowRect();
    const int  px    = static_cast<int>(base.left);
    const int  py    = static_cast<int>(base.top);
    const int  liveH = std::max(1, static_cast<int>(base.bottom - base.top));
    const int  centerY = py + liveH / 2;

    if (st == GeoState::Docked && m_dockEdge != DockEdge::None) {
        switch (m_dockEdge) {
            case DockEdge::Top:
                out = RECT{ px, wt, px + capW, wt + thick };
                break;
            case DockEdge::Bottom:
                out = RECT{ px, wb - thick + 1, px + capW, wb + 1 };
                break;
            case DockEdge::Left: {
                const int y = ClampInt(centerY - side / 2, wt, wb - side + 1);
                out = RECT{ wl, y, wl + thick, y + side };
                break;
            }
            case DockEdge::Right:
            default: {
                const int y = ClampInt(centerY - side / 2, wt, wb - side + 1);
                out = RECT{ wr - thick + 1, y, wr + 1, y + side };
                break;
            }
        }
        return;
    }

    int x = px;
    int y = py;
    switch (m_dockEdge) {
        case DockEdge::Top:    y = wt;                  break;
        case DockEdge::Bottom: y = wb - capH + 1;       break;
        case DockEdge::Left:   x = wl; y = centerY - capH / 2;  break;
        case DockEdge::Right:  x = wr - capW + 1; y = centerY - capH / 2; break;
        case DockEdge::None:
        default: break;
    }

    x = ClampInt(x, wl, wr - capW + 1);
    y = ClampInt(y, wt, wb - capH + 1);
    out = RECT{ x, y, x + capW, y + capH };
}

RECT IslandWindow::StateRect(GeoState st) const {
    RECT r{};
    RectForState(st, r);
    return r;
}

void IslandWindow::SetWindowRect(const RECT& r, UINT flags) {
    SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, flags);
}

void IslandWindow::StartAnimTo(GeoState st, bool animate) {
    const RECT target = StateRect(st);
    m_toState = st;
    if (!animate) {
        KillTimer(m_hwnd, kAnimTimerId);
        m_animating  = false;
        m_targetRect = target;
        SetWindowRect(target, SWP_NOACTIVATE | SWP_NOREDRAW);
        DrawCurrent();
        return;
    }
    m_animFrom  = CurrentWindowRect();
    m_targetRect = target;
    m_animStart = GetTickCount64();
    m_animating = true;
    SetTimer(m_hwnd, kAnimTimerId, kAnimTickMs, nullptr);
    StepAnimation();
}

void IslandWindow::StepAnimation() {
    if (!m_animating) return;
    const ULONGLONG elapsed = GetTickCount64() - m_animStart;
    double t = static_cast<double>(elapsed) / static_cast<double>(kDockAnimMs);
    if (t >= 1.0) {
        FinishAnimation();
        return;
    }
    const double e = EaseOutCubic(t);

    RECT r{};
    r.left   = static_cast<LONG>(std::lround(m_animFrom.left +
                                             (m_targetRect.left - m_animFrom.left) * e));
    r.top    = static_cast<LONG>(std::lround(m_animFrom.top +
                                             (m_targetRect.top - m_animFrom.top) * e));
    r.right  = static_cast<LONG>(std::lround(m_animFrom.right +
                                             (m_targetRect.right - m_animFrom.right) * e));
    r.bottom = static_cast<LONG>(std::lround(m_animFrom.bottom +
                                             (m_targetRect.bottom - m_animFrom.bottom) * e));
    SetWindowRect(r, SWP_NOACTIVATE | SWP_NOREDRAW);
    DrawCurrent();
}

void IslandWindow::FinishAnimation() {
    KillTimer(m_hwnd, kAnimTimerId);
    m_animating = false;
    SetWindowRect(m_targetRect, SWP_NOACTIVATE | SWP_NOREDRAW);
    DrawCurrent();
}

void IslandWindow::RequestState(GeoState st) {
    switch (st) {
        case GeoState::Docked: m_view = ViewMode::Docked; break;
        case GeoState::Peek:   m_view = ViewMode::Peek;   break;
        case GeoState::Normal:
        default:
            m_view = (m_dockEdge == DockEdge::None) ? ViewMode::Normal : ViewMode::Peek;
            break;
    }
    StartAnimTo(st, true);
}

// 现在该显示细条吗：已贴边、没在悬停滑出、而且拖着鼠标的时候也不行。
bool IslandWindow::WantsStrip() const {
    return m_view != ViewMode::Normal && m_dockEdge != DockEdge::None && !m_hoverPeek &&
           !m_dragging;
}

// 布局尺寸变化（FPS 段出现/消失、DPI 变化）：重新落位并重绘。
void IslandWindow::ApplyLayoutSize() {
    if (m_dragging) {   // 拖拽期间窗口位置归光标管，这里只重绘，绝不改几何
        DrawCurrent();
        return;
    }
    if (m_animating) {
        m_targetRect = StateRect(m_toState);
        StepAnimation();
        return;
    }
    const GeoState st = WantsStrip() ? GeoState::Docked : GeoState::Normal;
    const RECT target = StateRect(st);
    m_targetRect = target;
    SetWindowRect(target, SWP_NOACTIVATE | SWP_NOREDRAW);
    DrawCurrent();
}

// DockEdge → 细条亮线所在的那条边
static StripEdge ToStripEdge(DockEdge e) {
    switch (e) {
        case DockEdge::Top:    return StripEdge::Top;
        case DockEdge::Bottom: return StripEdge::Bottom;
        case DockEdge::Left:   return StripEdge::Left;
        case DockEdge::Right:  return StripEdge::Right;
        case DockEdge::None:
        default:               return StripEdge::None;
    }
}

void IslandWindow::DrawCurrent() {
    const RECT r = CurrentWindowRect();
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    const HDC screen = GetDC(nullptr);
    if (m_animating) {
        // 形变途中只画外壳：细条与胶囊互转时，头像/状态点/文本会突然挤出来。
        m_render.DrawPill(m_hwnd, screen, r.left, r.top, w, h, m_scale, m_style);
    } else if (WantsStrip()) {
        m_render.DrawStrip(m_hwnd, screen, r.left, r.top, w, h, m_scale, m_style,
                           ToStripEdge(m_dockEdge));
    } else {
        m_render.Draw(m_hwnd, screen, r.left, r.top, w, h, m_scale, m_sample, m_style);
    }
    ReleaseDC(nullptr, screen);
}

void IslandWindow::OnSample() {
    m_sample = m_sampler.Get();

    int w = m_floatW;
    int h = m_floatH;
    m_render.Measure(m_scale, m_sample, w, h);
    const bool layoutChanged = (w != m_floatW || h != m_floatH || m_floatW == 0);
    m_floatW = w;
    m_floatH = h;

    if (layoutChanged) ApplyLayoutSize();
    else               DrawCurrent();
}

// 首次落位：先把窗口真正摆到（已夹回工作区的）期望落点，再按停靠状态决定形态。
// 之所以要真摆一次，是因为 RectForState 以「当前窗口矩形」为基准（dsh-pet 同口径）。
void IslandWindow::ApplyInitialGeometry() {
    const RECT work = WorkAreaOfWindow();
    const int wl = static_cast<int>(work.left);
    const int wt = static_cast<int>(work.top);
    const int wr = static_cast<int>(work.right);
    const int wb = static_cast<int>(work.bottom);

    const int capW = std::max(m_floatW, 1);
    const int capH = std::max(m_floatH, 1);

    // 配置里的坐标可能来自已拔掉的显示器 → 先夹回工作区
    const int x = ClampInt(static_cast<int>(m_targetRect.left), wl, wr - capW + 1);
    const int y = ClampInt(static_cast<int>(m_targetRect.top), wt, wb - capH + 1);

    KillTimer(m_hwnd, kAnimTimerId);
    m_animating = false;
    m_targetRect = RECT{ x, y, x + capW, y + capH };
    SetWindowRect(m_targetRect, SWP_NOACTIVATE | SWP_NOREDRAW);

    if (m_dockEdge != DockEdge::None && m_cfg.edgeDock && !m_opt.noPersist) {
        // 启动即恢复停靠形态（dsh-pet 的 _apply_position 同口径）
        m_view      = ViewMode::Docked;
        m_hoverPeek = false;
    } else {
        m_view = ViewMode::Normal;
    }
    m_targetRect = StateRect(m_view == ViewMode::Docked ? GeoState::Docked : GeoState::Normal);
    SetWindowRect(m_targetRect, SWP_NOACTIVATE | SWP_NOREDRAW);
    DrawCurrent();
}

// 松手落点判定：四边等权取最近，<=40 逻辑像素才停靠（dsh-pet dock_edge_for）。
// 选屏按「松手时光标所在显示器」，不按窗口左上角 —— 跨屏拖拽时后者会把窗口夹回原屏，
// 表现成"拖过去又弹回来"（dsh-pet issue #154 记过这个坑）。
void IslandWindow::SnapReleaseToEdge(POINT screenPt) {
    const RECT work = WorkAreaOfPoint(screenPt);
    const int wl = static_cast<int>(work.left);
    const int wt = static_cast<int>(work.top);
    const int wr = static_cast<int>(work.right);
    const int wb = static_cast<int>(work.bottom);

    const int capW = std::max(m_floatW, 1);
    const int capH = std::max(m_floatH, 1);

    // 先夹回工作区，让「当前位置」成为后续形态计算的基准（dsh-pet 松手先 _clamp_rect）。
    const int x = ClampInt(screenPt.x - m_dragOffset.x, wl, wr - capW + 1);
    const int y = ClampInt(screenPt.y - m_dragOffset.y, wt, wb - capH + 1);
    m_animating  = false;
    m_targetRect = RECT{ x, y, x + capW, y + capH };
    SetWindowRect(m_targetRect, SWP_NOACTIVATE | SWP_NOREDRAW);

    const int threshold = std::max(1, static_cast<int>(std::lround(kDockThreshold * m_scale)));
    const struct { DockEdge edge; int dist; } cand[] = {
        { DockEdge::Top,    std::abs(y - wt) },
        { DockEdge::Bottom, std::abs(wb - (y + capH)) },
        { DockEdge::Left,   std::abs(x - wl) },
        { DockEdge::Right,  std::abs(wr - (x + capW)) },
    };
    int      bestDist = cand[0].dist;
    DockEdge bestEdge = cand[0].edge;
    for (int i = 1; i < 4; ++i) {
        if (cand[i].dist < bestDist) {
            bestDist = cand[i].dist;
            bestEdge = cand[i].edge;
        }
    }

    const bool wantDock = m_cfg.edgeDock && bestDist <= threshold;
    m_dockEdge  = wantDock ? bestEdge : DockEdge::None;
    m_hoverPeek = false;

    const GeoState st = wantDock ? GeoState::Docked : GeoState::Normal;
    m_targetRect = StateRect(st);

    SaveConfig(true, true);   // x/y 与 dock_edge 一起落盘（与 dsh-pet 一致）
    RequestState(st);
}

// 拖动开始：清掉动画与悬停计时器，恢复胶囊尺寸，并把拖动基准重算到当前光标。
void IslandWindow::BeginDrag() {
    m_dragging  = true;
    m_hoverPeek = false;
    m_view      = ViewMode::Normal;

    KillTimer(m_hwnd, kAnimTimerId);
    KillTimer(m_hwnd, kDockBackTimerId);
    m_animating = false;

    const int capW = std::max(m_floatW, 1);
    const int capH = std::max(m_floatH, 1);
    const RECT cur = CurrentWindowRect();
    const RECT work = WorkAreaOfPoint(POINT{ cur.left, cur.top });
    const int x = ClampInt(static_cast<int>(cur.left), static_cast<int>(work.left),
                           static_cast<int>(work.right) - capW + 1);
    const int y = ClampInt(static_cast<int>(cur.top), static_cast<int>(work.top),
                           static_cast<int>(work.bottom) - capH + 1);

    m_targetRect = RECT{ x, y, x + capW, y + capH };
    SetWindowRect(m_targetRect, SWP_NOACTIVATE | SWP_NOREDRAW);

    // 窗口刚变了尺寸/位置 → 用屏幕光标重算偏移，否则第一帧会跳一下
    POINT cursor{};
    GetCursorPos(&cursor);
    m_dragOffset = POINT{ cursor.x - x, cursor.y - y };
    DrawCurrent();
}

void IslandWindow::StopDrag() {
    m_dragging = false;
    m_pressed  = false;
    ReleaseCapture();
}

void IslandWindow::SaveConfig(bool position, bool edge) {
    if (m_opt.noPersist) return;
    IslandConfig cfg = m_cfg;
    // 落盘的是「窗口左上角」（dsh-pet 的 self.pos() 口径）：停靠时就是细条的位置，
    // 启动时 ApplyInitialGeometry 先摆到该点再算形态，因此不会逐次漂移。
    cfg.pos          = POINT{ m_targetRect.left, m_targetRect.top };
    cfg.edge         = m_dockEdge;
    cfg.edgeDock     = m_cfg.edgeDock;   // 保留用户在原配置里的开关
    cfg.rtssAutostart = m_rtssAutostart;

    std::string error;
    const bool ok = SaveIslandConfig(cfg, position, edge, false, std::string(), false,
                                     std::string(), -1.0f, &error);
    if (ok) {
        m_cfg.pos         = cfg.pos;
        m_cfg.edge        = cfg.edge;
        m_cfg.rtssAutostart = cfg.rtssAutostart;
    }
}


void IslandWindow::SetPassthrough(bool on) {
    m_passthrough = on;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT;
    else    ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    lstrcpynW(m_nid.szTip,
              on ? L"资源岛 · 鼠标穿透已开启（拖动不可用）"
                 : L"资源岛 · CPU / 内存 / GPU / FPS",
              ARRAYSIZE(m_nid.szTip));
    m_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void IslandWindow::TogglePassthrough() {
    SetPassthrough(!m_passthrough);
}

void IslandWindow::ToggleVisible() {
    if (IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
    } else {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ApplyLayoutSize();
    }
}

void IslandWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (m_passthrough ? MF_CHECKED : MF_UNCHECKED), kMenuPassthrough,
                L"鼠标穿透\tCtrl+Alt+I");
    AppendMenuW(menu, MF_STRING, kMenuShowHide, IsWindowVisible(m_hwnd) ? L"隐藏" : L"显示");
    AppendMenuW(menu, MF_STRING, kMenuRefresh, L"立即刷新");
    AppendMenuW(menu, MF_STRING | (m_rtssAutostart ? MF_CHECKED : MF_UNCHECKED), kMenuRtss,
                L"RTSS 不在时自动启动（会弹 UAC）");
    if (m_dockEdge != DockEdge::None) {
        AppendMenuW(menu, MF_STRING, kMenuUndock, L"取消贴边");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_hwnd, nullptr));
    DestroyMenu(menu);

    switch (cmd) {
        case kMenuPassthrough: TogglePassthrough(); break;
        case kMenuShowHide:    ToggleVisible(); break;
        case kMenuRefresh:     m_sampler.Wake(); break;
        case kMenuRtss:
            m_rtssAutostart = !m_rtssAutostart;
            SaveConfig(false, false);
            m_sampler.SetRtssAutostart(m_rtssAutostart);
            if (m_rtssAutostart) m_sampler.Wake();
            break;
        case kMenuUndock:
            m_dockEdge  = DockEdge::None;
            m_hoverPeek = false;
            SaveConfig(false, true);
            RequestState(GeoState::Normal);
            break;
        case kMenuQuit:        DestroyWindow(m_hwnd); break;
        default: break;
    }
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
}

void IslandWindow::InitTray() {
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = m_hwnd;
    m_nid.uID              = kTrayId;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = kTrayCallback;
    m_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(m_nid.szTip, L"资源岛 · CPU / 内存 / GPU / FPS", ARRAYSIZE(m_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &m_nid);
}

void IslandWindow::InitHotkey() {
    RegisterHotKey(m_hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'I');
}

void IslandWindow::Shutdown() {
    KillTimer(m_hwnd, kAnimTimerId);
    KillTimer(m_hwnd, kDockBackTimerId);
    m_sampler.Stop();
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    UnregisterHotKey(m_hwnd, kHotkeyId);
    m_render.Release();
}

LRESULT IslandWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kMsgSampleReady:
            OnSample();
            return 0;

        case kMsgRtssDeclined:
            // 用户在上次 UAC 上点了「否」：把自动拉起关掉并落盘，免得每次开机都弹
            m_rtssAutostart = false;
            SaveConfig(false, false);
            return 0;

        case WM_TIMER:
            if (wp == kAnimTimerId) {
                StepAnimation();
            } else if (wp == kDockBackTimerId) {
                KillTimer(m_hwnd, kDockBackTimerId);
                if (m_dockEdge != DockEdge::None && m_view != ViewMode::Normal && m_hoverPeek) {
                    m_hoverPeek = false;             // 800ms 到了：滑出态收回细条
                    RequestState(GeoState::Docked);
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            // 首次移动注册 TME_LEAVE，之后靠 WM_MOUSELEAVE 触发收回
            if (!m_tracking) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize    = sizeof(tme);
                tme.dwFlags   = TME_LEAVE;
                tme.hwndTrack = m_hwnd;
                if (TrackMouseEvent(&tme)) m_tracking = true;
            }

            if (m_pressed) {
                if (!m_dragging) {
                    const int dx = std::abs(GET_X_LPARAM(lp) - m_pressPt.x);
                    const int dy = std::abs(GET_Y_LPARAM(lp) - m_pressPt.y);
                    if (dx + dy >= m_dragThreshold) BeginDrag();
                }
                if (m_dragging) {
                    // 必须用屏幕坐标：窗口正被光标拖着走，用客户区坐标会自反馈
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    SetWindowPos(m_hwnd, HWND_TOPMOST, cursor.x - m_dragOffset.x,
                                 cursor.y - m_dragOffset.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
                    DrawCurrent();
                }
            } else if (m_dockEdge != DockEdge::None && m_view != ViewMode::Normal &&
                       !m_hoverPeek && !m_animating) {
                // 鼠标进入细条 → 滑出完整胶囊（dsh-pet enterEvent）。
                // 注意不动 m_view：悬停只是停靠态下的一种形态，收回后还要变回细条。
                KillTimer(m_hwnd, kDockBackTimerId);
                m_hoverPeek = true;
                StartAnimTo(GeoState::Peek, true);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            m_tracking = false;
            if (!m_pressed && !m_dragging && m_dockEdge != DockEdge::None && m_hoverPeek) {
                SetTimer(m_hwnd, kDockBackTimerId, kDockBackMs, nullptr);   // 800ms 后收回
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (m_passthrough) return 0;
            SetCapture(m_hwnd);          // 拖出窗口范围后仍要收到 mousemove / buttonup
            m_pressed    = true;
            m_dragging   = false;
            m_pressPt    = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            m_dragOffset = m_pressPt;    // 无边框 → 客户区原点就是窗口左上角
            return 0;

        case WM_LBUTTONUP:
            if (!m_pressed) return 0;
            if (m_dragging) {
                POINT cursor{};
                GetCursorPos(&cursor);
                StopDrag();
                SnapReleaseToEdge(cursor);   // 落点判定用松手时的屏幕光标
            } else {
                StopDrag();
            }
            return 0;

        case WM_CAPTURECHANGED:          // 被别的窗口抢走捕获：按取消拖拽处理
            m_dragging = false;
            m_pressed  = false;
            return 0;

        case WM_SHOWWINDOW:
            m_sampler.SetPaused(IsWindowVisible(m_hwnd) == FALSE);
            if (wp) ApplyLayoutSize();
            return 0;

        case WM_HOTKEY:
            if (static_cast<int>(wp) == kHotkeyId) TogglePassthrough();
            return 0;

        case kTrayCallback:
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    break;
                case WM_LBUTTONUP:
                    ToggleVisible();
                    break;
                default:
                    break;
            }
            return 0;

        case WM_DPICHANGED: {
            // 每显示器 DPI：先按系统建议矩形落位（自由态才跟），再按新 DPI 重算排版与形态
            const RECT* suggest = reinterpret_cast<const RECT*>(lp);
            m_scale = DpiScaleOf(m_hwnd);
            if (!m_animating && !m_dragging) {
                int w = m_floatW;
                int h = m_floatH;
                m_render.Measure(m_scale, m_sample, w, h);
                m_floatW = w;
                m_floatH = h;
                if (suggest && m_dockEdge == DockEdge::None) {
                    SetWindowRect(*suggest, SWP_NOACTIVATE | SWP_NOREDRAW);
                }
                m_targetRect =
                    StateRect(WantsStrip() ? GeoState::Docked : GeoState::Normal);
                SetWindowRect(m_targetRect, SWP_NOACTIVATE | SWP_NOREDRAW);
            }
            OnSample();
            return 0;
        }

        case WM_DISPLAYCHANGE:
            m_scale = DpiScaleOf(m_hwnd);
            OnSample();
            return 0;

        case WM_SETTINGCHANGE:
            ReloadStyle();
            OnSample();
            return 0;

        case WM_CLOSE:
        case WM_ENDSESSION:
            DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            Shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

int IslandWindow::Run(HINSTANCE hInst, const Options& opt) {
    m_inst = hInst;
    m_opt  = opt;

    ReloadStyle();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &IslandWindow::Thunk;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) return 1;

    m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                             kWndClass, L"资源岛", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hInst, this);
    if (!m_hwnd) {
        UnregisterClassW(kWndClass, hInst);
        return 1;
    }

    m_scale = DpiScaleOf(m_hwnd);
    m_dragThreshold = std::max(2, static_cast<int>(std::lround(4.0f * m_scale)));
    m_render.Measure(m_scale, m_sample, m_floatW, m_floatH);
    Reposition();
    ApplyInitialGeometry();

    InitTray();
    InitHotkey();
    if (m_opt.passthrough) SetPassthrough(true);

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    m_sampler.SetGpuEnabled(!m_opt.noGpu);

    // RTSS 自动拉起：配置里的 rtss_autostart 为准，启动 3 秒后在采样线程里试一次
    m_rtssAutostart = !m_opt.noPersist ? (m_cfg.pathOk ? m_cfg.rtssAutostart : true) : false;
    m_sampler.SetRtssAutostart(m_rtssAutostart);
    m_sampler.SetRtssDeclinedHandler(&OnRtssDeclined, m_hwnd);

    m_sampler.Start(&OnSampleReady, m_hwnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    m_render.Release();
    UnregisterClassW(kWndClass, hInst);
    return 0;
}

}  // namespace

bool ParseIslandOption(const wchar_t* arg, Options& opt) {
    if (!arg) return false;
    const auto has = [arg](const wchar_t* prefix) {
        return wcsncmp(arg, prefix, wcslen(prefix)) == 0;
    };

    if (has(L"--passthrough"))  { opt.passthrough = true; return true; }
    if (has(L"--no-gpu"))       { opt.noGpu = true; return true; }
    if (has(L"--attach-pet"))   { opt.attachPet = true; return true; }
    if (has(L"--no-restore"))   { opt.noPersist = true; return true; }
    if (has(L"--monitor="))     { opt.monitorIndex = _wtoi(arg + 10); return true; }
    if (has(L"--offset-y="))    { opt.offsetY = _wtoi(arg + 11); return true; }
    if (has(L"--opacity="))     { opt.opacity = static_cast<float>(_wtof(arg + 10)); return true; }
    if (has(L"--x="))           { opt.x = _wtoi(arg + 4); opt.hasX = true; return true; }
    if (has(L"--y="))           { opt.y = _wtoi(arg + 4); opt.hasY = true; return true; }
    return false;
}

int RunResourceIsland(HINSTANCE hInst, const Options& opt) {
    IslandWindow window;
    return window.Run(hInst, opt);
}
