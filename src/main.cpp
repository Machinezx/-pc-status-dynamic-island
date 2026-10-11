#include "island.h"

#include <windows.h>
#include <shellapi.h>

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Per-Monitor DPI v2：必须在创建任何窗口之前设置
    {
        using FnSetCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            auto fn = reinterpret_cast<FnSetCtx>(
                reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DSHResourceIsland.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single);
        return 0;
    }

    Options opt;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) ParseIslandOption(argv[i], opt);
        LocalFree(argv);
    }

    const int rc = RunResourceIsland(hInst, opt);
    if (single) CloseHandle(single);
    return rc;
}
