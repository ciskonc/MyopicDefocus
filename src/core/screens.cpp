// ==========================================
// screens.cpp — core 层：显示器枚举（DXGI）
// ==========================================
// 纯逻辑层：**不需要 D3D11 设备**，只用 DXGI 工厂 + Win32 显示设置 API。
//
// 枚举路径：
//   CreateDXGIFactory1 → IDXGIFactory1
//     → EnumAdapters1 → IDXGIAdapter1 → GetDesc1（适配器名）
//       → EnumOutputs → IDXGIOutput
//            → GetDesc   (DXGI_OUTPUT_DESC : DeviceName / DesktopCoordinates / Rotation)
//            → GetDesc1  (DXGI_OUTPUT_DESC1 : 额外 OutputColorSpace)  ← 需要 IDXGIOutput1
//
// 屏幕 id（配置按屏绑定用，必须跨重启稳定）：
//     adapterName + "#" + outputIndex
//   adapterName = adapter->GetDesc1().Description 转 UTF-8 后去掉尾部空格
//   outputIndex = 该适配器下 outputs 的**原始枚举序号**（不受 includeVirtual 影响）
//
// 注意：ScreenInfo::params 不在此处填充——EnumerateScreens 拿不到 Config，
//       由上层拿到 Config 后调用 Config::ResolveFor(screen.id) 填充。

#include "core.h"

#include <dxgi.h>
#include <dxgi1_2.h>

#include <string>
#include <vector>

