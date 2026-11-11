// ==========================================
// integrity.cpp — 进程完整性级别自检与自提升
// ==========================================
// 背景：DSH 工作区整棵树带 `Mandatory Label\Low Mandatory Level`，
// 在里面编译/运行的 exe 会以 Low IL(4096) 启动。此时向 explorer.exe
// (Medium IL) 发送的窗口消息会被 UIPI 拦截，Shell_NotifyIcon 恒返回
// FALSE + GetLastError()==5（ERROR_ACCESS_DENIED）。
//
// 关键：进程 IL = min(令牌 IL, 文件 IL 标签)。文件带 Low 标签时，
// 即使以管理员启动，min(High, Low) 仍是 Low —— 所以「提权」解决不了，
// 且 icacls /setintegritylevel M 需要 SeRelabelPrivilege（非管理员不行）。
// 但进程**可以自己把 IL 从 Low 提升到 Medium**
// （SetTokenInformation(TokenIntegrityLevel)，向上提升不需要任何特权）。
//
// 依据：https://learn.microsoft.com/en-us/windows/win32/secauthz/mandatory-integrity-control
//       https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessage
#include "integrity.h"

#include <windows.h>
#include <sddl.h>

namespace md {

unsigned long GetIntegrityRid() {
    // 查询时只能要 TOKEN_QUERY：TOKEN_ADJUST_DEFAULT 含需要特权的权限位，
    // Low IL 进程拿不到它，OpenProcessToken 会直接失败。
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return 0;
    }

    // 用大缓冲区：MinGW 的 TOKEN_MANDATORY_LABEL 大小可能与系统期望不一致，
    // 直接用 sizeof(该结构) 会返回 ERROR_INSUFFICIENT_BUFFER(122)。
    BYTE buf[512] = {};
    DWORD needed = 0;
    const BOOL ok = GetTokenInformation(token, TokenIntegrityLevel,
                                       buf, sizeof(buf), &needed);
    CloseHandle(token);
    if (!ok) return 0;

    // ⚠ GetTokenInformation(TokenIntegrityLevel) 实际写回的是
    //   TOKEN_MANDATORY_LABEL_AND_ATTRIBUTES { PSID Label; }（8 字节），
    //   而不是 TOKEN_MANDATORY_LABEL { SID_AND_ATTRIBUTES Label; }（16 字节）。
    //   两者首字段都是指针，所以要按 PSID 版本解析，否则取到的 RID 是垃圾值。
    // MinGW-w64 未导出 TOKEN_MANDATORY_LABEL_AND_ATTRIBUTES，这里按 { PSID Label; } 解析
    struct TokenMandatoryLabelAttributes { PSID Label; };
    TokenMandatoryLabelAttributes* tmla =
        reinterpret_cast<TokenMandatoryLabelAttributes*>(buf);
    PSID sid = tmla->Label;
    if (!sid) return 0;
    const DWORD count = GetLengthSid(sid);
    if (count < 4) return 0;
    const BYTE* p = reinterpret_cast<const BYTE*>(sid);
    return static_cast<unsigned long>(p[count - 1]);
}

// 若当前是 Low IL，尝试自提升到 Medium IL。返回提升后的实际 RID。
unsigned long EnsureMediumIntegrity() {
    const unsigned long rid = GetIntegrityRid();
    if (rid == 0) return 0;
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return rid;  // 已 Medium 及以上

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                           TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                           &token)) {
        return rid;
    }

    // Medium 完整性级别的 SID = S-1-16-8192
    // 用字符串 SID 构造，避开 MinGW-w64 缺失 SECURITY_MANDATORY_MEDIUM_SID 宏的问题
    TOKEN_MANDATORY_LABEL tml = {};
    bool ok = false;
    PSID mediumSid = nullptr;
    if (ConvertStringSidToSidW(L"S-1-16-8192", &mediumSid) && mediumSid) {
        tml.Label.Sid = mediumSid;
        ok = SetTokenInformation(token, TokenIntegrityLevel,
                                 &tml, sizeof(tml)) != FALSE;
        LocalFree(mediumSid);
    }
    CloseHandle(token);

    if (ok) {
        const unsigned long after = GetIntegrityRid();
        return after ? after : SECURITY_MANDATORY_MEDIUM_RID;
    }
    return rid;
}

const char* IntegrityName(unsigned long rid) {
    if (rid == 0) return "未知";
    if (rid >= SECURITY_MANDATORY_HIGH_RID) return "High";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return "Medium";
    if (rid >= SECURITY_MANDATORY_LOW_RID) return "Low";
    return "Untrusted";
}

} // namespace md
