// ==========================================
// types.h — 跨模块共享的数据契约
// ==========================================
// 本文件定义各模块之间传递的数据结构与接口。
// 模块边界：core（纯逻辑，无 D3D 依赖）→ capture/render（GPU）→ ui（界面）→ app（装配）
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <utility>
#include <cstdint>

namespace md {
namespace core {

// ==========================================
// 一、配置模型
// ==========================================

// 一组完整的离焦参数。通用配置与单屏覆盖共用同一结构，
// 单屏覆盖时其中「继承」项留空表示沿用通用值。
struct DefocusParams {
    // 效果强度 0.0 ~ 1.0
    float effectStrength = 0.25f;
    // 观看距离（厘米）
    float screenDistanceCM = 40.0f;
    // 瞳孔直径（微米）
    float pupilSizeUm = 6500.0f;
    // 屏幕对角线（英寸）
    float diagInch = 24.0f;

    // 合理物理域（耐操性审查 P0-B：只堵 ≤0 不够，巨大值/近零值会穿透到
    // 光学模型造成 σ 爆炸 → GPU 冻结，或 σ≈inf→0 → 效果静默消失）
    static constexpr float kDiagInchMin        = 5.0f;
    static constexpr float kDiagInchMax        = 200.0f;
    static constexpr float kDistanceMin        = 5.0f;    // cm
    static constexpr float kDistanceMax        = 500.0f;  // cm
    static constexpr float kPupilMin           = 1000.0f; // um
    static constexpr float kPupilMax           = 12000.0f;// um

    // 把非法值归到安全默认（P0-A/P0-B 统一钳制出口：
    // common 与 perScreen 覆盖项都必须过这里）
    void Sanitize() {
        if (!(diagInch        >= kDiagInchMin && diagInch        <= kDiagInchMax)) diagInch        = 24.0f;
        if (!(screenDistanceCM >= kDistanceMin && screenDistanceCM <= kDistanceMax)) screenDistanceCM = 40.0f;
        if (!(pupilSizeUm      >= kPupilMin    && pupilSizeUm      <= kPupilMax))    pupilSizeUm      = 6500.0f;
        // NaN 的比较恒为 false → 落进归默认分支，天然被防御
        if (!(effectStrength >= 0.0f)) effectStrength = 0.0f;
        else if (effectStrength > 1.0f) effectStrength = 1.0f;
    }
};

// 单屏覆盖项。std::nullopt = 该项沿用通用值。
struct ScreenOverride {
    std::optional<float> effectStrength;
    std::optional<float> screenDistanceCM;
    std::optional<float> pupilSizeUm;
    std::optional<float> diagInch;

    bool IsEmpty() const {
        return !effectStrength && !screenDistanceCM && !pupilSizeUm && !diagInch;
    }
    void ApplyTo(DefocusParams& p) const {
        if (effectStrength)  p.effectStrength  = *effectStrength;
        if (screenDistanceCM) p.screenDistanceCM = *screenDistanceCM;
        if (pupilSizeUm)      p.pupilSizeUm      = *pupilSizeUm;
        if (diagInch)        p.diagInch        = *diagInch;
        // 耐操性审查 P0-A：覆盖项此前零防御（0/负/巨大值直通光学模型）。
        // 合并后统一钳制——覆盖非法值等价于沿用通用值。
        p.Sanitize();
    }
};

// 全局配置
struct Config {
    int version = 2;

    // 一套通用参数：所有屏幕默认都用这套
    DefocusParams common;

    // 每块屏的单独设置，key = 屏幕唯一标识（见 ScreenInfo::id）
    std::vector<std::pair<std::string, ScreenOverride>> perScreen;

    // 是否让 overlay 窗口对截图/录屏隐藏（默认开）
    bool hideFromCapture = true;

    // 是否开机自启（默认关）
    bool autoStart = false;

    // 是否包含虚拟显示器（默认否）
    bool includeVirtualDisplays = false;

    // 检测到全屏独占游戏时自动暂停效果（默认开）
    // 判据是 DXGI 抓屏连续失败——独占全屏游戏接管了输出，Desktop Duplication 拿不到画面；
    // 普通全屏窗口不会触发这个信号，因此不会误伤。
    bool pauseOnFullscreenGame = true;

    // 托盘不可用时是否显示自建的右下角图标（默认跟随「托盘是否可用」自动决定）
    // -1 = 自动（托盘失败才显示）；0 = 强制不显示；1 = 强制显示
    int floatingIconMode = -1;

    // 目标帧率上限（0 = 跟随各屏刷新率，不额外限制）
    int fpsCap = 0;

    // 查找某屏的覆盖项；没有则返回空
    const ScreenOverride* FindOverride(const std::string& id) const;
    // 取得某屏最终生效的参数（通用 + 覆盖）
    DefocusParams ResolveFor(const std::string& id) const;
};

// 生效后的单屏参数（通用与覆盖合并结果）
using ResolvedParams = DefocusParams;

// ==========================================
// 二、显示器信息
// ==========================================

// 一块显示输出的描述信息
struct ScreenInfo {
    // 稳定标识：DXGI adapter 名 + output index + 设备名，供配置按屏绑定
    std::string id;
    // 人类可读名（设备名，如 "\\.\DISPLAY1"）
    std::string deviceName;
    // 显示适配器名
    std::string adapterName;
    // 在虚拟桌面中的位置与尺寸（像素，虚拟桌面坐标）
    int x = 0, y = 0, width = 0, height = 0;
    // 实际刷新率（Hz）；读不到时为 0
    int refreshHz = 0;
    // 当前显示模式位深（8 / 10 / 32）
    int bitsPerPixel = 8;
    // 是否为虚拟显示器（软件适配器 / IddCx）
    bool isVirtual = false;
    // 是否为主显示器
    bool isPrimary = false;

