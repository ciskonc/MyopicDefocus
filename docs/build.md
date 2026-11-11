# 构建说明 · 近视离焦护眼工具

> 面向要自己编译这份源码的开发者。
> 目标：在这台机器上把项目编译出来，并让测试全绿。
> 本文记录于 2026-11-09。

---

## 一、这个项目产出什么

| 产物 | 子系统 | 职责 |
|------|--------|------|
| `MyopicDefocus.exe` | Windows（无控制台，`-mwindows -municode`） | **单个自包含 exe**：托盘常驻 + 全屏 overlay + 设置面板 + 首次向导，多模式合一 |
| `MyopicDefocusTests.exe` | Console | 单元测试（光学层 + 配置 IO + 多屏参数解析 + 显示器枚举） |

> 早期形态是三个 exe（`MyopicDefocus` / `MyopicDefocusConfig` / `MyopicDefocusTests`）。改造后**主程序与配置器合并成一个 exe**，测试 exe 单独保留（它是开发期工具，不进分发包）。

**技术栈**：C++20 / D3D11 + DXGI 1.2 / Win32 / vendored nlohmann_json。

---

## 二、两条构建路径

项目支持两条独立的构建路径，**任选其一**。

| | 路径 A：LLVM MinGW + 构建脚本 | 路径 B：MSBuild + Visual Studio |
|---|---|---|
| **本机状态** | ✅ **已验证可用**（2026-11-09 实跑） | ⚠️ 工具链未装（缺 C++ 工作负载） |
| 工具链 | LLVM MinGW UCRT x86_64（clang 23.1.5），**静态链接** | VS 2026 Build Tools + 「使用 C++ 的桌面开发」+ Windows 11 SDK |
| 需要管理员 | **不需要** | **不需要**（装工作负载时需要） |
| 额外下载量 | **0**（工具链已在本地 `03_TOOLS/toolchains/`） | **4~6 GB** |
| 工程文件 | 无（脚本直接调 clang++） | `MyopicDefocus.slnx` + 三个 `.vcxproj` |
| 适合 | 日常开发、快速验证、单 exe 产出 | 需要 IDE 调试 / 需要 MSVC 工具链对齐 |

> **推荐先走路径 A**：它零安装成本、零提权、产出就是单文件 exe。路径 B 适合需要 Visual Studio IDE 调试时再用。

---

## 三、路径 A：LLVM MinGW + 构建脚本（推荐）

### 3.1 工具链

| 项 | 值 |
|----|-----|
| 工具链 | `llvm-mingw-20261104-ucrt-x86_64`（LLVM Clang 23.1.5，target `x86_64-w64-windows-gnu`，UCRT） |
| 默认路径 | `C:\Users\Gamer\Desktop\agent\03_TOOLS\toolchains\llvm-mingw-20261104-ucrt-x86_64` |
| 编译器 | `bin\x86_64-w64-mingw32-clang++.exe` |
| 资源编译器 | `bin\x86_64-w64-mingw32-windres.exe` |
| 需要 bash | 是（脚本是 bash 脚本；本机 Cygwin bash 5.3 可用） |

工具链不在默认位置时，用环境变量 `TC` 覆盖：

```bash
TC=/path/to/llvm-mingw-ucrt-x86_64 bash scripts/build.sh
```

### 3.2 编译

在**项目根目录**（不是 `src/`）执行：

```bash
# 构建全部（先测试 exe，再主程序 exe）
bash scripts/build.sh

# 只构建测试 exe
bash scripts/build.sh tests

# 只构建主程序 exe
bash scripts/build.sh app
```

产物：

```
build/MyopicDefocusTests.exe
build/MyopicDefocus.exe
```

### 3.3 脚本干了什么

| 环节 | 内容 |
|------|------|
| 编译选项 | `-std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter` |
| 字符集 | `-finput-charset=UTF-8 -fexec-charset=UTF-8` + `-DUNICODE -D_UNICODE` |
| 最低版本宏 | `-D_WIN32_WINNT=0x0A00`（= Windows 10 1809）/ `-DWINVER=0x0A00` / `-DNTDDI_VERSION=0x0A000000` |
| 头文件路径 | `-I <toolchain>/include -I src/src -I src/src/third_party` |
| 链接选项 | `-static -static-libgcc -static-libstdc++` —— **产出不依赖任何运行时 DLL** |
| 系统库 | `d3d11 dxgi d3dcompiler dwmapi shell32 shlwapi ole32 comctl32 user32 gdi32` |
| 编译单元 | core（`core/config.cpp` `core/screens.cpp` `optical_model.cpp`）+ 对应 app/ui/gpu 单元 |

