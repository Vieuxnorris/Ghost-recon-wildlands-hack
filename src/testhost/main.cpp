#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    bool present1 = false;
    int resize_every_ms = 0;
    int exit_after_ms = 0;
    int fps = 0;  // 0 = vsync
};

Options ParseArgs(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!wcscmp(argv[i], L"--present1")) o.present1 = true;
        else if (!wcscmp(argv[i], L"--resize-every") && has_value) o.resize_every_ms = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--exit-after") && has_value) o.exit_after_ms = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--fps") && has_value) o.fps = _wtoi(argv[++i]);
    }
    return o;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const Options o = ParseArgs(argc, argv);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"grw_testhost";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"grw testhost", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance,
                              nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> sc;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", hr);
        return 1;
    }
    ComPtr<IDXGISwapChain1> sc1;
    sc.As(&sc1);

    ComPtr<ID3D11RenderTargetView> rtv;
    auto make_rtv = [&] {
        ComPtr<ID3D11Texture2D> back;
        sc->GetBuffer(0, IID_PPV_ARGS(&back));
        dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    };
    make_rtv();

    const double start = NowMs();
    double last_resize = start, last_title = start;
    int frames = 0;
    bool big = false;
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const double now = NowMs();
        if (o.exit_after_ms > 0 && now - start > o.exit_after_ms) return 0;

        if (o.resize_every_ms > 0 && now - last_resize > o.resize_every_ms) {
            last_resize = now;
            big = !big;
            rtv.Reset();
            ctx->ClearState();
            ctx->Flush();
            hr = sc->ResizeBuffers(0, big ? 1600 : 1280, big ? 900 : 720, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) {
                std::fwprintf(stderr, L"ResizeBuffers failed: 0x%08lx\n", hr);
                return 2;
            }
            make_rtv();
        }

        const float t = float((now - start) / 1000.0);
        const float color[4] = {0.5f + 0.5f * std::sin(t), 0.3f, 0.5f + 0.5f * std::cos(t), 1.0f};
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->ClearRenderTargetView(rtv.Get(), color);

        const UINT sync = o.fps == 0 ? 1 : 0;
        if (o.present1 && sc1) {
            DXGI_PRESENT_PARAMETERS pp{};
            hr = sc1->Present1(sync, 0, &pp);
        } else {
            hr = sc->Present(sync, 0);
        }
        if (FAILED(hr)) {
            std::fwprintf(stderr, L"Present failed: 0x%08lx\n", hr);
            return 3;
        }
        if (o.fps > 0) Sleep(DWORD(1000 / o.fps));

        ++frames;
        if (now - last_title > 1000.0) {
            wchar_t title[64];
            std::swprintf(title, 64, L"grw testhost - %d fps", frames);
            SetWindowTextW(hwnd, title);
            frames = 0;
            last_title = now;
        }
    }
}
