// ==========================================
// wizard.cpp — 首次启动向导
// ==========================================
// 形态：模态对话框（DialogBoxIndirectW）。
//   · 对话框模板在运行时拼出来，不依赖 .rc 资源文件
//   · 2 个输入项：观看距离(cm)、瞳孔直径(μm)；屏幕对角线由 EDID 自动探测
//   · 「确定」→ 写进 state->config.common 并返回 true
//   · 「取消」→ 返回 false
//
// 模态对话框自带消息循环，不会和宿主的消息循环互锁；
// 在宿主 WndProc 里调用也是安全的（系统对话框会禁用宿主窗口）。

#include "ui.h"

#include "../core/diag_probe.h"

#include <cmath>
#include <cwchar>
#include <vector>

namespace md {
namespace ui {
namespace {

// 输入项 ID
enum WizardCtrlId {
    kIdDist  = 2001,   // 观看距离（厘米）
    kIdPupil = 2002    // 瞳孔直径（微米）
};

// 一个输入项：可接受的范围 + 默认值
struct WizardField {
    int   id;
    float minValue;
    float maxValue;
    float defValue;
};

const WizardField kFields[] = {
    { kIdDist,   10.0f,  150.0f,   40.0f },   // 观看距离（厘米）
    { kIdPupil, 2000.0f, 9000.0f, 6500.0f },  // 瞳孔直径（微米）
};

// ==========================================
// 运行时拼对话框模板
// ==========================================
// ⚠ 字节布局合约（历经四轮静默失败换来的教训，勿改回）：
//   · DLGTEMPLATE 头部 18 字节（style4/exstyle4/cdit2/x2/y2/cx2/cy2）WORD 连续
//   · **menu / windowClass / title / 字体块是一个连续的 WORD 流，段与段之间无任何填充**——
//     Windows 对话框解析器按流顺序读取（同 wine user32 与 RC 编译器行为）。
//     曾错误地「每段 DWORD 对齐」装配，插入的填充被 OS 当字段吃掉：
//     title 变空串、字号从 0xFFFF 原子标记开始读、typeface 乱码 → 条目从垃圾偏移解析 → 必败
//   · **只有条目数组（每个 DLGITEMTEMPLATE）的起点需要 DWORD 对齐**
//   · 条目头 18 字节（style4/ex4/x2/y2/cx2/cy2/id2）连续，id 是 WORD；
//     **条目内 id 之后紧跟 class/title/extraCount，段间同样无填充**
// 所以：Raw 系列只写数据；唯一的对齐点 = 每个条目的起点 Align()。

class TemplateWriter {
public:
    std::vector<BYTE> data;

    void Align() {
        while (data.size() % 4) data.push_back(0);
    }

    void RawDword(DWORD v) {
        data.push_back(static_cast<BYTE>(v & 0xFF));
        data.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
        data.push_back(static_cast<BYTE>((v >> 16) & 0xFF));
        data.push_back(static_cast<BYTE>((v >> 24) & 0xFF));
    }

    void RawWord(WORD v) {
        data.push_back(static_cast<BYTE>(v & 0xFF));
        data.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
    }

    void Raw(const void* p, size_t n) {
        const BYTE* b = static_cast<const BYTE*>(p);
        data.insert(data.end(), b, b + n);
    }

