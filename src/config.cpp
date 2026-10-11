#include "config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const char kIslandKey[] = "\"dynamic_island\"";
constexpr DWORD kMaxConfigBytes = 4u * 1024u * 1024u;

// ---------- 极简 JSON 导航（不构造文档，只拿原始字节偏移）----------

inline size_t SkipWs(const std::string& s, size_t i) {
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i;
        else break;
    }
    return i;
}

// i 指向开口引号，返回其后字节；out 为原文切片（含转义）。
size_t ReadString(const std::string& s, size_t i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return std::string::npos;
    const size_t open = i;
    ++i;
    for (;;) {
        if (i >= s.size()) return std::string::npos;   // 字符串没闭合 → 整份 JSON 视为不可用
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == '"') {
            out.assign(s, open + 1, i - open - 1);
            return i + 1;
        }
        ++i;
    }
}

// 跳过任意一个 JSON 值，返回其结束位置。
// 注意：不能"遇到引号就 i+=2"跳转——那样在字符串出现在数组/对象里且紧邻括号时
// 会漏掉真正的开括号（自检里 whitelist 数组就踩过）。这里统一按字符扫描。
size_t SkipValue(const std::string& s, size_t i) {
    i = SkipWs(s, i);
    if (i >= s.size()) return std::string::npos;
    const char c = s[i];
    if (c == '"') {
        std::string tmp;
        return ReadString(s, i, tmp);
    }
    if (c == '{' || c == '[') {
        const char open  = c;
        const char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (i < s.size()) {
            const char ch = s[i];
            if (ch == '"') {
                std::string tmp;
                const size_t next = ReadString(s, i, tmp);
                if (next == std::string::npos) return std::string::npos;
                i = next;
                continue;
            }
            if (ch == open) {
                ++depth;
            } else if (ch == close) {
                --depth;
                if (depth == 0) return i + 1;
            }
            ++i;
        }
        return std::string::npos;
    }
    // 标量：扫到分隔符为止
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' &&
           s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
        ++i;
    }
    return i;
}

bool ReadBool(const std::string& s, size_t begin, size_t end, bool& out) {
    const std::string v = s.substr(begin, end - begin);
    if (v == "true")  { out = true;  return true; }
    if (v == "false") { out = false; return true; }
    return false;
}

bool ReadDouble(const std::string& s, size_t begin, size_t end, double& out) {
    const std::string v = s.substr(begin, end - begin);
    if (v.empty()) return false;
    char* stop = nullptr;
    out = std::strtod(v.c_str(), &stop);
    return stop != v.c_str();
}

// 把动态值统一描述成「原始字节区间 + 类型」，读和写都用它。
struct RawValue {
    size_t begin = 0;
    size_t end   = 0;
    enum class Kind { String, Other } kind = Kind::Other;
};

// 在顶层对象里按 key 找成员。绝不使用裸 find(key)：config.json 里
// whitelist 数组等位置完全可能出现同一个字符串（自检已复现过这个坑）。
bool FindTopLevelMember(const std::string& s, const char* quotedKey, size_t& begin, size_t& end,
                        bool& isString) {
    size_t i = SkipWs(s, 0);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    const size_t docEnd = s.size();
    for (;;) {
        i = SkipWs(s, i);
        if (i >= docEnd) return false;
        if (s[i] == '}') return false;
        if (s[i] != '"') return false;

        std::string key;
        i = ReadString(s, i, key);
        if (i == std::string::npos) return false;
        i = SkipWs(s, i);
        if (i >= docEnd || s[i] != ':') return false;
        const size_t valueBegin = SkipWs(s, i + 1);
        if (valueBegin >= docEnd) return false;
        const bool str = (s[valueBegin] == '"');
        const size_t valueEnd = SkipValue(s, valueBegin);
        if (valueEnd == std::string::npos) return false;

        if (key == quotedKey) {
            begin    = valueBegin;
            end      = valueEnd;
            isString = str;
            return true;
        }
        i = SkipWs(s, valueEnd);
        if (i < docEnd && s[i] == ',') { ++i; continue; }
        return false;
    }
}

// 在对象/数组区间内按 key 查成员（只查第一层；key 需带引号）。
bool FindMember(const std::string& s, size_t objBegin, size_t objEnd, const char* quotedKey,
                RawValue& out) {
    size_t i = SkipWs(s, objBegin);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    for (;;) {
        i = SkipWs(s, i);
        if (i >= s.size() || s[i] == '}') return false;
        if (s[i] != '"') return false;
        std::string key;
        i = ReadString(s, i, key);
        if (i == std::string::npos) return false;
        i = SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        i = SkipWs(s, i + 1);
        if (i >= s.size()) return false;
        const bool isString = (s[i] == '"');
        const size_t valueBegin = i;
        const size_t valueEnd = SkipValue(s, i);
        if (valueEnd == std::string::npos || valueEnd > objEnd) return false;

        const std::string wanted = quotedKey;
        const std::string got = "\"" + key + "\"";
        if (got == wanted) {
            out.begin = valueBegin;
            out.end   = valueEnd;
            out.kind  = isString ? RawValue::Kind::String : RawValue::Kind::Other;
            return true;
        }
        i = SkipWs(s, valueEnd);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        return false;
    }
}

