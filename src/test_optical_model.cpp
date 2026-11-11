// ==========================================
// 简易插桩单测 - ComputeBlurRadii
// 不依赖任何测试框架，仅用 printf + 退出码
// ==========================================
#include "optical_model.h"

#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <string>

// 全局测试计数器,跨翻译单元共享 (test_config_io.cpp 也写入)
int g_passed = 0;
int g_failed = 0;

// 由 test_config_io.cpp 提供
int RunConfigIoTests();

// 输出助手: 格式化为 UTF-8 字符串, 然后用 WriteConsoleW 写宽字符
// 解决 cmd.exe 默认 GBK (CP936) 下中文 printf 输出乱码的问题
static void out(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // 尽量用 WriteConsoleW 直写宽字符（失败则退回 stdout）
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        int need = MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
        if (need > 0) {
            std::wstring w(static_cast<size_t>(need) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, w.data(), need);
            DWORD written = 0;
            WriteConsoleW(hOut, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
            return;
        }
    }
    fputs(buf, stdout);
    fflush(stdout);
}

// ----------- 断言宏 -----------
#define EXPECT_NEAR(actual, expected, tol) \
    do { \
        float _a = (actual); \
        float _e = (expected); \
        float _d = std::fabs(_a - _e); \
        if (_d <= (tol)) { \
            ++g_passed; \
            out("  [PASS] %-32s  got=%.4f  expected=%.4f  diff=%.5f\n", \
                #actual, _a, _e, _d); \
        } else { \
            ++g_failed; \
            out("  [FAIL] %-32s  got=%.4f  expected=%.4f  diff=%.5f > tol=%.4f\n", \
                #actual, _a, _e, _d, (float)(tol)); \
        } \
    } while(0)

#define EXPECT_TRUE(cond) \
    do { \
        if (cond) { \
            ++g_passed; \
            out("  [PASS] %s\n", #cond); \
        } else { \
            ++g_failed; \
            out("  [FAIL] %s\n", #cond); \
        } \
    } while(0)

#define CASE(name) out("\n[TEST] %s\n", name)

using md::core::BlurRadii;
using md::core::ScreenGeometry;

// ----------- 测试用例 -----------

// 构造测试用几何
static ScreenGeometry MakeGeo(int resX, int resY, float diagInch, float distCM, float pupilUm) {
    ScreenGeometry g;
    g.resX = resX;
    g.resY = resY;
    g.diagInch = diagInch;
    g.screenDistanceCM = distCM;
    g.pupilSizeUm = pupilUm;
    return g;
}

// 用 JS 上游默认参数验证 1:1 一致性
// (14", 2560x1440, 40cm, 6500um)
// 独立 JS 复算真值: blur_b = 9.140041, blur_g = 3.229939
static void test_js_upstream_defaults() {
    CASE("JS 上游默认参数 (14\", 2560x1440, 40cm, 6500um)");
    auto geo = MakeGeo(2560, 1440, 14.0f, 40.0f, 6500.0f);
    BlurRadii r = md::core::ComputeBlurRadii(geo);
    EXPECT_NEAR(r.blue,  9.1400f, 0.01f);
    EXPECT_NEAR(r.green, 3.2299f, 0.01f);
    EXPECT_TRUE(r.blue > r.green);  // 蓝光离焦更大
}

// 当前默认参数 (24" 显示器)
// 独立 JS 复算真值: blur_b = 3.9988, blur_g = 1.4131
// （注意：28" 同分辨率的真值是 3.4275 / 1.2112，两者不可混用）
static void test_cpp_defaults() {
    CASE("本机默认参数 (24\", 1920x1080, 40cm, 6500um)");
    auto geo = MakeGeo(1920, 1080, 24.0f, 40.0f, 6500.0f);
    BlurRadii r = md::core::ComputeBlurRadii(geo);
    EXPECT_NEAR(r.blue,  3.9988f, 0.01f);
    EXPECT_NEAR(r.green, 1.4131f, 0.01f);
}

// 28" 同分辨率：屏幕更大 → 每像素物理尺寸更大 → 模糊像素数更小
static void test_28inch_same_res() {
    CASE("对照: 28\" 1920x1080 (真值 3.4275 / 1.2112)");
    auto geo = MakeGeo(1920, 1080, 28.0f, 40.0f, 6500.0f);
    BlurRadii r = md::core::ComputeBlurRadii(geo);
    EXPECT_NEAR(r.blue,  3.4275f, 0.01f);
    EXPECT_NEAR(r.green, 1.2112f, 0.01f);
}

// 4K 分辨率: 同样 14"，更高 DPI → 模糊更大
static void test_4k_resolution() {
    CASE("4K 分辨率 (14\", 3840x2160, 40cm, 6500um)");
    auto geo = MakeGeo(3840, 2160, 14.0f, 40.0f, 6500.0f);
    BlurRadii r = md::core::ComputeBlurRadii(geo);
    EXPECT_NEAR(r.blue,  13.7095f, 0.01f);
    EXPECT_NEAR(r.green, 4.8451f, 0.01f);
}

// 安全性: 零分辨率不能崩溃
static void test_zero_resolution_safe() {
    CASE("边界: 零分辨率 (不崩溃, 输出有限值)");
    auto geo = MakeGeo(0, 0, 14.0f, 40.0f, 6500.0f);
    BlurRadii r = md::core::ComputeBlurRadii(geo);
    EXPECT_TRUE(std::isfinite(r.blue));
    EXPECT_TRUE(std::isfinite(r.green));
    EXPECT_TRUE(r.blue  >= 0.0f);
    EXPECT_TRUE(r.green >= 0.0f);
}

// 物理直觉: 屏幕越远，眼睛越难对焦，模糊越大
// 推导: circ = pupil*(screen-G)/G，screen 增大时此式单调递增
static void test_far_distance_larger_blur() {
    CASE("物理直觉: 视距 200cm > 40cm 的模糊 (屏幕越远越糊)");
    auto geo = MakeGeo(2560, 1440, 14.0f, 40.0f, 6500.0f);
    BlurRadii nearBlur = md::core::ComputeBlurRadii(geo);
    geo.screenDistanceCM = 200.0f;
    BlurRadii farBlur = md::core::ComputeBlurRadii(geo);
    EXPECT_TRUE(farBlur.blue  > nearBlur.blue);
    EXPECT_TRUE(farBlur.green > nearBlur.green);
}

// 物理直觉: 屏幕非常近时 (screen → 0)，模糊趋近 0
static void test_very_close_small_blur() {
    CASE("物理直觉: 视距 5cm → 模糊趋近 0");
    auto geo = MakeGeo(2560, 1440, 14.0f, 5.0f, 6500.0f);
    BlurRadii r = md::core::ComputeBlurRadii(geo);
    EXPECT_TRUE(r.blue  < 2.0f);
    EXPECT_TRUE(r.green < 2.0f);
}

// 物理直觉: 蓝通道始终比绿通道模糊更多 (LCA 决定)
static void test_blue_always_more_blur() {
    CASE("物理直觉: blue > green (LCA 决定)");
    const float dists[] = { 20.0f, 40.0f, 80.0f, 200.0f };
    for (float d : dists) {
        auto geo = MakeGeo(2560, 1440, 14.0f, d, 6500.0f);
        BlurRadii r = md::core::ComputeBlurRadii(geo);
        EXPECT_TRUE(r.blue > r.green);
    }
}

// 物理直觉: 瞳孔越大 → 弥散圆越大 → 越糊
static void test_larger_pupil_more_blur() {
    CASE("物理直觉: 瞳孔 8000um > 4000um (瞳孔越大越糊)");
    auto small = MakeGeo(2560, 1440, 14.0f, 40.0f, 4000.0f);
    auto large = MakeGeo(2560, 1440, 14.0f, 40.0f, 8000.0f);
    BlurRadii s = md::core::ComputeBlurRadii(small);
    BlurRadii l = md::core::ComputeBlurRadii(large);
    EXPECT_TRUE(l.blue  > s.blue);
    EXPECT_TRUE(l.green > s.green);
}

// 物理直觉: 屏幕越大（同样分辨率）→ 每像素物理尺寸越大 → 模糊像素数越小
static void test_larger_screen_same_res_smaller_blur() {
    CASE("物理直觉: 同分辨率下 28\" 比 14\" 模糊像素数更小");
    auto s14 = MakeGeo(2560, 1440, 14.0f, 40.0f, 6500.0f);
    auto s28 = MakeGeo(2560, 1440, 28.0f, 40.0f, 6500.0f);
    BlurRadii a = md::core::ComputeBlurRadii(s14);
    BlurRadii b = md::core::ComputeBlurRadii(s28);
    EXPECT_TRUE(a.blue > b.blue);
    EXPECT_TRUE(a.green > b.green);
}

int RunOpticalModelTests() {
    out("============================================\n");
    out("  MyopicDefocus 单测套件\n");
    out("============================================\n");

    out("\n>>>>>>>> Module 1: ComputeBlurRadii <<<<<<<<\n");
    test_js_upstream_defaults();
    test_cpp_defaults();
    test_28inch_same_res();
    test_4k_resolution();
    test_zero_resolution_safe();
    test_far_distance_larger_blur();
    test_very_close_small_blur();
    test_blue_always_more_blur();
    test_larger_pupil_more_blur();
    test_larger_screen_same_res_smaller_blur();
    out("  [ComputeBlurRadii 阶段] passed=%d failed=%d\n", g_passed, g_failed);
    return 0;
}
