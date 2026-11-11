// ==========================================
// test_wizard_template.cpp — 首次运行向导模板字节布局断言
// ==========================================
// 背景：向导曾连续五轮静默失败，病理链：
//   L"Dialog" 字符串类名 → 0xFFFF/0x8002「原子」编码（#32770=0x8002 是预定义句柄
//   常量，不是原子表条目，真机实验 E/F 证伪）→ 头部/变长段对齐错位 → 连续 WORD 流（本轮）。
// OS 合约（真机 PowerShell 内存模板对照实验 + wine 解析器一致）：
//   · 头部 18 字节 WORD 连续
//   · menu / windowClass / title / 字体块连续 WORD 流，段间无填充
//   · windowClass 单 WORD 0x0000 = 系统标准对话框类
//   · 只有每个条目（DLGITEMTEMPLATE）起点 DWORD 对齐；条目内 id 后紧跟 class 无填充
// 本文件断言按 v1.8.4 装配逐字节锚定：
//   menu@18、class@20、title@22、font@32、typeface@38、首条目@64
#include "ui/ui.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern int g_passed;
extern int g_failed;
// 定义于 app/selftest.cpp（GUI subsystem 下 stdout 不可见，输出同步写自检日志）
void SelfTestLogAppend(const char* text);

#define EXPECT_TRUE_WIZ(cond) \
    do { if (cond) { ++g_passed; out("  [PASS] %s\n", #cond); } \
         else       { ++g_failed; out("  [FAIL] %s\n", #cond); } } while (0)

#define EXPECT_EQ_WIZ(actual, expected) \
    do { const unsigned _a = static_cast<unsigned>(actual); \
         const unsigned _e = static_cast<unsigned>(expected); \
         if (_a == _e) { ++g_passed; out("  [PASS] %s == 0x%X\n", #actual, _e); } \
         else { ++g_failed; out("  [FAIL] %s: got 0x%X expected 0x%X\n", #actual, _a, _e); } } while (0)

static void out(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SelfTestLogAppend(buf);
    printf("%s", buf);
}

static WORD Rd16(const std::vector<BYTE>& b, size_t off) {
    return static_cast<WORD>(b[off] | (static_cast<WORD>(b[off + 1]) << 8));
}

static DWORD Rd32(const std::vector<BYTE>& b, size_t off) {
    return static_cast<DWORD>(b[off]) |
           (static_cast<DWORD>(b[off + 1]) << 8) |
           (static_cast<DWORD>(b[off + 2]) << 16) |
           (static_cast<DWORD>(b[off + 3]) << 24);
}

int RunWizardTemplateTests() {
    out("\n[TEST] 向导模板字节布局（连续 WORD 流 + 0x0000 类合约锚点）\n");
    const std::vector<BYTE> t = md::ui::BuildWizardTemplateForTest();

    EXPECT_TRUE_WIZ(t.size() > 400);

    // —— 头部 18 字节（WORD 连续，无对齐填充）——
    EXPECT_EQ_WIZ(Rd16(t, 8),  7);     // cdit：说明 + 2×(标签+输入) + 2 按钮
    EXPECT_EQ_WIZ(Rd16(t, 10), 0);     // x
    EXPECT_EQ_WIZ(Rd16(t, 12), 0);     // y
    EXPECT_EQ_WIZ(Rd16(t, 14), 310);   // cx
    EXPECT_EQ_WIZ(Rd16(t, 16), 132);   // cy

    // —— 变长段：连续 WORD 流（menu@18、class@20、title@22、font@32、typeface@38）——
    EXPECT_EQ_WIZ(Rd16(t, 18), 0x0000);          // menu：无菜单
    EXPECT_EQ_WIZ(Rd16(t, 20), 0x0000);          // windowClass：0x0000 = 系统标准对话框类
    EXPECT_EQ_WIZ(Rd16(t, 22), 0x9996);          // title@22：'首' (U+9996) 小端
    EXPECT_EQ_WIZ(Rd16(t, 32), 9);               // pointsize@32
    EXPECT_EQ_WIZ(Rd16(t, 34), 400);             // weight@34
    EXPECT_EQ_WIZ(t[36], 0);                     // bItalic@36
    EXPECT_EQ_WIZ(t[37], 1);                     // bCharSet@37 (DEFAULT_CHARSET)
    EXPECT_EQ_WIZ(Rd16(t, 38), 0x004D);          // typeface@38：'M'（MS Shell Dlg）

    // —— 首条目 @64（typeface 段结束 @64，64%4=0 无填充）：说明文字 STATIC ——
    // WS_CHILD|WS_VISIBLE|SS_LEFT|WS_GROUP：⚠ SS_LEFT 本身 = 0x0（静态控件默认值）
    EXPECT_EQ_WIZ(Rd32(t, 64), 0x50020000);
    EXPECT_EQ_WIZ(Rd32(t, 68), 0);               // exstyle
    EXPECT_EQ_WIZ(Rd16(t, 72), 12);              // x
    EXPECT_EQ_WIZ(Rd16(t, 74), 10);              // y
    EXPECT_EQ_WIZ(Rd16(t, 76), 286);             // cx
    EXPECT_EQ_WIZ(Rd16(t, 78), 28);              // cy
    EXPECT_EQ_WIZ(Rd16(t, 80), 0);               // id (WORD)
    // —— 条目内 id 后紧跟 class，无填充 ——
    EXPECT_EQ_WIZ(Rd16(t, 82), 0x0053);          // "STATIC" 的 'S'
    // class "STATIC\0"=7 WCHAR=14B @82-95 → title@96：'这' (U+8FD9)
    EXPECT_EQ_WIZ(Rd16(t, 96), 0x8FD9);
    // title 25 字符+终止=26 WCHAR=52B @96-147 → extraCount@148=0
    EXPECT_EQ_WIZ(Rd16(t, 148), 0);
    // —— 条目 2 起点 @152（150 对齐）：kStyleStatic 无 WS_GROUP ——
    EXPECT_EQ_WIZ(Rd32(t, 152), 0x50000000);

    return 0;
}
