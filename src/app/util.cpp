#include "util.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <filesystem>
#include <system_error>
#include <vector>

#include "../log.h"

namespace md {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(need) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), need);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<size_t>(need) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::wstring();
    std::wstring p(path, len);
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return std::wstring();
    return p.substr(0, slash);
}

std::wstring GetAppDataDir() {
    // 用 %APPDATA%（即 FOLDERID_RoamingAppData）。不用 SHGetKnownFolderPath：
    // MinGW-w64 的 knownfolders.h 未导出该 GUID 常量，会产生链接错误。
    const wchar_t* env = _wgetenv(L"APPDATA");
    if (!env || !*env) return std::wstring();

    // ⚠ 显式用反斜杠拼接：libc++/MinGW 的 path operator/ 会用 '/'，
    //   而 Win32 API 不接受正斜杠分隔符。
    std::wstring dir = std::wstring(env) + L"\\MyopicDefocus";

    // 用 Win32 原生递归建目录（libc++ 的 std::filesystem 在 MinGW 下路径语义不一致）
    for (size_t i = 1; i <= dir.size(); ++i) {
        const bool atEnd = (i == dir.size());
        if (!atEnd && dir[i] != L'\\' && dir[i] != L'/') continue;
        const std::wstring part = dir.substr(0, i);
        if (part.size() <= 2) continue;
        if (::CreateDirectoryW(part.c_str(), nullptr) == FALSE) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS) return std::wstring();
        }
    }
    return dir;
}

// ---------------------------------------------------
// 开机自启：HKCU\Software\Microsoft\Windows\CurrentVersion\Run
// ---------------------------------------------------
namespace {
constexpr wchar_t kRunKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"MyopicDefocus";

bool OpenRunKey(HKEY* out) {
    return RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, out) == ERROR_SUCCESS;
}
} // namespace

bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (!OpenRunKey(&key)) return false;
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    LONG r = RegQueryValueExW(key, kValueName, nullptr, nullptr,
                              reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool SetAutoStart(bool enable) {
    HKEY key = nullptr;
    if (!OpenRunKey(&key)) return false;

    bool ok = false;
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            std::wstring cmd = L"\"";
            cmd += exePath;
            cmd += L"\"";
            LONG r = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(cmd.c_str()),
                                    static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            ok = (r == ERROR_SUCCESS);
        }
    } else {
        LONG r = RegDeleteValueW(key, kValueName);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

} // namespace md
