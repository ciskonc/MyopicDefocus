// ==========================================
// panel.cpp — 设置面板
// ==========================================
// 形态：独立弹出窗口（WS_EX_TOOLWINDOW | WS_EX_TOPMOST），
//       非模态、不阻塞调用方。ShowSettingsPanel 建好窗口就返回，
//       面板自己的消息由宿主主消息循环分发（不会死锁）。
//
// 布局（客户区 420 × 520）：
//   A 区「一套通用参数」   4 个滑块 + 说明文字
//   B 区「单独改这块屏」   屏幕下拉框 + 该屏 4 个滑块
//                         +「恢复使用通用设置」/「全部恢复通用设置」
//   C 区「其他设置」       对截图和录屏隐藏 / 开机自启
//
// 交互规则：
//   · 改通用参数 → 没被单独改过的屏跟着变；被单独改过的屏不受影响
//   · 第一次在某屏拖滑块 → 该屏开始「单独调整」（把通用值整体复制成该屏的覆盖）
//   · 「恢复使用通用设置」→ 该屏回到通用参数
//   · 「全部恢复通用设置」→ 所有屏回到通用参数
//   · 面板里的「开机自启」与托盘菜单里的同名项联动
//
// 术语：界面上只说「一套通用参数」和「单独改这块屏」，
//       不出现「继承全局 / 单独指定」这类说法。

#include "ui.h"

#include <dwmapi.h>

#include <commctrl.h>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace md {
namespace ui {
namespace {

// ==========================================
// 常量
// ==========================================

constexpr wchar_t kPanelClassName[] = L"MDMyopicDefocusSettingsPanel";

constexpr int kClientWidth  = 420;
constexpr int kClientHeight = 470;

// 参数槽位
enum ParamSlot {
    kSlotStrength = 0,   // 效果强度 0 ~ 1
    kSlotDistance,       // 观看距离 cm
    kSlotPupil,          // 瞳孔直径 μm
    kSlotCount
};

// 每个滑块的量程：左端值 + 每格步进 + 滑块刻度上限
struct SlotSpec {
    float          minValue;    // 滑块最左端对应的实际值
    float          stepValue;   // 每格对应的实际值
    int            trackMax;    // 滑块刻度上限（最右端）
    const wchar_t* label;
};

const SlotSpec kSlots[kSlotCount] = {
    {   0.0f,   0.01f, 100, L"效果强度"   },   // 0 ~ 1，步长 0.01
    {  10.0f,   1.0f,  140, L"观看距离"   },   // 10 ~ 150 cm，步长 1
    { 2000.0f, 100.0f,  70, L"瞳孔直径"   },   // 2000 ~ 9000 μm，步长 100
};

// 控件 ID
enum PanelCtrlId {
    kIdCommonTrackBase = 1000,   // 1000 ~ 1003 通用区滑块
    kIdCommonValueBase = 1010,   // 1010 ~ 1013 通用区数值
    kIdScreenCombo     = 1020,
    kIdScreenTitle     = 1021,
    kIdScreenTrackBase = 1030,   // 1030 ~ 1033 单屏区滑块
    kIdScreenValueBase = 1040,   // 1040 ~ 1043 单屏区数值
    kIdBtnResetScreen  = 1050,
    kIdBtnResetAll     = 1051,
    kIdChkCapture      = 1060,
    kIdChkAutoStart    = 1061,
    kIdChkGamePause   = 1062,
    kIdBtnClose        = 1070
};

// 版面尺寸
constexpr int kGroupX     = 8;
constexpr int kGroupW     = 404;
constexpr int kInnerX     = 14;
constexpr int kLabelW     = 70;     // 「屏幕对角线」5 个汉字，70px 够放
constexpr int kTrackX     = 88;
constexpr int kTrackW     = 244;    // 伸展填满标签与数值之间的空间，不留空洞
constexpr int kValueX     = 338;
constexpr int kValueW     = 64;     // 数值最宽 '6500 μm' 约 55px，紧凑贴右
constexpr int kRowPitch   = 26;
constexpr int kRowH       = 20;
constexpr int kTrackH     = 22;

// ==========================================
// 面板状态（单例：同一时刻只有一个面板）
// ==========================================

struct PanelState {
    AppState*      app = nullptr;
    PanelCallbacks cb;
    HWND           hwnd = nullptr;

