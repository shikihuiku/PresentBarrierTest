#include <windows.h>
#include <wrl.h>

#include <string>
#include <sstream>
#include <vector>
#include <thread>
#include <semaphore>
#include <optional>

#include "PresentBarrierTest.h"

#include <dxgi1_6.h>

using Microsoft::WRL::ComPtr;

class D3DContext final
{

};

class ControlWindow final
{
private:
    static constexpr std::wstring_view WindowClassName{ L"ControlWindowClass" };
    static ATOM& WindowClass() { static ATOM atm{}; return atm; };
    HINSTANCE hInst{};
    HWND hWnd{};
    std::thread thd{};
    std::binary_semaphore semaphore{ 1 };

public:
    static bool RegisterWindowClass(HINSTANCE hInst)
    {
        if (WindowClass() == 0) {
            // Register the window class.
            WNDCLASS wc = { };
            wc.lpfnWndProc = Window::WndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = WindowClassName.data();

            WindowClass() = RegisterClassW(&wc);
        }

        if (WindowClass() == 0) {
            OutputDebugStringW(L"Failed to register application class.");
            return false;
        }
        return true;
    }

    static LRESULT __stdcall WndProc(HWND arg_hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(arg_hWnd, &ps);
            EndPaint(arg_hWnd, &ps);
        }
        break;
        case WM_CLOSE:
            DestroyWindow(arg_hWnd);
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(arg_hWnd, message, wParam, lParam);
        }
        return 0;
    }

    bool Init(HINSTANCE hInstance, const std::wstring windowName)
    {
        semaphore.acquire();

        hInst = hInstance;

        if (!RegisterWindowClass(hInstance)) {
            OutputDebugStringW(L"Failed to regiser window class.");
            return false;
        }

        thd = std::thread([&]() -> int
            {
                // naming the thread.
                if (FAILED(SetThreadDescription(GetCurrentThread(), windowName.c_str())))
                {
                    OutputDebugStringW(L"Failed to set the thread name.");
                    return 0;
                }

                // Create message pump for this thread.
                MSG msg;
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

                hWnd = CreateWindowExW(0, WindowClassName.data(), windowName.c_str(),
                    WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
                if (!hWnd)
                {
                    OutputDebugStringW(L"Failed to create a window.");
                    return 0;
                }

                ShowWindow(hWnd, SW_SHOWNORMAL);
                UpdateWindow(hWnd);

                // Release the main thread.
                semaphore.release();

                while (GetMessageW(&msg, nullptr, 0, 0))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                return (int)msg.wParam;
            });

        semaphore.acquire();
        semaphore.release();

        return true;
    }

    bool Terminate()
    {
        if (hWnd == 0)
            return true;

        if (PostMessage(hWnd, WM_CLOSE, 0, 0) == 0) {
            OutputDebugStringW(L"Failed to send WM_CLOSE to worker threads.");
            return false;
        }
        thd.join();

        hInst = 0;
        hWnd = 0;

        return true;
    }
};

class Window final
{
private:
    static constexpr std::wstring_view WindowClassName{ L"PresentBarrierClass" };
    static ATOM& WindowClass() {static ATOM atm{}; return atm; };
    HINSTANCE hInst{};
    HWND hWnd{};
    std::thread thd{};
    std::binary_semaphore semaphore{ 1 };

public:
    static bool RegisterWindowClass(HINSTANCE hInst)
    {
        if (WindowClass() == 0) {
            // Register the window class.
            WNDCLASS wc = { };
            wc.lpfnWndProc = Window::WndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = WindowClassName.data();

            WindowClass() = RegisterClassW(&wc);
        }

        if (WindowClass() == 0) {
            OutputDebugStringW(L"Failed to register application class.");
            return false;
        }
        return true;
    }

    static LRESULT __stdcall WndProc(HWND arg_hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(arg_hWnd, &ps);
            EndPaint(arg_hWnd, &ps);
        }
        break;
        case WM_CLOSE:
            DestroyWindow(arg_hWnd);
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(arg_hWnd, message, wParam, lParam);
        }
        return 0;
    }

    bool Init(HINSTANCE hInstance, const std::wstring windowName)
    {
        semaphore.acquire();

        hInst = hInstance;

        if (! RegisterWindowClass(hInstance)) {
            OutputDebugStringW(L"Failed to regiser window class.");
            return false;
        }

        thd = std::thread([&]() -> int
            {
                // naming the thread.
                if (FAILED(SetThreadDescription(GetCurrentThread(), windowName.c_str())))
                {
                    OutputDebugStringW(L"Failed to set the thread name.");
                    return 0;
                }

                // Create message pump for this thread.
                MSG msg;
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

                hWnd = CreateWindowExW(0, WindowClassName.data(), windowName.c_str(),
                    WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
                if (!hWnd)
                {
                    OutputDebugStringW(L"Failed to create a window.");
                    return 0;
                }

                ShowWindow(hWnd, SW_SHOWNORMAL);
                UpdateWindow(hWnd);

                // Release the main thread.
                semaphore.release();

                while (GetMessageW(&msg, nullptr, 0, 0))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                return (int)msg.wParam;
            });

        semaphore.acquire();
        semaphore.release();

        return true;
    }

    bool Terminate()
    {
        if (hWnd == 0)
            return true;

        if (PostMessage(hWnd, WM_CLOSE, 0, 0) == 0) {
            OutputDebugStringW(L"Failed to send WM_CLOSE to worker threads.");
            return false;
        }
        thd.join();

        hInst = 0;
        hWnd = 0;

        return true;
    }
};