### 3.4 一个容易踩的坑：TEMP / TMP

脚本里有一段专门处理这个：

```bash
# clang.exe 是原生 Windows 程序，拿 MSYS 的 /tmp 会导致 "unable to make temporary file"。
export TMP="$(cygpath -w "$TMP_WORK")"
export TEMP="$TMP"
export TMPDIR="$TMP"
```

原因是 clang.exe 是原生 Windows 程序，MSYS/Cygwin 的 `/tmp` 映射对它不可用，会报 `unable to make temporary file`。脚本把临时目录指到项目内的 `build/tmp` 再转成 Windows 路径。

如果你手动调 clang++ 而报这个错，把 `TEMP`/`TMP` 指到一个纯 Windows 路径即可：

```bash
export TEMP="C:\\Users\\Gamer\\Desktop\\agent\\01_PROJECTS\\近视离焦\\build\\tmp"
export TMP="$TEMP"
```

---

## 四、路径 B：MSBuild + Visual Studio

### 4.1 环境要求

| 项目 | 要求 |
|------|------|
| 工具链 | **Visual Studio 2026 Build Tools**，必须带 **「使用 C++ 的桌面开发」工作负载** |
| 工具集 | `v145`（工程文件里写死的 `PlatformToolset`）—— **必须 VS 2026 系列**，VS 2022 装的是 v143，编不过 |
| SDK | **Windows 11 SDK** |
| MSBuild | 18.x（随 Build Tools 一起装） |
| 磁盘 | 装工作负载大约需要 4~6 GB |
| 最低运行系统 | Windows 10 1809 |

### 4.2 先检查环境

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" `
    -all -products * -format json
```

关注 `productId`（应为 `Microsoft.VisualStudio.Product.BuildTools`）、`installationPath`、`catalog.productDisplayVersion`。

再查已装了哪些工作负载/组件：

```powershell
$inst = Get-ChildItem "C:\ProgramData\Microsoft\VisualStudio\Packages\_Instances" -Directory | Select-Object -First 1
(Get-Content (Join-Path $inst.FullName "state.json") -Raw | ConvertFrom-Json).selectedPackages |
    Where-Object { $_.type -in 'Workload','Component' } |
    Select-Object -ExpandProperty id
```

**装了 C++ 工作负载**的话，输出里会有 `Microsoft.VisualStudio.Workload.VCTools` 和 `Microsoft.VisualStudio.Component.Windows11SDK.*`。

**只看到 `Microsoft.VisualStudio.Workload.MSBuildTools`** = MSBuild 装了但 **C++ 工作负载没装**。

### 4.3 安装「使用 C++ 的桌面开发」工作负载

**图形界面（推荐）：**

1. 打开 **Visual Studio Installer**；
2. 找到已装的 **Visual Studio 生成工具 2026**，点右侧「**修改**」；
3. 在「工作负载」标签页勾选「**使用 C++ 的桌面开发**」；
4. 确认右侧「安装详细信息」里包含 **MSVC v145 生成工具（x64/x86）** 和 **Windows 11 SDK**；
5. 点「**安装**」，等它跑完。

**命令行：**

```powershell
# 管理员 PowerShell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify `
    --installPath "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" `
    --add Microsoft.VisualStudio.Workload.VCTools `
    --includeRecommended `
    --quiet `
    --norestart
```

| 参数 | 作用 |
|------|------|
| `modify` | 修改**已安装实例**（`install` 是全新安装） |
| `--installPath "..."` | 目标实例路径，**必须和 vswhere 报的 `installationPath` 一致** |
| `--add Microsoft.VisualStudio.Workload.VCTools` | 「使用 C++ 的桌面开发」工作负载的官方 ID |
| `--includeRecommended` | 连推荐组件一起装 —— **Windows 11 SDK 靠它带进来**，别省 |
| `--quiet` | 静默模式 |
| `--norestart` | 装完不自动重启 |

#### ⚠️ 关于 `--quiet` 的 Exit Code 5007

**用 `--quiet` 必须以管理员权限运行。**

权限不足时安装器不会说「需要管理员」，而是直接返回一个看不懂的退出码：

```
Exit Code 5007
```

含义是「安装失败 / 操作未被允许」，本质是**权限被拒**。两种解法：

1. 右键 PowerShell →「**以管理员身份运行**」再执行上面的命令（推荐）；
2. 或**去掉 `--quiet`**，让安装器弹图形界面自己提权。

> 同理，`--installPath` 写错（比如指向一个不存在的 2022 路径）也会失败。路径请从 `vswhere` 输出里**原样复制**。

### 4.4 装完验证

```powershell
# MSBuild 能跑
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" -version