    HWND commonTrack[kSlotCount] = {};
    HWND commonValue[kSlotCount] = {};
    HWND combo          = nullptr;
    HWND title          = nullptr;
    HWND screenTrack[kSlotCount] = {};
    HWND screenValue[kSlotCount] = {};
    HWND btnResetScreen = nullptr;
    HWND btnResetAll    = nullptr;
    HWND chkCapture     = nullptr;
    HWND chkAutoStart   = nullptr;
    HWND chkGamePause   = nullptr;
    HWND statHint       = nullptr;

    // 每块屏是否已「单独调整」：面板打开时按 ScreenOverride::IsEmpty() 取快照
    std::vector<bool> overridden;
    size_t            screenCount = 0;   // 上次刷新时的屏幕数量
};

PanelState& P() {
    static PanelState s;
    return s;
}

// ==========================================
// 基础工具
// ==========================================

// 初始化公共控件（Trackbar 属于 comctl32）。
// 用动态加载的方式调 InitCommonControlsEx，避免依赖 comctl32.lib 链接。
void EnsureCommonControls() {
    static bool done = false;
    if (done) return;
    done = true;

    using InitCCExFn = BOOL(WINAPI*)(INITCOMMONCONTROLSEX*);
    HMODULE mod = ::LoadLibraryW(L"comctl32.dll");
    if (!mod) return;
    FARPROC proc = ::GetProcAddress(mod, "InitCommonControlsEx");
    if (!proc) return;
    InitCCExFn fn = nullptr;
    std::memcpy(&fn, &proc, sizeof(fn));
    if (!fn) return;

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    fn(&icc);
}

// 给窗口套上系统主题（Explorer）。同样是动态拿 SetWindowTheme，
// 不依赖 uxtheme.lib。
void ApplyExplorerTheme(HWND hwnd) {
    using SetThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static SetThemeFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE mod = ::LoadLibraryW(L"uxtheme.dll");
        if (mod) {
            FARPROC proc = ::GetProcAddress(mod, "SetWindowTheme");
            if (proc) std::memcpy(&fn, &proc, sizeof(fn));
        }
    }
    if (fn) fn(hwnd, L"Explorer", nullptr);
}

// 建一个子控件
HWND AddChild(HWND parent, const wchar_t* cls, const wchar_t* text,
              DWORD style, int x, int y, int w, int h, int id) {
    return ::CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr), nullptr);
}

// 滑块刻度 ↔ 实际值
int ValueToPos(const SlotSpec& spec, float v) {
    int pos = static_cast<int>(std::lround((v - spec.minValue) / spec.stepValue));
    if (pos < 0) pos = 0;
    if (pos > spec.trackMax) pos = spec.trackMax;
    return pos;
}

float PosToValue(const SlotSpec& spec, int pos) {
    return spec.minValue + static_cast<float>(pos) * spec.stepValue;
}

void SetTrack(HWND tb, const SlotSpec& spec, float v) {
    if (!tb) return;
    ::SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, spec.trackMax));
    ::SendMessageW(tb, TBM_SETPOS, TRUE, static_cast<LPARAM>(ValueToPos(spec, v)));
}

float GetTrackValue(HWND tb, const SlotSpec& spec) {
    if (!tb) return spec.minValue;
    int pos = static_cast<int>(::SendMessageW(tb, TBM_GETPOS, 0, 0));
    return PosToValue(spec, pos);
}

// 数值显示文本
std::wstring FormatValue(int slot, float v) {
    wchar_t buf[64] = {};
    switch (slot) {
        case kSlotStrength:
            swprintf_s(buf, L"%.2f", static_cast<double>(v));
            break;
        case kSlotDistance:
            swprintf_s(buf, L"%d cm", static_cast<int>(std::lround(v)));
            break;
        case kSlotPupil:
            swprintf_s(buf, L"%d \u00B5m", static_cast<int>(std::lround(v)));
            break;
        default:
            break;
    }
    return std::wstring(buf);
}

void SetValueLabel(HWND label, int slot, float v) {
    if (!label) return;
    std::wstring text = FormatValue(slot, v);
    ::SetWindowTextW(label, text.c_str());
}

