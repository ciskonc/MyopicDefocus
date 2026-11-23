# MyopicDefocus / 近视离焦护眼滤镜

**English** | [中文](#中文说明)

A lightweight Windows desktop overlay that applies a **longitudinal chromatic aberration (LCA) based myopic-defocus blur** to your screen — the green and blue channels are gently gaussian-blurred while the red channel passes through untouched, mimicking the retinal defocus state used in myopia-defocus eye-care practice.

Single self-contained executable. No installation, no admin rights, no telemetry, no network access.

---

## Features / 特性

- **Full-screen overlay** over every application (DXGI Desktop Duplication → D3D11 separable gaussian blur → layered topmost window)
- **Per-monitor parameters** — one set of common parameters plus per-screen overrides
- **Optical model driven**: blur radius is computed from viewing distance, pupil size, and physical screen size (EDID auto-detection; a failed probe keeps the previous value), matching the upstream formula 1:1
- **System tray icon** with right-click menu, auto re-add after explorer restart
- **Global hotkeys**: `Ctrl+Alt+S` settings panel / `Ctrl+Alt+P` pause-resume (auto falls back to `Ctrl+Alt+O` when taken) / `Ctrl+Alt+Q` quit — they keep working while dialogs or menus are open
- **Fullscreen-exclusive game auto-pause**: overlay hides when capture fails (exclusive fullscreen), resumes automatically
- **Capture-hiding safety gate**: prevents the overlay from capturing itself (frame-stacking lockup), with a sharpness self-heal watchdog. Support-probed at startup (`WDA_EXCLUDEFROMCAPTURE` needs Win10 2004+; on older systems the gate is honestly disabled, the watchdog tightens, and you get a one-time warning)
- **Overlay follows display changes**: resolution switches and screen rotation are picked up within seconds
- **Multi-GPU laptops**: per-adapter device fallback when a secondary GPU's screen cannot be duplicated with the shared device
- **Hardened config parsing**: reasonable-range clamping for common *and* per-monitor values (NaN/zero/huge values can't reach the GPU), 1 MB size cap + nesting-depth pre-check (no crash on hostile JSON), UTF-16/ANSI mis-saved configs preserved as `.encoding-bak` instead of being overwritten
- **10-bit aware**: swapchain format follows the capture format (8-bit SDR verified; HDR is out of scope by design)
- **First-run wizard** for distance / pupil setup — shows over your normal desktop (the filter only appears after it closes) and is always topmost; everything adjustable live in the settings panel

## Download / 下载

Grab the prebuilt binary from [Releases](../../releases) — a single `MyopicDefocus.exe`, statically linked, no runtime DLLs needed.

- **Requirements**: Windows 10 1809 or later (x64), a GPU with D3D11
- Place the exe in a normal user directory (Desktop, `%LOCALAPPDATA%\MyopicDefocus`, …) — directories with a *Low integrity* mandatory label will break the tray icon (UIPI).

## Build from source / 从源码构建

Requires [LLVM MinGW](https://github.com/mstorsjo/llvm-mingw/releases) (Clang targeting `x86_64-w64-windows-gnu`) and any bash (Git Bash works):

```bash
TC=/path/to/llvm-mingw-ucrt-x86_64 bash scripts/build.sh        # build tests + app
TC=/path/to/llvm-mingw-ucrt-x86_64 bash scripts/build.sh test   # build + run self-tests
```

Output: `build/MyopicDefocus.exe` (statically linked) and `build/MyopicDefocusTests.exe`.
See [docs/build.md](docs/build.md) for details, and an MSVC/Visual Studio route.

## Usage / 使用

1. Run `MyopicDefocus.exe` — on first launch a wizard asks for **viewing distance (cm)** and **pupil diameter (µm)**; screen diagonal is auto-detected from EDID.
2. The overlay starts immediately. Fine-tune strength / distance / pupil in the settings panel (tray icon → 打开设置面板, or `Ctrl+Alt+S`).
3. Quit from the tray menu or `Ctrl+Alt+Q`.

Configuration lives at `%APPDATA%\MyopicDefocus\config.json` — see [`config.example.json`](config.example.json).

> **Tuning tip**: strength is a blend factor (result = `(1-strength)·original + strength·blurred`). On high-DPI screens viewed from farther away, perception fades quickly — start around 0.6–0.7 and raise until comfortable. Set the distance to your **actual measured** viewing distance, not a round number.

## Known limitations / 已知限制

| # | Issue |
|---|-------|
| 1 | Global hotkeys may be swallowed by remote-control/anti-cheat software with low-level keyboard hooks (Sunflower/Oray, RustDesk, some anti-cheats). Tray menu is the primary entry. `Ctrl+Alt+P` falls back to `Ctrl+Alt+O` when registered by another app. |
| 2 | `--capture` BMP evidence dump supports 8-bit SDR captures only (no HDR). |
| 3 | On Win11 the tray icon starts hidden in the overflow flyout — enable it in Taskbar settings. |
| 4 | **HDR is not supported** (by design): the pipeline targets SDR, and the sharpness self-heal watchdog has no data under HDR capture — if the picture ever looks stacked/corrupted under HDR, quit with `Ctrl+Alt+Q` instead of waiting for auto-recovery. |
| 5 | Fullscreen-exclusive auto-pause relies on capture failures; windowed-borderless games do not trigger it (intended). |
| 6 | Plugging/unplugging monitors while running is not re-enumerated live — restart the app after changing the physical monitor set. |

## Documentation / 文档

- [docs/user-guide.md](docs/user-guide.md) — full user guide (Chinese)
- [docs/optics.md](docs/optics.md) — the optical model explained (Chinese)
- [docs/build.md](docs/build.md) — build instructions, both LLVM-MinGW and MSVC routes (Chinese)
- [docs/troubleshooting.md](docs/troubleshooting.md) — troubleshooting (Chinese)

## License / 许可证

This project is released under the **MIT License** — see [LICENSE](LICENSE), and keep [LICENSE.upstream](LICENSE.upstream) alongside it.

- Code base: [Lerrenp/Myopic_defocus-cpp-build](https://github.com/Lerrenp/Myopic_defocus-cpp-build) (MIT) — original notice preserved in `LICENSE.upstream`
- Algorithm: the public LCA/myopic-defocus formulas from the refractify **myopic_defocus** browser extension (GPL-3.0) — only the formulas/ideas are used, reimplemented from scratch; no GPL code is included

> ⚠️ This is a comfort/utility tool, **not a medical device**. It does not treat, diagnose, or prevent any condition. Use at your own discretion.

---

<a name="中文说明"></a>
# 中文说明

一个轻量的 Windows 桌面悬浮滤镜：基于**纵向色差（LCA）的近视离焦光学模型**，对屏幕内容的绿/蓝通道做高斯模糊（红通道直通），模拟近视离焦护眼原理下的视网膜离焦状态。

单文件、免安装、免管理员、不联网、无遥测。

### 功能

- 全屏 overlay 覆盖所有应用（DXGI 桌面复制 → D3D11 可分离高斯模糊 → 置顶分层窗口）
- 每块显示器可单独调参（一套通用参数 + 单屏覆盖）
- 光学模型驱动：模糊半径由观看距离、瞳孔直径、屏幕物理尺寸（EDID 自动探测，失败保留历史值）计算，与上游公式 1:1
- 托盘图标 + 右键菜单，explorer 重启后自动重建图标
- 全局热键：`Ctrl+Alt+S` 设置面板 / `Ctrl+Alt+P` 暂停恢复（被占用自动回退 `Ctrl+Alt+O`）/ `Ctrl+Alt+Q` 退出——对话框/菜单打开时热键依然有效
- 独占全屏游戏自动暂停、抓屏恢复自动显示
- 截图隐藏安全闸门（防 overlay 自捕获死循环，清晰度自愈看门狗；启动时探测系统支持，Win10 2004 之前如实降级并提示）
- 分辨率切换/屏幕旋转自动跟随；多显卡笔记本按适配器回退（副屏不再静默失效）
- 配置解析加固：通用与单屏参数合理域钳制、超大/深嵌套 JSON 防崩溃、编码错误的配置自动备份不覆盖
- 10bit 自适应输出（交换链格式跟随抓屏格式；HDR 不在支持范围）
- 首次运行向导（距离/瞳孔）——在正常桌面上置顶显示，滤镜在向导关闭后才出现；面板实时调参

### 使用要点

1. 配置文件在 `%APPDATA%\MyopicDefocus\config.json`
2. exe 放在普通用户目录（桌面、`%LOCALAPPDATA%` 等），**不要放带低完整性标签的目录**（托盘会被 UIPI 拦截）
3. 调参建议：强度是混合系数（结果 = (1-强度)×原图 + 强度×模糊图），高分辨率大屏坐得远时感知衰减明显——从 0.6~0.7 起试；观看距离填**实测值**（卷尺量眼睛到屏幕），不要填凑整数
4. 详细文档见 `docs/` 目录（均为中文）

### 已知限制

- 远控/反作弊类软件的低级键盘钩子可能拦截全局热键（向日葵、RustDesk 等），托盘菜单为主入口；`Ctrl+Alt+P` 被占用自动回退 `Ctrl+Alt+O`
- `--capture` 取证仅支持 8bit SDR 抓屏
- Win11 新装托盘图标默认藏在溢出区，需手动设为常显
- **不支持 HDR**（设计边界）：HDR 抓屏下清晰度看门狗无数据——若画面出现异常叠加请直接 `Ctrl+Alt+Q` 退出，不要等自动恢复；独占全屏游戏的自动暂停不覆盖无边框窗口化游戏（设计行为）
- 运行中插拔显示器不会实时重新枚举——变更物理显示器后请重启程序

### 许可证与致谢

本项目以 **MIT** 发布。代码基座来自 [Lerrenp/Myopic_defocus-cpp-build](https://github.com/Lerrenp/Myopic_defocus-cpp-build)（MIT，原声明保留于 `LICENSE.upstream`）；离焦公式来自 refractify **myopic_defocus** 浏览器扩展（GPL-3.0）公开的物理公式——仅使用公式与算法思想，实现为全新代码，未包含任何 GPL 代码。

> ⚠️ 本软件是舒适度/工具类程序，**不是医疗器械**，不具备任何治疗、诊断或预防功能，请自行斟酌使用。