# C++ targets 出现了
Test-Path "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Microsoft\VC\v180\Microsoft.Cpp.Default.props"

# SDK 头文件在
Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Include" -Directory | Select-Object -ExpandProperty Name
```

第 2 步返回 `True`、第 3 步能列出 `10.0.2xxxx.0` 这样的版本号，环境就齐了。

### 4.5 编译

在 **`src/`** 目录下：

```powershell
cd 01_PROJECTS/近视离焦/src

# 方式 A：VS 的开发者命令提示符（msbuild 已在 PATH 里）
msbuild MyopicDefocus.slnx -p:Configuration=Release -p:Platform=x64

# 方式 B：任意 PowerShell，用 MSBuild 全路径
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
    MyopicDefocus.slnx -p:Configuration=Release -p:Platform=x64
```

> 「Developer Command Prompt」从开始菜单搜 **「Developer PowerShell for VS 2026」**。

| 目的 | 命令 |
|------|------|
| 编译 Debug 版（带调试信息、开启 D3D 调试层） | 加 `-p:Configuration=Debug` |
| 只编译测试工程 | `-t:MyopicDefocusTests` |
| 清理 | `-t:Clean` 后再 `-t:Build` |
| 详细日志 | 加 `-v:n` 或 `-v:d` |

**产物位置**：

| 内容 | 路径 |
|------|------|
| 可执行文件 | `src\bin\Release\` |
| 中间文件 | `src\obj\Release\<工程名>\` |

### 4.6 用 IDE 编译

双击 `src\MyopicDefocus.slnx`，选 **Release** + **x64**，生成解决方案。

> **`.slnx` 是什么**：Visual Studio 2026 起支持的新版解决方案格式（XML）。VS 2022 打不开 —— 需要 VS 2026 或更高。

---

## 五、目录结构

```
01_PROJECTS/近视离焦/
├─ AGENTS.md                     项目级约束（红线、编码规范、界面约束）
├─ README.md                     项目简介
├─ STATUS.md                     当前状态
├─ docs/                         文档
├─ scripts/
│  └─ build.sh                   LLVM MinGW 构建脚本（路径 A）
├─ build/                        路径 A 的产物输出目录（exe + 临时文件）
└─ src/                          源码根
   ├─ MyopicDefocus.slnx         解决方案（.slnx 格式，3 个工程）
   ├─ LICENSE.upstream            上游 MIT 版权声明（Lerrenp）
   ├─ MyopicDefocus/             工程文件 → 滤镜主程序
   ├─ MyopicDefocusConfig/       工程文件 → 配置器
   ├─ MyopicDefocusTests/        工程文件 → 测试
   └─ src/                       全部源码
      ├─ core/                   ① 核心层
      │  ├─ core.h               core 层内部工具（字符串 / 路径 / 文件 / COM 辅助）
      │  ├─ config.cpp           配置的读写、迁移、旧格式兼容、多屏参数解析
      │  └─ screens.cpp          显示器枚举（物理屏 / 虚拟屏 / 刷新率 / 是否主屏）
      ├─ gpu/                    ② GPU 层
      │  ├─ gpu.h                对外接口：单屏渲染上下文
      │  └─ gpu.cpp              DXGI 抓屏 + D3D11 双 pass 可分离高斯 + 呈现
      ├─ ui/                     ③ UI 层
      │  ├─ ui.h                 UI 层公共头与内部工具
      │  ├─ tray.cpp             托盘图标与右键菜单
      │  ├─ panel.cpp            设置面板（滑块 / 复选框 / 单屏覆盖）
      │  └─ wizard.cpp           首次运行向导
      ├─ app/                    ④ 应用装配层
      │  ├─ main.cpp             主程序入口、窗口生命周期、消息循环
      │  ├─ util.cpp/.h          公共小工具（UTF-8 转换等）
      │  ├─ selftest.cpp         自检用例（配置解析 / 默认值 / 显示器枚举）
      │  └─ test_main.cpp        测试入口
      ├─ third_party/
      │  └─ nlohmann/json.hpp    vendored 的 nlohmann/json 单头文件库
      ├─ optical_model.*         光学模型（纯函数，无 D3D 依赖）← 光学红线所在
      ├─ blur_shader.h           HLSL 着色器源码（内联字符串）
      ├─ config.h                旧版配置结构（上游遗留）
      ├─ config_io.*             旧版 JSON 序列化/反序列化（上游遗留）
      ├─ capture.* / renderer.*  旧版单屏抓屏与渲染（上游遗留）
      ├─ cli.*                   旧版命令行解析 + UTF-8 控制台输出
      ├─ MyopicDefocus.cpp       旧版主程序入口（上游遗留）
      ├─ MyopicDefocusConfig.cpp 旧版配置器入口（上游遗留）
      ├─ log.h                   错误日志（OutputDebugString + stderr）
      ├─ framework.h             公共 Windows 头聚合
      ├─ targetver.h             目标 Windows 版本宏
      ├─ types.h                 跨模块数据契约（md::core / md::gpu / md::ui 公共类型）
      ├─ test_optical_model.cpp  光学层单测
      └─ test_config_io.cpp      配置 IO 单测
