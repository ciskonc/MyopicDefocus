#include "optical_model.h"

#include <cmath>

namespace md {
namespace core {

// ==========================================
// 核心算法：与上游 refractify/myopic_defocus 的 get_blur_circles_px 1:1 对应
// ==========================================
BlurRadii ComputeBlurRadii(const ScreenGeometry& geo) {
    // 1. 计算物理像素比例 (mm per pixel)
    //    上游: const diag_px = Math.sqrt(resx*resx + resy*resy);
    float diag_px = std::sqrt(static_cast<float>(geo.resX) * static_cast<float>(geo.resX) +
                               static_cast<float>(geo.resY) * static_cast<float>(geo.resY));
    if (diag_px < 1.0f) diag_px = 1.0f;

    //    上游: const diag_mm = options.diagInch * 25.4;
    float diag_mm = geo.diagInch * 25.4f;

    //    上游: const mm_per_px = diag_mm/diag_px;
    float mm_per_px = diag_mm / diag_px;

    //    上游: let pix = realWidthMm / resx;  (这其实就是 mm_per_px)
    float pix = mm_per_px;

    // 2. 准备物理参数
    //    上游: const pupil = p_pupilSizeUm/1000.0;
    float pupil = geo.pupilSizeUm / 1000.0f;

    //    上游: const screen = p_screenDistanceMm;
    float screen = geo.screenDistanceCM * 10.0f;

    // 3. 屈光度常数 (Diopters) - 完全对应上游常量
    //    上游: const lca_nat_r = -0.23; const sh = -lca_nat_r;  (sh = 0.23)
    float sh = 0.23f;

    //    上游: const lca_rif_b = 1.10 + sh;   → 1.33
    float lca_b = 1.10f + sh;

    //    上游: const lca_rif_g = 0.24 + sh;   → 0.47
    float lca_g = 0.24f + sh;

    // 4. 蓝色通道模糊 (Blue)
    //    上游: const G = 1000 / (1000 / screen + lca);
    float G_b = 1000.0f / (1000.0f / screen + lca_b);

    //    上游: const circ = pupil * ((screen - G) / G);
    //    1:1 还原上游：不做 abs，允许负值（与上游 SVG 滤镜保持一致）
    float circ_b = pupil * ((screen - G_b) / G_b);

    //    上游: blur_b = circ / pix;
    float blur_b_raw = circ_b / pix;

    // 5. 绿色通道模糊 (Green)
    float G_g = 1000.0f / (1000.0f / screen + lca_g);
    float circ_g = pupil * ((screen - G_g) / G_g);
    float blur_g_raw = circ_g / pix;

    // 6. 应用系数 (上游 init() 中的 0.32)
    BlurRadii out;
    out.blue  = blur_b_raw * 0.32f;
    out.green = blur_g_raw * 0.32f;
    return out;
}

} // namespace core
} // namespace md
