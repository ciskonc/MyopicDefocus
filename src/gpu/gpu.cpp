// ==========================================
// gpu.cpp — 抓屏 + 双 pass 可分离高斯 + 呈现
// ==========================================
// 相对上游的改动：
//   1. 上游只 EnumOutputs(0) 抓第一块屏幕 → 这里按设备名匹配 output，支持多屏
//   2. 上游交换链格式硬编码 B8G8R8A8_UNORM → 这里跟随抓屏源格式，10bit 屏走 10bit
//   3. 上游全局单例 → 这里每屏一个 ScreenRenderer
//   4. 独占全屏抓不到画面时，连续失败计数用于降级提示
#include "gpu.h"

#include "../blur_shader.h"
#include "../optical_model.h"
#include "../log.h"

#include <cmath>
#include <cstring>
#include <algorithm>


namespace md {
namespace gpu {

// ==========================================
// 共享着色器资源
// ==========================================
namespace shader {
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11Buffer>          cbParams;
    ComPtr<ID3D11SamplerState>    sampler;

    bool Create(ID3D11Device* device) {
        if (!device) return false;

        ComPtr<ID3DBlob> vsBlob, psBlob;
        HRESULT hr = D3DCompile(kBlurShaderHLSL, strlen(kBlurShaderHLSL), nullptr, nullptr, nullptr,
                                "VS_Main", "vs_5_0", 0, 0, &vsBlob, nullptr);
        if (FAILED(hr)) { LogHr("D3DCompile(VS_Main)", hr); return false; }

        hr = D3DCompile(kBlurShaderHLSL, strlen(kBlurShaderHLSL), nullptr, nullptr, nullptr,
                        "PS_Main", "ps_5_0", 0, 0, &psBlob, nullptr);
        if (FAILED(hr)) { LogHr("D3DCompile(PS_Main)", hr); return false; }

        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(ShaderParams);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.Usage = D3D11_USAGE_DEFAULT;
        hr = device->CreateBuffer(&bd, nullptr, &cbParams);
        if (FAILED(hr)) { LogHr("CreateBuffer(cbParams)", hr); return false; }

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = device->CreateSamplerState(&sd, &sampler);
        if (FAILED(hr)) { LogHr("CreateSamplerState", hr); return false; }

        return true;
    }

