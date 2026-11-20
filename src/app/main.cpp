// ==========================================
// main.cpp — 单 exe 入口（托盘常驻 + 全屏 overlay）
// ==========================================
// 模式：
//   无参数          托盘常驻 + overlay 开启（首次运行弹向导）
//   --self-test     运行内置自检套件，退出码即结果
//   --reset-config  恢复默认配置
//   --help          显示用法
#include "../types.h"
#include "../gpu/gpu.h"
#include "../core/core.h"
#include "../ui/ui.h"
#include "../ui/controlbar.h"
#include "../log.h"
#include "util.h"
#include "integrity.h"
#include "../core/diag_probe.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <thread>


namespace {

md::ui::AppState  g_state;
md::gpu::RenderManager g_render;
HWND  g_mainWnd = nullptr;
HANDLE g_singleMutex = nullptr;

// 耐操性审查 P1-C：应用级退出标志。
// 模态循环（向导对话框/MessageBox/托盘 TrackPopupMenu）会消费 WM_QUIT，
// 导致模态期间的热键/菜单退出失效；改为「置标志 + 主循环与模态返回后检查」。
bool g_quitRequested = false;

// 耐操性审查 P1-F：脏标志——仅当会话内发生过显式参数变更才在退出时写盘，
// 防止「读失败→默认值」被无条件保存覆盖用户配置。
bool g_configDirty = false;

// ── 后台弹窗线程约定（耐操性审查 P2 备案，改代码前必读）──
// 下述三处 std::thread + detach 弹 MessageBox 的约束：
//   1. lambda 内【禁止】触碰任何全局状态（g_state/g_render/LogInfo/堆分配容器）——
//      线程会在进程退出瞬间被硬杀，触碰共享状态=未定义行为
//   2. 弹窗加 MB_TOPMOST：不能 join（没人点会挂死退出），TOPMOST 保证
//      用户至少能看见它；用户没点就消失属已知取舍
//   3. 只允许传值捕获，禁止引用捕获
constexpr UINT WM_APP_QUIT = WM_APP + 3;
constexpr wchar_t kMainClass[] = L"MyopicDefocusMain";

// explorer 重启时系统向所有顶层窗口广播 TaskbarCreated，收到后重建托盘图标
UINT g_msgTaskbarCreated = 0;

// 前向声明：WndProc 的热键处理要用到它们
md::ui::PanelCallbacks g_uiCallbacks;
void OnToggleOverlay();

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // TaskbarCreated：explorer 重启后重建托盘图标
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        md::ui::ReaddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_HOTKEY:
        // 全局热键：托盘不可用（某些环境下 Shell_NotifyIcon 会被拒）时的可靠出口
        switch (wp) {
        case 1:   // Ctrl+Alt+Q 退出
            g_quitRequested = true;   // P1-C：模态循环吞 WM_QUIT，改用标志位
            PostQuitMessage(0);
            return 0;
        case 2:   // Ctrl+Alt+S 打开设置面板
            md::ui::ShowSettingsPanel(hwnd, &g_state, g_uiCallbacks);
            return 0;
        case 3:   // Ctrl+Alt+P 暂停 / 恢复效果
            OnToggleOverlay();
            md::ui::ShowTrayTip(L"MyopicDefocus",
                                 g_state.overlayActive ? L"效果已恢复。" : L"效果已暂停。");
            return 0;
        default:
            break;
        }
        break;

    case md::ui::kTrayCallbackMessage:
        // 托盘右键菜单 / 双击由 UI 层消化
        md::ui::HandleTrayMessage(hwnd, lp);
        return 0;

