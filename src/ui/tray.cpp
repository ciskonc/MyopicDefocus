// ==========================================
// tray.cpp — 系统托盘图标 + 右键菜单
// ==========================================
// 职责：
//   · 建立/销毁托盘图标，接收鼠标事件
//   · 右键菜单固定四项（顺序固定，不多不少）：
//       1) 暂停效果 / 恢复效果   —— 勾选状态 = 当前效果是否生效
//       2) 打开设置面板
//       3) 开机自启              —— 勾选状态 = 是否已登记开机自启
//       4) 退出程序
//     「对截图和录屏隐藏」开关只在设置面板里，托盘菜单不放。
//   · 气泡提示（Shell_NotifyIcon + NIF_INFO）
//
// 宿主窗口负责把 WM_APP+1 转发给 md::ui::HandleTrayMessage：
//   case md::ui::kTrayCallbackMessage:
//       md::ui::HandleTrayMessage(hWnd, lParam);
//       return 0;

#include "ui.h"

#include <shellapi.h>
#include <objbase.h>

#include "../log.h"

// 老 SDK 上这几个常量可能缺失，给个兜底值，保证编译得过
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_BALLOONCLICK
#define NIN_BALLOONCLICK (WM_USER + 5)
#endif




namespace md {
namespace ui {
namespace {

// ---- 托盘菜单命令 ID ----
enum TrayMenuId : UINT {
    kMenuToggleEffect = 1001,   // 暂停效果 / 恢复效果
    kMenuOpenPanel    = 1002,   // 打开设置面板
    kMenuAutoStart    = 1003,   // 开机自启
    kMenuQuit         = 1004,   // 退出程序
};

// ---- 托盘全局上下文 ----
struct TrayContext {
    HWND                owner = nullptr;
    const AppState*     state = nullptr;
    PanelCallbacks      cb;
    NOTIFYICONDATAW     nid{};
    bool                iconAdded = false;
    HMENU               menu = nullptr;
};

TrayContext& Ctx() {
    static TrayContext ctx;
    return ctx;
}

// 当前托盘图标是否可用
bool HasIcon() {
    TrayContext& c = Ctx();
    return c.iconAdded && c.nid.hWnd != nullptr;
}

// 共享状态的可写指针。
// 说明：types.h 里 InitTray 的 state 形参是 const AppState*，
// 但托盘菜单要能改「开机自启」并回写配置。AppState 是上层共享的同一个对象
// （ShowSettingsPanel 拿到的就是它的非 const 指针），所以这里去掉 const 是安全的。
// 该契约别扭已上报 Lead。
AppState* MutableState() {
    return const_cast<AppState*>(Ctx().state);
}

// 执行一条托盘菜单命令
void RunMenuCommand(UINT cmd) {
    TrayContext& c = Ctx();

    switch (cmd) {
        case kMenuToggleEffect: {
            // 暂停 / 恢复效果由上层处理（它负责停启 overlay 与 GPU 资源）
            if (c.cb.onToggleOverlay) c.cb.onToggleOverlay();
            UpdateTrayMenu();
            break;
        }
        case kMenuOpenPanel: {
            ShowSettingsPanel(c.owner, MutableState(), c.cb);
            break;
        }
        case kMenuAutoStart: {
            AppState* s = MutableState();
            if (!s) break;
            bool enable = !s->config.autoStart;
            s->config.autoStart = enable;
            detail::SetAutoStartReg(enable);
            if (c.cb.onParamsChanged) c.cb.onParamsChanged();
            // 面板里的同名开关跟着变
            detail::RefreshPanelIfOpen();
            UpdateTrayMenu();
            break;
        }
        case kMenuQuit: {
            if (c.cb.onQuit) c.cb.onQuit();
            break;
        }
        default:
            break;
    }
}



} // namespace


// ==========================================
// explorer 重启后重建托盘图标（TaskbarCreated 广播处理）
// ==========================================
void ReaddTrayIcon() {
    TrayContext& c = Ctx();
    if (!c.nid.hWnd) return;   // 托盘从未初始化过

    // explorer 重启会清掉所有已注册图标，nid 数据仍在本地，直接重新 ADD
    if (::Shell_NotifyIconW(NIM_ADD, &c.nid)) {
        c.iconAdded = true;
        NOTIFYICONDATAW ver = c.nid;
        ver.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &ver);
        LogInfo("[tray] icon re-added after TaskbarCreated");
    } else {
        c.iconAdded = false;
        LogWin32("[tray] re-add NIM_ADD", ::GetLastError());
    }
}

// ==========================================
// 初始化托盘图标
// ==========================================
bool InitTray(HWND ownerWnd, AppState* state, const PanelCallbacks& cb) {
    TrayContext& c = Ctx();

    c.owner = ownerWnd;
    c.state = state;
    c.cb = cb;

    // 先把菜单建好，菜单文案/勾选随共享状态走
    UpdateTrayMenu();

    c.nid = NOTIFYICONDATAW{};
    // ⚠ 不能用 sizeof(NOTIFYICONDATAW)：MinGW-w64 的结构体比 Windows 侧多一个
    //   GUID guidItem 字段（16 字节），cbSize 偏大 → Shell_NotifyIconW 直接返回
    //   FALSE（GetLastError = ERROR_ACCESS_DENIED），托盘图标根本建不起来。
    //   Windows 侧的真实大小 = 到 dwInfoFlags 结束处 = 944 字节
    //   （等价于 SDK 里的 NOTIFYICONDATAW_V3_SIZE）。
    c.nid.cbSize = 944;
    c.nid.hWnd = ownerWnd;
    c.nid.uID = kTrayIconId;
    c.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    c.nid.uCallbackMessage = kTrayCallbackMessage;
    // 图标资源嵌入由 Lead 负责（icon.png）；这里先用系统图标自证链路可行，
    // Lead 后续把 LoadIconW(nullptr, IDI_APPLICATION) 换成资源 ID 即可。
    // 用嵌入在 exe 里的图标资源（ID 101，见 app.rc），不要用系统默认图标
    c.nid.hIcon = static_cast<HICON>(::LoadImageW(
        ::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!c.nid.hIcon) {
        c.nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcscpy_s(c.nid.szTip, L"近视离焦护眼");

    if (!::Shell_NotifyIconW(NIM_ADD, &c.nid)) {
        LogWin32("Shell_NotifyIconW(NIM_ADD)", ::GetLastError());
        return false;
    }
    c.iconAdded = true;

    // 版本 4：回调消息 lParam 的低 16 位是鼠标事件，高 16 位是图标 ID
    NOTIFYICONDATAW ver = c.nid;
    ver.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &ver);

    return true;
}

// ==========================================
// 退出时移除托盘图标（防止死图标残留堆积）
// ==========================================
void RemoveTrayIcon() {
    TrayContext& c = Ctx();
    if (!c.nid.hWnd) return;   // 托盘从未初始化过则跳过

    NOTIFYICONDATAW nid = c.nid;
    nid.uFlags = 0;            // NIM_DELETE 只需要 hWnd + uID
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    c.iconAdded = false;
    c.nid.hWnd = nullptr;      // 标记已移除，防重入
}

// ==========================================
// 刷新托盘菜单（文案 + 勾选状态）
// ==========================================
void UpdateTrayMenu() {
    TrayContext& c = Ctx();

    // 菜单每次重建，保证文案/勾选与共享状态一致
    if (c.menu) {
        ::DestroyMenu(c.menu);
        c.menu = nullptr;
    }
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    const bool effectOn = c.state ? c.state->overlayActive : true;
    const bool autoStart = c.state ? c.state->config.autoStart : false;

    // 1) 暂停 / 恢复效果：勾选表示「当前效果是否生效」
    ::AppendMenuW(menu,
                  MF_STRING | (effectOn ? MF_CHECKED : MF_UNCHECKED),
                  kMenuToggleEffect, effectOn ? L"暂停效果" : L"恢复效果");
    // 2) 打开设置面板
    ::AppendMenuW(menu, MF_STRING, kMenuOpenPanel, L"打开设置面板");
    // 3) 开机自启
    ::AppendMenuW(menu,
                  MF_STRING | (autoStart ? MF_CHECKED : MF_UNCHECKED),
                  kMenuAutoStart, L"开机自启");
    // 4) 退出程序
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出程序");

    c.menu = menu;
}

// ==========================================
// 托盘气泡提示
// ==========================================
void ShowTrayTip(const std::wstring& title, const std::wstring& msg) {
    if (!HasIcon()) return;

    TrayContext& c = Ctx();
    NOTIFYICONDATAW nid = c.nid;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO | NIIF_LARGE_ICON;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ==========================================
// 托盘消息处理（宿主窗口把 WM_APP+1 转发过来）
// ==========================================
void HandleTrayMessage(HWND ownerWnd, LPARAM lParam) {
    TrayContext& c = Ctx();

    // 版本 4：低 16 位是鼠标事件，高 16 位是图标 ID；
    // 版本 1（退化）时 lParam 直接是鼠标消息，低 16 位同样能取到。
    const UINT event = LOWORD(lParam);
    const UINT iconId = HIWORD(lParam);
    if (iconId != 0 && iconId != kTrayIconId) return;   // 不是本图标的事件

    HWND hwnd = c.owner ? c.owner : ownerWnd;
    if (!hwnd) return;

    switch (event) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            UpdateTrayMenu();
            if (!c.menu) break;

            POINT pt = {};
            ::GetCursorPos(&pt);

            // 让窗口先拿到前台，否则菜单点完不会自动消失
            ::SetForegroundWindow(hwnd);
            UINT cmd = ::TrackPopupMenu(c.menu,
                                        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                        pt.x, pt.y, 0, hwnd, nullptr);
            ::PostMessageW(hwnd, WM_NULL, 0, 0);
            if (cmd != 0) RunMenuCommand(cmd);
            break;
        }
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_BALLOONCLICK: {
            ShowSettingsPanel(hwnd, MutableState(), c.cb);
            break;
        }
        default:
            break;
    }
}

} // namespace ui
} // namespace md