    void Destroy() {
        sampler.Reset();
        cbParams.Reset();
        ps.Reset();
        vs.Reset();
    }
}


// ---------------------------------------------------------------
// overlay 覆盖区域：主屏让开任务栏，保证托盘图标可见、可点
// ---------------------------------------------------------------
namespace {

struct OverlayArea {
    RECT window{ 0, 0, 0, 0 };   // 虚拟桌面坐标
    RECT tex{ 0, 0, 0, 0 };      // 纹理坐标（相对该输出左上角，y 向下）
};

OverlayArea ComputeOverlayArea(const ::md::core::ScreenInfo& s) {
    OverlayArea a;
    const RECT full = { s.x, s.y, s.x + s.width, s.y + s.height };
    RECT area = full;

    // 注：曾经尝试「只覆盖工作区（让开任务栏）」，但窗口尺寸与交换链尺寸不一致时，
    //   DWM 对 layered 窗口的合成会出现「任务栏图标平铺」这类残影。
    //   这里回到全屏覆盖（与上游一致，已验证稳定）：
    //   · 任务栏会被轻微模糊（纯色背景，视觉影响很小）
    //   · 但 WS_EX_TRANSPARENT 保证鼠标点击穿透，托盘图标依然点得到
    //   · 另有全局热键 Ctrl+Alt+S/P/Q 作为不依赖托盘的可靠出口

    a.window = area;
    a.tex.left   = area.left   - full.left;
    a.tex.top    = area.top    - full.top;
    a.tex.right  = area.right  - full.left;
    a.tex.bottom = area.bottom - full.top;
    return a;
}

// ==========================================
// 截图隐藏：能力探测 + 统一包装（耐操性审查 P1-A）
// ==========================================
// WDA_EXCLUDEFROMCAPTURE (0x11) 是 Win10 2004 (build 19041) 才引入的值；
// 1809~1909（含 LTSC 2019）上调用会失败 → overlay 进入自身捕获流 →
// 逐帧递归叠加 → GPU 打满卡死，且递归自愈调的也是同一个失败 API。
// 所以：启动时探测一次；不支持时视为「未隐藏」，看门狗阈值收紧，
// 主循环弹一次性风险提示。全部调用点必须走 TrySetDisplayAffinity。
bool g_wdaExcludeSupported = false;
bool g_wdaProbed = false;

void ProbeDisplayAffinitySupport() {
    if (g_wdaProbed) return;
    g_wdaProbed = true;
    // 用一个 1x1 的临时窗口试设一次：成功即支持
    HWND probe = ::CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
                                   0, 0, 1, 1, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
    if (probe) {
        g_wdaExcludeSupported = ::SetWindowDisplayAffinity(probe, WDA_EXCLUDEFROMCAPTURE) != FALSE;
        ::DestroyWindow(probe);
    }
    if (!g_wdaExcludeSupported) {
        LogInfo("[gpu] WDA_EXCLUDEFROMCAPTURE unsupported (Win10 <2004) - capture hiding disabled, recursion watchdog tightened");
    }
}

// 统一入口：支持才真正设置；不支持返回 false（调用方按"未隐藏"处理）
bool TrySetDisplayAffinity(HWND hwnd, bool hide) {
    ProbeDisplayAffinitySupport();
    if (hide && !g_wdaExcludeSupported) return false;
    return ::SetWindowDisplayAffinity(hwnd, hide ? WDA_EXCLUDEFROMCAPTURE : 0) != FALSE;
}

} // namespace

bool ScreenRenderer::IsCaptureHidingSupported() {
    ProbeDisplayAffinitySupport();
    return g_wdaExcludeSupported;
}


// ---------------------------------------------------------------
// 递归检测：抓取自身叠加会让画面逐帧变糊，清晰度持续递降
// ---------------------------------------------------------------
float ScreenRenderer::MeasureSharpness() {
    if (!capturedTexture_ || !device_ || !context_) return -1.0f;

    D3D11_TEXTURE2D_DESC desc{};
    capturedTexture_->GetDesc(&desc);
    if (desc.Width < 64 || desc.Height < 64) return -1.0f;
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return -1.0f;

    // 整帧回读后稀疏采样（每 8 像素取一点）计算拉普拉斯方差。
    // 不能只取一小块：那一块可能是纯色区域（如标签栏），方差恒为 0，检测不出递降。
    D3D11_TEXTURE2D_DESC st = desc;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    st.MiscFlags = 0;
    st.MipLevels = 1;
    st.ArraySize = 1;
    st.SampleDesc.Count = 1;
    st.SampleDesc.Quality = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&st, nullptr, &staging))) return -1.0f;

    context_->CopyResource(staging.Get(), capturedTexture_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return -1.0f;

    const unsigned char* px = static_cast<const unsigned char*>(mapped.pData);
    const int pitch = static_cast<int>(mapped.RowPitch);
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    const int step = 8;

    auto lum = [&](int y, int x) -> double {
        const unsigned char* p = px + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
        return (p[0] + p[1] + p[2]) / 3.0;
    };

    double sum = 0.0, sum2 = 0.0;
    int n = 0;
    for (int y = step; y + step < h; y += step) {
        for (int x = step; x + step < w; x += step) {
            const double lap = 4.0 * lum(y, x) - lum(y - step, x) - lum(y + step, x)
                             - lum(y, x - step) - lum(y, x + step);
            sum += lap; sum2 += lap * lap; ++n;
        }
    }
    context_->Unmap(staging.Get(), 0);

    if (n == 0) return -1.0f;
    const double mean = sum / n;
    const double var = sum2 / n - mean * mean;
    return static_cast<float>(var);
}

void RenderManager::CheckRecursion() {
    for (auto& e : entries_) {
        if (!e->renderer) continue;

        const float sharp = e->renderer->MeasureSharpness();
        if (sharp < 0.0f) {
            static int warned = 0;
            if (warned++ < 3) LogInfo("[gpu] MeasureSharpness unavailable");
            continue;
        }
        {
            // 降噪：正常帧静默（逐帧输出曾在 stderr 重定向时产生 90 万字节日志），
            // 仅当清晰度处于异常下降（streak>0，取上一轮判定值）时输出观测值
            if (e->renderer->sharpnessDropStreak_ > 0) {
                char buf[160];
                sprintf_s(buf, "[gpu] sharpness=%.1f last=%.1f streak=%d",
                         sharp, e->renderer->lastSharpness_, e->renderer->sharpnessDropStreak_);
                LogInfo(buf);
            }
        }

        auto& r = e->renderer;
        if (r->lastSharpness_ > 1.0f && sharp < r->lastSharpness_ * 0.75f) {
            ++r->sharpnessDropStreak_;
        } else {
            r->sharpnessDropStreak_ = 0;
        }
        r->lastSharpness_ = sharp;

        // 连续 3 次检测都大幅变糊 → 判定为「抓到自己」的递归叠加
        // P1-A：老系统无截图隐藏能力时，唯一防线只剩「隐藏 overlay 窗口」，
        // 看门狗阈值收紧到 2 次（约 1 秒@30帧检测频度），更早介入
        const int recursionThreshold = hideFromCapture_ ? 3 : 2;
        if (r->sharpnessDropStreak_ >= recursionThreshold) {
            r->sharpnessDropStreak_ = 0;
            LogInfo("[gpu] recursion detected - force enable capture hiding");
            if (!hideFromCapture_) {
                hideFromCapture_ = true;
                for (auto& e2 : entries_) {
                    if (e2->hwnd) TrySetDisplayAffinity(e2->hwnd, true);
                }
            }
            if (recursionCb_) recursionCb_(recursionUser_);
        }
    }
}

