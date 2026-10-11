#include "fps.h"

#include <shellapi.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// 官方 SDK 结构的前缀部分（字段偏移与 RTSSSharedMemory.h 一致）。
// 完整条目 12416 字节（本机 RTSS v2.21 实测），我们只读到前 300 字节，
// 条目步长一律用运行时的 dwAppEntrySize，绝不硬编码结构体大小。

struct RtssAppEntryPrefix {
    unsigned long dwProcessID;       // 0
    char          szName[MAX_PATH];  // 4
    unsigned long dwFlags;           // 264
    unsigned long dwTime0;           // 268
    unsigned long dwTime1;           // 272
    unsigned long dwFrames;          // 276
    unsigned long dwFrameTime;       // 280 (us)
};

struct RtssHeaderPrefix {
    unsigned long dwSignature;       // 'RTSS'
    unsigned long dwVersion;         // (major<<16)+minor
    unsigned long dwAppEntrySize;
    unsigned long dwAppArrOffset;
    unsigned long dwAppArrSize;
    unsigned long dwOSDEntrySize;
    unsigned long dwOSDArrOffset;
    unsigned long dwOSDArrSize;
};

constexpr unsigned long kSigRTSS = 0x53535452;   // 'RTSS'
constexpr unsigned long kSigDead = 0x0000DEAD;   // 标记待释放，数据无效
constexpr unsigned long kMaxEntries = 256;
constexpr unsigned long kForegroundRefreshMs = 1000;

// 2^32ms 回绕安全的间隔。
inline unsigned long ElapsedMs(unsigned long t0, unsigned long t1) {
    return (t1 >= t0) ? (t1 - t0) : (0xFFFFFFFFul - t0 + t1 + 1ul);
}

const char* BaseName(const char* path) {
    const char* slash = std::strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

struct Candidate {
    unsigned long pid       = 0;
    unsigned long frames    = 0;
    unsigned long time0     = 0;
    unsigned long time1     = 0;
    unsigned long frameTime = 0;
    double        fps       = 0.0;
    const char*   name      = nullptr;
    unsigned long slot      = 0;
};

}  // namespace

// ---------------------------------------------------------------- RTSS 进程

namespace {

constexpr wchar_t kRtssMappingName[] = L"RTSSSharedMemoryV2";
constexpr wchar_t kRtssKeyWow64[] = L"SOFTWARE\\WOW6432Node\\Unwinder\\RTSS";
constexpr wchar_t kRtssKeyNative[] = L"SOFTWARE\\Unwinder\\RTSS";

bool FileExistsW(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 从注册表读 InstallPath（RTSS 安装时写下的完整 exe 路径）。
bool RtssPathFromRegistry(HKEY root, const wchar_t* subkey, std::wstring& out) {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD cb = static_cast<DWORD>(sizeof(buf));
    const LSTATUS st = RegGetValueW(root, subkey, L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &cb);
    if (st != ERROR_SUCCESS) return false;
    if (!FileExistsW(buf)) return false;
    out = buf;
    return true;
}

}  // namespace

bool RtssSharedMemoryPresent() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kRtssMappingName);
    if (!mapping) return false;
    CloseHandle(mapping);
    return true;
}