```

### 分层与职责

| 层 | 职责（一句话） | 关键文件 |
|----|--------------|---------|
| **core**（核心/平台无关） | 配置读写、显示器枚举、路径与字符串工具，纯逻辑无图形依赖，可脱离 GPU 单测 | `core/config.cpp`、`core/screens.cpp`、`core/core.h` |
| **gpu**（图形管线） | 每块显示器各持一个渲染上下文：抓屏 → 双 pass 可分离高斯 → 呈现 | `gpu/gpu.cpp`、`gpu/gpu.h`、`blur_shader.h` |
| **ui**（用户界面） | 托盘图标与右键菜单、设置面板、首次运行向导 | `ui/tray.cpp`、`ui/panel.cpp`、`ui/wizard.cpp` |
| **app**（应用装配） | 主程序入口、窗口生命周期、消息循环、模式分发、自检 | `app/main.cpp`、`app/util.cpp`、`app/selftest.cpp` |

**依赖方向单向**：`app → ui / gpu → core`。

**跨模块通信只走 `types.h`** —— `ui` 层不链接 `gpu`/`core` 的实现符号，`core` 层不允许包含 `d3d11.h` 这类图形头。光学层因此能在没有 GPU 的环境下单独测试。

### 第三方代码

`src/src/third_party/nlohmann/json.hpp` 是 **vendored（直接放进仓库）** 的 nlohmann/json 单头文件库，用来解析/生成 `config.json`。

- vendored 的原因：不引入包管理、不联网拉依赖、保证任何人 clone 下来就能编；
- 它自带 MIT 许可证，文件头部有版权声明，**不要删**；
- 项目**不引入任何测试框架**（GoogleTest 等），单测用手写断言宏实现。

### 关于 `src/src/` 根目录下仍存在的旧版文件

`optical_model.*`、`blur_shader.h`、`config.h`、`config_io.*`、`capture.*`、`renderer.*`、`cli.*`、`MyopicDefocus.cpp`、`MyopicDefocusConfig.cpp` 是**上游 Lerrenp 原始布局**的遗留文件，新的 `core/` `gpu/` `ui/` `app/` 分层正逐步取代它们。

**`optical_model.*` 特别说明**：它是**光学红线的落点**，无论目录怎么重组，公式实现和它的单测真值必须始终保持同步 —— 详见第八节。

---

## 六、跑测试

### 6.1 运行

```bash
# 路径 A
./build/MyopicDefocusTests.exe

# 路径 B（MSBuild 产物）
.\src\bin\Release\MyopicDefocusTests.exe
```

### 6.2 期望结果

```
============================================
  总计: <N> 通过, 0 失败
============================================
```

**退出码 0 = 全绿；非 0 = 有失败。**

| 阶段 | 断言数 | 说明 |
|------|-------|------|
| 上游基线（光学层 + 配置 IO） | **43** | 光学 19 + 配置 IO 24 |
| 改造后新增 | +7 | 配置解析（通用 + 单屏覆盖）、默认值、显示器枚举 |
| **当前合计** | **50** | 目标：50 全绿 |

**上游基线的 43 条已实测全绿**（2026-11-09，直接运行上游 v3.1 预编译版 `MyopicDefocusTests.exe`，退出码 0）：

```
  [ComputeBlurRadii 阶段] passed=19 failed=0
  [config_io 阶段] passed=24 failed=0
  总计: 43 通过, 0 失败