// 参数读写
float SlotValue(const md::core::DefocusParams& p, int slot) {
    switch (slot) {
        case kSlotStrength: return p.effectStrength;
        case kSlotDistance: return p.screenDistanceCM;
        case kSlotPupil:    return p.pupilSizeUm;
        default: return 0.0f;
    }
}

void SetSlotValue(md::core::DefocusParams& p, int slot, float v) {
    switch (slot) {
        case kSlotStrength: p.effectStrength = v; break;
        case kSlotDistance: p.screenDistanceCM = v; break;
        case kSlotPupil:    p.pupilSizeUm = v; break;
        default: break;
    }
}

// 屏幕下拉项文字：\\.\DISPLAY1  1920×1080 60Hz
std::wstring ScreenItemText(const md::core::ScreenInfo& s) {
    std::wstring text = detail::Utf8ToWide(s.deviceName);
    text += L"  ";
    text += std::to_wstring(s.width);
    text += L"\u00D7";
    text += std::to_wstring(s.height);
    if (s.refreshHz > 0) {
        text += L" ";
        text += std::to_wstring(s.refreshHz);
        text += L"Hz";
    }
    return text;
}

// 当前选中的屏幕下标；没有则 -1
int CurrentScreenIndex() {
    PanelState& ps = P();
    if (!ps.combo) return -1;
    LRESULT idx = ::SendMessageW(ps.combo, CB_GETCURSEL, 0, 0);
    if (idx == CB_ERR || idx < 0) return -1;
    return static_cast<int>(idx);
}

// 通知上层：参数变了
void NotifyParamsChanged() {
    PanelState& ps = P();
    if (ps.cb.onParamsChanged) ps.cb.onParamsChanged();
}

// ==========================================
// 控件同步
// ==========================================

// 通用参数 → 控件
void SyncCommonControls() {
    PanelState& ps = P();
    if (!ps.app) return;
    for (int i = 0; i < kSlotCount; ++i) {
        float v = SlotValue(ps.app->config.common, i);
        SetTrack(ps.commonTrack[i], kSlots[i], v);
        SetValueLabel(ps.commonValue[i], i, v);
    }
}

