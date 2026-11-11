#pragma once
// ==========================================
// ui.h — UI 层公共头（托盘 / 设置面板 / 首次向导）
// ==========================================
// 模块边界：
//   · 只依赖 ../types.h 的「数据契约」，不链接 gpu/core 的实现符号
//     （core::Config::FindOverride / md::Utf8ToWide 等一律不调用，
//       需要的逻辑在 detail 命名空间里就地实现，保证本层可独立编译）
//   · 所有界面文案使用宽字符字面量 L"..."，源码 UTF-8，编译需 /utf-8
//   · 不自绘深色主题：全部使用系统原生控件 + 系统颜色，跟随系统主题
//
// types.h 里 md::ui::AppState / md::ui::PanelCallbacks 的声明已经定稿，
// 本头不再重复声明，只补齐 types.h 未提供的 UI 层内部工具。

// 说明：
//   types.h 里 md::core::Config / ScreenInfo / DefocusParams / ScreenOverride
//   是「结构体在 core 命名空间下」的唯一事实来源，本层一律用 md::core::X 限定名引用，
//   不写 md::X，也不用 using 把它们拉进 md::ui，避免与别的模块撞名。
//
//   types.h 自己已经包含了 <functional> / <utility>；这里仍然先包含 <functional>，
//   是为了让本头在 types.h 之后被单独改动时也不至于缺符号（无害的冗余）。
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../types.h"

#include <winreg.h>

namespace md {
namespace ui {

// ==========================================
// 托盘
// ==========================================

// 托盘自定义消息。宿主窗口的 WndProc 里这样接：
//
//   case md::ui::kTrayCallbackMessage:
//       md::ui::HandleTrayMessage(hWnd, lParam);
//       return 0;
//
// 托盘图标的右键/双击事件全部由 HandleTrayMessage 消化，
// 宿主只要把消息转发过来即可。
inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;

// 托盘图标 ID（同一时刻只存在一个托盘图标）
inline constexpr UINT kTrayIconId = 1;

// 处理托盘回调消息（WM_APP+1）。
//   · 右键 → 弹出托盘菜单（暂停/恢复、打开设置面板、开机自启、退出程序）
//   · 双击 → 打开设置面板
void HandleTrayMessage(HWND ownerWnd, LPARAM lParam);

// ==========================================
// 首次运行向导
// ==========================================

// 仅供自检（test_wizard_template.cpp）使用：
// 导出运行时拼装的 DLGTEMPLATE 字节，供字节布局断言（防向导静默失败回归）。
std::vector<BYTE> BuildWizardTemplateForTest();

namespace detail {

// ==========================================
// 面板内部接口
// ==========================================

// 设置面板若已打开，把外部改动（托盘菜单改了开机自启等）同步进面板控件。
// 面板没开时什么都不做。托盘侧在改完共享状态后调用它。
void RefreshPanelIfOpen();

// ==========================================
// 本地小工具（inline 实现，不产生任何额外链接符号）
// ==========================================

// UTF-8 → UTF-16
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out;
    out.resize(static_cast<size_t>(need));
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &out[0], need);
    return out;
}

// 找某块屏的覆盖项；没有覆盖项返回 nullptr
inline const md::core::ScreenOverride* FindOverride(const md::core::Config& cfg,
                                                    const std::string& id) {
    for (const auto& kv : cfg.perScreen) {
        if (kv.first == id) return &kv.second;
    }
    return nullptr;
}

// 通用参数 + 该屏覆盖 = 该屏最终生效的参数
inline md::core::DefocusParams ResolveParams(const md::core::Config& cfg,
                                              const std::string& id) {
    md::core::DefocusParams p = cfg.common;
    if (const md::core::ScreenOverride* ov = FindOverride(cfg, id)) {
        ov->ApplyTo(p);
    }
    return p;
}

// 取得某块屏可写的覆盖项；不存在则新建一个空覆盖项
inline md::core::ScreenOverride* MutableOverride(md::core::Config& cfg,
                                                  const std::string& id) {
    for (auto& kv : cfg.perScreen) {
        if (kv.first == id) return &kv.second;
    }
    cfg.perScreen.emplace_back(id, md::core::ScreenOverride{});
    return &cfg.perScreen.back().second;
}

// 清掉某块屏的覆盖项（该屏回到通用参数）
inline void ClearOverride(md::core::Config& cfg, const std::string& id) {
    for (auto it = cfg.perScreen.begin(); it != cfg.perScreen.end(); ++it) {
        if (it->first == id) {
            cfg.perScreen.erase(it);
            return;
        }
    }
}

// ==========================================
// 开机自启：HKCU\Software\Microsoft\Windows\CurrentVersion\Run
// ==========================================
// 注册表值名固定为 "MyopicDefocus"，数据为带引号的 exe 完整路径。
// 说明：types.h 里另有 md::SetAutoStart / md::IsAutoStartEnabled 声明，
// 本层没有调用它们（避免链接尚未实现的符号），这里给的是 UI 自用版本。

inline bool SetAutoStartReg(bool enable) {
    // 统一走 app 层实现（app/util.cpp），避免两套注册表逻辑状态打架
    return ::md::SetAutoStart(enable);
}

inline bool IsAutoStartReg() {
    return ::md::IsAutoStartEnabled();
}

} // namespace detail
} // namespace ui
} // namespace md