```

> **当前快照（2026-11-09 13:05，重构进行中）**：重构后的测试 exe 报告 **50 通过 / 9 失败**。9 条失败全部集中在**新增的配置保存/加载往返**（`SaveConfig` 返回 0，随后 6 条字段回读断言失败），光学层与新加的多屏参数解析、默认值、显示器枚举用例**全部通过**。这是改造过程中的中间状态，功能开发完成后应回到全绿。

### 6.3 覆盖内容

| 模块 | 覆盖内容 |
|------|---------|
| **光学模型** | 上游默认参数一致性、本机默认参数、28″ 对照真值、4K 分辨率、零分辨率不崩溃、视距越远越糊、极近距离趋近 0、蓝通道恒大于绿通道、瞳孔越大越糊、同分辨率下大屏模糊像素数更小 |
| **配置 IO** | JSON 往返一致性、部分字段加载保留默认值、分辨率 0 序列化为 null、无效 JSON 返回 false 且不破坏原值、文件保存/读取一致、文件不存在视为默认、**旧版 Lerrenp config.json 格式兼容**、**旧版格式缺字段保留默认值** |
| **新增配置逻辑** | 通用参数 + 单屏覆盖的解析、默认值（强度 0.25 / 距离 40 / 瞳孔 6500 / 对角线 24 / 隐藏截图开 / 开机自启关）、显示器枚举不崩溃且至少拿到一块屏 |
| **光学红线** | 24″/1920×1080/40cm 的真值 **σ蓝 3.998768 / σ绿 1.413098**；28″/1920×1080/40cm 的真值 **3.427515 / 1.211227** |

### 6.4 测试框架说明

项目**不使用 GoogleTest / Catch2 等外部框架**，原因是不想给分发包和构建环境增加额外依赖。断言宏（`EXPECT_NEAR` / `EXPECT_TRUE` / `EXPECT_EQ_INT`）直接写在测试文件里，计数器 `g_passed` / `g_failed` 跨翻译单元共享。

控制台输出走 `WriteConsoleW`（不修改控制台代码页），在 cmd.exe 默认 GBK（CP936）下中文提示也不会乱码。

自检用例还会写一份日志：

```
%LOCALAPPDATA%\Temp\myopic-selftest.log
```

---

## 七、编码约定

### 7.1 文件编码

**所有源文件一律 UTF-8。**

- **MSVC 路径**：工程文件里显式加了 `/utf-8`，否则 MSVC 会按本地代码页读 UTF-8 中文，导致编译报错或字符串乱码：

  ```xml
  <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
  ```

  新增工程文件时**记得带上这一行**。

- **LLVM MinGW 路径**：脚本里用 `-finput-charset=UTF-8 -fexec-charset=UTF-8` 达到同样效果。

### 7.2 其他约定

| 项 | 约定 |
|----|------|
| 语言标准 | C++20 |
| 字符集 | Unicode（`UNICODE` / `_UNICODE` 预定义宏） |
| 警告级别 | MSVC: Level3 + `SDLCheck` + `ConformanceMode`；Clang: `-Wall -Wextra` |
| 宏 | `WIN32_LEAN_AND_MEAN`、`_WIN32_WINNT=0x0A00`（Win10 1809） |
| 命名空间 | `md::core` / `md::gpu` / `md::ui`；层内私有工具放 `md::<层>::detail` |
| COM 对象 | 一律用 `ComPtr<T>`（自动 Release），**不要手写 `Release()`** |
| UI 文案 | 一律用宽字符字面量 `L"..."`，源码 UTF-8，编译需 `/utf-8` |
| 头文件 | 用 `#pragma once`；跨模块只 include `types.h` |
| 换行符 | LF |

---

## 八、光学层红线（改公式之前先看这里）

光学模型是**这个项目的核心资产**，任何改动都必须满足下面几条。

### 8.1 必须 1:1 保持的量

```
LCA_nat = { r: -0.23 D, g: +0.24 D, b: +1.10 D }
sh      = +0.23 D
G       = 1000 / (1000/screen + lca)
circ    = pupil * (screen - G) / G
sigma   = circ / mm_per_px * 0.32
```

### 8.2 三条不可动摇的规则

| # | 规则 | 原因 |
|---|------|------|
| 1 | **负值不取 `abs()`** | `circ` 的正负代表焦点偏前/偏后，取绝对值会把物理含义翻转；上游 SVG 滤镜也是这个行为 |
| 2 | **红通道直通** | 近视离焦点 `+0.23 D` 恰好补偿红光天然 LCA `-0.23 D`，补偿后红通道剩余 LCA = 0，不需要模糊 |
| 3 | **只对 G / B 通道做高斯模糊** | 两通道用各自的 σ，**逐通道独立归一化** |