// 复选框 → 共享状态
void SyncCheckboxes() {
    PanelState& ps = P();
    if (!ps.app) return;
    if (ps.chkCapture) {
        ::SendMessageW(ps.chkCapture, BM_SETCHECK,
                       ps.app->config.hideFromCapture ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (ps.chkAutoStart) {
        ::SendMessageW(ps.chkAutoStart, BM_SETCHECK,
                       ps.app->config.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (ps.chkGamePause) {
        ::SendMessageW(ps.chkGamePause, BM_SETCHECK,
                       ps.app->config.pauseOnFullscreenGame ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

// 单屏区 → 控件
void SyncScreenControls() {
    PanelState& ps = P();
    const int idx = CurrentScreenIndex();
    const bool hasScreen =
        ps.app && idx >= 0 && idx < static_cast<int>(ps.app->screens.size());

    if (!hasScreen) {
        for (int i = 0; i < kSlotCount; ++i) {
            SetTrack(ps.screenTrack[i], kSlots[i], kSlots[i].minValue);
            SetValueLabel(ps.screenValue[i], i, kSlots[i].minValue);
            if (ps.screenTrack[i]) ::EnableWindow(ps.screenTrack[i], FALSE);
        }
        ::SetWindowTextW(ps.title, L"未检测到显示器");
        if (ps.btnResetScreen) ::EnableWindow(ps.btnResetScreen, FALSE);
        return;
    }

    const md::core::ScreenInfo& scr = ps.app->screens[idx];
    const bool overridden =
        (idx < static_cast<int>(ps.overridden.size())) ? ps.overridden[idx] : false;

    const md::core::DefocusParams p =
        detail::ResolveParams(ps.app->config, scr.id);
    for (int i = 0; i < kSlotCount; ++i) {
        float v = SlotValue(p, i);
        SetTrack(ps.screenTrack[i], kSlots[i], v);
        SetValueLabel(ps.screenValue[i], i, v);
        if (ps.screenTrack[i]) ::EnableWindow(ps.screenTrack[i], TRUE);
    }

    std::wstring title = L"屏幕 ";
    title += std::to_wstring(idx + 1);
    title += L"：";
    title += detail::Utf8ToWide(scr.deviceName);
    title += overridden ? L"（已单独调整）" : L"（使用通用设置）";
    ::SetWindowTextW(ps.title, title.c_str());

    if (ps.btnResetScreen) {
        ::EnableWindow(ps.btnResetScreen, overridden ? TRUE : FALSE);
    }
}

// 下拉框重建
void FillScreenCombo() {
    PanelState& ps = P();
    if (!ps.combo) return;

    ::SendMessageW(ps.combo, CB_RESETCONTENT, 0, 0);
    if (!ps.app) return;
    for (const auto& s : ps.app->screens) {
        std::wstring text = ScreenItemText(s);
        ::SendMessageW(ps.combo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (!ps.app->screens.empty()) {
        ::SendMessageW(ps.combo, CB_SETCURSEL, 0, 0);
    }
}

// 记录每块屏「是否已单独调整」的快照
void SnapshotOverrides() {
    PanelState& ps = P();
    ps.overridden.clear();
    if (!ps.app) return;
    ps.overridden.resize(ps.app->screens.size(), false);
    for (size_t i = 0; i < ps.app->screens.size(); ++i) {
        const md::core::ScreenOverride* ov =
            detail::FindOverride(ps.app->config, ps.app->screens[i].id);
        ps.overridden[i] = (ov && !ov->IsEmpty());
    }
}

// ==========================================
// 参数变更处理
// ==========================================

// 通用参数变了
void ApplyCommonChange(int slot, float v) {
    PanelState& ps = P();
    if (!ps.app) return;

    SetSlotValue(ps.app->config.common, slot, v);
    SetValueLabel(ps.commonValue[slot], slot, v);

    // 没被单独改过的屏跟着通用值走
    for (size_t i = 0; i < ps.app->screens.size(); ++i) {
        if (i < ps.overridden.size() && ps.overridden[i]) continue;
        ps.app->screens[i].params =
            detail::ResolveParams(ps.app->config, ps.app->screens[i].id);
    }

    // 当前选中屏若没被单独改过，滑块也跟着刷
    const int idx = CurrentScreenIndex();
    if (idx >= 0 && idx < static_cast<int>(ps.overridden.size()) &&
        !ps.overridden[idx]) {
        SyncScreenControls();
    }

    NotifyParamsChanged();
}

// 单屏参数变了
void ApplyScreenChange(int slot, float v) {
    PanelState& ps = P();
    if (!ps.app) return;

    const int idx = CurrentScreenIndex();
    if (idx < 0 || idx >= static_cast<int>(ps.app->screens.size())) return;
    const std::string& id = ps.app->screens[idx].id;

    // 第一次拖动 = 开始单独调整这块屏：
    // 先把通用值整体复制成这块屏的覆盖，之后这块屏不再跟随通用值
    if (idx >= static_cast<int>(ps.overridden.size()) || !ps.overridden[idx]) {
        md::core::ScreenOverride* ov = detail::MutableOverride(ps.app->config, id);
        const md::core::DefocusParams& c = ps.app->config.common;
        ov->effectStrength   = c.effectStrength;
        ov->screenDistanceCM = c.screenDistanceCM;
        ov->pupilSizeUm      = c.pupilSizeUm;
        ov->diagInch         = c.diagInch;

        if (idx >= static_cast<int>(ps.overridden.size())) {
            ps.overridden.resize(ps.app->screens.size(), false);
        }
        ps.overridden[idx] = true;
    }

    md::core::ScreenOverride* ov = detail::MutableOverride(ps.app->config, id);
    switch (slot) {
        case kSlotStrength: ov->effectStrength = v; break;
        case kSlotDistance: ov->screenDistanceCM = v; break;
        case kSlotPupil:    ov->pupilSizeUm = v; break;
        default: break;
    }

    ps.app->screens[idx].params = detail::ResolveParams(ps.app->config, id);
    SyncScreenControls();      // 刷新这块屏的 4 个滑块与标题
    NotifyParamsChanged();
}

// 当前屏恢复通用设置
void ResetCurrentScreen() {
    PanelState& ps = P();
    if (!ps.app) return;

    const int idx = CurrentScreenIndex();
    if (idx < 0 || idx >= static_cast<int>(ps.app->screens.size())) return;

    detail::ClearOverride(ps.app->config, ps.app->screens[idx].id);
    if (idx < static_cast<int>(ps.overridden.size())) ps.overridden[idx] = false;
    ps.app->screens[idx].params =
        detail::ResolveParams(ps.app->config, ps.app->screens[idx].id);

    SyncScreenControls();
    NotifyParamsChanged();
}

// 全部恢复通用设置
void ResetAllScreens() {
    PanelState& ps = P();
    if (!ps.app) return;

    for (size_t i = 0; i < ps.app->screens.size(); ++i) {
        detail::ClearOverride(ps.app->config, ps.app->screens[i].id);
        ps.app->screens[i].params =
            detail::ResolveParams(ps.app->config, ps.app->screens[i].id);
    }
    ps.overridden.assign(ps.app->screens.size(), false);

    SyncScreenControls();
    NotifyParamsChanged();
}

// ==========================================
// 暗色标题栏 + Win11 圆角
// ==========================================
// 不设置时窗口标题栏是 Win95 风格的浅灰老样式（含右上角关闭按钮）。
// DWMWA_USE_IMMERSIVE_DARK_MODE 让 DWM 按系统深色主题画标题栏；
// DWMWA_WINDOW_CORNER_PREFERENCE=ROUND 是 Win11 的窗口圆角。
// 两者都是公开 API（dwmapi.dll），失败静默降级为默认样式。
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20   // 1903+；1809 上是 19
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

static void ApplyModernWindowChrome(HWND hwnd) {
    BOOL dark = TRUE;
    if (FAILED(::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                       &dark, sizeof(dark)))) {
        // 老系统上属性号不同，回退 19
        ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1,
                                &dark, sizeof(dark));
    }
    INT round = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                            &round, sizeof(round));
}

// ==========================================
// 建界面
// ==========================================

// ==========================================
// 统一 UI 字体
// ==========================================
// Win32 控件默认用 GDI 的 "System" 字体（中文环境下是宋体 12pt），
// 字号偏大，静态文本会撑出预设宽度、压到旁边的按钮上。
// 这里统一换成系统 UI 字体（Segoe UI / 微软雅黑 9pt）。
void ApplyUiFont(HWND parent) {
    HFONT font = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    if (!font) return;

    ::SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND child = ::GetWindow(parent, GW_CHILD); child;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void BuildPanel(HWND hwnd) {
    PanelState& ps = P();

    // ---- A 区：一套通用参数 ----
    AddChild(hwnd, L"BUTTON", L"一套通用参数", BS_GROUPBOX,
             kGroupX, 8, kGroupW, 120, 0);

    for (int i = 0; i < kSlotCount; ++i) {
        const int y = 26 + i * kRowPitch;
        AddChild(hwnd, L"STATIC", kSlots[i].label, SS_LEFT,
                 kInnerX, y + 2, kLabelW, kRowH, 0);
        ps.commonTrack[i] = AddChild(hwnd, TRACKBAR_CLASS, nullptr,
                                     TBS_HORZ | TBS_NOTICKS | TBS_DOWNISLEFT | WS_TABSTOP,
                                     kTrackX, y, kTrackW, kTrackH,
                                     kIdCommonTrackBase + i);
        ps.commonValue[i] = AddChild(hwnd, L"STATIC", L"", SS_RIGHT,
                                     kValueX, y + 2, kValueW, kRowH,
                                     kIdCommonValueBase + i);
    }
    AddChild(hwnd, L"STATIC", L"这一组是所有屏幕默认都用这套。", SS_LEFT,
             kInnerX, 106, kGroupW - 12, 16, 0);

    // ---- B 区：单独改这块屏 ----
    AddChild(hwnd, L"BUTTON", L"单独改这块屏", BS_GROUPBOX,
             kGroupX, 134, kGroupW, 192, 0);

    // 下拉框：创建高度 = 收起高度 + 下拉列表高度；
    // 收起时只有顶部一条是可见的，下面的区域留给后面的控件（后建的控件层级更高，不受影响）
    ps.combo = AddChild(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                         kInnerX, 152, kGroupW - 12, 124, kIdScreenCombo);
    ps.title = AddChild(hwnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS,
                        kInnerX, 182, 250, 20, kIdScreenTitle);
    ps.btnResetScreen = AddChild(hwnd, L"BUTTON", L"恢复使用通用设置", BS_PUSHBUTTON | WS_TABSTOP,
                                 272, 180, 136, 24, kIdBtnResetScreen);

    for (int i = 0; i < kSlotCount; ++i) {
        const int y = 210 + i * kRowPitch;
        AddChild(hwnd, L"STATIC", kSlots[i].label, SS_LEFT,
                 kInnerX, y + 2, kLabelW, kRowH, 0);
        ps.screenTrack[i] = AddChild(hwnd, TRACKBAR_CLASS, nullptr,
                                     TBS_HORZ | TBS_NOTICKS | TBS_DOWNISLEFT | WS_TABSTOP,
                                     kTrackX, y, kTrackW, kTrackH,
                                     kIdScreenTrackBase + i);
        ps.screenValue[i] = AddChild(hwnd, L"STATIC", L"", SS_RIGHT,
                                     kValueX, y + 2, kValueW, kRowH,
                                     kIdScreenValueBase + i);
    }
    ps.btnResetAll = AddChild(hwnd, L"BUTTON", L"全部恢复通用设置", BS_PUSHBUTTON | WS_TABSTOP,
                              kInnerX, 290, 150, 26, kIdBtnResetAll);

    // ---- C 区：其他设置 ----
    AddChild(hwnd, L"BUTTON", L"其他设置", BS_GROUPBOX,
             kGroupX, 312, kGroupW, 104, 0);
    ps.chkCapture = AddChild(hwnd, L"BUTTON", L"对截图和录屏隐藏", BS_AUTOCHECKBOX | WS_TABSTOP,
                              16, 328, 240, 22, kIdChkCapture);
    ps.chkAutoStart = AddChild(hwnd, L"BUTTON", L"开机自启", BS_AUTOCHECKBOX | WS_TABSTOP,
                               16, 354, 240, 22, kIdChkAutoStart);
    ps.chkGamePause = AddChild(hwnd, L"BUTTON", L"检测到全屏游戏时自动暂停效果",
                               BS_AUTOCHECKBOX | WS_TABSTOP,
                               16, 380, 300, 22, kIdChkGamePause);

    // ---- 底部 ----
    ps.statHint = AddChild(hwnd, L"STATIC", L"", SS_LEFT,
                           kInnerX, 424, 270, 20, 0);
    AddChild(hwnd, L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP,
             300, 422, 106, 28, kIdBtnClose);

    // 统一字体（必须在所有控件创建完后调用）
    ApplyUiFont(hwnd);

    // ---- 初值 ----
    FillScreenCombo();
    SnapshotOverrides();
    SyncCommonControls();
    SyncScreenControls();
    SyncCheckboxes();

    const wchar_t* hint =
        (ps.app && ps.app->captureDegraded)
            ? L"有程序正在独占全屏，效果可能未生效。"
            : L"参数改动立即生效，无需重启。";
    ::SetWindowTextW(ps.statHint, hint);
}

// ==========================================
// 窗口过程
// ==========================================

LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PanelState& ps = P();

    switch (msg) {
        case WM_CREATE: {
            ps.hwnd = hwnd;
            BuildPanel(hwnd);
            ApplyExplorerTheme(hwnd);
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            switch (id) {
                case kIdScreenCombo: {
                    if (code == CBN_SELCHANGE) SyncScreenControls();
                    return 0;
                }
                case kIdBtnResetScreen: {
                    ResetCurrentScreen();
                    return 0;
                }
                case kIdBtnResetAll: {
                    ResetAllScreens();
                    return 0;
                }
                case kIdChkCapture: {
                    if (code != BN_CLICKED || !ps.app) return 0;
                    const bool on =
                        (::SendMessageW(ps.chkCapture, BM_GETCHECK, 0, 0) == BST_CHECKED);

                    if (!on) {
                        // 关闭它会让 overlay 重新进入 Desktop Duplication 的捕获流，
                        // 形成「抓到自己 -> 画面逐帧叠加溶解 -> GPU 打满卡死」的递归。
                        // 这是全屏 overlay 的固有限制，必须让用户知情后再决定。
                        const int r = ::MessageBoxW(
                            ps.hwnd,
                            L"关闭「对截图和录屏隐藏」后，滤镜窗口会重新被系统捕获，\n"
                            L"造成画面逐帧叠加、最终 GPU 打满卡死。\n\n"
                            L"（这是全屏滤镜的固有限制，无法绕过。）\n\n"
                            L"仍要关闭吗？",
                            L"可能导致画面卡死", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                        if (r != IDYES) {
                            ::SendMessageW(ps.chkCapture, BM_SETCHECK, BST_CHECKED, 0);
                            return 0;
                        }
                    }

                    ps.app->config.hideFromCapture = on;
                    NotifyParamsChanged();
                    return 0;
                }
                case kIdChkGamePause: {
                    if (code != BN_CLICKED || !ps.app) return 0;
                    ps.app->config.pauseOnFullscreenGame =
                        (::SendMessageW(ps.chkGamePause, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    NotifyParamsChanged();
                    return 0;
                }
                case kIdChkAutoStart: {
                    if (code != BN_CLICKED || !ps.app) return 0;
                    const bool on =
                        (::SendMessageW(ps.chkAutoStart, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    ps.app->config.autoStart = on;
                    detail::SetAutoStartReg(on);
                    NotifyParamsChanged();
                    UpdateTrayMenu();          // 与托盘菜单里的同名项联动
                    return 0;
                }
                case kIdBtnClose: {
                    ::DestroyWindow(hwnd);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }

        case WM_HSCROLL: {
            if (!ps.app) break;
            HWND tb = reinterpret_cast<HWND>(lp);
            if (!tb) break;
            const int id = ::GetDlgCtrlID(tb);
            int slot = -1;
            bool common = false;
            if (id >= kIdCommonTrackBase && id < kIdCommonTrackBase + kSlotCount) {
                slot = id - kIdCommonTrackBase;
                common = true;
            } else if (id >= kIdScreenTrackBase && id < kIdScreenTrackBase + kSlotCount) {
                slot = id - kIdScreenTrackBase;
            }
            if (slot < 0) break;

            const float v = GetTrackValue(tb, kSlots[slot]);
            if (common) {
                ApplyCommonChange(slot, v);
            } else {
                ApplyScreenChange(slot, v);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            ::DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            ps.hwnd = nullptr;
            return 0;
        }

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterPanelClass() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
    wc.lpszClassName = kPanelClassName;

    if (::RegisterClassExW(&wc) == 0) {
        if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    registered = true;
    return true;
}

} // namespace

// ==========================================
// 对外接口
// ==========================================

void ShowSettingsPanel(HWND ownerWnd, AppState* state, const PanelCallbacks& cb) {
    PanelState& ps = P();
    ps.app = state;
    ps.cb = cb;

    const size_t count = state ? state->screens.size() : 0;

    // 面板已经开着：同步状态并置前，不重建窗口
    if (ps.hwnd && ::IsWindow(ps.hwnd)) {
        if (count != ps.screenCount) {
            FillScreenCombo();
            ps.screenCount = count;
        }
        SnapshotOverrides();
        SyncCommonControls();
        SyncScreenControls();
        SyncCheckboxes();
        ::SetForegroundWindow(ps.hwnd);
        return;
    }

    EnsureCommonControls();
    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    if (!RegisterPanelClass()) return;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

    RECT rect = { 0, 0, kClientWidth, kClientHeight };
    ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int winW = rect.right - rect.left;
    const int winH = rect.bottom - rect.top;

    // 摆在主屏工作区中间
    RECT work = {};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - winW) / 2;
    int y = work.top + ((work.bottom - work.top) - winH) / 2;

    HWND hwnd = ::CreateWindowExW(exStyle, kPanelClassName, L"近视离焦设置",
                                  style, x, y, winW, winH,
                                  ownerWnd, nullptr, inst, nullptr);
    if (!hwnd) return;

    ApplyModernWindowChrome(hwnd);

    ps.screenCount = count;
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);
    ::SetForegroundWindow(hwnd);
}

namespace detail {

void RefreshPanelIfOpen() {
    PanelState& ps = P();
    if (!ps.hwnd || !::IsWindow(ps.hwnd)) return;
    SyncCheckboxes();
    SyncScreenControls();
}

} // namespace detail

} // namespace ui
} // namespace md