bool FindRtssPath(std::wstring& out) {
    // 1) 权威：安装时写下的 InstallPath（本机 = C:\Program Files (x86)\...\RTSS.exe）
    if (RtssPathFromRegistry(HKEY_LOCAL_MACHINE, kRtssKeyWow64, out)) return true;
    if (RtssPathFromRegistry(HKEY_LOCAL_MACHINE, kRtssKeyNative, out)) return true;

    // 2) 兜底：按各 Program Files 根拼默认安装目录
    const wchar_t* const kRootVars[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432" };
    for (const wchar_t* var : kRootVars) {
        wchar_t root[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(var, root, static_cast<DWORD>(std::size(root))) == 0) continue;
        std::wstring candidate = std::wstring(root) + L"\\RivaTuner Statistics Server\\RTSS.exe";
        if (FileExistsW(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

RtssLaunch LaunchRtss() {
    if (RtssSharedMemoryPresent()) return RtssLaunch::AlreadyRunning;

    std::wstring exe;
    if (!FindRtssPath(exe)) return RtssLaunch::NotFound;

    // 先按普通权限直接起：岛自身被提权时能成，也兼容将来改成 asInvoker 清单的版本。
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                       &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return RtssLaunch::Started;
    }
    const DWORD createErr = GetLastError();
    if (createErr != ERROR_ELEVATION_REQUIRED) return RtssLaunch::Failed;

    // 清单是 requireAdministrator：只能走 runas，会弹一次 UAC（用户点否 → ERROR_CANCELLED）。
    SHELLEXECUTEINFOW sei{};
    sei.cbSize      = sizeof(sei);
    sei.fMask       = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb      = L"runas";
    sei.lpFile      = exe.c_str();
    sei.nShow       = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        return (err == ERROR_CANCELLED) ? RtssLaunch::Declined : RtssLaunch::Failed;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return RtssLaunch::Started;
}

FpsReader::~FpsReader() = default;

void FpsReader::Collect(FpsMetrics& out) {
    out = FpsMetrics{};

    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!mapping) return;
    const unsigned char* base =
        static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!base) {
        CloseHandle(mapping);
        return;
    }

    struct Unmap {
        const unsigned char* p;
        HANDLE h;
        ~Unmap() {
            UnmapViewOfFile(p);
            CloseHandle(h);
        }
    } guard{ base, mapping };

    const auto* hdr = reinterpret_cast<const RtssHeaderPrefix*>(base);
    if (hdr->dwSignature != kSigRTSS || hdr->dwSignature == kSigDead ||
        hdr->dwAppEntrySize < sizeof(RtssAppEntryPrefix) || hdr->dwAppArrSize == 0 ||
        hdr->dwAppArrSize > kMaxEntries) {
        return;
    }

    // 前台进程：优先展示"用户正在看的那个游戏"。
    const unsigned long now = GetTickCount();
    if (ElapsedMs(m_lookupTick, now) >= kForegroundRefreshMs) {
        m_lookupTick = now;
        m_foregroundPid = 0;
        if (HWND fg = GetForegroundWindow()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            m_foregroundPid = pid;
        }
    }

    Candidate best;
    Candidate foregroundMatch;
    bool haveBest = false;
    bool haveForeground = false;

    for (unsigned long i = 0; i < hdr->dwAppArrSize; ++i) {
        const auto* e = reinterpret_cast<const RtssAppEntryPrefix*>(
            base + hdr->dwAppArrOffset + static_cast<size_t>(i) * hdr->dwAppEntrySize);
        const unsigned long pid = e->dwProcessID;
        if (pid == 0) {
            if (m_lastValid) m_last[i] = Last{};
            continue;
        }

        // 判活：与上一次采样相比，计数/时间戳/帧时间任意一项变化才算新鲜。
        bool fresh = false;
        if (m_lastValid && i < kMaxEntries) {
            const Last& prev = m_last[i];
            fresh = (e->dwFrames != prev.frames) || (e->dwTime1 != prev.time1) ||
                    (e->dwFrameTime != prev.frameTime);
        }
        if (i < kMaxEntries) {
            m_last[i] = Last{ e->dwFrames, e->dwTime1, e->dwFrameTime };
        }

        const unsigned long span = ElapsedMs(e->dwTime0, e->dwTime1);
        const double fps1s = (span > 0) ? (1000.0 * static_cast<double>(e->dwFrames) / span) : 0.0;
        const double fpsFrame =
            (e->dwFrameTime > 0) ? (1000000.0 / static_cast<double>(e->dwFrameTime)) : 0.0;
        const double fps = (fps1s > 0.5) ? fps1s : fpsFrame;
        if (!fresh || fps <= 0.5 || e->dwFrames == 0) continue;

        Candidate c;
        c.pid       = pid;
        c.frames    = e->dwFrames;
        c.time0     = e->dwTime0;
        c.time1     = e->dwTime1;
        c.frameTime = e->dwFrameTime;
        c.fps       = fps;
        c.name      = e->szName[0] ? e->szName : "?";
        c.slot      = i;

        if (pid == m_foregroundPid) {
            if (!haveForeground || c.fps > foregroundMatch.fps) {
                foregroundMatch = c;
                haveForeground  = true;
            }
        } else if (!haveBest || c.fps > best.fps) {
            best     = c;
            haveBest = true;
        }
    }
    m_lastValid = true;

    const Candidate* chosen = nullptr;
    if (haveForeground) chosen = &foregroundMatch;
    else if (haveBest)  chosen = &best;
    if (!chosen) return;

    out.available = true;
    out.fps       = static_cast<float>(chosen->fps);
    out.frameMs   = (chosen->frameTime > 0)
                        ? static_cast<float>(chosen->frameTime / 1000.0)
                        : static_cast<float>(1000.0 / chosen->fps);
    out.pid       = chosen->pid;
    std::snprintf(out.app, sizeof(out.app), "%s", BaseName(chosen->name));
}
