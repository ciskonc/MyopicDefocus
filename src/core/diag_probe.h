#pragma once

namespace md {
namespace core {

// 通过 EDID（注册表）探测桌面主显示器的物理对角线（英寸）。
// 返回 0 表示探测失败（虚拟显示器 / 信息缺失）。
float ProbeDiagonalInch();

// 带回退值的探测：失败时返回 fallback
float ProbeDiagonalInchOrDefault(float fallback);

} // namespace core
} // namespace md
