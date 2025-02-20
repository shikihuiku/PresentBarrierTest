// PresentBarrierTest1.cpp : Defines the entry point for the application.
//
#include <windows.h>
#include "PresentBarrierTest.h"
#include <string>
#include <vector>
#include <thread>
#include <semaphore>

class App
{
public:
    static constexpr std::wstring_view ClassName = L"PresentBarrier";
    ATOM RegisterAppClass(HINSTANCE hInstance);
};

class Window
{
public:
    HINSTANCE hInst = 0;
    HWND hWnd = 0;
    std::thread thd;
    DWORD thID = 0;
    std::binary_semaphore semaphore{1};

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

    BOOL InitInstance(HINSTANCE hInstance, const wchar_t *windowName)
    {
        semaphore.acquire();

        hInst = hInstance; // Store instance handle in our global variable

        thd = std::thread([&]() -> int
            {
                thID = GetThreadId(GetCurrentThread());

                hWnd = CreateWindowExW(0, App::ClassName.data(), windowName, WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

                if (!hWnd)
                {
                    return FALSE;
                }

                ShowWindow(hWnd, SW_SHOWNORMAL);
                UpdateWindow(hWnd);

                MSG msg;

                // Create message pump for this thread.
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

                // Release the main thread.
                semaphore.release();

                while (GetMessageW(&msg, nullptr, 0, 0))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                return (int)msg.wParam;
            });

        semaphore.acquire();
        semaphore.release();

        return TRUE;
    }

    void CloseWindow()
    {
        if (hWnd == 0)
            return;

        DWORD a_thID = GetThreadId(reinterpret_cast<HANDLE>(thd.native_handle()));

        PostThreadMessageW(a_thID, WM_CLOSE, 0, 0);
        thd.join();

        {
            hInst = 0;
            hWnd = 0;
        }
    }
};

ATOM App::RegisterAppClass(HINSTANCE hInstance)
{
    // Register the window class.
    WNDCLASS wc = { };
    wc.lpfnWndProc = Window::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = ClassName.data();

    return RegisterClassW(&wc);
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    App app;

    if (app.RegisterAppClass(hInstance) == 0) {
        OutputDebugStringW(L"Failed to register application class.");
        return FALSE;
    }

    std::vector<std::unique_ptr<Window>> wArr;

    for (size_t i = 0; i < 3; ++i) {
        auto w = std::make_unique<Window>();

        if (!w->InitInstance(hInstance, L"Window 0"))
        {
            return FALSE;
        }

        wArr.push_back(std::move(w));
    }

    Sleep(3000);

    for (auto& w : wArr) {
        w->CloseWindow();
    }

    return 0;
}