// ==========================================
// ScreenRenderer
// ==========================================
ScreenRenderer::~ScreenRenderer() {
    Shutdown();
}

void ScreenRenderer::SetDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    context_ = context;
}

void ScreenRenderer::Shutdown() {
    capturedSRV_.Reset();
    capturedTexture_.Reset();
    duplication_.Reset();
    tempSRV_.Reset();
    tempRTV_.Reset();
    tempTexture_.Reset();
    rtv_.Reset();
    swapChain_.Reset();
}

// ---------------------------------------------------------------
// 找到本屏对应的 DXGI output 并建立 duplication
// ---------------------------------------------------------------
bool ScreenRenderer::InitDuplication() {
    duplication_.Reset();
    capturedTexture_.Reset();
    capturedSRV_.Reset();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { LogHr("CreateDXGIFactory1", hr); return false; }

    // 遍历所有 adapter 的所有 output，按设备名匹配本屏
    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;

        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) continue;

            DXGI_OUTPUT_DESC desc = {};
            if (FAILED(output->GetDesc(&desc))) continue;

            // 设备名比对（\\.\DISPLAY1 这种形式）
            std::wstring want = Utf8ToWide(screen_.deviceName);
            if (wcscmp(desc.DeviceName, want.c_str()) != 0) continue;

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) { LogHr("Output->Output1", hr); return false; }

            hr = output1->DuplicateOutput(device_, &duplication_);
            if (FAILED(hr)) {
                LogHr("DuplicateOutput", hr);
                // 耐操性审查 P1-D：DuplicateOutput 要求 output 与 device 同适配器。
                // 共享设备建在默认适配器上——多 GPU 机器（Optimus/双独显）挂第二块
                // GPU 的屏在这里必败。按该 output 所在适配器重建专用设备重试一次。
                Microsoft::WRL::ComPtr<ID3D11Device> altDevice;
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> altContext;
                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
                hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &altDevice, &fl, &altContext);
                if (SUCCEEDED(hr)) {
                    hr = output1->DuplicateOutput(altDevice.Get(), &duplication_);
                    if (SUCCEEDED(hr)) {
                        device_ = altDevice.Get();
                        context_ = altContext.Get();
                        swapChain_.Reset();
                        rtv_.Reset();
                        LogInfo("[gpu] screen " + screen_.deviceName +
                                " now attached to its own adapter - per-GPU device");
                        return true;
                    }
                    LogHr("DuplicateOutput(on-own-adapter)", hr);
                } else {
                    LogHr("D3D11CreateDevice(on-own-adapter)", hr);
                }
                return false;
            }
            return true;
        }
    }

    LogInfo("[gpu] no DXGI output matched device name: " + screen_.deviceName);
    return false;
}

// ---------------------------------------------------------------
// 交换链：格式跟随抓屏源格式
// ---------------------------------------------------------------
bool ScreenRenderer::InitSwapChain(HWND hwnd, UINT width, UINT height) {
    swapChain_.Reset();
    rtv_.Reset();

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) { LogHr("D3DDevice->IDXGIDevice", hr); return false; }

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) { LogHr("GetAdapter", hr); return false; }

    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) { LogHr("GetParent(IDXGIFactory2)", hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Format = swapFormat_;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    // 分层窗口（WS_EX_LAYERED）只与 BitBlt/DISCARD 交换链有明确定义的互操作
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    hr = dxgiFactory->CreateSwapChainForHwnd(device_, hwnd, &scd, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) { LogHr("CreateSwapChainForHwnd", hr); return false; }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) { LogHr("GetBuffer(0)", hr); return false; }

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);

    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
    if (FAILED(hr)) { LogHr("CreateRTV from backBuffer", hr); return false; }

    D3D11_VIEWPORT vp = { 0, 0, (float)desc.Width, (float)desc.Height, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    CreateTempResources(desc.Width, desc.Height);
    return true;
}

// 只把 overlay 覆盖的那部分纹理画出来（主屏排除任务栏 → 托盘图标可见）
void ScreenRenderer::ApplyOverlayViewport(UINT texW, UINT texH) {
    if (!context_) return;
    const RECT t = overlayTex_;
    if (t.right <= t.left || t.bottom <= t.top || texW == 0 || texH == 0) {
        D3D11_VIEWPORT full = { 0, 0, (float)texW, (float)texH, 0.0f, 1.0f };
        context_->RSSetViewports(1, &full);
        return;
    }
    // 全屏覆盖：viewport 就是整个渲染目标。
    // （D3D11_VIEWPORT.TopLeftY 相对渲染目标左上角，不是 OpenGL 的左下角。）
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(texW), static_cast<float>(texH), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
}

