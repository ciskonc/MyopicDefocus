// ==========================================
// config_io 简易插桩单测
// 测试新版配置序列化/反序列化 + 旧版格式兼容 + 文件往返
// ==========================================
#include "core/core.h"
#include "../types.h"
#include "../app/util.h"

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

// 由 test_optical_model.cpp 定义
extern int g_passed;
extern int g_failed;
extern void SelfTestLogAppend(const char* text);

// 输出助手: 写到 stdout + 自检日志（GUI subsystem 下 stdout 不可见）
static void out(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fputs(buf, stdout);
    fflush(stdout);
    SelfTestLogAppend(buf);
}

#define EXPECT_EQ_INT(actual, expected) \
    do { \
        int _a = (actual), _e = (expected); \
        if (_a == _e) { \
            ++g_passed; \
            out("  [PASS] %s == %d\n", #actual, _e); \
        } else { \
            ++g_failed; \
            out("  [FAIL] %s: got %d expected %d\n", #actual, _a, _e); \
        } \
    } while(0)

#define EXPECT_NEAR(actual, expected, tol) \
    do { \
        float _a = (actual), _e = (expected); \
        if (std::fabs(_a - _e) <= (tol)) { \
            ++g_passed; \
            out("  [PASS] %s ~= %g (diff=%g)\n", #actual, _e, std::fabs(_a-_a)); \
        } else { \
            ++g_failed; \
            out("  [FAIL] %s: got %g expected %g (diff=%g > %g)\n", \
                #actual, _a, _e, std::fabs(_a-_e), (float)(tol)); \
        } \
    } while(0)

#define EXPECT_TRUE(cond) \
    do { \
        if (cond) { ++g_passed; out("  [PASS] %s\n", #cond); } \
        else       { ++g_failed; out("  [FAIL] %s\n", #cond); } \
    } while(0)

#define CASE(name) out("\n[TEST] %s\n", name)

using md::core::Config;
using md::core::DefocusParams;
using md::core::ScreenOverride;

// ---------------------------------------------------------------
// 1. 往返一致性：SaveConfig 落盘 → LoadConfig 读回
//    会碰真实的 %APPDATA%\MyopicDefocus\config.json，
//    所以先备份原内容，测完原样恢复。
// ---------------------------------------------------------------
static bool ReadWholeFile(const std::wstring& path, std::string& out) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return false;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    fclose(fp);
    return true;
}

static bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    return true;
}

static void test_roundtrip_via_file() {
    CASE("往返一致性 (SaveConfig -> LoadConfig)");

    Config cfg = md::core::MakeDefaultConfig();
    cfg.common.diagInch        = 27.5f;
    cfg.common.screenDistanceCM = 50.0f;
    cfg.common.pupilSizeUm      = 7000.0f;
    cfg.common.effectStrength   = 0.42f;
    cfg.hideFromCapture = false;
    cfg.autoStart = true;

    // 单屏覆盖
    ScreenOverride ov;
    ov.effectStrength = 0.18f;
    cfg.perScreen.emplace_back("NVIDIA#0", ov);

    std::string json = md::core::SerializeConfig(cfg);
    out("  JSON output:\n%s\n", json.c_str());

    // 落盘 → 读回
    std::wstring dir = md::GetAppDataDir();
    EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;

    const std::wstring path = dir + L"\\config.json";

    // 备份原文件，测完原样恢复（避免污染用户真实配置）
    std::string backup;
    const bool hadBackup = ReadWholeFile(path, backup);

    // 落盘
    EXPECT_EQ_INT(md::core::SaveConfig(cfg), 1);

    Config back;
    bool ok = md::core::LoadConfig(back);
    EXPECT_EQ_INT(ok, 1);

    EXPECT_NEAR(back.common.diagInch,         27.5f,  0.01f);
    EXPECT_NEAR(back.common.screenDistanceCM, 50.0f,  0.01f);
    EXPECT_NEAR(back.common.pupilSizeUm,      7000.0f,0.01f);
    EXPECT_NEAR(back.common.effectStrength,   0.42f,  0.01f);
    EXPECT_EQ_INT(back.hideFromCapture, 0);
    EXPECT_EQ_INT(back.autoStart, 1);

    // 单屏覆盖应被读回
    const ScreenOverride* got = back.FindOverride("NVIDIA#0");
    EXPECT_TRUE(got != nullptr);
    if (got) {
        EXPECT_TRUE(got->effectStrength.has_value());
        if (got->effectStrength) {
            EXPECT_NEAR(*got->effectStrength, 0.18f, 0.01f);
        }
    }

    // 恢复原文件
    if (hadBackup) {
        WriteWholeFile(path, backup);
    } else {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

// ---------------------------------------------------------------
// 2. 旧版格式兼容（Lerrenp 的 config.json）
// ---------------------------------------------------------------
static void test_legacy_compat() {
    CASE("旧版格式兼容 (Lerrenp config.json)");

    const char* legacy = R"({
        "version": 1,
        "note": "Myopic Defocus 配置 - 修改后保存即可生效",
        "diagInch": 28.0,
        "screenDistanceCM": 40.0,
        "pupilSizeUm": 6500.0,
        "effectStrength": 0.1,
        "resX": null,
        "resY": null,
        "targetFps": 120
    })";

    Config cfg = md::core::MakeDefaultConfig();
    bool ok = md::core::ParseLegacyConfig(legacy, cfg);
    EXPECT_EQ_INT(ok, 1);
    EXPECT_NEAR(cfg.common.diagInch,         28.0f,  0.01f);
    EXPECT_NEAR(cfg.common.screenDistanceCM, 40.0f,  0.01f);
    EXPECT_NEAR(cfg.common.pupilSizeUm,      6500.0f,0.01f);
    EXPECT_NEAR(cfg.common.effectStrength,   0.1f,  0.01f);
}

// 旧版缺少部分字段时，其余保持默认值
static void test_legacy_partial() {
    CASE("旧版格式: 缺字段保留默认值");
    const char* legacy = R"({"diagInch": 32.0})";
    Config cfg = md::core::MakeDefaultConfig();
    bool ok = md::core::ParseLegacyConfig(legacy, cfg);
    EXPECT_EQ_INT(ok, 1);
    EXPECT_NEAR(cfg.common.diagInch, 32.0f, 0.01f);
    EXPECT_NEAR(cfg.common.screenDistanceCM, 40.0f, 0.01f);
    EXPECT_NEAR(cfg.common.pupilSizeUm, 6500.0f, 0.01f);
    EXPECT_NEAR(cfg.common.effectStrength, 0.25f, 0.01f);
}

// 无效 JSON 必须返回 false 且不破坏现有值
static void test_invalid_json() {
    CASE("无效 JSON: 返回 false 且不破坏现有值");
    Config cfg = md::core::MakeDefaultConfig();
    cfg.common.diagInch = 14.0f;
    bool ok = md::core::ParseLegacyConfig("{not json}", cfg);
    EXPECT_EQ_INT(ok, 0);
    EXPECT_NEAR(cfg.common.diagInch, 14.0f, 0.01f);
}

// ---------------------------------------------------------------
// 3. 参数解析：通用 + 单屏覆盖
// ---------------------------------------------------------------
static void test_resolve_for() {
    CASE("参数解析: 通用 + 单屏覆盖");

    Config cfg = md::core::MakeDefaultConfig();
    cfg.common.effectStrength = 0.30f;
    cfg.common.screenDistanceCM = 45.0f;

    // 未覆盖的屏 → 用通用值
    DefocusParams p1 = cfg.ResolveFor("NVIDIA#0");
    EXPECT_NEAR(p1.effectStrength, 0.30f, 0.001f);
    EXPECT_NEAR(p1.screenDistanceCM, 45.0f, 0.001f);

    // 覆盖了强度的屏 → 该项用覆盖值，其余仍用通用值
    ScreenOverride ov;
    ov.effectStrength = 0.10f;
    cfg.perScreen.emplace_back("NVIDIA#1", ov);

    DefocusParams p2 = cfg.ResolveFor("NVIDIA#1");
    EXPECT_NEAR(p2.effectStrength, 0.10f, 0.001f);
    EXPECT_NEAR(p2.screenDistanceCM, 45.0f, 0.001f);

    // 找不到的屏 → 用通用值
    DefocusParams p3 = cfg.ResolveFor("UNKNOWN");
    EXPECT_NEAR(p3.effectStrength, 0.30f, 0.001f);
}

// ---------------------------------------------------------------
// 4. 默认值
// ---------------------------------------------------------------
static void test_defaults() {
    CASE("默认值");
    Config cfg = md::core::MakeDefaultConfig();
    EXPECT_NEAR(cfg.common.effectStrength, 0.25f, 0.001f);
    EXPECT_NEAR(cfg.common.screenDistanceCM, 40.0f, 0.001f);
    EXPECT_NEAR(cfg.common.pupilSizeUm, 6500.0f, 0.001f);
    EXPECT_NEAR(cfg.common.diagInch, 24.0f, 0.001f);
    EXPECT_EQ_INT(cfg.hideFromCapture, 1);   // 默认开
    EXPECT_EQ_INT(cfg.autoStart, 0);         // 默认关
}

// ---------------------------------------------------------------
// 5. 显示器枚举（不应崩溃，至少返回主屏）
// ---------------------------------------------------------------
static void test_enumerate_screens() {
    CASE("显示器枚举: 不崩溃且至少拿到一块屏");
    auto screens = md::core::EnumerateScreens(false);
    out("  枚举到 %zu 块屏（含虚拟）\n", screens.size());
    for (const auto& s : screens) {
        out("    - id=%s  dev=%s  %dx%d@%dHz  %dbit  virtual=%d primary=%d\n",
            s.id.c_str(), s.deviceName.c_str(), s.width, s.height,
            s.refreshHz, s.bitsPerPixel, s.isVirtual ? 1 : 0, s.isPrimary ? 1 : 0);
    }
    EXPECT_TRUE(!screens.empty());
}

int RunConfigIoTests() {
    out("  ---- config / 枚举 简易插桩 ----\n");

    test_roundtrip_via_file();
    test_legacy_compat();
    test_legacy_partial();
    test_invalid_json();
    test_resolve_for();
    test_defaults();
    test_enumerate_screens();

    std::fflush(stdout);
    std::fflush(stderr);
    return g_failed > 0 ? 1 : 0;
}