### 8.3 改公式 → 必须同步改测试真值

**改了光学层，就必须同步更新 `src/src/test_optical_model.cpp` 里的 `expected` 常量**，否则测试会红。

正确流程：

1. 改 `src/src/optical_model.cpp`（或 `.h`）里的实现；
2. 改 `src/src/test_optical_model.cpp` 里对应的 `EXPECT_NEAR` 的 `expected` 值和 `tol`；
3. 重新编译并运行 `MyopicDefocusTests.exe`，确认**全部断言全绿**；
4. 如果改动影响了与上游 JS 的一致性，**必须重新做数值对拍**（用上游 `myopic_defocus` 的 `get_blur_circles_px` 独立复算同一组参数，逐位比对）；
5. 在代码注释里保留 `// JS: xxx` 的对应关系，方便日后回溯。

**已知真值（改公式后请用这组做交叉验证）：**

| 用例 | σ蓝 | σ绿 |
|------|-----|-----|
| 24″ / 1920×1080 / 40cm / 6500μm | 3.998768 | 1.413098 |
| 28″ / 1920×1080 / 40cm / 6500μm | 3.427515 | 1.211227 |
| 14″ / 2560×1440 / 40cm / 6500μm | 9.140041 | 3.229939 |
| 28″ / 2560×1440 / 40cm / 6500μm | 4.570021 | 1.614970 |

> **不允许**为了「让它看起来更好」而调整 `0.32`。它是视觉标定系数，改它必须走一次完整的视觉评估，不是改一行代码的事。

---

## 九、许可证与溯源

