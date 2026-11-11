// ==========================================
// selftest.cpp — 内置自检套件总入口
// ==========================================
// 被两个 target 共用：
//   MyopicDefocus.exe --self-test   （GUI subsystem，输出写日志文件）
//   MyopicDefocusTests.exe           （console subsystem，输出走 stdout）
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

// 由 test_optical_model.cpp 定义
extern int RunOpticalModelTests();
extern int g_passed;
extern int g_failed;
// 由 test_config_io.cpp 定义
extern int RunConfigIoTests();
// 由 test_wizard_template.cpp 定义
extern int RunWizardTemplateTests();

// 自检日志路径（GUI subsystem 下 stdout 不可见，结果写这里）
static std::string GetSelfTestLogPath() {
    char buf[MAX_PATH] = {};
    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len == 0 || len >= MAX_PATH) return "myopic-selftest.log";
    return std::string(buf, len) + "myopic-selftest.log";
}

// 供测试输出助手调用：追加一行到日志
void SelfTestLogAppend(const char* text) {
    static std::ofstream file;
    static bool opened = false;
    if (!opened) {
        file.open(GetSelfTestLogPath(), std::ios::out | std::ios::trunc);
        opened = true;
    }
    if (file.is_open()) {
        file << text;
        file.flush();
    }
}

int RunSelfTest() {
    SelfTestLogAppend("============================================\n");
    SelfTestLogAppend("  MyopicDefocus 自检套件\n");
    SelfTestLogAppend("============================================\n");

    RunOpticalModelTests();
    RunConfigIoTests();
    RunWizardTemplateTests();

    char summary[256];
    snprintf(summary, sizeof(summary),
             "\n============================================\n"
             "  总计: %d 通过, %d 失败\n"
             "  日志: %s\n"
             "============================================\n",
             g_passed, g_failed, GetSelfTestLogPath().c_str());
    SelfTestLogAppend(summary);
    printf("%s", summary);

    return g_failed == 0 ? 0 : 1;
}
