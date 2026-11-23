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
    // D2（对端复测发现）：往返用例会真实读写配置文件——不重定向就会碰用户的
    // %APPDATA%\MyopicDefocus\config.json（mtime 被更新；中途被杀还会把
    // 测试值留在用户配置里）。GetAppDataDir 每次现读环境变量，这里改即全局生效。
    //
    // ⚠ sandbox 选 exe 同目录而非 %TEMP%：受限环境（Job Object/低完整性标签）
    //   禁写 %TEMP% 时 CreateDirectory 静默失败，SaveConfig 会回退到 exe 同
    //   目录读到历史残留（实测 5 断言假阴性）。exe 所在目录始终可写。
    {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
            std::string dir(exePath);
            const size_t slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) {
                dir.resize(slash);
                std::string sandbox = dir + "\\selftest-sandbox";
                CreateDirectoryA(sandbox.c_str(), nullptr);
                // ⚠ 必须用 _putenv 而非 SetEnvironmentVariableA：
                //   GetAppDataDir 走 _wgetenv（CRT 环境副本），Win32 层设置
                //   不会同步进 CRT 副本——重定向会静默失效（实测 5 断言假阴性）
                std::string env = "APPDATA=" + sandbox;
                _putenv(env.c_str());
            }
        }
    }

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
