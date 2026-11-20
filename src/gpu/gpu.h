// ==========================================
// gpu.h — GPU 层对外接口（capture + render）
// ==========================================
// 相对上游的改动：原上游是「单例全局 + 只抓 EnumOutputs(0)」。
// 这里改为「按屏持有渲染上下文」，支持多显示器各自独立渲染。
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl.h>

#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include "../types.h"

using Microsoft::WRL::ComPtr;

namespace md {
namespace gpu {

// ==========================================
// 单屏渲染上下文：抓屏 + 双 pass 模糊 + 呈现
// ==========================================
// 继承 types.h 里定义的抽象接口 md::gpu::ScreenContext
class ScreenRenderer : public ::md::gpu::ScreenContext {
public:
    ScreenRenderer() = default;
    ~ScreenRenderer() override;

    // 注入共享的 D3D 设备（由 RenderManager 统一创建，所有屏幕共用）
    void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context);

    // 对截图/录屏隐藏。必须在 Init 之前设置。
    // 这不只是隐私开关：Desktop Duplication 会把本进程自己的 overlay 窗口
    // 也抓进帧里，形成「抓到自己 → 画面逐帧叠加 → GPU 打满卡死」的递归。
    // 开启后 overlay 不进入捕获流，递归从根上消失。
    void SetHideFromCapture(bool hide) { hideFromCapture_ = hide; }

    // P1-A：本机是否支持截图隐藏（WDA_EXCLUDEFROMCAPTURE，Win10 2004+）。
    // 由 RenderManager 转发；不支持时 Init 内自动降级（视为未隐藏 + 看门狗收紧）。
    static bool IsCaptureHidingSupported();

    // 绑定一块屏幕并初始化 GPU 资源
    bool Init(const ::md::core::ScreenInfo& screen, HWND hwnd) override;

    // 窗口尺寸变化：重建交换链与中间纹理
    void Resize(int width, int height) override;

    // 抓一帧 + 双 pass 模糊 + 呈现
    void Render() override;

    // 只绘制到交换链后缓冲，不 Present（供读回取证用）
    void DrawFrame();

    // 把 viewport 限制到 overlay 实际覆盖的纹理区域（主屏排除任务栏）
    void ApplyOverlayViewport(UINT texW, UINT texH);

    // 递归检测：返回当前帧的清晰度（拉普拉斯方差，越低越糊）；失败返回 -1
    float MeasureSharpness();
    // 连续递降计数与历史值
    float lastSharpness_ = -1.0f;
    int   sharpnessDropStreak_ = 0;

    // 参数变化：重算蓝/绿通道模糊半径
    void UpdateParams(const ::md::core::ResolvedParams& p) override;

    // 释放 GPU 资源
    void Shutdown() override;

    // 抓屏是否健康（独占全屏等情况下 AcquireNextFrame 会持续失败）
    bool IsHealthy() const override { return healthy_; }

    // 所属屏幕
    const ::md::core::ScreenInfo& Screen() const { return screen_; }

    // overlay 只覆盖工作区（主屏排除任务栏），保证托盘图标可见、可点。
    // 纹理坐标（左上原点），相对该输出。
    RECT overlayTex_{ 0, 0, 0, 0 };

    // 连续抓屏失败次数（用于触发"抓不到画面"降级提示）
    int ConsecutiveFailures() const { return failStreak_; }

    // 把当前抓到的画面读回并存成 BMP（效果取证 / 诊断用）
    bool SaveCapturedBmp(const wchar_t* path) const;
    // 把渲染输出（交换链后缓冲）读回并存成 BMP
    bool SaveRenderedBmp(const wchar_t* path);

private:
    bool InitSwapChain(HWND hwnd, UINT width, UINT height);
    bool InitDuplication();
    void CaptureFrame();
    void CreateTempResources(UINT w, UINT h);
    void UpdateShaderParams();

    // --- 共享设备（外部持有，本类不释放） ---
    ID3D11Device*       device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // --- 本屏专属资源 ---
    ComPtr<IDXGISwapChain1>         swapChain_;
    ComPtr<ID3D11RenderTargetView>   rtv_;
    ComPtr<ID3D11Texture2D>          tempTexture_;
    ComPtr<ID3D11RenderTargetView>   tempRTV_;
    ComPtr<ID3D11ShaderResourceView> tempSRV_;

    // --- 抓屏资源 ---
    ComPtr<IDXGIOutputDuplication>   duplication_;
    ComPtr<ID3D11Texture2D>          capturedTexture_;
    ComPtr<ID3D11ShaderResourceView> capturedSRV_;

