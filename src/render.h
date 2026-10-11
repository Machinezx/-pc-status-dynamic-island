// 32bpp DIB + GDI 文字 → UpdateLayeredWindow 逐像素 alpha。
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "metrics.h"

// 停靠细条贴着屏幕的那条边：主题色亮线画在靠屏一侧（dsh-pet 同口径）。
enum class StripEdge { None, Top, Bottom, Left, Right };

struct IslandStyle {
    COLORREF fill     = RGB(0x15, 0x19, 0x20);
    COLORREF edge     = RGB(0x30, 0x38, 0x45);
    COLORREF textMain = RGB(0xF3, 0xF6, 0xFA);
    COLORREF textDim  = RGB(0x96, 0xA1, 0xB0);
    COLORREF accent   = RGB(0x4C, 0x8D, 0xF6);
    COLORREF dot      = RGB(0xFA, 0xCC, 0x15);   // 停靠细条底部的状态点（黄色）
    float    opacity  = 1.0f;
};

class IslandRenderer {
public:
    IslandRenderer() = default;
    ~IslandRenderer();

    IslandRenderer(const IslandRenderer&) = delete;
    IslandRenderer& operator=(const IslandRenderer&) = delete;

    void Measure(float scale, const SystemSample& s, int& w, int& h);

    // 正常胶囊：CPU/MEM/GPU[/FPS] 文本。
    void Draw(HWND hwnd, HDC screen, int x, int y, int w, int h, float scale,
              const SystemSample& s, const IslandStyle& style);

    // 形变途中只画胶囊外形（不画内容）：细条与胶囊互转时内容突然出现/消失太跳。
    void DrawPill(HWND hwnd, HDC screen, int x, int y, int w, int h, float scale,
                  const IslandStyle& style);

    // 停靠细条：圆角条 + 靠屏侧主题色亮线 + 状态点（左右竖条再带头像）。
    void DrawStrip(HWND hwnd, HDC screen, int x, int y, int w, int h, float scale,
                   const IslandStyle& style, StripEdge edge);

    void Release();

private:
    void EnsureFonts(float scale);
    bool EnsureDibs(int w, int h);
    void DestroyDibs();
    void Present(HWND hwnd, HDC screen, int x, int y, int w, int h);

    // 按目标像素高度缩好的角色头像（RGBA，直通 alpha）；boxPx 为主边长。
    const uint32_t* AvatarRgba(int boxPx, int& outW, int& outH);

    HDC     m_measureDc = nullptr;
    HFONT   m_fonts[3]  = { nullptr, nullptr, nullptr };
    int     m_fontKey   = -1;

    HBITMAP m_bmp      = nullptr;
    HDC     m_dc       = nullptr;
    void*   m_bits     = nullptr;
    HGDIOBJ m_origBmp  = nullptr;
    HBITMAP m_textBmp  = nullptr;
    HDC     m_textDc   = nullptr;
    void*   m_textBits = nullptr;
    HGDIOBJ m_origText = nullptr;
    int     m_w = 0;
    int     m_h = 0;

    std::vector<uint32_t> m_avatar;      // 0xAARRGGBB，已按 m_avatarBox 缩放
    int     m_avatarW   = 0;
    int     m_avatarH   = 0;
    int     m_avatarBox = 0;
};