// ---------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------
bool ScreenRenderer::Init(const ::md::core::ScreenInfo& screen, HWND hwnd) {
    screen_ = screen;
    UpdateParams(screen.params);

    if (!device_ || !context_) { LogInfo("[gpu] device not set"); return false; }

    // ⚠ 必须在窗口显示前设置：Desktop Duplication 会抓到自己进程的 overlay 窗口，
    //   形成逐帧叠加的递归（画面溶解重叠 + GPU 打满卡死）。
    //   P1-A：老系统（<2004）不支持时 TrySetDisplayAffinity 返回 false——
    //   不假装已隐藏，hideFromCapture_ 如实翻 false，看门狗阈值据此收紧。
    if (hideFromCapture_) {
        if (!TrySetDisplayAffinity(hwnd, true)) {
            hideFromCapture_ = false;   // 不支持 = 实际未隐藏，如实反映
        }
    }

    // 1. 先建立 duplication 并抓第一帧，确定源格式
    if (!InitDuplication()) {
        healthy_ = false;
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = duplication_->AcquireNextFrame(1000, &frameInfo, &desktopResource);
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(desktopResource.As(&tex))) {
            D3D11_TEXTURE2D_DESC srcDesc = {};
            tex->GetDesc(&srcDesc);
            swapFormat_ = srcDesc.Format;   // 跟随源格式：8bit→8bit，10bit→10bit
        }
        duplication_->ReleaseFrame();
    }
    if (swapFormat_ == DXGI_FORMAT_UNKNOWN) {
        swapFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;   // 兜底
    }

    // 2. 建交换链
    if (!InitSwapChain(hwnd, (UINT)screen.width, (UINT)screen.height)) return false;

    // 3. 抓屏健康度初值
    healthy_ = true;
    failStreak_ = 0;
    return true;
}

// ---------------------------------------------------------------
// 中间渲染目标
// ---------------------------------------------------------------
void ScreenRenderer::CreateTempResources(UINT w, UINT h) {
    tempTexture_.Reset();
    tempRTV_.Reset();
    tempSRV_.Reset();
    if (w == 0 || h == 0) return;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width     = w;
    desc.Height    = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format    = swapFormat_;
    desc.SampleDesc.Count = 1;
    desc.Usage     = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &tempTexture_);
    if (FAILED(hr)) { LogHr("CreateTempTexture", hr); return; }

    hr = device_->CreateRenderTargetView(tempTexture_.Get(), nullptr, &tempRTV_);
    if (FAILED(hr)) { LogHr("CreateTempRTV", hr); return; }

    hr = device_->CreateShaderResourceView(tempTexture_.Get(), nullptr, &tempSRV_);
    if (FAILED(hr)) { LogHr("CreateTempSRV", hr); return; }
}

