// ==========================================
// test_main.cpp — console 测试入口（仅开发期 MyopicDefocusTests.exe 使用）
// ==========================================
#include <cstdio>

extern int RunSelfTest();

int main() {
    int r = RunSelfTest();
    return r;
}