class App final
{
private:
    class Adapter
    {
        ComPtr<IDXGIAdapter4>               adapter;
        std::vector<ComPtr<IDXGIOutput6>>   outputs;
        std::vector<DXGI_OUTPUT_DESC>       outputDescs;
        std::vector<DXGI_MODE_DESC>         currentModeDescs;

    public:
        bool Init(ComPtr<IDXGIAdapter>& a)
        {
            DXGI_ADAPTER_DESC desc;
            a->GetDesc(&desc);
            wprintf(L"Adapter device-id: %d vendor-id: %d description:%s\n", desc.DeviceId, desc.VendorId, desc.Description);

            if (desc.VendorId != 0x10DE)
                return false;
            // Found NVIDIA Adapter.

            if (FAILED(a.As(&adapter)))
                return false;

            ComPtr<IDXGIOutput> dxgiOut;
            for (UINT i = 0; adapter->EnumOutputs(i, &dxgiOut) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_OUTPUT_DESC desc;
                dxgiOut->GetDesc(&desc);

                wprintf(L"Output HMONITOR: %p\n", desc.Monitor);

                ComPtr<IDXGIOutput6> dxgiOut6;
                if (FAILED(dxgiOut.As(&dxgiOut6)))
                    return false;

                DXGI_MODE_DESC currentMode{};
                if (!GetClosestCurrentDisplayModeToCurrent(desc.Monitor, &currentMode)) {
                    return false;
                }
                if (FAILED(dxgiOut6->FindClosestMatchingMode(&currentMode, &currentMode, NULL))) {
                    return false;
                }

                outputs.push_back(dxgiOut6);
                outputDescs.push_back(desc);
                currentModeDescs.push_back(currentMode);
            }

            return true;
        }

        bool GetClosestCurrentDisplayModeToCurrent(HMONITOR hMon, DXGI_MODE_DESC* outCurrent)
        {
            *outCurrent = {};

            MONITORINFOEXW monitorInfo{};
            monitorInfo.cbSize = sizeof(MONITORINFOEXW);
            if (GetMonitorInfoW(hMon, &monitorInfo) == 0) {
                return false;
            }

            DEVMODEW devMode{};
            devMode.dmSize = sizeof(DEVMODEW);
            if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode) == 0) {
                return false;
            }

            outCurrent->Width = devMode.dmPelsWidth;
            outCurrent->Height = devMode.dmPelsHeight;
            if (devMode.dmDisplayFrequency != 0 && devMode.dmDisplayFrequency != 1) {
                outCurrent->RefreshRate.Denominator = 1;
                outCurrent->RefreshRate.Numerator = devMode.dmDisplayFrequency;
            }
            outCurrent->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            outCurrent->ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
            outCurrent->Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        }
    };

    ComPtr<IDXGIFactory7>   dxgiFactory;
    std::vector<std::unique_ptr<Adapter>> adapters;
    std::vector<std::unique_ptr<Window>> windows;

public:
    bool Init(HINSTANCE hInstance)
    {
        {
            UINT flags{};
#if defined(_DEBUG)
            flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
            HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&dxgiFactory));
            if (FAILED(hr)) {
                OutputDebugStringW(L"Failed to create a DXGIFactory interface.");
                return false;
            }
        }

        ComPtr<IDXGIAdapter> adapter;
        for (UINT i = 0; dxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            auto a = std::make_unique<Adapter>();
            if (!a->Init(adapter))
                continue;
            adapters.push_back(std::move(a));
        }

        {


        }

        for (size_t i = 0; i < 3; ++i) {
            auto w = std::make_unique<Window>();
            std::wstringstream ss;
            ss << L"Window: " << i;

            if (!w->Init(hInstance, ss.str()))
            {
                OutputDebugStringW(L"Failed to create a window.");
                return false;
            }

            windows.push_back(std::move(w));
        }

        return true;
    };

    bool Exit()
    {
        for (auto& w : windows) {
            w->Terminate();
            w.reset();
        }

        dxgiFactory.Reset();

        return true;
    }
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

#if defined(_DEBUG)
    {
        FILE* fp;

        AllocConsole();
        freopen_s(&fp, "CONIN$", "r", stdin);
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
#endif

    App app;
    if (! app.Init(hInstance)) {
        return 1;
    }

    Sleep(3000);

    app.Exit();

    return 0;
}