// ---------------------------------------------------------------
// 抓一帧
// ---------------------------------------------------------------
void ScreenRenderer::CaptureFrame() {
    if (!duplication_) {
        if (!InitDuplication()) return;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopResource;

    // 首帧等待桌面就绪，之后非阻塞
    UINT timeoutMs = capturedTexture_ ? 0 : 1000;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);

    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> tex;
        hr = desktopResource.As(&tex);
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC srcDesc = {};
            tex->GetDesc(&srcDesc);

            D3D11_TEXTURE2D_DESC curDesc = {};
            if (capturedTexture_) capturedTexture_->GetDesc(&curDesc);

            bool needRecreate = !capturedTexture_ ||
                curDesc.Width != srcDesc.Width ||
                curDesc.Height != srcDesc.Height ||
                curDesc.Format != srcDesc.Format;

            if (needRecreate) {
                capturedTexture_.Reset();
                capturedSRV_.Reset();

                D3D11_TEXTURE2D_DESC newDesc = srcDesc;
                newDesc.BindFlags     = D3D11_BIND_SHADER_RESOURCE;
                newDesc.MiscFlags     = 0;
                newDesc.CPUAccessFlags = 0;
                newDesc.Usage         = D3D11_USAGE_DEFAULT;
                newDesc.MipLevels     = 1;
                newDesc.ArraySize     = 1;
                newDesc.SampleDesc.Count = 1;
                newDesc.SampleDesc.Quality = 0;

                hr = device_->CreateTexture2D(&newDesc, nullptr, &capturedTexture_);
                if (FAILED(hr)) {
                    LogHr("CaptureFrame: CreateTexture2D", hr);
                    duplication_->ReleaseFrame();
                    return;
                }

                hr = device_->CreateShaderResourceView(capturedTexture_.Get(), nullptr, &capturedSRV_);
                if (FAILED(hr)) {
                    LogHr("CaptureFrame: CreateSRV", hr);
                    duplication_->ReleaseFrame();
                    return;
                }

                // 源格式变了 → 交换链与中间纹理跟着换
                if (srcDesc.Format != swapFormat_) {
                    swapFormat_ = srcDesc.Format;
                    HWND hwnd = nullptr;
                    if (swapChain_) swapChain_->GetHwnd(&hwnd);
                    if (hwnd) InitSwapChain(hwnd, srcDesc.Width, srcDesc.Height);
                }

                // 耐操性审查 P1-B：分辨率/旋转变化后 overlay 窗口矩形必须同步，
                // 否则旧尺寸窗口残留桌面（实测残留 12s+ / 竖屏溢出 840px）。
                {
                    HWND hwnd = nullptr;
                    if (swapChain_) swapChain_->GetHwnd(&hwnd);
                    if (hwnd) {
                        ::SetWindowPos(hwnd, nullptr,
                                       screen_.x, screen_.y,
                                       static_cast<int>(srcDesc.Width),
                                       static_cast<int>(srcDesc.Height),
                                       SWP_NOACTIVATE | SWP_NOZORDER);
                    }
                }
                screen_.width  = static_cast<int>(srcDesc.Width);
                screen_.height = static_cast<int>(srcDesc.Height);
                LogInfo("[gpu] display mode changed - overlay resized to " +
                        std::to_string(srcDesc.Width) + "x" + std::to_string(srcDesc.Height));

                UpdateShaderParams();
            }

            if (capturedTexture_) {
                context_->CopyResource(capturedTexture_.Get(), tex.Get());
            }
        }
        duplication_->ReleaseFrame();

        if (failStreak_ != 0) { failStreak_ = 0; healthy_ = true; }
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST) {
        // 桌面切换 / 显示模式变更：重建 duplication
        duplication_.Reset();
        failStreak_++;
    }
    else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // 画面没更新（静态桌面），属正常，不算失败
    }
    else {
        // 真正的失败（如独占全屏抢占了输出）
        failStreak_++;
        // 阈值 5（约 0.1s@60fps）：抓不到画面时尽快隐藏 overlay，
        // 否则会把最后一帧旧画面冻结在屏幕上。偶发单次 ACCESS_LOST 不会触发。
        if (failStreak_ >= 5) healthy_ = false;
        if (failStreak_ == 30) {
            LogInfo("[gpu] capture degraded repeatedly on " + screen_.deviceName);
        }
    }
}

// ---------------------------------------------------------------
// 参数更新
// ---------------------------------------------------------------
void ScreenRenderer::UpdateParams(const ::md::core::ResolvedParams& p) {
    screen_.params = p;
    UpdateShaderParams();
}

void ScreenRenderer::UpdateShaderParams() {
    // 光学层是纯函数，这里用屏幕自身的分辨率与参数算模糊半径
    ::md::core::ScreenInfo s = screen_;
    s.params = screen_.params;

    D3D11_TEXTURE2D_DESC capDesc{};
    if (capturedTexture_) {
        capturedTexture_->GetDesc(&capDesc);
        s.width  = static_cast<int>(capDesc.Width);
        s.height = static_cast<int>(capDesc.Height);
    }

    // 光学层入参：分辨率 + 该屏生效的观看参数
    ::md::core::ScreenGeometry geo;
    geo.resX = s.width;
    geo.resY = s.height;
    geo.diagInch = s.params.diagInch;
    geo.screenDistanceCM = s.params.screenDistanceCM;
    geo.pupilSizeUm = s.params.pupilSizeUm;

    ::md::core::BlurRadii r = ::md::core::ComputeBlurRadii(geo);

    // 耐操性审查 P0-A②/P1-E（渲染端双保险）：
    // 卷积核 range = max(8, floor(σ*1.7)) 是逐像素动态循环，σ 上不封顶时
    // 一个合法滑块组合（4K + 10cm + 大瞳孔）即可让 GPU 负载爆炸/驱动 TDR。
    // 这里封死非有限值与过大的 σ（128px 上限 ≈ 物理上远超任何真实离焦场景）。
    const float kSigmaMax = 128.0f;
    const auto safe = [kSigmaMax](float v) {
        if (!(v >= 0.0f) || v > kSigmaMax) return kSigmaMax;   // NaN/负/inf → 上限
        return v;
    };
    blurB_ = safe(r.blue);
    blurG_ = safe(r.green);
}