// 在 dynamic_island 段里取字符串（不含引号）。
bool GetString(const std::string& s, size_t objBegin, size_t objEnd, const char* quotedKey,
               std::string& out) {
    RawValue v;
    if (!FindMember(s, objBegin, objEnd, quotedKey, v)) return false;
    if (v.kind != RawValue::Kind::String) return false;
    out.assign(s, v.begin + 1, v.end - v.begin - 2);
    return true;
}

bool GetNumber(const std::string& s, size_t objBegin, size_t objEnd, const char* quotedKey,
               double& out) {
    RawValue v;
    if (!FindMember(s, objBegin, objEnd, quotedKey, v)) return false;
    return ReadDouble(s, v.begin, v.end, out);
}

bool GetBool(const std::string& s, size_t objBegin, size_t objEnd, const char* quotedKey,
             bool& out) {
    RawValue v;
    if (!FindMember(s, objBegin, objEnd, quotedKey, v)) return false;
    return ReadBool(s, v.begin, v.end, out);
}

bool IsJsonNumber(const std::string& s, const RawValue& v) {
    if (v.kind != RawValue::Kind::Other || v.end <= v.begin) return false;
    const char c = s[v.begin];
    return c == '-' || (c >= '0' && c <= '9');
}

// 值的结束位置若紧跟 } 或 ]，说明它已经是该容器最后一个成员，
// 在其后追加新键就不需要补前导逗号。
bool IsLastMember(const std::string& s, size_t valueEnd) {
    size_t i = SkipWs(s, valueEnd);
    return i < s.size() && (s[i] == '}' || s[i] == ']');
}

std::string FormatNumber(double v) {
    char buf[64];
    if (std::fabs(v - std::floor(v + 0.5)) < 1e-9) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%g", v);
    }
    return buf;
}

