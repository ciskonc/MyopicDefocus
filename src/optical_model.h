#pragma once

// ==========================================
// 光学算法层：纯函数（无 D3D 依赖，可单测）
// ==========================================
// 物理模型 1:1 对应上游 refractify/myopic_defocus 的 get_blur_circles_px：
//   自然纵向色差 LCA_nat = { R:-0.23, G:+0.24, B:+1.10 } D
//   近视离焦点 sh = +0.23 D（恰好补偿红光 LCA，故红通道直通）
//   离焦后 G 通道 lca = 0.47 D，B 通道 lca = 1.33 D
//   等效焦距   G     = 1000 / (1000/screen_mm + lca)
//   弥散圆半径 circ  = pupil_mm * (screen_mm - G) / G
//   模糊半径   σ(px) = circ / mm_per_px * 0.32
//   mm_per_px        = (diagInch * 25.4) / sqrt(resX² + resY²)
//
// ⚠ 红线：公式与上游保持 1:1，包括 0.32 经验系数与「负值不取 abs」。
//   改动本文件必须同步更新 test_optical_model.cpp 的真值断言。
//
// 与上游的唯一差异：入参从全局 Config 改为独立的 ScreenGeometry（纯值类型），
//   以便每块显示器用各自的分辨率与参数独立计算。

namespace md {
namespace core {

// 各通道的模糊半径（像素）。已应用经验系数 0.32。
struct BlurRadii {
    float blue;   // 蓝通道（波长短，离焦大）
    float green;  // 绿通道
};

// 光学计算所需的屏幕几何与观看条件
struct ScreenGeometry {
    int   resX = 0;                 // 屏幕横向像素
    int   resY = 0;                 // 屏幕纵向像素
    float diagInch = 24.0f;         // 屏幕对角线（英寸）
    float screenDistanceCM = 40.0f; // 观看距离（厘米）
    float pupilSizeUm = 6500.0f;    // 瞳孔直径（微米）
};

// 根据屏幕几何 + 眼睛参数 + 离焦模型，计算蓝/绿通道的高斯模糊半径。
BlurRadii ComputeBlurRadii(const ScreenGeometry& geo);

} // namespace core
} // namespace md