// ==========================================
// 帧读回（BMP）——效果取证 / 诊断
// ==========================================
namespace {

// 写 24 位 BMP（源为 BGRA，行自下而上翻转）
bool WriteBmpFile(const wchar_t* path, int w, int h, const unsigned char* bgra, int pitch) {
    if (!path || !bgra || w <= 0 || h <= 0) return false;

    const DWORD rowSize = static_cast<DWORD>((w * 3 + 3) & ~3);
    const DWORD imageSize = rowSize * static_cast<DWORD>(h);
    const DWORD fileSize  = 54 + imageSize;

    std::vector<unsigned char> buf(fileSize, 0);
    unsigned char* p = buf.data();

    auto put32 = [](unsigned char* q, DWORD v) {
        q[0] = static_cast<unsigned char>(v);
        q[1] = static_cast<unsigned char>(v >> 8);
        q[2] = static_cast<unsigned char>(v >> 16);
        q[3] = static_cast<unsigned char>(v >> 24);
    };

    p[0] = 'B'; p[1] = 'M';
    put32(p + 2,  fileSize);
    put32(p + 10, 54);
    put32(p + 14, 40);                 // BITMAPINFOHEADER
    put32(p + 18, static_cast<DWORD>(w));
    put32(p + 22, static_cast<DWORD>(h));
    p[26] = 1;  p[27] = 0;             // planes
    p[28] = 24; p[29] = 0;             // 24 bpp
    put32(p + 30, 0);                  // BI_RGB
    put32(p + 34, imageSize);
    put32(p + 38, 2835);
    put32(p + 42, 2835);

    unsigned char* out = p + 54;
    for (int y = h - 1; y >= 0; --y) {
        const unsigned char* src = bgra + static_cast<size_t>(y) * pitch;
        unsigned char* dst = out + static_cast<size_t>(h - 1 - y) * rowSize;
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];   // B
            dst[x * 3 + 1] = src[x * 4 + 1];   // G
            dst[x * 3 + 2] = src[x * 4 + 2];   // R
        }
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    const size_t n = fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return n == buf.size();
}

// 把纹理读回 CPU 并写 BMP（只支持 8bit BGRA）
bool SaveTextureBmp(ID3D11Device* device, ID3D11DeviceContext* context,
                    ID3D11Texture2D* texture, const wchar_t* path) {
    if (!device || !context || !texture || !path) return false;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        LogInfo("[gpu] SaveTextureBmp: only B8G8R8A8_UNORM supported");
        return false;
    }

    D3D11_TEXTURE2D_DESC st = desc;
    st.Usage          = D3D11_USAGE_STAGING;
    st.BindFlags      = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    st.MiscFlags      = 0;
    st.MipLevels      = 1;
    st.ArraySize      = 1;
    st.SampleDesc.Count  = 1;
    st.SampleDesc.Quality = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&st, nullptr, &staging);
    if (FAILED(hr)) { LogHr("SaveTextureBmp: CreateTexture2D", hr); return false; }

    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { LogHr("SaveTextureBmp: Map", hr); return false; }

    const bool ok = WriteBmpFile(path, static_cast<int>(desc.Width), static_cast<int>(desc.Height),
                                 static_cast<const unsigned char*>(mapped.pData),
                                 static_cast<int>(mapped.RowPitch));
    context->Unmap(staging.Get(), 0);
    return ok;
}

} // namespace

bool ScreenRenderer::SaveCapturedBmp(const wchar_t* path) const {
    if (!capturedTexture_) return false;
    return SaveTextureBmp(device_, context_, capturedTexture_.Get(), path);
}

bool ScreenRenderer::SaveRenderedBmp(const wchar_t* path) {
    if (!swapChain_) return false;
    // 重绘一帧但不 Present：DISCARD 交换链在 Present 之后 back buffer 内容未定义，
    // 直接读会得到全黑。
    DrawFrame();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SaveTextureBmp(device_, context_, back.Get(), path);
}

