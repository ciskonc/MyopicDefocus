// ==========================================
// controlbar.h — 悬浮控制条（不依赖 explorer 的常驻入口）
// ==========================================
// 用途：某些环境下 Shell_NotifyIcon 会被系统拒绝（GetLastError = 5），
//       托盘图标根本建不出来，用户就没有任何可见的入口来暂停或退出。
//       悬浮控制条是纯 Win32 窗口，不经过 shell，因此永远可用。
#pragma once

#include <windows.h>

#include "../types.h"

namespace md {
namespace ui {

// 创建并显示控制条。ownerWnd 为消息宿主窗口。
// 返回 false 表示创建失败（极少见）。
bool ShowControlBar(HWND ownerWnd, AppState* state, const PanelCallbacks& cb);

// 更新控制条上的状态文字（暂停/恢复）
void UpdateControlBarState(bool effectOn);

// 隐藏控制条
void HideControlBar();

} // namespace ui
} // namespace md
