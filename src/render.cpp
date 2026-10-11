#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "avatar_data.h"

namespace {

// ---- 逻辑单位（96 DPI 为 1.0）----
constexpr float kHeightLogical = 36.0f;
constexpr float kRadiusLogical = 18.0f;
constexpr float kPadXLogical   = 16.0f;
constexpr float kGapLabel      = 7.0f;    // 标签 → 数值
constexpr float kGapValue      = 6.0f;    // 数值 → 副值
constexpr float kSepGap        = 10.0f;   // 分隔线左右留白
constexpr float kSepInsetY     = 8.0f;    // 分隔线上下内缩
constexpr float kSepWidth      = 1.0f;
constexpr float kLabelPx       = 11.0f;
constexpr float kValuePx       = 14.0f;
constexpr float kSubPx         = 11.0f;

enum RunFont { RF_LABEL = 0, RF_VALUE = 1, RF_SUB = 2 };

struct Run {
    std::wstring text;
    int          font      = RF_LABEL;
    bool         sepBefore = false;
};

struct LayoutEntry {
    float x = 0.0f;
    float w = 0.0f;
};

struct Layout {
    std::vector<LayoutEntry> entries;
    std::vector<float>       seps;
    float                    width  = 0.0f;
    float                    height = 0.0f;
};

// 圆角矩形有符号距离场 → 覆盖率（1px 抗锯齿）
inline float RoundedCoverage(float px, float py, float w, float h, float radius) {
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    float r = radius;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    if (r < 0.0f) r = 0.0f;

    const float qx = std::fabs(px - hw) - (hw - r);
    const float qy = std::fabs(py - hh) - (hh - r);
    const float ax = qx > 0.0f ? qx : 0.0f;
    const float ay = qy > 0.0f ? qy : 0.0f;
    const float d  = std::sqrt(ax * ax + ay * ay) + std::fmin(std::fmax(qx, qy), 0.0f) - r;
    const float c  = 0.5f - d;
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

inline uint32_t LerpColor(COLORREF a, COLORREF b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float ar = static_cast<float>(GetRValue(a));
    const float ag = static_cast<float>(GetGValue(a));
    const float ab = static_cast<float>(GetBValue(a));
    const float br = static_cast<float>(GetRValue(b));
    const float bg = static_cast<float>(GetGValue(b));
    const float bb = static_cast<float>(GetBValue(b));
    const uint32_t r = static_cast<uint32_t>(ar + (br - ar) * t + 0.5f);
    const uint32_t g = static_cast<uint32_t>(ag + (bg - ag) * t + 0.5f);
    const uint32_t bl = static_cast<uint32_t>(ab + (bb - ab) * t + 0.5f);
    return (r << 16) | (g << 8) | bl;
}

// ---- 细条要一层层叠（底条 → 亮线 → 头像 → 状态点），用预乘 RGBA 做 alpha 合成 ----
struct Premul {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

inline void BlendOver(Premul& dst, float sr, float sg, float sb, float sa) {
    if (sa <= 0.0f) return;
    if (sa > 1.0f) sa = 1.0f;
    const float inv = 1.0f - sa;
    dst.r = sr * sa + dst.r * inv;
    dst.g = sg * sa + dst.g * inv;
    dst.b = sb * sa + dst.b * inv;
    dst.a = sa + dst.a * inv;
}

// 椭圆覆盖率（1px 抗锯齿），坐标为像素中心
inline float EllipseCoverage(float px, float py, float x0, float y0, float x1, float y1) {
    const float rx = (x1 - x0) * 0.5f;
    const float ry = (y1 - y0) * 0.5f;
    if (rx <= 0.0f || ry <= 0.0f) return 0.0f;
    const float dx = (px - (x0 + rx)) / rx;
    const float dy = (py - (y0 + ry)) / ry;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float c = (1.0f - len) * std::fmin(rx, ry) + 0.5f;
    return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
}

inline void SplitColor(COLORREF c, float& r, float& g, float& b) {
    r = static_cast<float>(GetRValue(c)) / 255.0f;
    g = static_cast<float>(GetGValue(c)) / 255.0f;
    b = static_cast<float>(GetBValue(c)) / 255.0f;
}

inline float ClampOpacity(float v) {
    if (v < 0.15f) return 0.15f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline uint32_t PackPremul(const Premul& c) {
    if (c.a <= 0.0f) return 0u;
    int a = static_cast<int>(c.a * 255.0f + 0.5f);
    int r = static_cast<int>(c.r * 255.0f + 0.5f);
    int g = static_cast<int>(c.g * 255.0f + 0.5f);
    int b = static_cast<int>(c.b * 255.0f + 0.5f);
    if (a > 255) a = 255;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

std::wstring FmtPercent(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    wchar_t buf[16];
    swprintf_s(buf, L"%.0f%%", static_cast<double>(v));
    return buf;
}

std::wstring FmtGHz(float mhz) {
    if (mhz <= 0.5f) return L"--";
    wchar_t buf[24];
    swprintf_s(buf, L"%.1fGHz", static_cast<double>(mhz) / 1000.0);
    return buf;
}

std::wstring FmtPair(uint64_t used, uint64_t total) {
    if (total == 0) return L"--";
    constexpr double kGiB = 1073741824.0;
    wchar_t buf[40];
    swprintf_s(buf, L"%.1f/%.1fG", static_cast<double>(used) / kGiB,
               static_cast<double>(total) / kGiB);
    return buf;
}

std::wstring FmtGpuSub(const GpuMetrics& g) {
    if (!g.available) return L"--";
    const std::wstring pair = FmtPair(g.usedBytes, g.totalBytes);
    if (g.clockMHz <= 0.5f) return pair;
    wchar_t buf[64];
    swprintf_s(buf, L"%ls %.0fM", pair.c_str(), static_cast<double>(g.clockMHz));
    return buf;
}

// 帧时间带一位小数；>=100ms 时小数没有意义，直接取整。
std::wstring FmtFrameMs(float ms) {
    if (ms <= 0.0f) return L"--";
    wchar_t buf[24];
    if (ms >= 100.0f) swprintf_s(buf, L"%.0fms", static_cast<double>(ms));
    else              swprintf_s(buf, L"%.1fms", static_cast<double>(ms));
    return buf;
}

std::vector<Run> BuildRuns(const SystemSample& s) {
    std::vector<Run> runs;
    runs.reserve(12);

    runs.push_back({ L"CPU", RF_LABEL, false });
    runs.push_back({ FmtPercent(s.cpu.usagePercent), RF_VALUE, false });
    runs.push_back({ FmtGHz(s.cpu.freqMHz), RF_SUB, false });

    runs.push_back({ L"MEM", RF_LABEL, true });
    runs.push_back({ FmtPercent(s.mem.usagePercent), RF_VALUE, false });
    runs.push_back({ FmtPair(s.mem.usedBytes, s.mem.totalBytes), RF_SUB, false });

    runs.push_back({ L"GPU", RF_LABEL, true });
    runs.push_back({ s.gpu.available ? FmtPercent(s.gpu.usagePercent) : std::wstring(L"--"),
                     RF_VALUE, false });
    runs.push_back({ FmtGpuSub(s.gpu), RF_SUB, false });

    // FPS 段只在真的拿到游戏帧率时出现：没有 RTSS / 没有游戏时整段隐藏，
    // 不让胶囊常驻一个 "--" 占位。
    if (s.fps.available) {
        runs.push_back({ L"FPS", RF_LABEL, true });
        runs.push_back({ FmtPercent(s.fps.fps), RF_VALUE, false });
        runs.push_back({ FmtFrameMs(s.fps.frameMs), RF_SUB, false });
    }

    return runs;
}

void ComputeLayout(HDC dc, const HFONT* fonts, float scale, const std::vector<Run>& runs,
                   Layout& out) {
    out.entries.clear();
    out.seps.clear();
    out.entries.reserve(runs.size());

    float x = kPadXLogical * scale;
    for (const Run& r : runs) {
        if (r.sepBefore) {
            x += kSepGap * scale;
            out.seps.push_back(x);
            x += kSepWidth * scale;
            x += kSepGap * scale;
        } else if (r.font == RF_VALUE) {
            x += kGapLabel * scale;
        } else if (r.font == RF_SUB) {
            x += kGapValue * scale;
        }

        const HGDIOBJ old = SelectObject(dc, fonts[r.font]);
        SIZE sz{};
        GetTextExtentPoint32W(dc, r.text.c_str(), static_cast<int>(r.text.size()), &sz);
        SelectObject(dc, old);

        out.entries.push_back({ x, static_cast<float>(sz.cx) });
        x += static_cast<float>(sz.cx);
    }

    out.width  = x + kPadXLogical * scale;
    out.height = kHeightLogical * scale;
}

}  // namespace

IslandRenderer::~IslandRenderer() {
    Release();
}

void IslandRenderer::EnsureFonts(float scale) {
    const int key = static_cast<int>(std::lround(scale * 1000.0f));
    if (key == m_fontKey && m_fonts[0]) return;

    for (HFONT& f : m_fonts) {
        if (f) { DeleteObject(f); f = nullptr; }
    }
    if (!m_measureDc) m_measureDc = CreateCompatibleDC(nullptr);

    const wchar_t* face = L"Segoe UI";
    const auto make = [&](float px, int weight) -> HFONT {
        int h = static_cast<int>(std::lround(px * scale));
        if (h < 6) h = 6;
        return CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, face);
    };
    m_fonts[RF_LABEL] = make(kLabelPx, FW_SEMIBOLD);
    m_fonts[RF_VALUE] = make(kValuePx, FW_BOLD);
    m_fonts[RF_SUB]   = make(kSubPx, FW_NORMAL);
    m_fontKey = key;
}

void IslandRenderer::DestroyDibs() {
    if (m_dc) {
        if (m_origBmp) SelectObject(m_dc, m_origBmp);
        DeleteDC(m_dc);
        m_dc = nullptr;
    }
    if (m_bmp) { DeleteObject(m_bmp); m_bmp = nullptr; }
    if (m_textDc) {
        if (m_origText) SelectObject(m_textDc, m_origText);
        DeleteDC(m_textDc);
        m_textDc = nullptr;
    }
    if (m_textBmp) { DeleteObject(m_textBmp); m_textBmp = nullptr; }

    m_bits = nullptr;
    m_textBits = nullptr;
    m_origBmp = nullptr;
    m_origText = nullptr;
    m_w = 0;
    m_h = 0;
}

bool IslandRenderer::EnsureDibs(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (m_w == w && m_h == h && m_bmp && m_textBmp) return true;

    DestroyDibs();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    const HDC screen = GetDC(nullptr);
    m_dc      = CreateCompatibleDC(screen);
    m_bmp     = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    m_textDc  = CreateCompatibleDC(screen);
    m_textBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &m_textBits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!m_dc || !m_bmp || !m_bits || !m_textDc || !m_textBmp || !m_textBits) {
        DestroyDibs();
        return false;
    }

    m_origBmp  = SelectObject(m_dc, m_bmp);
    m_origText = SelectObject(m_textDc, m_textBmp);
    m_w = w;
    m_h = h;
    return true;
}

void IslandRenderer::Measure(float scale, const SystemSample& s, int& w, int& h) {
    EnsureFonts(scale);
    const std::vector<Run> runs = BuildRuns(s);
    Layout layout;
    ComputeLayout(m_measureDc, m_fonts, scale, runs, layout);
    w = static_cast<int>(std::ceil(layout.width));
    h = static_cast<int>(std::ceil(layout.height));
}

void IslandRenderer::Draw(HWND hwnd, HDC screen, int x, int y, int w, int h, float scale,
                          const SystemSample& s, const IslandStyle& style) {
    if (w <= 0 || h <= 0) return;
    EnsureFonts(scale);
    if (!EnsureDibs(w, h)) return;

    const std::vector<Run> runs = BuildRuns(s);
    Layout layout;
    ComputeLayout(m_measureDc, m_fonts, scale, runs, layout);

    // ---------- 文本层：以不透明底色绘制文字与分隔线，只取回 RGB ----------
    HBRUSH bgBrush = CreateSolidBrush(style.fill);
    RECT full{ 0, 0, w, h };
    FillRect(m_textDc, &full, bgBrush);
    DeleteObject(bgBrush);

    const float sepHalf = static_cast<float>(h) * 0.5f - kSepInsetY * scale;
    if (sepHalf > 1.0f) {
        HBRUSH sepBrush = CreateSolidBrush(style.edge);
        for (float sx : layout.seps) {
            RECT r{};
            r.left   = static_cast<LONG>(std::lround(sx));
            r.right  = static_cast<LONG>(std::lround(sx + kSepWidth * scale)) + 1;
            r.top    = static_cast<LONG>(std::lround(static_cast<float>(h) * 0.5f - sepHalf));
            r.bottom = static_cast<LONG>(std::lround(static_cast<float>(h) * 0.5f + sepHalf));
            FillRect(m_textDc, &r, sepBrush);
        }
        DeleteObject(sepBrush);
    }

    SetBkMode(m_textDc, OPAQUE);
    SetBkColor(m_textDc, style.fill);
    for (size_t i = 0; i < runs.size(); ++i) {
        COLORREF color = style.textDim;
        if (runs[i].font == RF_LABEL)      color = style.accent;
        else if (runs[i].font == RF_VALUE) color = style.textMain;
        SetTextColor(m_textDc, color);

        RECT r{};
        r.left   = static_cast<LONG>(std::lround(layout.entries[i].x));
        r.right  = static_cast<LONG>(std::lround(layout.entries[i].x + layout.entries[i].w)) + 2;
        r.top    = 0;
        r.bottom = h;

        const HGDIOBJ oldFont = SelectObject(m_textDc, m_fonts[runs[i].font]);
        DrawTextW(m_textDc, runs[i].text.c_str(), static_cast<int>(runs[i].text.size()), &r,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_LEFT);
        SelectObject(m_textDc, oldFont);
    }
    GdiFlush();

    // ---------- 合成：胶囊底 + 文字，最后预乘 alpha ----------
    const float W = static_cast<float>(w);
    const float H = static_cast<float>(h);
    const float radius = kRadiusLogical * scale;
    const float edgePx = 1.0f * scale;

    float opacity = style.opacity;
    if (opacity < 0.15f) opacity = 0.15f;
    if (opacity > 1.0f) opacity = 1.0f;

    auto* dst = static_cast<uint32_t*>(m_bits);
    const auto* src = static_cast<const uint32_t*>(m_textBits);

    for (int py = 0; py < h; ++py) {
        const float fy = static_cast<float>(py) + 0.5f;
        for (int px = 0; px < w; ++px) {
            const float fx = static_cast<float>(px) + 0.5f;
            const float c1 = RoundedCoverage(fx, fy, W, H, radius);
            uint32_t out = 0;
            if (c1 > 0.0015f) {
                const float c2 = RoundedCoverage(fx, fy, W - 2.0f * edgePx, H - 2.0f * edgePx,
                                                 radius - edgePx);
                int a = static_cast<int>(c1 * 255.0f + 0.5f);
                if (a > 255) a = 255;
                if (opacity < 1.0f) {
                    a = static_cast<int>(static_cast<float>(a) * opacity + 0.5f);
                    if (a > 255) a = 255;
                }

                uint32_t rgb;
                if (c2 >= 0.999f) {
                    rgb = src[py * w + px] & 0x00FFFFFFu;   // 纯内部：取文字层 RGB
                } else {
                    rgb = LerpColor(style.edge, style.fill, c2);   // 边缘描边 → 填充
                }

                const uint32_t r = (rgb >> 16) & 0xFFu;
                const uint32_t g = (rgb >> 8) & 0xFFu;
                const uint32_t b = rgb & 0xFFu;
                const uint32_t a32 = static_cast<uint32_t>(a);
                out = (a32 << 24) |
                      ((r * a32 / 255u) << 16) |
                      ((g * a32 / 255u) << 8) |
                      (b * a32 / 255u);
            }
            dst[py * w + px] = out;
        }
    }

    Present(hwnd, screen, x, y, w, h);
}

void IslandRenderer::Present(HWND hwnd, HDC screen, int x, int y, int w, int h) {
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    POINT dstPt{ x, y };
    POINT srcPt{ 0, 0 };
    SIZE  size{ w, h };
    UpdateLayeredWindow(hwnd, screen, &dstPt, &size, m_dc, &srcPt, 0, &bf, ULW_ALPHA);
}

// 形变途中：只画胶囊外形（圆角 + 描边），不画任何内容。
// 细条 ↔ 胶囊互转时，头像/状态点/文本会突然挤出或突然消失，中间帧只保留外壳最稳。
void IslandRenderer::DrawPill(HWND hwnd, HDC screen, int x, int y, int w, int h, float scale,
                              const IslandStyle& style) {
    if (w <= 0 || h <= 0) return;
    if (!EnsureDibs(w, h)) return;

    const float W      = static_cast<float>(w);
    const float H      = static_cast<float>(h);
    const float radius = std::fmin(kRadiusLogical * scale, std::fmin(W, H) * 0.5f);
    const float edgePx = 1.0f * scale;
    const float opacity = ClampOpacity(style.opacity);

    auto* dst = static_cast<uint32_t*>(m_bits);
    for (int py = 0; py < h; ++py) {
        const float fy = static_cast<float>(py) + 0.5f;
        for (int px = 0; px < w; ++px) {
            const float fx = static_cast<float>(px) + 0.5f;
            const float c1 = RoundedCoverage(fx, fy, W, H, radius);
            uint32_t out = 0u;
            if (c1 > 0.0015f) {
                const float c2 = RoundedCoverage(fx, fy, W - 2.0f * edgePx, H - 2.0f * edgePx,
                                                 radius - edgePx);
                const uint32_t rgb = LerpColor(style.edge, style.fill, c2);
                int a = static_cast<int>(c1 * opacity * 255.0f + 0.5f);
                if (a > 255) a = 255;
                const uint32_t a32 = static_cast<uint32_t>(a);
                out = (a32 << 24) |
                      ((((rgb >> 16) & 0xFFu) * a32 / 255u) << 16) |
                      ((((rgb >> 8) & 0xFFu) * a32 / 255u) << 8) |
                      ((rgb & 0xFFu) * a32 / 255u);
            }
            dst[py * w + px] = out;
        }
    }

    Present(hwnd, screen, x, y, w, h);
}

// 停靠细条：圆角条（不描边）+ 靠屏侧 2px 主题色亮线 + 状态点；左右竖条再带上头像。
// 几何与配色对齐 dsh-pet 的 _paint_strip：亮线两端内缩 6 逻辑像素、alpha 220；
// 头像 16 逻辑像素见方、贴顶 8 逻辑像素；状态点直径 8 逻辑像素，竖条贴底内缩 8、
// 横条居中。底色仍用本岛自己的 style.fill，以求与胶囊一致。
void IslandRenderer::DrawStrip(HWND hwnd, HDC screen, int x, int y, int w, int h, float scale,
                               const IslandStyle& style, StripEdge edge) {
    if (w <= 0 || h <= 0) return;
    if (!EnsureDibs(w, h)) return;

    const float W       = static_cast<float>(w);
    const float H       = static_cast<float>(h);
    const float radius  = std::fmin(W, H) * 0.5f;   // 细条两端全圆
    const float opacity = ClampOpacity(style.opacity);

    float fr, fg, fb;
    float ar, ag, ab;
    float dr, dg, db;
    SplitColor(style.fill, fr, fg, fb);
    SplitColor(style.accent, ar, ag, ab);
    SplitColor(style.dot, dr, dg, db);

    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<Premul> layer(count);
    std::vector<float>  cover(count, 0.0f);

    // 1) 底条
    for (int py = 0; py < h; ++py) {
        const float fy = static_cast<float>(py) + 0.5f;
        for (int px = 0; px < w; ++px) {
            const float c = RoundedCoverage(static_cast<float>(px) + 0.5f, fy, W, H, radius);
            cover[static_cast<size_t>(py) * w + px] = c;
            if (c > 0.0015f) {
                BlendOver(layer[static_cast<size_t>(py) * w + px], fr, fg, fb, c * opacity);
            }
        }
    }

    // 2) 靠屏侧亮线
    const float thick = 2.0f * scale;
    const float inset = 6.0f * scale;
    float lx0 = 0.0f, ly0 = 0.0f, lx1 = 0.0f, ly1 = 0.0f;
    switch (edge) {
        case StripEdge::Top:    lx0 = inset;        ly0 = 0.0f;         lx1 = W - inset;  ly1 = thick;      break;
        case StripEdge::Bottom: lx0 = inset;        ly0 = H - thick;    lx1 = W - inset;  ly1 = H;          break;
        case StripEdge::Left:   lx0 = 0.0f;         ly0 = inset;        lx1 = thick;      ly1 = H - inset;  break;
        case StripEdge::Right:  lx0 = W - thick;    ly0 = inset;        lx1 = W;          ly1 = H - inset;  break;
        case StripEdge::None:   break;
    }
    if (edge != StripEdge::None && lx1 > lx0 && ly1 > ly0) {
        const float lr  = thick * 0.5f;
        const int   px0 = std::max(0, static_cast<int>(std::floor(lx0)) - 1);
        const int   px1 = std::min(w, static_cast<int>(std::ceil(lx1)) + 1);
        const int   py0 = std::max(0, static_cast<int>(std::floor(ly0)) - 1);
        const int   py1 = std::min(h, static_cast<int>(std::ceil(ly1)) + 1);
        for (int py = py0; py < py1; ++py) {
            const float fy = static_cast<float>(py) + 0.5f;
            for (int px = px0; px < px1; ++px) {
                const size_t idx = static_cast<size_t>(py) * w + px;
                const float c = RoundedCoverage(static_cast<float>(px) + 0.5f - lx0, fy - ly0,
                                                lx1 - lx0, ly1 - ly0, lr);
                if (c <= 0.0015f) continue;
                // 乘上底条覆盖率：亮线不许探出圆头
                BlendOver(layer[idx], ar, ag, ab,
                          c * (220.0f / 255.0f) * opacity * cover[idx]);
            }
        }
    }

    // 3) 头像：只有左右竖条有
    if (edge == StripEdge::Left || edge == StripEdge::Right) {
        const int boxPx = std::max(1, static_cast<int>(std::lround(16.0f * scale)));
        int aw = 0;
        int ah = 0;
        const uint32_t* av = AvatarRgba(boxPx, aw, ah);
        if (av && aw > 0 && ah > 0) {
            const int ax = (w - aw) / 2;
            const int ay = std::max(0, static_cast<int>(std::lround(8.0f * scale)));
            for (int sy = 0; sy < ah; ++sy) {
                const int py = ay + sy;
                if (py < 0 || py >= h) continue;
                for (int sx = 0; sx < aw; ++sx) {
                    const int px = ax + sx;
                    if (px < 0 || px >= w) continue;
                    const uint32_t v = av[static_cast<size_t>(sy) * aw + sx];
                    const float sa = static_cast<float>((v >> 24) & 0xFFu) / 255.0f * opacity;
                    if (sa <= 0.002f) continue;
                    BlendOver(layer[static_cast<size_t>(py) * w + px],
                              static_cast<float>((v >> 16) & 0xFFu) / 255.0f,
                              static_cast<float>((v >> 8) & 0xFFu) / 255.0f,
                              static_cast<float>(v & 0xFFu) / 255.0f, sa);
                }
            }
        }
    }

    // 4) 状态点
    const float dotSize = 8.0f * scale;
    const float dotX    = (W - dotSize) * 0.5f;
    const float dotY    = (edge == StripEdge::Left || edge == StripEdge::Right)
                              ? (H - dotSize - 8.0f * scale)
                              : ((H - dotSize) * 0.5f);
    {
        const int px0 = std::max(0, static_cast<int>(std::floor(dotX)) - 1);
        const int px1 = std::min(w, static_cast<int>(std::ceil(dotX + dotSize)) + 1);
        const int py0 = std::max(0, static_cast<int>(std::floor(dotY)) - 1);
        const int py1 = std::min(h, static_cast<int>(std::ceil(dotY + dotSize)) + 1);
        for (int py = py0; py < py1; ++py) {
            for (int px = px0; px < px1; ++px) {
                const float c = EllipseCoverage(static_cast<float>(px) + 0.5f,
                                                static_cast<float>(py) + 0.5f,
                                                dotX, dotY, dotX + dotSize, dotY + dotSize);
                if (c <= 0.0015f) continue;
                BlendOver(layer[static_cast<size_t>(py) * w + px], dr, dg, db, c * opacity);
            }
        }
    }

    auto* dst = static_cast<uint32_t*>(m_bits);
    for (size_t i = 0; i < count; ++i) dst[i] = PackPremul(layer[i]);

    Present(hwnd, screen, x, y, w, h);
}

// 头像按 boxPx 缩放后缓存（缓存 key 就是 boxPx，DPI 变了才重算）。
// 盒式滤波：RGB 按 alpha 加权平均，否则透明像素会把边缘往黑里拉。
const uint32_t* IslandRenderer::AvatarRgba(int boxPx, int& outW, int& outH) {
    const island_asset::Image& src = island_asset::AvatarImage();
    if (boxPx <= 0 || src.w <= 0 || src.h <= 0 || !src.rgba) return nullptr;

    if (boxPx == m_avatarBox && !m_avatar.empty()) {
        outW = m_avatarW;
        outH = m_avatarH;
        return m_avatar.data();
    }

    const float s = static_cast<float>(boxPx) / static_cast<float>(std::max(src.w, src.h));
    int dw = static_cast<int>(std::lround(static_cast<float>(src.w) * s));
    int dh = static_cast<int>(std::lround(static_cast<float>(src.h) * s));
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    std::vector<uint32_t> scaled(static_cast<size_t>(dw) * static_cast<size_t>(dh), 0u);
    for (int dy = 0; dy < dh; ++dy) {
        const int sy0 = dy * src.h / dh;
        int sy1 = (dy + 1) * src.h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; ++dx) {
            const int sx0 = dx * src.w / dw;
            int sx1 = (dx + 1) * src.w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            double sumA = 0.0;
            double sumR = 0.0;
            double sumG = 0.0;
            double sumB = 0.0;
            int    n    = 0;
            for (int sy = sy0; sy < sy1 && sy < src.h; ++sy) {
                for (int sx = sx0; sx < sx1 && sx < src.w; ++sx) {
                    const unsigned char* p =
                        src.rgba + (static_cast<size_t>(sy) * src.w + sx) * 4u;
                    const double a = static_cast<double>(p[3]);
                    sumA += a;
                    sumR += static_cast<double>(p[0]) * a;
                    sumG += static_cast<double>(p[1]) * a;
                    sumB += static_cast<double>(p[2]) * a;
                    ++n;
                }
            }
            if (n <= 0 || sumA <= 0.0) continue;

            int a8 = static_cast<int>(sumA / n + 0.5);
            int r8 = static_cast<int>(sumR / sumA + 0.5);
            int g8 = static_cast<int>(sumG / sumA + 0.5);
            int b8 = static_cast<int>(sumB / sumA + 0.5);
            if (a8 > 255) a8 = 255;
            if (r8 > 255) r8 = 255;
            if (g8 > 255) g8 = 255;
            if (b8 > 255) b8 = 255;
            scaled[static_cast<size_t>(dy) * dw + dx] =
                (static_cast<uint32_t>(a8) << 24) | (static_cast<uint32_t>(r8) << 16) |
                (static_cast<uint32_t>(g8) << 8) | static_cast<uint32_t>(b8);
        }
    }

    m_avatar.swap(scaled);
    m_avatarW   = dw;
    m_avatarH   = dh;
    m_avatarBox = boxPx;
    outW = dw;
    outH = dh;
    return m_avatar.data();
}

void IslandRenderer::Release() {
    DestroyDibs();
    for (HFONT& f : m_fonts) {
        if (f) { DeleteObject(f); f = nullptr; }
    }
    m_fontKey = -1;
    if (m_measureDc) {
        DeleteDC(m_measureDc);
        m_measureDc = nullptr;
    }
}