    ::md::core::ScreenInfo screen_;
    bool hideFromCapture_ = false;
    bool healthy_ = true;
    int  failStreak_ = 0;
    float blurB_ = 0.0f;
    float blurG_ = 0.0f;
    // 当前交换链/中间纹理格式，跟随抓屏源格式
    DXGI_FORMAT swapFormat_ = DXGI_FORMAT_UNKNOWN;
};

// GPU 常量缓冲区布局（与 HLSL cbuffer 对应，共 32 字节）
struct ShaderParams {
    float width, height, blurB, blurG, strength;
    int   direction;   // 0 = 水平, 1 = 垂直
    float pad[2];
};

// ==========================================
// 共享着色器资源（所有屏幕共用一套）
// ==========================================
namespace shader {
    extern ComPtr<ID3D11VertexShader>   vs;
    extern ComPtr<ID3D11PixelShader>    ps;
    extern ComPtr<ID3D11Buffer>          cbParams;
    extern ComPtr<ID3D11SamplerState>    sampler;
    bool Create(ID3D11Device* device);
    void Destroy();
}

// ==========================================
// 渲染管理器：管理所有屏幕的上下文
// ==========================================
class RenderManager {
public:
    // 创建共享 D3D 设备
    bool Init(HWND hwnd);

    // 按屏幕列表创建上下文（每屏一个 overlay 窗口 + 一套管线）
    bool SetupScreens(const std::vector<::md::core::ScreenInfo>& screens);

    // 全部渲染一遍
    void RenderAll();

    // 更新某屏参数
    void UpdateParams(const std::string& screenId, const ::md::core::ResolvedParams& p);

    // 某屏抓屏是否健康
    bool IsScreenHealthy(const std::string& screenId) const;

    // 是否存在任何降级屏（用于 UI 提示）
    bool HasDegradedScreen() const;

    // 只清理各屏上下文（保留共享设备与 shader 资源）
    void ClearScreens();

    // 完整清理：屏幕 + 共享设备 + shader
    void Shutdown();

    // 取得某屏的 overlay 窗口句柄
    HWND GetOverlayWindow(const std::string& screenId) const;

    // 取得共享设备（供外部使用）
    ID3D11Device* Device() const { return device_.Get(); }

    // 递归检测回调：检测到画面在自我叠加时触发（参数为 true）
    using RecursionCallback = void(*)(void* user);
    void SetRecursionCallback(RecursionCallback cb, void* user) {
        recursionCb_ = cb; recursionUser_ = user;
    }

    // 帧率上限（0 = 不额外限制，各屏按自身刷新率）
    void SetFpsCap(int fps) { fpsCap_ = fps; }

    // P1-A：本机是否支持截图隐藏（Win10 2004+），供主循环弹风险提示
    static bool IsCaptureHidingSupported() {
        return ::md::gpu::ScreenRenderer::IsCaptureHidingSupported();
    }

    // 把各屏的抓屏帧与渲染帧存成 BMP（效果取证 / 诊断）
    bool SaveFrames(const std::wstring& dir, const std::wstring& tag);

    // 显示 / 隐藏所有 overlay 窗口。
    // 「暂停效果」必须真正把窗口隐藏掉，否则画面会冻结在最后一帧。
    void SetOverlayVisible(bool visible);

    // 对截图/录屏隐藏：WDA_EXCLUDEFROMCAPTURE
    // 开启后 overlay 窗口不进入任何屏幕捕获（防录屏泄露隐私），
    // 同时也避免 Desktop Duplication 把 overlay 自己抓进帧里造成递归变黑。
    // 必须在 SetupScreens 之前调用（它会下发给随后创建的每个屏幕上下文）。
    void SetHideFromCapture(bool hide);

private:
    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> context_;

    struct Entry {
        std::unique_ptr<ScreenRenderer> renderer;
        HWND hwnd = nullptr;
        // 该屏下一帧的到期时刻（按各自刷新率独立节流）
        std::chrono::steady_clock::time_point nextFrame;
    };
    std::vector<std::unique_ptr<Entry>> entries_;
    int fpsCap_ = 0;
    bool hideFromCapture_ = false;
    RecursionCallback recursionCb_ = nullptr;
    void* recursionUser_ = nullptr;
    int frameCounter_ = 0;

    // 定期检查各屏是否出现「抓到自己」的递归叠加
    void CheckRecursion();
};

} // namespace gpu
} // namespace md