std::string QuoteJson(const std::string& raw) {
    std::string out = "\"";
    for (char c : raw) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadWholeFile(const std::wstring& path, std::string& out, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "CreateFileW failed: " + std::to_string(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaxConfigBytes)) {
        CloseHandle(file);
        if (error) *error = "config size invalid";
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = out.empty() ? TRUE
                                : ReadFile(file, out.data(), static_cast<DWORD>(out.size()),
                                           &read, nullptr);
    CloseHandle(file);
    if (!ok || read != out.size()) {
        if (error) *error = "ReadFile failed";
        return false;
    }
    return true;
}

// 写临时文件 + 一次备份 + MoveFileEx 原子替换。
bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::string* error) {
    const std::wstring tmp = path + L".dsh-tmp";
    const std::wstring bak = path + L".dsh-bak";

    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "temp CreateFileW failed: " + std::to_string(GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = data.empty()
                        ? TRUE
                        : WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written,
                                    nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        if (error) *error = "WriteFile failed";
        return false;
    }

    if (FileExists(path)) {
        CopyFileW(path.c_str(), bak.c_str(), FALSE);   // 每次覆盖上一份备份
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        if (error) *error = "MoveFileExW failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

}  // namespace

const wchar_t* DockEdgeName(DockEdge edge) {
    switch (edge) {
        case DockEdge::Top:    return L"top";
        case DockEdge::Bottom: return L"bottom";
        case DockEdge::Left:   return L"left";
        case DockEdge::Right:  return L"right";
        case DockEdge::None:
        default:               return L"none";
    }
}

DockEdge DockEdgeFromName(const std::string& name) {
    if (name == "top")    return DockEdge::Top;
    if (name == "bottom") return DockEdge::Bottom;
    if (name == "left")   return DockEdge::Left;
    if (name == "right")  return DockEdge::Right;
    return DockEdge::None;
}

POINT DefaultIslandPos(const RECT& work) {
    POINT p{};
    p.x = work.right - 160 - kEdgeMarginDefault;   // 无窗口尺寸时的保守估计
    p.y = work.top + kEdgeMarginDefault;
    return p;
}

std::wstring IslandConfigPath() {
    wchar_t appdata[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata,
                                            static_cast<DWORD>(std::size(appdata)));
    if (n == 0 || n >= std::size(appdata)) return std::wstring();
    std::wstring path = appdata;
    path += L"\\dsh-pet-standalone-webm-chat\\config.json";
    return path;
}

IslandConfig LoadIslandConfig() { return LoadIslandConfigAt(IslandConfigPath()); }

IslandConfig LoadIslandConfigAt(const std::wstring& path) {
    IslandConfig cfg;
    if (path.empty()) return cfg;
    cfg.pathOk = true;
    if (!FileExists(path)) return cfg;

    std::string data;
    if (!ReadWholeFile(path, data, nullptr)) return cfg;

    size_t objBegin = 0;
    size_t objEnd   = 0;
    bool   isString = false;
    if (!FindTopLevelMember(data, "dynamic_island", objBegin, objEnd, isString)) return cfg;
    if (isString) return cfg;
    if (SkipWs(data, objBegin) >= data.size() || data[SkipWs(data, objBegin)] != '{') return cfg;

    cfg.ok = true;

    double nx = 0.0;
    double ny = 0.0;
    const bool hasX = GetNumber(data, objBegin, objEnd, "\"x\"", nx);
    const bool hasY = GetNumber(data, objBegin, objEnd, "\"y\"", ny);
    if (hasX && hasY) {
        cfg.pos.x = static_cast<LONG>(std::lround(nx));
        cfg.pos.y = static_cast<LONG>(std::lround(ny));
        cfg.hasPos = true;
    }

    std::string edge;
    if (GetString(data, objBegin, objEnd, "\"dock_edge\"", edge)) {
        cfg.edge    = DockEdgeFromName(edge);
        cfg.hasEdge = true;
    }

    bool dock = true;
    if (GetBool(data, objBegin, objEnd, "\"edge_dock\"", dock)) cfg.edgeDock = dock;

    GetString(data, objBegin, objEnd, "\"accent\"", cfg.accent);
    GetString(data, objBegin, objEnd, "\"style\"", cfg.style);

    double op = 0.0;
    if (GetNumber(data, objBegin, objEnd, "\"opacity\"", op) && op > 0.0 && op <= 1.0) {
        cfg.opacity = static_cast<float>(op);
    }

    bool rtss = true;
    if (GetBool(data, objBegin, objEnd, "\"rtss_autostart\"", rtss)) cfg.rtssAutostart = rtss;
    return cfg;
}

namespace {

// 把一组「已带引号的键 → 原始 JSON 值文本」应用到 dynamic_island 段并原子写回。
// 读-改-写全程只动指定键的值：已存在的按键替换，不存在的追加到段尾，
// 其余字节（含 dsh-pet 自己写的键、缩进、键序）原样保留。
bool PatchIslandSection(const std::wstring& path, const std::map<std::string, std::string>& want,
                        std::string* error) {
    const auto fail = [error](const char* msg) {
        if (error) *error = msg;
        return false;
    };

    if (path.empty()) return fail("APPDATA unavailable");
    if (want.empty()) return true;

    std::string data;
    if (FileExists(path)) {
        if (!ReadWholeFile(path, data, error)) return false;
    } else {
        data = "{\n  \"dynamic_island\": {\n  }\n}\n";
    }

    // 定位 dynamic_island 段（顶层成员精确匹配）；没有就补一段空的。
    size_t objBegin = 0;
    size_t objEnd   = 0;
    bool   isString = false;
    if (!FindTopLevelMember(data, "dynamic_island", objBegin, objEnd, isString)) {
        size_t brace = data.find('{');
        if (brace == std::string::npos) return fail("config is not a JSON object");
        data.insert(brace + 1, "\n  \"dynamic_island\": {\n  },\n");
        if (!FindTopLevelMember(data, "dynamic_island", objBegin, objEnd, isString)) {
            return fail("failed to create dynamic_island section");
        }
    }
    if (isString) return fail("dynamic_island is not an object");
    if (SkipWs(data, objBegin) >= data.size() || data[SkipWs(data, objBegin)] != '{') {
        return fail("dynamic_island is not an object");
    }

    // 收集替换区间（原始偏移），新键留到最后统一追加。
    std::vector<std::pair<std::pair<size_t, size_t>, std::string>> patches;
    std::vector<std::pair<std::string, std::string>>              additions;

    for (const auto& kv : want) {
        RawValue v;
        if (FindMember(data, objBegin, objEnd, kv.first.c_str(), v)) {
            patches.push_back({{v.begin, v.end}, kv.second});
        } else {
            additions.push_back(kv);
        }
    }

    // 从后往前替换，保证前面的偏移仍然有效。
    std::sort(patches.begin(), patches.end(),
              [](const auto& a, const auto& b) { return a.first.first > b.first.first; });
    for (const auto& p : patches) {
        data.replace(p.first.first, p.first.second - p.first.first, p.second);
    }

    // 新键插入到 dynamic_island 对象末尾（替换后重新定位，偏移已变）。
    if (!additions.empty()) {
        size_t b2 = 0;
        size_t e2 = 0;
        bool   s2 = false;
        if (!FindTopLevelMember(data, "dynamic_island", b2, e2, s2) || s2) {
            return fail("dynamic_island vanished after patch");
        }
        objBegin = SkipWs(data, b2);
        if (objBegin >= data.size() || data[objBegin] != '{') {
            return fail("dynamic_island is not an object");
        }
        // 追加新键：用 JSON 扫描重新取对象区间，避免字符串里的 '}' 造成误判。
        const size_t objEnd2 = SkipValue(data, objBegin);
        if (objEnd2 == std::string::npos) return fail("malformed dynamic_island object");
        const size_t closeBrace = objEnd2 - 1;   // SkipValue 返回的是闭括号之后
        if (closeBrace <= objBegin) {
            return fail("cannot locate closing brace");
        }
        const size_t afterOpen = SkipWs(data, objBegin + 1);
        const bool   hasMembers = (afterOpen < closeBrace);

        std::string body;
        for (size_t k = 0; k < additions.size(); ++k) {
            body += "\n    ";
            body += additions[k].first;
            body += ": ";
            body += additions[k].second;
            if (k + 1 < additions.size()) body += ",";
        }

        // 插入点要落在「最后一个成员之后」，而不是闭合括号之前：两者之间还有
        // 缩进/换行，若在括号前插逗号会得到 `"y": 47\n  ,\n    "k": v` 这种丑写法。
        size_t ins = closeBrace;
        while (ins > objBegin + 1) {
            const char c = data[ins - 1];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            --ins;
        }
        const std::string tail(data, ins, closeBrace - ins);   // 成员与闭合括号之间的缩进
        const std::string add = (hasMembers ? "," : "") + body + tail;
        data.replace(ins, closeBrace - ins, add);
    }

    return WriteFileAtomic(path, data, error);
}

}  // namespace

bool SaveIslandConfigAt(const std::wstring& path, const IslandConfig& cfg, bool setPos,
                        bool setEdge, bool setAccent, const std::string& accentName,
                        bool setStyle, const std::string& styleName, float opacity,
                        std::string* error) {
    std::map<std::string, std::string> want;
    if (setPos) {
        want["\"x\""] = FormatNumber(cfg.pos.x);
        want["\"y\""] = FormatNumber(cfg.pos.y);
    }
    if (setEdge) {
        const char* name = "none";
        if (cfg.edgeDock) {
            switch (cfg.edge) {
                case DockEdge::Top:    name = "top";    break;
                case DockEdge::Bottom: name = "bottom"; break;
                case DockEdge::Left:   name = "left";   break;
                case DockEdge::Right:  name = "right";  break;
                case DockEdge::None:
                default:               name = "none";   break;
            }
        }
        want["\"dock_edge\""] = QuoteJson(name);
    }
    if (setAccent && !accentName.empty()) want["\"accent\""] = QuoteJson(accentName);
    if (setStyle && !styleName.empty())   want["\"style\""]  = QuoteJson(styleName);
    if (opacity > 0.0f)                   want["\"opacity\""] = FormatNumber(opacity);
    // 岛自己的开关也一并落盘：任何一次保存都把它对齐到当前内存状态，
    // 这样「托盘里切过 / UAC 被拒过」之后，配置里的值永远是权威值。
    want["\"rtss_autostart\""] = cfg.rtssAutostart ? "true" : "false";
    return PatchIslandSection(path, want, error);
}

bool PatchIslandConfigKeysAt(const std::wstring& path,
                             const std::vector<std::pair<std::string, std::string>>& pairs,
                             std::string* error) {
    std::map<std::string, std::string> want;
    for (const auto& kv : pairs) {
        if (kv.first.empty()) continue;
        want[QuoteJson(kv.first)] = kv.second;
    }
    return PatchIslandSection(path, want, error);
}

bool PatchIslandConfigKeys(const std::vector<std::pair<std::string, std::string>>& pairs,
                           std::string* error) {
    return PatchIslandConfigKeysAt(IslandConfigPath(), pairs, error);
}

bool SaveIslandConfig(const IslandConfig& cfg, bool setPos, bool setEdge, bool setAccent,
                      const std::string& accentName, bool setStyle, const std::string& styleName,
                      float opacity, std::string* error) {
    return SaveIslandConfigAt(IslandConfigPath(), cfg, setPos, setEdge, setAccent, accentName,
                              setStyle, styleName, opacity, error);
}