namespace md {
namespace core {

namespace {

// ------------------------------------------------------------
// 虚拟显示器特征串
// ------------------------------------------------------------
// 命中任一即视为虚拟显示器（软件适配器 / IddCx 间接显示 / 远程显示）。
// 刻意不包含 NVIDIA / AMD / Intel(R) Graphics 这类真实 GPU 名，避免误判。
const char* const kVirtualMarkers[] = {
    "indirect display",      // IddCx 间接显示适配器
    "iddcx",                // Indirect Display Driver 扩展
    "virtual display",      // 显式虚拟显示适配器
    "microsoft basic render",// 系统软件渲染适配器
    "remote display",       // 远程桌面显示适配器
    "software adapter",     // 通用软件适配器
    "virtual",              // 兜底：xx Virtual Adapter / VirtualBox Graphics 等
};

// 取主显示器的设备名（形如 "\\.\DISPLAY1"）；取不到返回空串。
// 注：MinGW-w64 的 winuser.h 未提供 GetPrimaryMonitor，改用 EnumDisplayMonitors ——
// 按虚拟桌面坐标顺序枚举，第一个即主显示器。
std::string PrimaryDeviceName() {
    struct Ctx {
        std::wstring device;
    } ctx;

    BOOL ok = EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                c->device = mi.szDevice;
                return FALSE;   // 停止枚举
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

    if (!ok || ctx.device.empty()) return std::string();
    return detail::NarrowUtf8(ctx.device);
}

} // namespace

// ==========================================
// LooksLikeVirtualAdapter
// ==========================================

bool LooksLikeVirtualAdapter(const std::string& adapterName) {
    if (adapterName.empty()) return false;

    const std::string lower = detail::ToLowerAscii(adapterName);
    for (const char* marker : kVirtualMarkers) {
        if (lower.find(marker) != std::string::npos) return true;
    }
    return false;
}

// ==========================================
// FillDisplayMode
// ==========================================

bool FillDisplayMode(const std::string& deviceName, int& refreshHz, int& bitsPerPixel) {
    if (deviceName.empty()) return false;

    const std::wstring wide = detail::WidenUtf8(deviceName);

    DEVMODE dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(wide.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;

    // dmDisplayFrequency：0 或 1 表示「未指定 / 默认」，按 0 处理
    const int hz = static_cast<int>(dm.dmDisplayFrequency);
    refreshHz = (hz <= 1) ? 0 : hz;

    bitsPerPixel = (dm.dmBitsPerPel > 0) ? static_cast<int>(dm.dmBitsPerPel) : 0;
    return true;
}

// ==========================================
// EnumerateScreens
// ==========================================

std::vector<ScreenInfo> EnumerateScreens(bool includeVirtual) {
    std::vector<ScreenInfo> screens;

    // ---- 1) 拿 DXGI 工厂（不需要 D3D11 设备）----
    IDXGIFactory1* rawFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(detail::IidOf<IDXGIFactory1>(),
                                    reinterpret_cast<void**>(&rawFactory));
    if (FAILED(hr) || !rawFactory) return screens;

    detail::ComPtr<IDXGIFactory1> factory(rawFactory);

    // ---- 2) 主显示器判定用的设备名 ----
    const std::string primaryDevice = PrimaryDeviceName();
    const int primaryWidth  = GetSystemMetrics(SM_CXSCREEN);
    const int primaryHeight = GetSystemMetrics(SM_CYSCREEN);

    // ---- 3) 遍历适配器 → 输出 ----
    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        IDXGIAdapter1* rawAdapter = nullptr;
        hr = factory->EnumAdapters1(adapterIndex, &rawAdapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !rawAdapter) {
            if (FAILED(hr) && hr != DXGI_ERROR_NOT_FOUND) break;  // 枚举出错，停手
            continue;
        }
        detail::ComPtr<IDXGIAdapter1> adapter(rawAdapter);

        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (FAILED(adapter->GetDesc1(&adapterDesc))) continue;

        // 适配器名：DXGI 描述是空格补齐的定长数组，去掉尾部空格
        std::string adapterName = detail::TrimTrailingWhitespace(
            detail::NarrowUtf8(adapterDesc.Description));

        const bool adapterIsVirtual = LooksLikeVirtualAdapter(adapterName);

        for (UINT outputIndex = 0; ; ++outputIndex) {
            IDXGIOutput* rawOutput = nullptr;
            HRESULT oh = adapter->EnumOutputs(outputIndex, &rawOutput);
            if (oh == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(oh) || !rawOutput) {
                if (FAILED(oh) && oh != DXGI_ERROR_NOT_FOUND) break;
                continue;
            }
            detail::ComPtr<IDXGIOutput> output(rawOutput);

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;

            // 注：DXGI_OUTPUT_DESC1（OutputColorSpace）在本版本未使用，
            // 且 MinGW-w64 的 IDXGIOutput1 未导出 GetDesc1，故此处不查询。

            ScreenInfo s;

            // 稳定 id：适配器名 + "#" + 输出序号
            s.id          = adapterName + "#" + std::to_string(outputIndex);
            s.adapterName = adapterName;

            // 设备名（形如 "\\.\DISPLAY1"）
            s.deviceName = detail::NarrowUtf8(std::wstring(desc.DeviceName));

            // 虚拟桌面坐标与尺寸
            s.x      = desc.DesktopCoordinates.left;
            s.y      = desc.DesktopCoordinates.top;
            s.width  = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
            s.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

            // 刷新率与位深（EnumDisplaySettings）
            int hz = 0, bpp = 0;
            if (FillDisplayMode(s.deviceName, hz, bpp)) {
                s.refreshHz     = hz;
                s.bitsPerPixel  = bpp;
            } else {
                s.refreshHz     = 0;
                s.bitsPerPixel  = 8;   // 读不到时给个保守默认值
            }

            // 虚拟显示器判定
            s.isVirtual = adapterIsVirtual;

            // 主显示器判定：优先比设备名，退化时比 (0,0) + 屏幕尺寸
            if (!primaryDevice.empty()) {
                s.isPrimary = detail::EqualsIgnoreCase(s.deviceName, primaryDevice);
            } else {
                s.isPrimary = (s.x == 0 && s.y == 0 &&
                               s.width == primaryWidth && s.height == primaryHeight);
            }

            // params 保持默认值，由上层用 Config::ResolveFor(s.id) 填充

            // 虚拟显示器默认跳过
            if (s.isVirtual && !includeVirtual) continue;

            screens.push_back(s);
        }
    }

    return screens;
}

} // namespace core

} // namespace md
