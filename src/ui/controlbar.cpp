// ==========================================
// controlbar.cpp — 右下角图标 + 右键菜单（自建托盘）
// ==========================================
// 为什么不用系统托盘：某些环境下 Shell_NotifyIcon 会被系统直接拒绝
// （GetLastError = ERROR_ACCESS_DENIED），托盘图标根本建不出来，
// 用户就没有任何可见入口来暂停或退出。
// 这里自建一个贴在右下角的小图标 + 菜单，行为与托盘图标一致，
// 纯 Win32 实现，不经过 explorer，与权限无关。
#include "controlbar.h"

#include "ui.h"

#include <windows.h>
#include <commctrl.h>

namespace md {
namespace ui {
namespace {

constexpr wchar_t kBarClass[] = L"MyopicDefocusTrayIcon";
constexpr int kIconSize = 20;   // 小一点，别抢任务栏位置
constexpr int kGap = 2;

// 与 app.rc 中的 101 ICON "resources/icon.ico" 对应
#ifndef IDI_MYOPIC
#define IDI_MYOPIC MAKEINTRESOURCEW(101)
#endif

enum MenuId : UINT {
    kMenuToggle    = 2001,
    kMenuPanel     = 2002,
    kMenuAutoStart = 2003,
    kMenuQuit      = 2004,
};

struct BarContext {
    HWND          hwnd = nullptr;
    AppState*     state = nullptr;
    PanelCallbacks cb;
    bool          effectOn = true;
    HICON         icon = nullptr;
};

BarContext& Ctx() {
    static BarContext ctx;
    return ctx;
}

void PlaceAtBottomRight(HWND hwnd) {
    RECT wa{};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    }
    const int x = wa.right - kIconSize - kGap;
    const int y = wa.bottom - kIconSize - kGap;
    ::SetWindowPos(hwnd, HWND_TOPMOST, x, y, kIconSize, kIconSize, SWP_SHOWWINDOW);
}

void ShowMenu(HWND hwnd) {
    BarContext& c = Ctx();

    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    const bool effectOn = c.state ? c.state->overlayActive : true;
    const bool autoStart = c.state ? c.state->config.autoStart : false;

    ::AppendMenuW(menu, MF_STRING | (effectOn ? MF_CHECKED : MF_UNCHECKED),
                  kMenuToggle, effectOn ? L"暂停效果" : L"恢复效果");
    ::AppendMenuW(menu, MF_STRING, kMenuPanel, L"打开设置面板");
    ::AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : MF_UNCHECKED),
                  kMenuAutoStart, L"开机自启");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出程序");

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(hwnd);
    const UINT cmd = ::TrackPopupMenu(menu,
                                      TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                      pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
    case kMenuToggle:
        if (c.cb.onToggleOverlay) c.cb.onToggleOverlay();
        break;
    case kMenuPanel:
        if (c.state) ShowSettingsPanel(hwnd, c.state, c.cb);
        break;
    case kMenuAutoStart:
        if (c.state) {
            c.state->config.autoStart = !c.state->config.autoStart;
            detail::SetAutoStartReg(c.state->config.autoStart);
            if (c.cb.onParamsChanged) c.cb.onParamsChanged();
        }
        break;
    case kMenuQuit:
        if (c.cb.onQuit) c.cb.onQuit();
        break;
    default:
        break;
    }
}

LRESULT CALLBACK BarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    BarContext& c = Ctx();

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        c.hwnd = hwnd;
        // 从 exe 资源加载嵌入的图标（ID 101，见 app.rc）
        c.icon = static_cast<HICON>(::LoadImageW(
            cs->hInstance, IDI_MYOPIC, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        if (!c.icon) {
            c.icon = static_cast<HICON>(::LoadImageW(
                cs->hInstance, IDI_MYOPIC, IMAGE_ICON, 16, 16, 0));
        }
        if (!c.icon) {
            // 兜底：系统默认图标（仅在资源缺失时）
            c.icon = static_cast<HICON>(::LoadImageW(
                nullptr, IDI_APPLICATION, IMAGE_ICON, kIconSize, kIconSize, 0));
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        if (c.icon) {
            ::DrawIconEx(dc, 0, 0, c.icon, kIconSize, kIconSize, 0, nullptr, DI_NORMAL);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        ShowMenu(hwnd);
        return 0;

    case WM_LBUTTONDBLCLK:
        if (c.state) ShowSettingsPanel(hwnd, c.state, c.cb);
        return 0;

    case WM_DESTROY:
        if (c.icon) { ::DestroyIcon(c.icon); c.icon = nullptr; }
        c.hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool ShowControlBar(HWND ownerWnd, AppState* state, const PanelCallbacks& cb) {
    BarContext& c = Ctx();
    c.state = state;
    c.cb = cb;
    c.effectOn = state ? state->overlayActive : true;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = BarWndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBarClass;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        registered = true;
    }

    if (!c.hwnd) {
        c.hwnd = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kBarClass, L"MyopicDefocus",
            WS_POPUP,
            0, 0, kIconSize, kIconSize,
            ownerWnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!c.hwnd) return false;
        ::SetLayeredWindowAttributes(c.hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    }

    PlaceAtBottomRight(c.hwnd);
    ::ShowWindow(c.hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(c.hwnd);
    return true;
}

void UpdateControlBarState(bool effectOn) {
    BarContext& c = Ctx();
    c.effectOn = effectOn;
    if (c.hwnd) ::InvalidateRect(c.hwnd, nullptr, FALSE);
}

void HideControlBar() {
    BarContext& c = Ctx();
    if (c.hwnd) ::ShowWindow(c.hwnd, SW_HIDE);
}

} // namespace ui
} // namespace md