    case WM_APP_QUIT:
        g_quitRequested = true;   // P1-C：托盘菜单退出同样置标志
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        g_quitRequested = true;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 重新枚举显示器并把解析后的参数灌进 state
void RefreshScreens() {
    g_state.screens = md::core::EnumerateScreens(g_state.config.includeVirtualDisplays);
    for (auto& s : g_state.screens) {
        s.params = g_state.config.ResolveFor(s.id);
    }
}

// 参数变化 → 落盘 + 推给渲染器
void OnParamsChanged() {
    g_configDirty = true;
    md::core::SaveConfig(g_state.config);
    // 耐操性审查 P2：hideFromCapture 开关此前只写 config 不下发渲染器——
    // 面板取消勾选后什么都没发生，重启又被安全闸门改回。现在真切换。
    // （渲染端能力感知：老系统不支持时 SetHideFromCapture 内部按未隐藏处理）
    g_render.SetHideFromCapture(g_state.config.hideFromCapture);
    for (const auto& s : g_state.screens) {
        g_render.UpdateParams(s.id, s.params);
    }
    md::ui::UpdateTrayMenu();
}

// 暂停 / 恢复
// overlay 窗口当前是否可见（主循环与手动暂停共同维护）
static bool g_overlayShown = true;   // 程序启动时 SetupScreens 已把窗口显示出来

void OnToggleOverlay() {
    g_state.overlayActive = !g_state.overlayActive;

    if (!g_state.overlayActive) {
        // 手动暂停：立即隐藏，主循环下一轮停止渲染
        g_render.SetOverlayVisible(false);
        g_overlayShown = false;
        LogInfo("[app] overlay paused");
    }
    // 手动恢复不在这里显示窗口：交给主循环在抓屏成功后显示，
    // 避免游戏运行中恢复时把最后一帧旧画面冻结在屏幕上。
    md::ui::UpdateTrayMenu();
    md::ui::UpdateControlBarState(g_state.overlayActive);
    if (g_state.overlayActive) {
        LogInfo("[app] overlay resumed");
    } else {
        LogInfo("[app] overlay paused");
    }
}

void PrintUsage() {
    const wchar_t* text =
        L"MyopicDefocus — 近视离焦护眼滤镜（Windows 版）\n"
        L"\n"
        L"用法：\n"
        L"  MyopicDefocus.exe              启动（托盘常驻 + 效果开启）\n"
        L"  MyopicDefocus.exe --self-test  运行内置自检套件\n"
        L"  MyopicDefocus.exe --reset-config  恢复默认配置\n"
        L"  MyopicDefocus.exe --help       显示本帮助\n"
        L"\n"
        L"配置文件：%APPDATA%\\MyopicDefocus\\config.json\n";
    MessageBoxW(nullptr, text, L"MyopicDefocus", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // ① 完整性级别自提升（必须最早做）
    //   若 exe 所在目录带 `Mandatory Label\Low Mandatory Level`（DSH 工作区就是），
    //   进程会以 Low IL 运行，向 explorer.exe(Medium IL) 发消息会被 UIPI 拦截，
    //   导致 Shell_NotifyIcon 恒返回 FALSE + err=5（托盘图标建不出来）。
    //   进程 IL = min(令牌 IL, 文件 IL 标签)，所以「提权」没用，但可以自己向上提升。
    {
        const unsigned long before = md::GetIntegrityRid();
        const unsigned long after  = md::EnsureMediumIntegrity();
        char buf[200];
        sprintf_s(buf, "[app] integrity: %s(rid=%lu) -> %s(rid=%lu)  lastErr=%lu",
                  md::IntegrityName(before), before,
                  md::IntegrityName(after), after, GetLastError());
        LogInfo(buf);
    }

    // DPI 感知必须在创建任何窗口前设置
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // ---------------- 命令行 ----------------
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    std::wstring captureDir;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--self-test")            mode = L"self-test";
        else if (a == L"--reset-config")   mode = L"reset";
        else if (a == L"--help" || a == L"-h" || a == L"/?") mode = L"help";
        else if (a.rfind(L"--capture=", 0) == 0) { mode = L"capture"; captureDir = a.substr(10); }
    }
    if (argv) LocalFree(argv);

    if (mode == L"help") { PrintUsage(); return 0; }

    if (mode == L"self-test") {
        extern int RunSelfTest();
        return RunSelfTest();
    }

    if (mode == L"reset") {
        g_state.config = md::core::MakeDefaultConfig();
        md::core::SaveConfig(g_state.config);
        MessageBoxW(nullptr, L"配置已恢复默认。", L"MyopicDefocus", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return 0;
    }

    // ---------------- 单实例 ----------------
    g_singleMutex = CreateMutexW(nullptr, TRUE, L"Local\\MyopicDefocus.SingleInstance");
    if (g_singleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"程序已在运行。\n请在系统托盘（右下角）找到图标并右键打开设置面板。",
                    L"MyopicDefocus", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        if (g_singleMutex) CloseHandle(g_singleMutex);
        return 0;
    }

    // ---------------- 配置 ----------------
    bool hadConfig = md::core::LoadConfig(g_state.config);
    if (!hadConfig) {
        g_state.config = md::core::MakeDefaultConfig();
    }

    // 开机自启状态以注册表为准（配置里存的只是副本）
    g_state.config.autoStart = md::IsAutoStartEnabled();

    // 屏幕物理对角线自动探测（EDID 注册表）
    // 面板不再提供对角线输入——它是硬件固定属性，应由系统数据决定而非用户猜。
    // 耐操性审查 P2：探测失败时**保留磁盘上的历史成功值**——旧逻辑用回退值 24
    // 覆盖了探测成功落盘的 27.5，且日志声称 "kept" 与实际写入矛盾。语义改为：
    //   probe 成功 → 用探测值（变更才落盘）
    //   probe 失败 → 原 keep 语义真正落地（绝不用回退值覆盖）
    {
        const float before = g_state.config.common.diagInch;
        const float probed = md::core::ProbeDiagonalInch();      // 失败返回 0
        if (probed > 0.0f && probed != before) {
            g_state.config.common.diagInch = probed;
            g_configDirty = true;
            md::core::SaveConfig(g_state.config);
            char b[160];
            sprintf_s(b, "[app] diagonal auto-detected: %.1f -> %.1f inch", before, probed);
            LogInfo(b);
        } else if (probed <= 0.0f) {
            char b[160];
            sprintf_s(b, "[app] diagonal probe unavailable - keeping %.1f inch", before);
            LogInfo(b);
        }
    }

    // ⚠ 安全闸门：「对截图和录屏隐藏」一旦关闭，overlay 就会重新进入
    //   Desktop Duplication 的捕获流，形成「抓到自己 → 画面逐帧叠加溶解 →
    //   GPU 打满 → 整机卡死」的递归。这是全屏 overlay 的物理限制，无法绕过。
    //   因此启动时若发现它是关闭的，直接强制恢复开启，避免用户拿到一个
    //   必然卡死的配置。
    if (!g_state.config.hideFromCapture) {
        LogInfo("[app] hideFromCapture was off - forcing it back on (recursion safety)");
        g_state.config.hideFromCapture = true;
        g_configDirty = true;
        md::core::SaveConfig(g_state.config);
        std::thread([] {
            ::MessageBoxW(nullptr,
                (std::wstring(L"检测到配置里「对截图和录屏隐藏」处于关闭状态。") + std::wstring(1, (wchar_t)10) +
                std::wstring(L"该状态会导致画面无限叠加、整机卡死，已自动恢复为开启。") + std::wstring(1, (wchar_t)10) +
                std::wstring(1, (wchar_t)10) +
                L"（如需在截图里看到滤镜效果，请用 MyopicDefocus.exe --capture 导出画面。）").c_str(),
                L"MyopicDefocus", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }).detach();
    }

    RefreshScreens();

    // ---------------- 主窗口（消息宿主，不可见） ----------------
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    // 注意：这里用普通隐藏窗口而不是 HWND_MESSAGE。
    // Shell_NotifyIcon 对 message-only 窗口可能直接返回 FALSE（托盘图标不出现），
    // 而托盘图标是本程序唯一的常驻入口，必须保证它能建起来。
    g_mainWnd = CreateWindowExW(0, kMainClass, L"MyopicDefocus", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_mainWnd) {
        LogWin32("CreateWindowEx(main)", GetLastError());
        return -1;
    }

    // ---------------- GPU 与屏幕 ----------------
    if (!g_render.Init(g_mainWnd)) {
        MessageBoxW(nullptr, L"无法初始化图形设备（D3D11）。\n请确认显卡驱动正常。",
                    L"MyopicDefocus", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return -1;
    }
    g_render.SetFpsCap(g_state.config.fpsCap);
    // 对截图/录屏隐藏（默认开）
    g_render.SetHideFromCapture(g_state.config.hideFromCapture);

    if (!g_render.SetupScreens(g_state.screens)) {
        MessageBoxW(nullptr, L"未能创建任何屏幕的滤镜窗口。",
                    L"MyopicDefocus", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }

    // P1-A 收尾：老系统（Win10 <2004）不支持截图隐藏 API——递归叠加灾难的
    // 唯一系统级防线缺失。渲染端已自动降级（隐藏视为关闭 + 看门狗收紧），
    // 这里给用户一次性的明示，避免"莫名卡死"时无从归因。
    if (!g_render.IsCaptureHidingSupported()) {
        std::thread([] {
            ::MessageBoxW(nullptr,
                L"当前 Windows 版本（Win10 2004 之前）不支持「对截图/录屏隐藏」。\n\n"
                L"软件已自动改用增强看门狗防护，但极端情况下（滤镜画面自我叠加）\n"
                L"仍可能造成画面异常，此时请用 Ctrl+Alt+Q 退出。\n\n"
                L"建议升级到 Win10 2004 或更新版本以获得完整防护。",
                L"MyopicDefocus", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }).detach();
    }

    // 注册 TaskbarCreated 广播消息（explorer 重启时系统广播）
    g_msgTaskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_msgTaskbarCreated) {
        LogWin32("RegisterWindowMessage(TaskbarCreated)", GetLastError());
    }

    // ---------------- 全局热键（可靠出口） ----------------
    // overlay 是 NOACTIVATE，键盘焦点不在它身上，Esc 未必生效；
    // 托盘图标又可能被任务栏遮挡，某些环境下 Shell_NotifyIcon 还会直接被拒。
    // 因此无条件注册三个全局热键，保证用户永远有办法操作与退出本程序。
    if (!::RegisterHotKey(g_mainWnd, 1, MOD_CONTROL | MOD_ALT, 'Q')) {
        LogWin32("RegisterHotKey(Ctrl+Alt+Q)", GetLastError());
    }
    if (!::RegisterHotKey(g_mainWnd, 2, MOD_CONTROL | MOD_ALT, 'S')) {
        LogWin32("RegisterHotKey(Ctrl+Alt+S)", GetLastError());
    }
    if (!::RegisterHotKey(g_mainWnd, 3, MOD_CONTROL | MOD_ALT, 'P')) {
        // 1409 = ERROR_HOTKEY_ALREADY_REGISTERED（被远程控制类软件占用，真机实测）
        if (::RegisterHotKey(g_mainWnd, 3, MOD_CONTROL | MOD_ALT, 'O')) {
            LogInfo("[app] Ctrl+Alt+P taken by another app - pause hotkey falls back to Ctrl+Alt+O");
        } else {
            LogWin32("RegisterHotKey pause (both P and O)", GetLastError());
        }
    }

    // ---------------- 托盘 ----------------
    md::ui::PanelCallbacks& cb = g_uiCallbacks;
    cb.onParamsChanged    = OnParamsChanged;
    cb.onToggleOverlay   = OnToggleOverlay;
    cb.onRestartRequired = []() {};
    cb.onQuit             = []() { PostMessageW(g_mainWnd, WM_APP_QUIT, 0, 0); };

    // 递归自愈：检测到画面自我叠加时，自动恢复「对截图和录屏隐藏」并告知用户
    g_render.SetRecursionCallback(
        [](void* user) {
            auto* st = static_cast<md::ui::AppState*>(user);
            if (!st) return;
            st->config.hideFromCapture = true;
            g_configDirty = true;
            md::core::SaveConfig(st->config);
            md::ui::ShowTrayTip(L"MyopicDefocus",
                L"检测到画面在自我叠加（会导致卡死），\n"
                L"已自动重新开启「对截图和录屏隐藏」。");
        },
        &g_state);

    if (!md::ui::InitTray(g_mainWnd, &g_state, cb)) {
        LogInfo("[app] InitTray failed - 托盘不可用，改用全局热键操作");
        // 必须放到独立线程：模态 MessageBox 会把主循环卡住，
        // 结果是「画面在动但程序完全不响应」，用户连退出都做不到。
        std::thread([] {
            ::MessageBoxW(nullptr,
                L"系统托盘图标无法创建。\n\n"
                L"最常见原因：此 exe 所在目录被标记为「低完整性级别」\n"
                L"（Mandatory Label\\Low Mandatory Level），进程被降权后，\n"
                L"向 explorer.exe 发的消息被 UIPI 拦截。\n\n"
                L"解决办法：把 exe 复制到桌面或 %LOCALAPPDATA% 下的目录再运行。\n\n"
                L"程序仍正常工作，可用以下快捷键：\n"
                L"    Ctrl+Alt+S   打开设置面板\n"
                L"    Ctrl+Alt+P   暂停 / 恢复效果\n"
                L"    Ctrl+Alt+Q   退出程序",
                L"MyopicDefocus", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        }).detach();

        // 托盘不可用时，用纯 Win32 的右下角小图标兜底：
        // 不经过 explorer，权限无关，永远可见可点。
        // floatingIconMode：-1 自动（托盘失败才显示）/ 0 强制不显示 / 1 强制显示
        if (g_state.config.floatingIconMode != 0) {
            md::ui::ShowControlBar(g_mainWnd, &g_state, cb);
        }
    }

    // ---------------- 取证模式：存一帧就退出 ----------------
    if (mode == L"capture") {
        // 取证模式：抓第 1 帧 → 再跑 200 帧 → 抓第 2 帧。
        // 两帧的清晰度差即可判定是否存在「抓到自己」的递归叠加。
        g_render.RenderAll();
        g_render.SaveFrames(captureDir, L"f1");
        for (int i = 0; i < 200; ++i) {
            g_render.RenderAll();
            Sleep(5);
        }
        g_render.SaveFrames(captureDir, L"f2");
        g_render.Shutdown();
        return 0;
    }

    // ---------------- 首次运行向导 ----------------
    if (!hadConfig) {
        if (md::ui::RunFirstRunWizard(g_mainWnd, &g_state)) {
            RefreshScreens();
            OnParamsChanged();
        }
    }

    // ---------------- 主循环 ----------------
    // 可见性模型：overlayActive 只表示「用户想要效果」，
    // 滤镜窗口实际显示与否 = overlayActive && 抓屏健康。
    // 抓不到画面（独占全屏游戏）时自动隐藏，绝不把旧帧冻结在屏幕上；
    // 抓屏恢复后自动回来，无需用户干预。
    // 窗口可见性状态源 = g_overlayShown（文件级，OnToggleOverlay 也会更新它）
    bool wasDegraded  = false;    // 上一轮抓屏健康状态（边沿提示用）

    // 耐操性审查 P2（J-3）：`cmd start /b` 等非标准启动方式会得到一个
    // 「初始化完成但消息循环永不运转」的僵死实例（实测 CPU=0/无窗口/无日志）。
    // 自检：主循环建立 10 秒内必须消费过至少一条消息或渲染过一帧，
    // 否则判定僵死自杀退出（日志留痕），避免留下无声进程。
    const DWORD mainLoopStart = GetTickCount();
    bool mainLoopProvenAlive = false;

    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            mainLoopProvenAlive = true;
            if (msg.message == WM_QUIT) goto shutdown;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // P1-C：模态循环可能已吞掉 WM_QUIT，但标志位一定还在——兜底退出
        if (g_quitRequested) goto shutdown;

        if (!mainLoopProvenAlive &&
            GetTickCount() - mainLoopStart > 10000) {
            LogInfo("[app] main loop unresponsive for 10s after startup (non-interactive launch?) - exiting");
            goto shutdown;
        }

        if (g_state.overlayActive) {
            // 持续渲染：抓屏健康时更新画面；降级时靠 CaptureFrame 的结果
            // 探测游戏是否退出（这是恢复显示的唯一依据）
            g_render.RenderAll();
            DwmFlush();
            static int beat = 0;
            if ((++beat % 600) == 0) LogInfo("[app] heartbeat - main loop alive");

            const bool degraded = g_render.HasDegradedScreen();
            g_state.captureDegraded = degraded;

            // 窗口可见性跟随抓屏健康度
            const bool shouldShow = !degraded;
            if (shouldShow != g_overlayShown) {
                g_render.SetOverlayVisible(shouldShow);
                g_overlayShown = shouldShow;
                if (!shouldShow) {
                    LogInfo("[app] overlay hidden - capture degraded (fullscreen game)");
                    md::ui::ShowTrayTip(L"MyopicDefocus",
                        L"检测到全屏独占游戏，效果已暂停。" + std::wstring(1, (wchar_t)10) +
                        L"切回桌面后会自动恢复。");
                } else {
                    LogInfo("[app] overlay shown - capture healthy");
                    md::ui::ShowTrayTip(L"MyopicDefocus", L"已恢复离焦效果。");
                }
            }

            // 降级状态边沿提示（配置关闭自动隐藏时提示改无边框）
            if (degraded && !wasDegraded &&
                !g_state.config.pauseOnFullscreenGame) {
                md::ui::ShowTrayTip(L"MyopicDefocus",
                    L"抓不到屏幕画面（可能正在运行全屏独占游戏）。" + std::wstring(1, (wchar_t)10) +
                    L"请把游戏改为无边框窗口模式。");
            }
            wasDegraded = degraded;
        } else {
            // 手动暂停：不渲染，省资源
            g_state.captureDegraded = false;
            wasDegraded = false;
            Sleep(100);
        }
    }

shutdown:
    md::ui::RemoveTrayIcon();   // 退出前移除托盘图标（防死图标堆积）
    g_render.Shutdown();
    // 耐操性审查 P1-F：只有本次会话确实改过参数才落盘。
    // 无条件保存会把「读失败 → 内存里是默认值」的状态覆盖回磁盘，抹掉用户手调参数
    //（config 被存成 UTF-16 等编码错误场景）。启动后的显式变更都会置 dirty。
    if (g_configDirty) md::core::SaveConfig(g_state.config);
    if (g_singleMutex) { CloseHandle(g_singleMutex); g_singleMutex = nullptr; }
    return 0;
}