| 来源 | 许可证 | 我们拿了什么 | 义务 |
|------|-------|------------|------|
| [Lerrenp/Myopic_defocus-cpp-build](https://github.com/Lerrenp/Myopic_defocus-cpp-build) | **MIT** | **代码基座**：C++/D3D11 骨架、DXGI 抓屏、HLSL 着色器、`.slnx`/`.vcxproj` 工程结构 | 保留原作者版权声明 —— 原文在 `src/LICENSE.upstream`，**不许删** |
| [refractify/myopic_defocus](https://github.com/refractify/myopic_defocus) | **GPL-3.0** | **仅算法思想与公开物理公式**（LCA 常数、屈光度算式、弥散圆公式、0.32 标定系数、SVG 滤镜的通道拆分/混合方式） | **不得复制其代码正文** |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | `src/src/third_party/nlohmann/json.hpp` 单头文件 | 保留文件头部的版权声明 |

### 关于「公式能抄、代码不能抄」

- **数学公式本身不受版权保护** —— `G = 1000/(1000/screen + lca)` 这种表达式谁都可以用；
- **但代码的表达形式受保护** —— 逐行翻译上游的 JS 到 C++ 会构成对 GPL 代码的衍生作品；
- 本项目的做法是：**理解物理含义后，用自己的代码结构和自己的命名重新实现**，并在注释里标注 `// JS: xxx` 作为溯源说明。

### 本项目成品

**以 MIT 发布。** 成品中包含 Lerrenp 的 MIT 代码，因此必须附带他的版权声明。

---

## 十、常见构建问题

| 报错 / 现象 | 原因 | 解决 |
|------------|------|------|
| `error MSB4019: 找不到导入的项目"...\Microsoft\VC\v180\Microsoft.Cpp.Default.props"` | **C++ 工作负载没装** | 按 4.3 节装「使用 C++ 的桌面开发」，装完确认 `v180` 目录出现 |
| `error C1083: 无法打开包括文件: "d3d11.h"` | Windows SDK 没装 | 装 Windows 11 SDK；或用 `--includeRecommended` 重跑安装命令 |
| `setup.exe` 返回 **Exit Code 5007** | `--quiet` 没管理员权限，或 `--installPath` 写错 | 用管理员 PowerShell；路径从 `vswhere` 原样复制 |
| `找不到 v145 工具集` | 装的是 VS 2022（v143） | 装 VS 2026 Build Tools；工程里的 `PlatformToolset` 就是 `v145` |
| `msbuild 不是内部或外部命令` | 没进 PATH | 用 MSBuild 全路径调用，或开「Developer PowerShell for VS 2026」 |
| `.slnx` 打不开 / 提示格式错误 | VS 版本太老 | 需要 VS 2026 或更高 |
| `unable to make temporary file` | clang.exe 拿到了 MSYS 的 `/tmp` | 用 `scripts/build.sh`（已处理）；手动调 clang++ 时把 `TEMP`/`TMP` 指到纯 Windows 路径 |
| `找不到编译器: .../x86_64-w64-mingw32-clang++.exe` | 工具链路径不对 | 用 `TC` 环境变量指向正确的 llvm-mingw 目录 |
| MinGW 下 `winreg.h` 报 `unknown type name 'DWORD'` 等 | 头文件包含顺序不对（`winreg.h` 需要先有 windows.h 的类型） | 在包含 `<winreg.h>` 之前先 `#include <windows.h>` |
| MinGW 下 `no type named 'BlurRadii' in namespace 'md::core'` | core 层 API 变更后 gpu 层没跟上 | 两层一起改，或检查 `types.h` 的契约是否已更新 |
| 测试跑出中文乱码 | 在 cmd.exe 里看 UTF-8 输出 | 用 Windows Terminal 或 PowerShell 7；代码本身已用 `WriteConsoleW` 处理 |
| 改了光学公式后测试红了 | 没同步改真值 | 按 8.3 节同步更新 `test_optical_model.cpp` 的 `expected` |
| 编译出 exe 但运行没效果 | 抓屏失败 / 参数算出来 σ≈0 | 这不是构建问题，去排查运行环境（独占全屏、HDR、参数填写） |

---

## 十一、LLVM MinGW / libc++ 踩坑清单

用 LLVM MinGW（clang 23 + libc++）交叉编译时，MSVC 的习惯写法会连环踩坑。以下全部为本项目实测确认。

| 症状 | 原因 | 正确写法 |
|---|---|---|
| `unable to make temporary file: Permission denied` | `clang.exe` 是原生 Windows 程序，读到 MSYS 的 `TEMP=/tmp` 无法创建临时文件 | 构建脚本里把 `TEMP`/`TMP` 指向 **Windows 格式**的可写目录 |
| `unknown type name 'ACCESS_MASK'` | `<winreg.h>` 排在 `<windows.h>` 之前被包含 | 先包含 `types.h`（它带 `windows.h`），再包含 `<winreg.h>` |
| `use of undeclared identifier 'FOLDERID_AppData'` | MinGW 的 `knownfolders.h` 未导出该 GUID 常量，会变成链接期缺符号 | 改用环境变量 `%APPDATA%`（本来就是同一个目录），别用 `SHGetKnownFolderPath` |
| `no member named 'GetPrimaryMonitor'` | MinGW 的 `winuser.h` 未提供该 API | 改用 `EnumDisplayMonitors`，按枚举顺序第一个即主屏 |
| `unknown type name 'DXGI_OUTPUT_DESC1'` | MinGW 把它放在 `dxgi1_6.h` 而不是 `dxgi1_2.h` | include `<dxgi1_6.h>`；或干脆不用这个结构体 |
| `too few arguments to function call`（`GetHwnd` / `GetDesc`） | MinGW 的 D3D 接口是 C 风格，参数要显式传指针 | `swapChain->GetHwnd(&hwnd)`、`texture->GetDesc(&desc)` |
| `DialogBoxIndirectW` 不存在 | MinGW 只有 `DialogBoxIndirect` 宏和 `DialogBoxIndirectParamW` | 用 `DialogBoxIndirectParamW(inst, tmpl, hwnd, proc, lParam)` |
| `no member named 'dwInfoVersion'` | MinGW 的 `NOTIFYICONDATAW` 把版本字段放在 union 里叫 `uVersion` | 写 `nid.uVersion` |
| `NIIF_LARGE` 未定义 | MinGW 里叫 `NIIF_LARGE_ICON` | 换名 |
| `struct { enum } = { 0 }` 报类型错误 | libc++ 不做 int→enum 的隐式转换 | 用 `= {}` |
| `ofstream` 不接受 `std::wstring` 路径 | libc++ 未实现 `basic_ofstream(const filesystem::path&)` | 改用 `_wfopen_s` + `fread`/`fwrite` |
| `std::filesystem` 路径操作莫名失败 | libc++ 的 `path::operator/` 用 `/` 作分隔符，而 **Win32 API 不接受正斜杠** | 显式用反斜杠拼接；建目录用 Win32 `CreateDirectoryW` 递归，不用 `create_directories` |
| **画面全黑**（无编译错、无运行错） | `PSSetShaderResources(slot, 0, ptr)` 中 `NumViews=0` 的语义是**解除绑定**，采样恒返回 0 | 必须写 `NumViews=1` |


| `Shell_NotifyIconW` 返回 FALSE 且 `GetLastError = 5` | MinGW-w64 的 `NOTIFYICONDATAW` 比 Windows 侧**多一个 `GUID guidItem` 字段（16 字节）**，`sizeof` 比系统期望的大 16 字节 → 托盘图标建不起来 | `cbSize` 写成 **944**（= Windows 侧到 `dwInfoFlags` 结束的长度，等价于 SDK 的 `NOTIFYICONDATAW_V3_SIZE`）。注意别用 MinGW 的 `NOTIFYICONDATAW_V3_SIZE` 宏，它按 MinGW 布局算出来是 960，不对 |
| `Shell_NotifyIconW` 在受限会话里恒返回 FALSE（err=5） | explorer 的托盘组件不可达（与权限无关） | 这不是代码问题。程序应改用自绘的右下角小图标 + 全局热键作为不依赖 explorer 的兜底入口 |


### 托盘图标（Shell_NotifyIcon）在本机完全不可用

实测 `Shell_NotifyIconW(NIM_ADD, ...)` **150+ 次调用全部 FALSE，GetLastError = 5 (ERROR_ACCESS_DENIED)**。穷举维度：

| 变量 | 试过的取值 | 结果 |
|---|---|---|
| `cbSize` | `sizeof`(976) / 952 / 944 / 936 | 全败 |
| `uFlags` | MSG\|ICON\|TIP / ICON\|TIP / MSG\|ICON\|TIP\|SHOWTIP | 全败 |
| 窗口类型 | 普通顶层 / TOOLWINDOW / TOOLWINDOW\|NOACTIVATE | 全败 |
| `uID` | 1 / 4242 / 7777 | 全败 |
| `NIM_SETVERSION` 顺序 | 先设版本 / 后设版本 | 全败 |
| `hIcon` | `LoadIcon` / 资源 ICO 32×32 / **NULL** | 全败 |

环境侧已排除：Session（本进程与 explorer 同为 Session 1）、窗口站（同为 WinSta0 / Default 桌面）、explorer 状态（pid 10180 正常运行）、管理员权限（UAC 提权后仍 err=5）、.NET 调用链（复测为异常被吞的假象）。

替代接口 `ITrayNotifyWin10`（CLSID `{25DEEA4A-4BD0-4E2C-B541-35DB5B5F1F3B}`）也不可用：`CoCreateInstance` 返回 **0x80040154 (REGDB_E_CLASSNOTREG)**。

**对策**：托盘失败时程序自动降级为自绘的右下角小图标 + 菜单（纯 Win32，不经过 explorer），并无条件注册 `Ctrl+Alt+S/P/Q` 全局热键作为保底出口。

### 中文弹窗乱码：MessageBoxA 不能传 UTF-8

```cpp
MessageBoxA(nullptr, "中文...", ...)   // ❌ 按系统 ANSI 代码页（中文系统=GBK）解读 → 乱码
MessageBoxW(nullptr, L"中文...", ...)  // ✅
```

本项目所有用户可见文案一律走 `MessageBoxW` + 宽字符字面量。


### 最坑的一个

最后那条 `NumViews` 值得单独记住：它**不报任何错误**，程序照常启动、照常抓屏、照常呈现，只是画面全黑。
本项目就是卡在这里排查了最久——表现是「抓屏原图正常、渲染输出全黑」，靠把渲染输出单独读回成 BMP 才定位到。

### 排查这类问题的方法

D3D 渲染出问题时，不要靠猜。用内置的取证模式把中间结果导出来：

```powershell
MyopicDefocus.exe --capture="C:\Users\你的用户名\Desktop\mdcap"
```

导出的 `*_captured.bmp`（抓屏原图）与 `*_rendered.bmp`（渲染输出）能一刀切开问题在哪一段：

- captured 有内容、rendered 全黑 → 渲染段（着色器资源绑定 / 常量缓冲 / 呈现）
- captured 也全黑 → 抓屏段（DXGI duplication / 显示模式 / 驱动）
- 两者都正常但没效果 → 光学参数段（强度为 0 或模糊半径过小）

进一步定位渲染段时，可以把 shader 的输出临时改成常量色（例：`return float4(1.0, 0.0, 0.0, 1.0);`）：
若 rendered 变成纯红，说明呈现链路正常，问题在采样输入；仍是黑，则是后缓冲或绑定问题。