    // 一个以 \0 结尾的宽字符串（sz_Or_Ord 数据本身；连续写入，不引入任何对齐）
    void Text(const wchar_t* s) {
        const size_t n = ::wcslen(s);
        for (size_t i = 0; i <= n; ++i) {
            const WORD c = static_cast<WORD>(s[i]);
            data.push_back(static_cast<BYTE>(c & 0xFF));
            data.push_back(static_cast<BYTE>((c >> 8) & 0xFF));
        }
    }
};

// 追加一个对话框控件
void AddItem(TemplateWriter& w, DWORD style, short x, short y, short cx, short cy,
             WORD id, const wchar_t* cls, const wchar_t* text) {
    w.Align();                        // ⚠ 唯一的对齐点：每个条目从 DWORD 边界开始
    w.RawDword(style);
    w.RawDword(0);                    // dwExtendedStyle
    w.RawWord(static_cast<WORD>(x));
    w.RawWord(static_cast<WORD>(y));
    w.RawWord(static_cast<WORD>(cx));
    w.RawWord(static_cast<WORD>(cy));
    w.RawWord(id);                    // ⚠ id 是 WORD（DLGITEMTEMPLATE 定义）
    w.Text(cls);                      // ⚠ 紧跟 id，无填充
    w.Text(text);                     // 紧跟 class
    w.RawWord(0);                     // extraCount 紧跟 title（无额外数据）
}

// 常用控件样式
constexpr DWORD kStyleStatic = WS_CHILD | WS_VISIBLE | SS_LEFT;
constexpr DWORD kStyleEdit   = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                               ES_AUTOHSCROLL | ES_NUMBER;
constexpr DWORD kStyleButton = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
constexpr DWORD kStyleDefBtn = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;

std::vector<BYTE> BuildWizardTemplate() {
    TemplateWriter w;

    const DWORD style = DS_SETFONT | DS_MODALFRAME | DS_CENTER |
                        WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const WORD cdit = 7;             // 控件个数：说明 + 2 组标签/输入 + 2 个按钮（增删控件必须同步此数）

    // —— 模板头 18 字节：WORD 连续 ——
    w.RawDword(style);
    w.RawDword(0);                   // dwExtendedStyle
    w.RawWord(cdit);
    w.RawWord(0);                    // x
    w.RawWord(0);                    // y
    w.RawWord(310);                  // cx（DLU）
    w.RawWord(132);                  // cy（DLU）
    // —— 变长段：连续 WORD 流，段间无任何填充（OS 解析合约）——
    w.RawWord(0);                    // menu @18：0x0000 = 无菜单
    w.RawWord(0x0000);               // windowClass @20：0x0000 = 系统标准对话框类（#32770）
    // ⚠ 历史教训（第五层根因）：曾写 0xFFFF+0x8002——把预定义句柄常量 #32770
    //   （十进制 32770 = 0x8002）误当全局原子编号；原子表里不存在 0x8002，
    //   FindWindowClass 恒失败 → ERROR_CANNOT_FIND_WND_CLASS(1407) → 向导静默不弹。
    //   0xFFFF+n 编码的语义是「原子编号 n」，需 GlobalFindAtom 真实存在；
    //   系统对话框类只能用 0x0000（预定义类）或字符串 L"#32770"。
    //   真机 PowerShell 内存模板对照实验（E/F/G/H）已实证。
    w.Text(L"首次设置");              // title @22
    w.RawWord(9);                    // fontSize @32
    w.RawWord(400);                  // fontWeight @34
    w.Raw("\x00\x01", 2);            // italic=0, charset=1(DEFAULT_CHARSET) @36
    w.Text(L"MS Shell Dlg");         // typeface @38（连续）
    // 条目数组起点 DWORD 对齐由首个 AddItem 的 Align() 完成（64，64%4=0 无填充）

    // 说明文字
    AddItem(w, kStyleStatic | WS_GROUP,
            12, 10, 286, 28, 0, L"STATIC",
            L"这些参数决定离焦模糊的强度，可在设置面板里随时修改");

    // 观看距离（厘米）
    AddItem(w, kStyleStatic, 12, 48, 120, 10, 0, L"STATIC", L"观看距离（厘米）");
    AddItem(w, kStyleEdit, 150, 46, 70, 12, kIdDist, L"EDIT", L"40");

    // 瞳孔直径（微米）
    AddItem(w, kStyleStatic, 12, 68, 120, 10, 0, L"STATIC", L"瞳孔直径（微米）");
    AddItem(w, kStyleEdit, 150, 66, 70, 12, kIdPupil, L"EDIT", L"6500");

    // 确定 / 取消
    AddItem(w, kStyleDefBtn, 150, 94, 60, 16, IDOK, L"BUTTON", L"确定");
    AddItem(w, kStyleButton, 220, 94, 60, 16, IDCANCEL, L"BUTTON", L"取消");

    return w.data;
}

// ==========================================
// 取值与校验
// ==========================================

// 读一个输入框；不是合法数字返回 false
bool ReadField(HWND hwnd, int id, float& out) {
    wchar_t buf[64] = {};
    ::GetDlgItemTextW(hwnd, id, buf, 64);

    wchar_t* end = nullptr;
    const double v = std::wcstod(buf, &end);
    if (end == buf) return false;
    while (end && *end == L' ') ++end;
    if (end && *end != L'\0') return false;

    out = static_cast<float>(v);
    return true;
}

void ShowFieldError(HWND hwnd, const wchar_t* text) {
    ::MessageBoxW(hwnd, text, L"首次设置", MB_OK | MB_ICONWARNING);
}

// 校验并写进配置
bool CommitValues(HWND hwnd, AppState* state) {
    if (!state) return false;

    // 数组大小跟随 kFields 实际项数（向导现为 2 项；旧版 3 项循环越界已修）
    float vals[std::size(kFields)] = {};
    for (int i = 0; i < static_cast<int>(std::size(kFields)); ++i) {
        if (!ReadField(hwnd, kFields[i].id, vals[i])) {
            ShowFieldError(hwnd, L"请填写数字。");
            return false;
        }
        if (vals[i] < kFields[i].minValue || vals[i] > kFields[i].maxValue) {
            ShowFieldError(hwnd, L"数值超出允许范围，请重新填写。");
            return false;
        }
    }

    state->config.common.screenDistanceCM = vals[0];
    state->config.common.pupilSizeUm      = vals[1];
    // 屏幕对角线不再询问：由程序通过 EDID 自动探测（探测失败用默认 24 英寸）
    state->config.common.diagInch = md::core::ProbeDiagonalInchOrDefault(state->config.common.diagInch);
    return true;
}

// ==========================================
// 对话框过程
// ==========================================

INT_PTR CALLBACK WizardDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            // 两个输入项的默认值（跟随 kFields 数量）
            for (const WizardField& f : kFields) {
                wchar_t buf[32] = {};
                swprintf_s(buf, L"%g", static_cast<double>(f.defValue));
                ::SetDlgItemTextW(hwnd, f.id, buf);
            }
            ::SetFocus(::GetDlgItem(hwnd, kIdDist));
            return TRUE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDOK) {
                AppState* state = reinterpret_cast<AppState*>(lp);
                if (CommitValues(hwnd, state)) {
                    ::EndDialog(hwnd, IDOK);
                }
                return TRUE;
            }
            if (id == IDCANCEL) {
                ::EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
        }

        default:
            break;
    }
    return FALSE;
}

} // namespace

// ==========================================
// 对外接口
// ==========================================

bool RunFirstRunWizard(HWND ownerWnd, AppState* state) {
    std::vector<BYTE> tmpl = BuildWizardTemplate();
    if (tmpl.empty()) return false;

    DLGTEMPLATE* dt = reinterpret_cast<DLGTEMPLATE*>(tmpl.data());
    const INT_PTR r = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), dt, ownerWnd, WizardDlgProc,
        reinterpret_cast<LPARAM>(state));

    return r == IDOK;
}

// 自检专用：导出模板字节（见 ui.h 说明）
std::vector<BYTE> BuildWizardTemplateForTest() {
    return BuildWizardTemplate();
}

} // namespace ui
} // namespace md