// ---------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------
void ScreenRenderer::DrawFrame() {
    CaptureFrame();
    if (!rtv_ || !context_ || !capturedSRV_ || !tempRTV_) return;

    // ⚠ 必须显式绑定 SRV：D3D11 的着色器资源是「脏标记」绑定，
    //   从未绑定过的槽位采样恒返回 0 → 全黑。
    //   t0 = 抓到的原始画面，t1 = 同上（PASS 2 用它做强度混合的基准）。
    // ⚠ NumViews 必须为 1：NumViews=0 在 D3D11 里表示「解除该槽位绑定」，
    //   会让采样恒返回 0（画面全黑）。
    context_->PSSetShaderResources(0, 1, capturedSRV_.GetAddressOf());
    context_->PSSetShaderResources(1, 1, capturedSRV_.GetAddressOf());

    // viewport 决定画哪块纹理区域（overlay 只覆盖工作区）
    {
        D3D11_TEXTURE2D_DESC cd{};
        if (capturedTexture_) {
            capturedTexture_->GetDesc(&cd);
            ApplyOverlayViewport(cd.Width, cd.Height);
        }
    }

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(shader::vs.Get(), nullptr, 0);
    context_->PSSetShader(shader::ps.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, shader::cbParams.GetAddressOf());
    context_->PSSetSamplers(0, 1, shader::sampler.GetAddressOf());

    ShaderParams p1 = { (float)screen_.width, (float)screen_.height,
                        blurB_, blurG_, screen_.params.effectStrength, 0 };
    context_->UpdateSubresource(shader::cbParams.Get(), 0, nullptr, &p1, 0, 0);

    // PASS 1: 水平模糊 → 中间纹理
    float clearColor[4] = { 0, 0, 0, 0 };
    context_->ClearRenderTargetView(tempRTV_.Get(), clearColor);

    ID3D11RenderTargetView* rtvTmp = tempRTV_.Get();
    context_->OMSetRenderTargets(1, &rtvTmp, nullptr);
    context_->Draw(4, 0);

    // PASS 2: 垂直模糊 + 与原图按强度混合 → 交换链
    ShaderParams p2 = { (float)screen_.width, (float)screen_.height,
                        blurB_, blurG_, screen_.params.effectStrength, 1 };
    context_->UpdateSubresource(shader::cbParams.Get(), 0, nullptr, &p2, 0, 0);

    ID3D11RenderTargetView* rtvOut = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtvOut, nullptr);
    context_->PSSetShaderResources(0, 1, tempSRV_.GetAddressOf());
    context_->PSSetShaderResources(1, 1, capturedSRV_.GetAddressOf());
    context_->Draw(4, 0);
}

void ScreenRenderer::Render() {
    DrawFrame();
    if (swapChain_) swapChain_->Present(0, 0);
}

void ScreenRenderer::Resize(int width, int height) {
    if (!swapChain_ || width <= 0 || height <= 0) return;

    rtv_.Reset();
    tempRTV_.Reset();
    tempSRV_.Reset();
    tempTexture_.Reset();

    HRESULT hr = swapChain_->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { LogHr("ResizeBuffers", hr); return; }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) { LogHr("GetBuffer(0)", hr); return; }

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
    if (FAILED(hr)) { LogHr("CreateRTV from backBuffer", hr); return; }

    D3D11_VIEWPORT vp = { 0, 0, (float)desc.Width, (float)desc.Height, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    CreateTempResources(desc.Width, desc.Height);
}

// ==========================================
// RenderManager
// ==========================================
namespace {

// overlay 窗口类与消息处理
constexpr wchar_t kOverlayClass[] = L"MyopicDefocusOverlay";

struct OverlayUserData {
    md::gpu::RenderManager* manager = nullptr;
    md::gpu::ScreenRenderer* renderer = nullptr;
    std::string screenId;
};

std::vector<OverlayUserData*> g_overlayData;

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayUserData* data = reinterpret_cast<OverlayUserData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (data && data->renderer) {
            data->renderer->Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_DESTROY:
        if (data) {
            auto it = std::find(g_overlayData.begin(), g_overlayData.end(), data);
            if (it != g_overlayData.end()) g_overlayData.erase(it);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_APP + 2:   // Ctrl+Alt+Q 退出兜底
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool RenderManager::Init(HWND hwnd) {
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   createDeviceFlags, featureLevels, 1,
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        LogHr("D3D11CreateDevice", hr);
        // 回退到 WARP 软件渲染，保证至少能出画面
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               createDeviceFlags, featureLevels, 1,
                               D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(hr)) { LogHr("D3D11CreateDevice(WARP)", hr); return false; }
        LogInfo("[gpu] fell back to WARP software renderer");
    }

    if (!shader::Create(device_.Get())) return false;
    return true;
}

bool RenderManager::SetupScreens(const std::vector<::md::core::ScreenInfo>& screens) {
    // ⚠ 只清理屏幕上下文，**不能**调 Shutdown()：
    //   Shutdown() 会销毁共享的 shader / 常量缓冲，而 SetupScreens 之后紧接着就要渲染，
    //   届时 cbParams 已为空 → UpdateSubresource 空指针崩溃。
    ClearScreens();

    // 注册 overlay 窗口类（只注册一次）
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = OverlayWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = kOverlayClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            LogWin32("RegisterClassEx", GetLastError());
            return false;
        }
        classRegistered = true;
    }

    for (const auto& screen : screens) {
        auto entry = std::make_unique<Entry>();
        entry->renderer = std::make_unique<ScreenRenderer>();
        entry->renderer->SetDevice(device_.Get(), context_.Get());
        entry->renderer->SetHideFromCapture(hideFromCapture_);

        // overlay 覆盖整块输出（与交换链尺寸严格一致，避免 DWM 合成残影）
        const OverlayArea area = ComputeOverlayArea(screen);
        entry->renderer->overlayTex_ = area.tex;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClass, L"Overlay", WS_POPUP,
            area.window.left, area.window.top,
            area.window.right - area.window.left,
            area.window.bottom - area.window.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) { LogWin32("CreateWindowEx(overlay)", GetLastError()); continue; }

        // 分层窗口必须调用 SetLayeredWindowAttributes，否则行为未定义
        if (!SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)) {
            LogWin32("SetLayeredWindowAttributes", GetLastError());
        }

        auto* data = new OverlayUserData{ this, entry->renderer.get(), screen.id };
        g_overlayData.push_back(data);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));

        if (!entry->renderer->Init(screen, hwnd)) {
            LogInfo("[gpu] screen init failed: " + screen.deviceName);
            // 保留窗口但不渲染，由 UI 提示
        }

        entry->hwnd = hwnd;
        entries_.push_back(std::move(entry));
    }

    // 首帧渲染完成后再显示，避免未初始化内容闪烁
    RenderAll();
    for (auto& e : entries_) {
        if (e->hwnd) ShowWindow(e->hwnd, SW_SHOWNOACTIVATE);
    }
    return !entries_.empty();
}