    // 该屏的离焦参数（通用 + 覆盖，已解析）
    ResolvedParams params;
};

// ==========================================
// 三、核心接口（core 层，纯逻辑，不依赖 D3D）
// ==========================================

// --- 配置 ---

// 默认配置
Config MakeDefaultConfig();

// 读取配置。优先读 %APPDATA%\MyopicDefocus\config.json；
// 若不存在，回退读取旧版 exe 同目录的 config.json（Lerrenp 字段名兼容）。
// 返回是否成功读到配置（false 表示用默认值）。
bool LoadConfig(Config& out);

// 保存配置到 %APPDATA%\MyopicDefocus\config.json。
bool SaveConfig(const Config& cfg);

// 旧版字段名兼容转换：读取 Lerrenp 格式的 JSON 到 Config（只填 common）。
bool ParseLegacyConfig(const std::string& json, Config& out);

// 把 Config 序列化为 JSON 字符串（新版格式）。
std::string SerializeConfig(const Config& cfg);

// --- 显示器 ---

// 枚举当前所有显示输出。virtualScreens=true 时包含虚拟显示器。
// 需 D3D11 设备上下文，由 render 层调用 core 的纯逻辑部分填充。
std::vector<ScreenInfo> EnumerateScreens(bool includeVirtual);

// 按 deviceName 查刷新率与位深（EnumDisplaySettings）。
// 找到则写入 refreshHz / bitsPerPixel 并返回 true。
bool FillDisplayMode(const std::string& deviceName, int& refreshHz, int& bitsPerPixel);

// 判断适配器名是否属于虚拟显示器（软件适配器特征串）。
bool LooksLikeVirtualAdapter(const std::string& adapterName);

// --- 光学 ---

// 光学层见 optical_model.h（1:1 沿用上游，不得随意改动）

} // namespace core

// ==========================================
// 四、GPU 层接口（capture + render）
// ==========================================

namespace gpu {

// 一块屏幕的完整渲染上下文。
// 原上游是单例全局变量；这里改为按屏持有，支持多显示器。
class ScreenContext {
public:
    virtual ~ScreenContext() = default;

    // 绑定一块屏幕并初始化资源。失败返回 false。
    virtual bool Init(const core::ScreenInfo& screen, HWND hwnd) = 0;
    // 窗口尺寸变化时重建交换链与中间纹理
    virtual void Resize(int width, int height) = 0;
    // 抓一帧 + 双 pass 模糊 + 呈现
    virtual void Render() = 0;
    // 参数变化时重算模糊半径
    virtual void UpdateParams(const core::ResolvedParams& p) = 0;
    // 关闭并释放 GPU 资源
    virtual void Shutdown() = 0;
    // 当前捕获是否健康（独占全屏等情况下会失败）
    virtual bool IsHealthy() const = 0;
};

// 创建 D3D11 设备与上下文（所有屏幕共享同一 device）
bool InitD3DDevice(HWND hwnd, void** outDevice, void** outContext);

// 创建一块屏幕的渲染上下文
ScreenContext* CreateScreenContext(void* device, void* context);

} // namespace gpu

// ==========================================
// 五、UI 层接口
// ==========================================

namespace ui {

// 应用运行时状态（UI 与核心之间共享）
struct AppState {
    core::Config config;
    std::vector<core::ScreenInfo> screens;
    bool overlayActive = true;       // 用户想要的效果开关（单一事实源）
    bool captureDegraded = false;     // 抓屏是否降级（独占全屏等）
    // 注意：滤镜窗口的实际可见性 = overlayActive && 抓屏健康，
    // 由主循环按帧维护；不存在独立的「游戏暂停」状态。
};

// 面板回调：参数变更后通知核心层立即生效
struct PanelCallbacks {
    std::function<void()> onParamsChanged;       // 任一参数变化
    std::function<void()> onToggleOverlay;        // 暂停/恢复
    std::function<void()> onRestartRequired;      // 需要重启才能生效
    std::function<void()> onQuit;                 // 退出程序
};

// 初始化托盘图标。返回 false 表示失败。
// state 为可写引用：托盘菜单要能切换「开机自启」并回写 config。
bool InitTray(HWND ownerWnd, AppState* state, const PanelCallbacks& cb);

// 显示设置面板（非模态独立窗口：调用立即返回，用户关闭时自行销毁）。
void ShowSettingsPanel(HWND ownerWnd, AppState* state, const PanelCallbacks& cb);

// 首次启动向导。返回 false 表示用户取消。
bool RunFirstRunWizard(HWND ownerWnd, AppState* state);

// 托盘气泡提示
void ShowTrayTip(const std::wstring& title, const std::wstring& msg);

// 刷新托盘菜单的勾选状态
void UpdateTrayMenu();

// explorer 重启后重建托盘图标（响应 TaskbarCreated 广播）
void ReaddTrayIcon();

// 退出时移除托盘图标（防止死图标残留堆积）
void RemoveTrayIcon();

} // namespace ui

// ==========================================
// 六、辅助
// ==========================================

// 把 UTF-8 字符串转 wstring
std::wstring Utf8ToWide(const std::string& s);

// 应用数据目录：%APPDATA%\MyopicDefocus（不存在则创建）
std::wstring GetAppDataDir();

// 开机自启（HKCU Run 键）
bool SetAutoStart(bool enable);
bool IsAutoStartEnabled();

} // namespace md
