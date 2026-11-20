// ==========================================
// config.cpp — core 层：配置持久化（新版 v2 格式 + 旧版 Lerrenp 格式兼容）
// ==========================================
// 纯逻辑层：不依赖 D3D11，只用 Win32 文件/路径 API 与 nlohmann/json。
//
// 存储位置：
//   新版：%APPDATA%\MyopicDefocus\config.json
//   旧版：exe 同目录 config.json（Lerrenp/Myopic_defocus-cpp-build 字段名）
//
// 旧版字段映射：
//   diagInch        → common.diagInch
//   screenDistanceCM→ common.screenDistanceCM
//   pupilSizeUm     → common.pupilSizeUm
//   effectStrength  → common.effectStrength
//   resX / resY / targetFps → 故意忽略
//        （新版分辨率由程序按每屏实际探测，帧率各屏跟随自身刷新率）

#include "core.h"

#include "../third_party/nlohmann/json.hpp"

// shlobj.h  → SHGetKnownFolderPath + FOLDERID_AppData（内部含 knownfolders.h）
// objbase.h → CoTaskMemFree
// knownfolders.h → MinGW 下 shlobj.h 不会自动引入，需显式包含
#include <shlobj.h>
#include <knownfolders.h>
#include <objbase.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace md {
namespace core {

// ==========================================
// Config 成员函数
// ==========================================

const ScreenOverride* Config::FindOverride(const std::string& id) const {
    for (const auto& kv : perScreen) {
        if (kv.first == id) return &kv.second;
    }
    return nullptr;
}

DefocusParams Config::ResolveFor(const std::string& id) const {
    DefocusParams p = common;
    const ScreenOverride* ov = FindOverride(id);
    if (ov) ov->ApplyTo(p);
    return p;
}

namespace {

using json  = nlohmann::json;
using ojson = nlohmann::ordered_json;

// ------------------------------------------------------------
// JSON 键名（新版 v2）
// ------------------------------------------------------------
namespace key {
inline constexpr const char* kVersion                 = "version";
inline constexpr const char* kCommon                 = "common";
inline constexpr const char* kPerScreen              = "perScreen";
inline constexpr const char* kHideFromCapture        = "hideFromCapture";
inline constexpr const char* kAutoStart              = "autoStart";
inline constexpr const char* kIncludeVirtualDisplays = "includeVirtualDisplays";
inline constexpr const char* kFpsCap                 = "fpsCap";
inline constexpr const char* kPauseOnFullscreenGame  = "pauseOnFullscreenGame";
inline constexpr const char* kFloatingIconMode        = "floatingIconMode";

inline constexpr const char* kEffectStrength   = "effectStrength";
inline constexpr const char* kScreenDistanceCM = "screenDistanceCM";
inline constexpr const char* kPupilSizeUm      = "pupilSizeUm";
inline constexpr const char* kDiagInch         = "diagInch";
} // namespace key

// 旧版（Lerrenp）顶层键名与新版 common 的键名同名；
// 旧版独有的 resX / resY / targetFps 故意忽略——
//   新版分辨率由程序按每块屏实际探测，帧率各屏跟随自身刷新率。
// 这些字段存在与否都不影响解析结果。

// float → double 会把 0.25 膨胀成 0.20000000298023224；
// round 到 6 位小数后再 dump，dump 会自动选最短唯一表示。
double Round6(float v) {
    return std::round(static_cast<double>(v) * 1000000.0) / 1000000.0;
}

// 安全解析：任何异常都吞掉，返回 false
// 耐操性审查 P0-C：nlohmann 递归 parser 无深度限制，深嵌套 JSON 会栈溢出
//（0xC00000FD 无法被 try/catch 捕获）→ 解析前先做字符级嵌套深度预检。
bool ParseJsonSafe(const std::string& text, json& out) {
    // 嵌套深度预检：只数 [] {} 的净深度（字符串内的引号粗略跳过）
    constexpr int kMaxNesting = 200;   // 合法配置最深层不超过 5 层
    int depth = 0;
    bool inString = false;
    for (char c : text) {
        if (inString) {
            if (c == '\\') continue;          // 粗略跳过转义（够用：只为限深）
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '[' || c == '{') {
            if (++depth > kMaxNesting) return false;   // 判非法 → 走默认配置
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }
    try {
        out = json::parse(text);
        return true;
    } catch (const json::exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

// 从 JSON 对象读取通用四参数；缺失 / 非数值则保持原值不变。
// 返回命中的字段个数。
int ReadCommonParams(const json& src, DefocusParams& p) {
    if (!src.is_object()) return 0;
    int hit = 0;
    if (src.contains(key::kEffectStrength) && src[key::kEffectStrength].is_number()) {
        p.effectStrength = src[key::kEffectStrength].get<float>(); ++hit;
    }
    if (src.contains(key::kScreenDistanceCM) && src[key::kScreenDistanceCM].is_number()) {
        p.screenDistanceCM = src[key::kScreenDistanceCM].get<float>(); ++hit;
    }
    if (src.contains(key::kPupilSizeUm) && src[key::kPupilSizeUm].is_number()) {
        p.pupilSizeUm = src[key::kPupilSizeUm].get<float>(); ++hit;
    }
    if (src.contains(key::kDiagInch) && src[key::kDiagInch].is_number()) {
        p.diagInch = src[key::kDiagInch].get<float>(); ++hit;
    }
    return hit;
}

// 读取单屏覆盖里的一个可空数值：缺失 / null / 非数值 → 留空（表示沿用通用值）
void ReadOptionalParam(const json& src, const char* name, std::optional<float>& out) {
    if (!src.is_object()) return;
    auto it = src.find(name);
    if (it == src.end() || it->is_null() || !it->is_number()) return;
    out = it->get<float>();
}

// 写一个可空数值：有值写数字，没值写 JSON null（默认构造的 ojson 即 null）
void setOptional(ojson& dst, const char* name, const std::optional<float>& v) {
    if (v.has_value()) {
        dst[name] = Round6(*v);
    } else {
        dst[name] = ojson();
    }
}

} // namespace

// ==========================================
// 默认配置
// ==========================================

Config MakeDefaultConfig() {
    Config c;
    c.version             = 2;
    c.common              = DefocusParams{};   // 0.25 / 40 / 6500 / 24
    c.perScreen.clear();
    c.hideFromCapture     = true;
    c.autoStart           = false;
    c.includeVirtualDisplays = false;
    c.fpsCap              = 0;                 // 0 = 跟随各屏刷新率
    return c;
}

// ==========================================
// 旧版格式解析（Lerrenp config.json）
// ==========================================

bool ParseLegacyConfig(const std::string& json_text, Config& out) {
    json j;
    if (!ParseJsonSafe(json_text, j) || !j.is_object()) return false;

    int hit = 0;
    if (j.contains(key::kDiagInch) && j[key::kDiagInch].is_number()) {
        out.common.diagInch = j[key::kDiagInch].get<float>(); ++hit;
    }
    if (j.contains(key::kScreenDistanceCM) && j[key::kScreenDistanceCM].is_number()) {
        out.common.screenDistanceCM = j[key::kScreenDistanceCM].get<float>(); ++hit;
    }
    if (j.contains(key::kPupilSizeUm) && j[key::kPupilSizeUm].is_number()) {
        out.common.pupilSizeUm = j[key::kPupilSizeUm].get<float>(); ++hit;
    }
    if (j.contains(key::kEffectStrength) && j[key::kEffectStrength].is_number()) {
        out.common.effectStrength = j[key::kEffectStrength].get<float>(); ++hit;
    }

    // resX / resY / targetFps 故意忽略（见文件头说明）。
    return hit > 0;
}

// ==========================================
// 新版格式序列化
// ==========================================

std::string SerializeConfig(const Config& cfg) {
    ojson j = ojson::object();
    j[key::kVersion] = 2;

    ojson common = ojson::object();
    common[key::kEffectStrength]   = Round6(cfg.common.effectStrength);
    common[key::kScreenDistanceCM] = Round6(cfg.common.screenDistanceCM);
    common[key::kPupilSizeUm]      = Round6(cfg.common.pupilSizeUm);
    common[key::kDiagInch]         = Round6(cfg.common.diagInch);
    j[key::kCommon] = common;

    // perScreen：屏幕 id → 四个可空参数（null = 该项沿用通用值）
    ojson per = ojson::object();
    for (const auto& kv : cfg.perScreen) {
        const ScreenOverride& o = kv.second;
        ojson ov = ojson::object();
        setOptional(ov, key::kEffectStrength,   o.effectStrength);
        setOptional(ov, key::kScreenDistanceCM, o.screenDistanceCM);
        setOptional(ov, key::kPupilSizeUm,      o.pupilSizeUm);
        setOptional(ov, key::kDiagInch,         o.diagInch);
        per[kv.first] = ov;
    }
    j[key::kPerScreen] = per;

    j[key::kHideFromCapture]        = cfg.hideFromCapture;
    j[key::kAutoStart]              = cfg.autoStart;
    j[key::kIncludeVirtualDisplays] = cfg.includeVirtualDisplays;
    j[key::kFpsCap]                 = cfg.fpsCap;
    j[key::kPauseOnFullscreenGame]  = cfg.pauseOnFullscreenGame;
    j[key::kFloatingIconMode]        = cfg.floatingIconMode;

    return j.dump(2);
}

// ==========================================
// 读配置
// ==========================================

bool LoadConfig(Config& out) {
    // 统一出口防御（P0-A/P0-B）：改用 DefocusParams::Sanitize 合理域钳制——
    // 旧「≤0 归默认」防不住巨大正值（pupilSizeUm:9000000 → σ 爆炸 GPU 冻结）
    // 与近零正值（diagInch:1e-9）；NaN 因比较恒 false 也被 Sanitize 归默认。
    // perScreen 覆盖项由 ApplyTo 内的 Sanitize 同步覆盖。
    auto finish = [&out](bool ok) {
        out.common.Sanitize();
        return ok;
    };

    out = MakeDefaultConfig();

    std::string text;

    // 1) 新路径 %APPDATA%\MyopicDefocus\config.json
    if (detail::ReadTextFile(detail::NewConfigPath(), text)) {
        if (detail::ParseConfigJsonV2(text, out)) return finish(true);
        // 新路径里放的也可能是旧格式文件（用户手动拷的）→ 照样能读
        if (ParseLegacyConfig(text, out)) return finish(true);
    }

    // 2) 回退：exe 同目录的旧版 config.json
    if (detail::ReadTextFile(detail::LegacyConfigPath(), text)) {
        if (detail::ParseConfigJsonV2(text, out)) return finish(true);
        if (ParseLegacyConfig(text, out)) return finish(true);
    }

    out = MakeDefaultConfig();
    return finish(false);
}

// ==========================================
// 写配置
// ==========================================

namespace detail {
// 前置声明：定义在下方 detail 块中（SaveConfig 用得到）
bool EnsureDirectory(const std::wstring& dir);
} // namespace detail

bool SaveConfig(const Config& cfg) {
    const std::filesystem::path path = detail::NewConfigPath();
    if (path.empty()) return false;

    const std::filesystem::path dir = path.parent_path();
    if (!dir.empty()) {
        // 用 Win32 原生递归建目录：libc++/MinGW 的 std::filesystem 路径语义
        // 与 Win32 不一致，create_directories 实测会失败。
        if (!detail::EnsureDirectory(dir.native())) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return false;
        }
    }

    if (detail::WriteTextFileAtomic(path, SerializeConfig(cfg))) return true;

    // 兜底：%APPDATA% 不可写时（例如受限运行环境）退回 exe 同目录
    const std::filesystem::path fallback = detail::LegacyConfigPath();
    if (!fallback.empty() && fallback != path) {
        return detail::WriteTextFileAtomic(fallback, SerializeConfig(cfg));
    }
    return false;
}

// ==========================================
// detail：JSON 解析（新版）
// ==========================================

namespace detail {

bool ParseConfigJsonV2(const std::string& text, Config& out) {
    json j;
    if (!ParseJsonSafe(text, j) || !j.is_object()) return false;

    Config cfg = MakeDefaultConfig();
    int hit = 0;

    // --- 通用参数 ---
    // 优先 "common" 子对象；没有则兼容顶层平铺写法（旧版文件的形状）
    if (j.contains(key::kCommon) && j[key::kCommon].is_object()) {
        hit += ReadCommonParams(j[key::kCommon], cfg.common);
    } else {
        hit += ReadCommonParams(j, cfg.common);
    }

    // --- 每屏覆盖 ---
    if (j.contains(key::kPerScreen) && j[key::kPerScreen].is_object()) {
        for (auto it = j[key::kPerScreen].begin(); it != j[key::kPerScreen].end(); ++it) {
            const json& o = it.value();
            if (!o.is_object()) continue;
            ScreenOverride ov;
            ReadOptionalParam(o, key::kEffectStrength,   ov.effectStrength);
            ReadOptionalParam(o, key::kScreenDistanceCM, ov.screenDistanceCM);
            ReadOptionalParam(o, key::kPupilSizeUm,      ov.pupilSizeUm);
            ReadOptionalParam(o, key::kDiagInch,         ov.diagInch);
            cfg.perScreen.emplace_back(it.key(), ov);
        }
        ++hit;
    }

    // --- 开关 ---
    if (j.contains(key::kHideFromCapture) && j[key::kHideFromCapture].is_boolean()) {
        cfg.hideFromCapture = j[key::kHideFromCapture].get<bool>(); ++hit;
    }
    if (j.contains(key::kAutoStart) && j[key::kAutoStart].is_boolean()) {
        cfg.autoStart = j[key::kAutoStart].get<bool>(); ++hit;
    }
    if (j.contains(key::kIncludeVirtualDisplays) && j[key::kIncludeVirtualDisplays].is_boolean()) {
        cfg.includeVirtualDisplays = j[key::kIncludeVirtualDisplays].get<bool>(); ++hit;
    }
    if (j.contains(key::kFloatingIconMode) && j[key::kFloatingIconMode].is_number()) {
        cfg.floatingIconMode = j[key::kFloatingIconMode].get<int>(); ++hit;
    }
    if (j.contains(key::kPauseOnFullscreenGame) && j[key::kPauseOnFullscreenGame].is_boolean()) {
        cfg.pauseOnFullscreenGame = j[key::kPauseOnFullscreenGame].get<bool>(); ++hit;
    }
    if (j.contains(key::kFpsCap) && j[key::kFpsCap].is_number()) {
        cfg.fpsCap = j[key::kFpsCap].get<int>(); ++hit;
    }

    // version 本身也是一个「已知字段」标记：{"version":2} 是一份合法的新版配置
    if (j.contains(key::kVersion) && j[key::kVersion].is_number()) {
        ++hit;
    }

    if (hit == 0) return false;   // 不认识的 JSON，不当作配置

    cfg.version = 2;              // 落盘格式版本恒为 2
    out = cfg;
    return true;
}

// ==========================================
// detail：字符串 / 路径 / 文件辅助
// ==========================================

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::string NarrowUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need, nullptr, nullptr);
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string TrimTrailingWhitespace(std::string s) {
    size_t end = s.size();
    while (end > 0) {
        const char c = s[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { --end; continue; }
        break;
    }
    s.resize(end);
    return s;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

// 递归创建目录。用 Win32 原生 API 而不是 std::filesystem::create_directories ——
// libc++/MinGW 的 path 语义与 Win32 不一致（分隔符/父路径推导），实测会失败。
bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;

    for (size_t i = 1; i <= dir.size(); ++i) {
        const bool atEnd = (i == dir.size());
        if (!atEnd && dir[i] != L'\\' && dir[i] != L'/') continue;

        const std::wstring part = dir.substr(0, i);
        // 跳过 "C:" 这类没有分隔符的片段
        if (part.size() <= 2) continue;

        if (::CreateDirectoryW(part.c_str(), nullptr) == FALSE) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

std::filesystem::path AppDataDir() {
    std::wstring base;

    // 用环境变量 APPDATA（即 FOLDERID_RoamingAppData）。
    // 不用 SHGetKnownFolderPath：MinGW-w64 的 knownfolders.h 未导出 FOLDERID_AppData，
    // 且 %APPDATA% 本来就是同一个目录，零额外依赖。
    const wchar_t* env = _wgetenv(L"APPDATA");
    if (env && *env) base.assign(env);

    if (base.empty()) return std::filesystem::path();

    // ⚠ 不要用 path(base) / L"xxx" 拼接：libc++/MinGW 的 operator/ 会用 '/' 作分隔符，
    //   而 Win32 API（CreateDirectoryW / _wfopen_s）**不接受正斜杠**，
    //   拼出的 "C:\...\Roaming/MyopicDefocus" 会以 ERROR_ACCESS_DENIED 失败。
    //   这里显式用反斜杠拼接。
    return std::filesystem::path(base + L"\\MyopicDefocus");
}

std::filesystem::path NewConfigPath() {
    const std::filesystem::path dir = AppDataDir();
    if (dir.empty()) return std::filesystem::path();
    // 显式反斜杠拼接，理由见 AppDataDir 的注释
    return std::filesystem::path(dir.native() + L"\\config.json");
}

std::filesystem::path ExeDir() {
    std::wstring buf(512, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::filesystem::current_path();
        if (n < buf.size()) {
            buf.resize(n);
            // 同 AppDataDir：反斜杠转正斜杠，兼容 libc++ 的路径解析
            for (wchar_t& ch : buf) {
                if (ch == L'\\') ch = L'/';
            }
            return std::filesystem::path(buf).parent_path();
        }
        // 被截断 → 换更大的缓冲再来一次
        if (buf.size() >= 32768) break;
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::current_path();
}

std::filesystem::path LegacyConfigPath() {
    const std::filesystem::path dir = ExeDir();
    // 显式反斜杠拼接，理由见 AppDataDir 的注释
    return std::filesystem::path(dir.native() + L"\\config.json");
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out) {
    if (path.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;

    // 耐操性审查 P0-C：配置文件大小上限（合法配置 <10KB；超大文件必然非法，
    // 也防止对超大输入做嵌套预检与解析的时间开销）
    constexpr auto kMaxConfigBytes = 1ull * 1024ull * 1024ull;   // 1 MB
    const auto fsize = static_cast<unsigned long long>(std::filesystem::file_size(path, ec));
    if (ec || fsize == 0 || fsize > kMaxConfigBytes) return false;

    // 耐操性审查 P1-F：编码嗅探。记事本「另存为」会把 UTF-8 config 写成
    // UTF-16/ANSI——直接读出来是乱码、解析必失败，随后退出时的无条件保存
    // 会把用户手调参数覆盖成默认值（数据丢失）。检测到非 UTF-8 编码时
    // 先把原文件改存为 .bak（不覆盖已存在的备份），再返回失败。
    {
        unsigned char b[2] = {0, 0};
        FILE* probe = nullptr;
        if (_wfopen_s(&probe, path.c_str(), L"rb") == 0 && probe) {
            const size_t rn = fread(b, 1, 2, probe);
            fclose(probe);
            const bool utf16le = (rn >= 2 && b[0] == 0xFF && b[1] == 0xFE);
            const bool utf16be = (rn >= 2 && b[0] == 0xFE && b[1] == 0xFF);
            // 无 BOM 且首字节含 0x00 → 高概率 UTF-16 无 BOM；或出现 GBK 高位字节对
            const bool maybeUtf16NoBom = (rn >= 1 && b[0] == 0x00);
            if (utf16le || utf16be || maybeUtf16NoBom) {
                std::filesystem::path bak = path;
                bak += L".encoding-bak";
                std::error_code mec;
                if (!std::filesystem::exists(bak, mec)) {
                    std::filesystem::copy_file(path, bak,
                        std::filesystem::copy_options::overwrite_existing, mec);
                }
                // core 层无日志依赖（模块约束）：静默降级，.encoding-bak 文件本身即提示
                return false;
            }
        }
    }

    // 用 C 文件 API（_wfopen）以支持非 ASCII 路径 —— libc++ 的 fstream 没有 wstring 构造
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;

    std::string data;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        data.append(buf, n);
    }
    const bool failed = (ferror(fp) != 0);
    fclose(fp);
    if (failed) return false;

    // 剥掉 UTF-8 BOM（nlohmann::json 解析带 BOM 的文本会失败）
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }

    out = std::move(data);
    return true;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content) {
    if (path.empty()) return false;

    std::filesystem::path tmp = path;
    tmp += L".tmp";

    {
        // 同上：用 _wfopen 支持非 ASCII 路径
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, tmp.c_str(), L"wb") != 0 || !fp) return false;
        const size_t written = fwrite(content.data(), 1, content.size(), fp);
        bool ok = (written == content.size());
        if (fflush(fp) != 0) ok = false;
        if (fclose(fp) != 0) ok = false;
        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    // 原子替换（同一卷内 MoveFileEx 是原子的）
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace detail

} // namespace core

} // namespace md