void RenderManager::RenderAll() {
    // 每 30 帧做一次递归检测（画面是否在自我叠加）
    if ((++frameCounter_ % 30) == 0) {
        CheckRecursion();
    }

    // 每块屏按自身刷新率独立节流
    for (auto& e : entries_) {
        if (!e->renderer || !e->hwnd) continue;

        int hz = e->renderer->Screen().refreshHz;
        if (hz <= 0) hz = 60;                 // 读不到刷新率时按 60 处理
        if (fpsCap_ > 0 && hz > fpsCap_) hz = fpsCap_;

        auto interval = std::chrono::microseconds(1000000 / hz);
        auto now = std::chrono::steady_clock::now();
        if (now < e->nextFrame) continue;
        e->nextFrame = now + interval;

        e->renderer->Render();
    }
}

void RenderManager::UpdateParams(const std::string& screenId, const ::md::core::ResolvedParams& p) {
    for (auto& e : entries_) {
        if (e->renderer && e->renderer->Screen().id == screenId) {
            e->renderer->UpdateParams(p);
        }
    }
}

bool RenderManager::IsScreenHealthy(const std::string& screenId) const {
    for (const auto& e : entries_) {
        if (e->renderer && e->renderer->Screen().id == screenId) {
            return e->renderer->IsHealthy();
        }
    }
    return true;
}

bool RenderManager::HasDegradedScreen() const {
    for (const auto& e : entries_) {
        if (e->renderer && !e->renderer->IsHealthy()) return true;
    }
    return false;
}

HWND RenderManager::GetOverlayWindow(const std::string& screenId) const {
    for (const auto& e : entries_) {
        if (e->renderer && e->renderer->Screen().id == screenId) return e->hwnd;
    }
    return nullptr;
}

bool RenderManager::SaveFrames(const std::wstring& dir, const std::wstring& tag) {
    bool any = false;
    for (auto& e : entries_) {
        if (!e->renderer) continue;
        const std::string id = e->renderer->Screen().id;
        std::wstring safe;
        for (char ch : id) safe += (isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
        const std::wstring cap = dir + L"/" + tag + L"_" + safe + L"_captured.bmp";
        const std::wstring ren = dir + L"/" + tag + L"_" + safe + L"_rendered.bmp";
        if (e->renderer->SaveCapturedBmp(cap.c_str())) any = true;
        if (e->renderer->SaveRenderedBmp(ren.c_str())) any = true;
    }
    return any;
}

void RenderManager::SetOverlayVisible(bool visible) {
    for (auto& e : entries_) {
        if (!e->hwnd) continue;
        if (visible) {
            ::ShowWindow(e->hwnd, SW_SHOWNOACTIVATE);
        } else {
            ::ShowWindow(e->hwnd, SW_HIDE);
        }
    }
}

void RenderManager::SetHideFromCapture(bool hide) {
    // 记下标志，供随后 SetupScreens 创建的窗口使用；
    // 对已存在的窗口立即生效。
    hideFromCapture_ = hide;
    for (auto& e : entries_) {
        if (!e->hwnd) continue;
        // P1-A：统一走能力感知包装（老系统 hide=true 时返回 false 且不改状态）
        if (!TrySetDisplayAffinity(e->hwnd, hide)) {
            if (hide) LogWin32("SetWindowDisplayAffinity", ::GetLastError());
        }
    }
}

void RenderManager::ClearScreens() {
    for (auto& e : entries_) {
        if (e->hwnd) {
            DestroyWindow(e->hwnd);
            e->hwnd = nullptr;
        }
        if (e->renderer) e->renderer->Shutdown();
    }
    entries_.clear();
    g_overlayData.clear();
}

void RenderManager::Shutdown() {
    ClearScreens();
    shader::Destroy();
    device_.Reset();
    context_.Reset();
}

} // namespace gpu
} // namespace md
