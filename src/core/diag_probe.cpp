// ==========================================
// diag_probe.cpp — 屏幕物理对角线自动探测（EDID）
// ==========================================
// 显示器的物理尺寸写在 EDID 数据块里（字节 21/22 = 水平/垂直，单位 cm），
// EDID 存在注册表：
//   HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<监视器>\<实例>\Device Parameters\EDID
// 尺寸必须从硬件读——Windows 没有公开 API 直接给出"屏幕对角线英寸数"。
// 策略：枚举所有 EDID，取**最大的**物理对角线（桌面主显示器通常是最大的一块），
// 单屏用户 100% 准确；探测失败（虚拟显示器/信息缺失）返回 0，由调用方回退默认值。
#include "diag_probe.h"

#include <windows.h>
#include <cmath>
#include <string>
#include <vector>

namespace md {
namespace core {

float ProbeDiagonalInch() {
    // 遍历 HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY 的两层子键
    // ⚠ 双反斜杠必须保留：单反斜杠的 \C \E \D 是未知转义，编译器丢反斜杠后
    //   路径变成 "SYSTEMCurrentControlSetEnumDISPLAY" → RegOpenKeyExW 必失败
    const wchar_t* kBase =
        L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY";

    HKEY base = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBase, 0, KEY_READ, &base) != ERROR_SUCCESS) {
        return 0.0f;
    }

    float best = 0.0f;

    // 第一层：厂商（DEL4089、PHL0091…）
    for (DWORD vi = 0;; ++vi) {
        wchar_t vendor[128] = {};
        DWORD vlen = 128;
        if (RegEnumKeyExW(base, vi, vendor, &vlen, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS) break;

        HKEY vendorKey = nullptr;
        if (RegOpenKeyExW(base, vendor, 0, KEY_READ, &vendorKey) != ERROR_SUCCESS) continue;

        // 第二层：实例（6&2abcdef&0&UID…）
        for (DWORD ii = 0;; ++ii) {
            wchar_t inst[128] = {};
            DWORD ilen = 128;
            if (RegEnumKeyExW(vendorKey, ii, inst, &ilen, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS) break;

            // 逐层打开：vendorKey 已在 DISPLAY\<厂商> 层，
            // ⚠ 不可再把 vendor 拼进相对路径（曾写成 vendor+inst 双层 → rc=2 全灭）
            HKEY instKey = nullptr;
            if (RegOpenKeyExW(vendorKey, inst, 0, KEY_READ, &instKey) != ERROR_SUCCESS) continue;
            HKEY dpKey = nullptr;
            if (RegOpenKeyExW(instKey, L"Device Parameters", 0, KEY_READ, &dpKey)
                != ERROR_SUCCESS) {
                RegCloseKey(instKey);
                continue;
            }

            // EDID 常见 128/256B，部分机型 384B → 必须处理 ERROR_MORE_DATA 重试
            std::vector<BYTE> edid(512, 0);
            DWORD size = static_cast<DWORD>(edid.size());
            LONG r = RegQueryValueExW(dpKey, L"EDID", nullptr, nullptr, edid.data(), &size);
            if (r == ERROR_MORE_DATA) {
                edid.resize(size);
                size = static_cast<DWORD>(edid.size());
                r = RegQueryValueExW(dpKey, L"EDID", nullptr, nullptr, edid.data(), &size);
            }
            RegCloseKey(dpKey);
            RegCloseKey(instKey);
            if (r != ERROR_SUCCESS || size < 23) continue;

            // EDID 基本块字节 21/22 = 可显示区域水平/垂直尺寸（cm）
            const int wcm = edid[21];
            const int hcm = edid[22];
            if (wcm <= 0 || hcm <= 0) continue;

            const float inch = std::sqrt(static_cast<float>(wcm * wcm + hcm * hcm)) / 2.54f;
            if (inch > best) best = inch;
        }
        RegCloseKey(vendorKey);
    }
    RegCloseKey(base);

    // 合理范围过滤（<10 寸或 >100 寸视为解析错误）
    if (best < 10.0f || best > 100.0f) return 0.0f;
    return best;
}

float ProbeDiagonalInchOrDefault(float fallback) {
    const float v = ProbeDiagonalInch();
    return v > 0.0f ? v : fallback;
}

} // namespace core
} // namespace md
