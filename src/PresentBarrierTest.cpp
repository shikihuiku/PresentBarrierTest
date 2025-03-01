#include <windows.h>
#include <wrl.h>

#include <string>
#include <sstream>
#include <array>
#include <vector>
#include <list>
#include <deque>
#include <thread>
#include <semaphore>
#include <mutex>
#include <algorithm>
#include <span>
#include <chrono>
#include <functional>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#include <dxgi1_6.h>
#include <d3d12.h>

#define NVAPI_ENABLED

#if defined(NVAPI_ENABLED)
#pragma warning(push)
#pragma warning(disable:4819)
#include "nvapi.h"
#pragma warning(pop)

#pragma comment(lib, "nvapi64.lib")
#endif

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

using Microsoft::WRL::ComPtr;

namespace {
#include "shaders/VSMain.h"
#include "shaders/PSMain.h"
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
    std::string ToUTF8(const std::wstring& src)
    {
        std::vector<char> mbs(src.length() * 2, '\0');
        size_t converted{};
        wcstombs_s(&converted, mbs.data(), mbs.size(), src.c_str(), src.length());
        assert(converted < mbs.size());

        return std::string(mbs.data());
    }
    std::wstring ToUTF16(const std::string& src)
    {
        std::vector<wchar_t> wcs(src.length() * 2, '\0');
        size_t converted{};
        mbstowcs_s(&converted, wcs.data(), wcs.size(), src.c_str(), src.length());
        assert(converted < wcs.size());

        return std::wstring(wcs.data());
    }
    std::string ToStr(const char* format, ...)
    {
        std::array<char, 1024> str;

        va_list args;
        va_start(args, format);

        vsprintf_s(str.data(), str.size(), format, args);
        va_end(args);

        return std::string(str.data());
    }
    std::wstring ToStr(const wchar_t* format, ...)
    {
        std::array<wchar_t, 1024> str;

        va_list args;
        va_start(args, format);

        vswprintf_s(str.data(), str.size(), format, args);
        va_end(args);

        return std::wstring(str.data());
    }

    struct LogBuffer final
    {
    private:
        std::mutex              mtx;
        std::list<std::string>  logTextList;
        uint32_t                idx{};
        static constexpr size_t maxLines{ 20 };

    public:
        void Addline(const std::string& line) {
            std::scoped_lock<std::mutex> l{ mtx };

            logTextList.push_back(line);
            if (logTextList.size() > maxLines)
                logTextList.pop_front();
            ++idx;
        }
        uint32_t Index() const
        {
            return idx;
        }
        void Lock()
        {
            mtx.lock();
        }
        const std::list<std::string>& LogTextList() const
        {
            return logTextList;
        }
        void Unlock()
        {
            mtx.unlock();
        }
    };
    std::weak_ptr<LogBuffer>   weak_logBuffer;

    void ImGui_AddLogText(uint32_t& currentLogIdx)
    {
        ImGui::Separator();
        ImGui::BeginChild("##scrolling", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing()));

        if (std::shared_ptr<LogBuffer> t = weak_logBuffer.lock()) {
            t->Lock();
            for (auto& s : t->LogTextList())
                ImGui::Text(s.c_str());
            if (currentLogIdx != t->Index()) {
                ImGui::SetScrollHereY(1.0f);
                currentLogIdx = t->Index();
            }
            t->Unlock();
        }
        ImGui::EndChild();
    }

    void Log(const wchar_t* format, ...)
    {
        std::array<wchar_t, 1024> str;
        va_list args;
        va_start(args, format);

        vswprintf_s(str.data(), str.size(), format, args);
        wprintf(str.data());
        OutputDebugStringW(str.data());

        if (std::shared_ptr<LogBuffer> t = weak_logBuffer.lock()) {
            t->Addline(ToUTF8(str.data()).c_str());
        }

        va_end(args);
    }
    void Log(const char* format, ...)
    {
        std::array<char, 1024> str;
        va_list args;
        va_start(args, format);

        vsprintf_s(str.data(), str.size(), format, args);
        printf(str.data());
        OutputDebugStringA(str.data());

        if (std::shared_ptr<LogBuffer> t = weak_logBuffer.lock()) {
            t->Addline(str.data());
        }

        va_end(args);
    }
};

enum class WindowMode
{
    windowed = 0,
    borderlessWindowed,
    fullSceen,
    numWindowMode
};

#ifdef NVAPI_ENABLED
enum class PresentBarrierMode
{
    join,
    leave,
};
#endif

class App final
{
public:
    class Context final
    {
    public:
        enum class Mode {
            control,
            test,
            exit
        } mode{ Mode::control };

        class Display {
        public:
            bool        selected{ false };
            WindowMode  windowMode{ WindowMode::windowed };
            uint32_t    adapterIdx{};
            uint32_t    outputIdx{};
            std::string description;

            float       threadWaitMs{};

#ifdef NVAPI_ENABLED
            NV_PRESENT_BARRIER_FRAME_STATISTICS nvapi_PBStats{};
            PresentBarrierMode                  nvapi_PresentBarrierMode{ PresentBarrierMode::leave };
#endif
        };

        std::vector<Display> displays;
        uint64_t globalCounter{};
    } ctx;

    class Adapter final
    {
    public:
        class Output final {
        public:
            ComPtr<IDXGIOutput6>  dxgiOut;
            DXGI_OUTPUT_DESC      desc;
            DXGI_MODE_DESC        currentModeDesc;
        };
        ComPtr<IDXGIAdapter4>       adapter;
        DXGI_ADAPTER_DESC           desc{};
        ComPtr<ID3D12Device>        device;
        ComPtr<ID3D12CommandQueue>  queue;
        std::vector<Output>         outputs;

    public:
        bool Init(ComPtr<IDXGIAdapter>& a)
        {
            a->GetDesc(&desc);

            if (desc.VendorId != 0x10DE) {
                Log(L"Found a non-NVIDIA adapter device-id: %d vendor-id: %d description:%s\n", desc.DeviceId, desc.VendorId, desc.Description);
                desc = {};
                return false;
            }
            // Found NVIDIA Adapter.

            if (FAILED(a.As(&adapter))) {
                Log(L"Faild to get IDXGIAdapter4 interface.\n");
                return false;
            }

            Log(L"Found NVIDIA Adapter device-id: %d vendor-id: %d description:%s\n", desc.DeviceId, desc.VendorId, desc.Description);

#ifdef DX12_ENABLE_DEBUG_LAYER
            {
                ComPtr<ID3D12Debug> d3d12Debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d3d12Debug))))
                    d3d12Debug->EnableDebugLayer();
            }
#endif
            {
                D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_2;
                if (D3D12CreateDevice(adapter.Get(), featureLevel, IID_PPV_ARGS(&device)) != S_OK)
                    return false;
            }

#ifdef DX12_ENABLE_DEBUG_LAYER
            {
                ComPtr<ID3D12InfoQueue> infoQueue;
                device->QueryInterface(IID_PPV_ARGS(&infoQueue));
                if (infoQueue) {
                    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
                    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
                    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
                }
            }
#endif
            {
                D3D12_COMMAND_QUEUE_DESC desc{};
                desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                desc.NodeMask = 1;
                if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
                    return false;
            }

            ComPtr<IDXGIOutput> dxgiOut;
            for (UINT i = 0; adapter->EnumOutputs(i, &dxgiOut) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_OUTPUT_DESC desc;
                dxgiOut->GetDesc(&desc);

                Log(L"Output HMONITOR: %p\n", desc.Monitor);

                ComPtr<IDXGIOutput6> dxgiOut6;
                if (FAILED(dxgiOut.As(&dxgiOut6))) {
                    Log(L"Faild to get IDXGIOutput6 interface.\n");
                    return false;
                }

                DXGI_MODE_DESC currentMode{};
                if (!GetClosestDisplayModeToCurrent(desc.Monitor, &currentMode)) {
                    Log(L"Faild to find the closest curernt display mode.\n");
                    return false;
                }
                if (FAILED(dxgiOut6->FindClosestMatchingMode(&currentMode, &currentMode, NULL))) {
                    Log(L"Faild to find the closest curernt display mode..\n");
                    return false;
                }

                outputs.push_back({ dxgiOut6 , desc, currentMode });
            }

            return true;
        }

        void Terminate()
        {
            outputs.clear();

            queue.Reset();
            device.Reset();
            adapter.Reset();
        }

        bool GetClosestDisplayModeToCurrent(HMONITOR hMon, DXGI_MODE_DESC* outCurrent)
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

            return true;
        }
    };
    
public:
    std::mutex                              mtx;
    ComPtr<IDXGIFactory7>                   dxgiFactory;
    std::vector<std::unique_ptr<Adapter>>   adapters;
    std::shared_ptr<LogBuffer>              logBuffer;

#ifdef NVAPI_ENABLED
    bool            nvapi_Initialized{ false };
#endif

public:
    bool Init(HINSTANCE hInstance)
    {
        std::scoped_lock<std::mutex> l{ mtx };

        logBuffer = std::make_shared<LogBuffer>();
        weak_logBuffer = logBuffer;

#ifdef NVAPI_ENABLED
        if (NvAPI_Initialize() != NVAPI_OK) {
            Log("Failed to initialize NvAPI()\n");
            nvapi_Initialized = false;
        }
        else {
            nvapi_Initialized = true;
        }
#endif

        {
            UINT flags{};
#if defined(_DEBUG)
            flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
            HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&dxgiFactory));
            if (FAILED(hr)) {
                Log(L"Failed to create a DXGIFactory interface.\n");
                return false;
            }
        }

        ComPtr<IDXGIAdapter> adapter;
        for (UINT i = 0; dxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            auto a = std::make_unique<Adapter>();
            if (!a->Init(adapter)) {
                continue;
            }
            adapters.push_back(std::move(a));
        }

        return true;
    };

    bool Terminate()
    {
        std::scoped_lock<std::mutex> l{ mtx };

        for (auto& a : adapters) {
            a->Terminate();
        }
        adapters.clear();

        dxgiFactory.Reset();

#ifdef NVAPI_ENABLED
        if (nvapi_Initialized) {
            NvAPI_Unload();
            nvapi_Initialized = false;
        }
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
        {
            ComPtr<IDXGIDebug1> pDebug;
            if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
            {
                pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
            }
        }
#endif

        logBuffer.reset();

        return true;
    }
};

class D3DContext_Base
{
public:
    enum class WindowModeTransitionStatus {
        inProgress,
        completed,
        error
    };

protected:
    static constexpr size_t NUM_BACK_BUFFERS{ 2 };
    static constexpr size_t DESC_HEAP_SIZE{ 256 };

    WindowMode currentWindowMode{ WindowMode::windowed };
    WindowMode requestedWindowMode{ WindowMode::windowed };
    WindowMode setWindowMode{ WindowMode::windowed };
    bool       internalWindowModeChange{ false };

    std::shared_ptr<App>    app;
    uint32_t                appListIdx{};

    ComPtr<IDXGIFactory7>   factory;
    ComPtr<ID3D12Device>    dev;
    ComPtr<IDXGIOutput6>    output;
    DXGI_OUTPUT_DESC        outputDesc{};
    ComPtr<ID3D12CommandQueue>          queue;
    ComPtr<ID3D12GraphicsCommandList>   cList;

    std::array<ComPtr<ID3D12DescriptorHeap>, NUM_BACK_BUFFERS>   rtvDescHeap;
    std::array<ComPtr<ID3D12DescriptorHeap>, NUM_BACK_BUFFERS>   descHeap;
    std::array<ComPtr<ID3D12CommandAllocator>, NUM_BACK_BUFFERS> cAllocator;

    ComPtr<ID3D12Fence> fence;
    HANDLE      fenceEvent{};
    uint64_t    fenceLastSignaledValue{};

    ComPtr<IDXGISwapChain3> swapChain;
    std::array<ComPtr<ID3D12Resource>, NUM_BACK_BUFFERS>  backbuffers;
    bool   swapChainOccluded{ false };
    HANDLE swapChainWaitableObject{};
    std::array<uint32_t, 2> currentSwapchainSize{ (uint32_t)-1, (uint32_t)-1};
    RECT                    storedWindowPosition{};

    class ShaderAssets {
    public:
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12Resource>      uploadHeap;
        static constexpr uint64_t   uploadHeapSize{ 65536 * 32 };
        std::list<std::tuple<uintptr_t, uintptr_t, size_t>> mappedHeapChunks;

    public:
        bool Init(ComPtr<ID3D12Device>& dev)
        {
            {
                D3D12_ROOT_SIGNATURE_DESC desc{ 0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
                ComPtr<ID3DBlob> sig;
                ComPtr<ID3DBlob> err;
                if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
                    Log("Failed to serialize a root signature.\n");
                }
                if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rootSig)))) {
                    Log("Failed to create a root signature.\n");
                }
            }
            {
                std::array<D3D12_INPUT_ELEMENT_DESC, 2> ieDesc{ {
                    { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                    { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
                } };

                D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
                psoDesc.InputLayout = { ieDesc.data(), (UINT)ieDesc.size() };
                psoDesc.pRootSignature = rootSig.Get();
                psoDesc.VS = { VSMain_cso, VSMain_cso_len };
                psoDesc.PS = { PSMain_cso, PSMain_cso_len };
                psoDesc.RasterizerState = {
                    D3D12_FILL_MODE_SOLID,
                    D3D12_CULL_MODE_NONE,
                    FALSE,
                    0,
                    0.0f,
                    0.0f,
                    FALSE,
                    FALSE,
                    FALSE,
                    0,
                    D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF
                };
                psoDesc.BlendState = { FALSE, FALSE, {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_SET, (UINT8)0x000F } };
                psoDesc.DepthStencilState.DepthEnable = FALSE;
                psoDesc.DepthStencilState.StencilEnable = FALSE;
                psoDesc.SampleMask = UINT_MAX;
                psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                psoDesc.NumRenderTargets = 1;
                psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                psoDesc.SampleDesc.Count = 1;
                if (FAILED(dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)))) {
                    Log("Failed to create a PSO.\n");
                    return false;
                }
            }

            // Create GPU Upload heap for a vertex buffer.
            {
                constexpr size_t nbChunks{ 32 };
                constexpr size_t chunkSize{ 65536 };
                constexpr uint64_t bufferSize{ chunkSize * nbChunks }; // 32 x 64KB
                D3D12_HEAP_PROPERTIES       heapProp{
                    D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                    D3D12_MEMORY_POOL_UNKNOWN,
                    1, 1 };
                D3D12_RESOURCE_DESC         resDesc{
                    D3D12_RESOURCE_DIMENSION_BUFFER,
                    D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                    bufferSize, 1, 1,
                    1,
                    DXGI_FORMAT_UNKNOWN,
                    {1, 0},
                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                    D3D12_RESOURCE_FLAG_NONE
                };

                if (FAILED(dev->CreateCommittedResource(
                    &heapProp, D3D12_HEAP_FLAG_NONE,
                    &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&uploadHeap)))) {
                    Log("Failed to create a upload heap.\n");
                    return false;
                }

                // permanently mapped until it gets destructed.
                D3D12_GPU_VIRTUAL_ADDRESS gpuPtr{ uploadHeap->GetGPUVirtualAddress() };
                uintptr_t mapped{};
                D3D12_RANGE readRange{};
                if (FAILED(uploadHeap->Map(0, &readRange, (void**)&mapped))) {
                    Log("Failed to map buffer.\n");
                    uploadHeap.Reset();
                    return false;
                }
                for (size_t i = 0; i < nbChunks; ++i) {
                    mappedHeapChunks.push_back({ mapped + chunkSize * i, gpuPtr + chunkSize * i, chunkSize });
                }
            }
            return true;
        };

        bool Terminate()
        {
            if (uploadHeap) {
                uploadHeap->Unmap(0, nullptr);
                uploadHeap.Reset();
            }
            rootSig.Reset();
            pso.Reset();

            return true;
        }

        std::tuple<uintptr_t, uintptr_t, size_t> GetUploadChunk()
        {
            auto f = mappedHeapChunks.front();
            mappedHeapChunks.pop_front();
            mappedHeapChunks.push_back(f);

            return f;
        }
    };
    std::unique_ptr<ShaderAssets>       shaderAssets;

#ifdef NVAPI_ENABLED
    bool    nvapi_PresentBarrierIsSupported{ false };
    bool    nvapi_PresentBarrierHasJoined{ false };
    bool    nvapi_PresentBarrierClientHandleCreated{ false };
    NvPresentBarrierClientHandle nvapi_PresentBarrierClientHandle{};
    ComPtr<ID3D12Fence> presentBarrierFence;
#endif

public:
    void SetApp(std::shared_ptr<App> inApp, uint32_t listIdx)
    {
        std::swap(app, inApp);
        appListIdx = listIdx;
        {
            std::scoped_lock<std::mutex> l{ app->mtx };

            factory = app->dxgiFactory;

            auto& display = app->ctx.displays.at(appListIdx);
            auto& a{ app->adapters.at(display.adapterIdx) };
            auto& o{ a->outputs.at(display.outputIdx) };

            dev = a->device;
            queue = a->queue;
            output = o.dxgiOut;
            outputDesc = o.desc;
        }
    }

    bool ShowWindowOnTheAssociatedOutput(HWND hWnd)
    {
        SetWindowPos(hWnd, nullptr,
            outputDesc.DesktopCoordinates.left,
            outputDesc.DesktopCoordinates.top,
            (outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left) / 2,
            (outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top) / 2,
            SWP_NOZORDER);
        ShowWindow(hWnd, SW_SHOWNORMAL);
        UpdateWindow(hWnd);

        WINDOWPLACEMENT pls{};
        GetWindowPlacement(hWnd, &pls);
        storedWindowPosition = pls.rcNormalPosition;

        return true;
    }

    bool CreateDeviceResources()
    {
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = 1;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;
            for (size_t i = 0; i < NUM_BACK_BUFFERS; ++i) {
                if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvDescHeap[i]))))
                    return false;
            }
        }

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = DESC_HEAP_SIZE;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            for (size_t i = 0; i < NUM_BACK_BUFFERS; ++i) {
                if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descHeap[i]))))
                    return false;
            }
        }

        for (size_t i = 0; i < NUM_BACK_BUFFERS; ++i) {
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cAllocator[i]))))
                return false;
        }

        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cAllocator[0].Get(), nullptr, IID_PPV_ARGS(&cList))))
            return false;

        if (FAILED(cList->Close()))
            return false;

        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        fenceEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (fenceEvent == nullptr)
            return false;

#ifdef NVAPI_ENABLED
        {
            std::scoped_lock<std::mutex> l{ app->mtx };

            if (app->nvapi_Initialized) {
                bool sts{ false };
                if (NvAPI_D3D12_QueryPresentBarrierSupport(dev.Get(), &sts) != NVAPI_OK) {
                    Log("Failed to call QueryPresentBarrierSupport\n");
                    nvapi_PresentBarrierIsSupported = false;
                }
                else {
                    nvapi_PresentBarrierIsSupported = sts;
                }
                Log("PresentBarrierIsSupported status : %s\n", nvapi_PresentBarrierIsSupported ? "TRUE" : "FALSE");
            }
        }

        if (nvapi_PresentBarrierIsSupported) {
            if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentBarrierFence))))
                return false;
        }
#endif

        return true;
    }

    DWORD WaitForFence(bool leavePresentBarrier = true, uint64_t behind = 0, DWORD waitMs = INFINITE)
    {
        if (leavePresentBarrier) {
#ifdef NVAPI_ENABLED
            // Leave from the present barrier before taking a GPU CPU sync.
            if (nvapi_PresentBarrierHasJoined) {
                std::scoped_lock<std::mutex> l{ app->mtx };
                if (NvAPI_LeavePresentBarrier(nvapi_PresentBarrierClientHandle) != NVAPI_OK) {
                    Log("Failed to leave from the Present Barrier.\n");
                    return WAIT_FAILED;
                }
                nvapi_PresentBarrierHasJoined = false;
            }
#endif
        }

        if (fenceLastSignaledValue == 0 || fenceLastSignaledValue <= behind)
            return WAIT_OBJECT_0;
        uint64_t targetValue = fenceLastSignaledValue - behind;
        if (fence->GetCompletedValue() >= targetValue)
            return WAIT_OBJECT_0;

        if (!ResetEvent(fenceEvent)) {
            Log("Failed to reset event.\n");
            return WAIT_FAILED;
        };
        if (FAILED(fence->SetEventOnCompletion(targetValue, fenceEvent))) {
            Log("Failed to SetEventOnCompletion.\n");
            return WAIT_FAILED;
        };

        return WaitForSingleObject(fenceEvent, waitMs);
    }

    bool CreateSwapChain(HWND hWnd, uint32_t width, uint32_t height)
    {
        // take GPU-CPU sync
        if (WaitForFence() != WAIT_OBJECT_0)
            return false;

        // release all backbuffers.
        for (auto& b : backbuffers) {
            b.Reset();
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        bool resize = false;
        if (swapChain) {
            swapChain->GetDesc(&desc);
            if (hWnd == desc.OutputWindow) {
                resize = true;
            }
        }

        if (resize) {
            Log(L"Resizing swapchain: %d x %d -> %d x %d\n", desc.BufferDesc.Width, desc.BufferDesc.Height, width, height);
        }
        else {
            Log(L"Create swapchain: %d x %d -> %d x %d\n", desc.BufferDesc.Width, desc.BufferDesc.Height, width, height);
        }

        if (resize) {
            if (FAILED(swapChain->ResizeBuffers(
                NUM_BACK_BUFFERS,
                width, height,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))) {
                Log("Failed to resize a swap chain.\n");
                return false;
            }
        }
        else {
            // Create new swapchain.
#ifdef NVAPI_ENABLED
            // Destroy PB client if exists.
            if (nvapi_PresentBarrierClientHandleCreated) {
                std::scoped_lock<std::mutex> l{ app->mtx };
                if (NvAPI_DestroyPresentBarrierClient(nvapi_PresentBarrierClientHandle) != NVAPI_OK) {
                    Log("Failed to destroy Present Barrier Client.\n");
                }
                nvapi_PresentBarrierClientHandle = {};
                nvapi_PresentBarrierClientHandleCreated = false;;
            }
#endif
            swapChain.Reset();

            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.BufferCount = NUM_BACK_BUFFERS;
            sd.Width = width;
            sd.Height = height;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.Stereo = FALSE;

            ComPtr<IDXGISwapChain1> sc1;
            if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hWnd, &sd, nullptr, nullptr, &sc1)))
                return false;

            if (FAILED(sc1->QueryInterface(IID_PPV_ARGS(&swapChain)))) {
                return false;
            }

#ifdef NVAPI_ENABLED
            // Create Present Barrier client.
            if (nvapi_PresentBarrierIsSupported) {
                std::scoped_lock<std::mutex> l{ app->mtx };

                if (NvAPI_D3D12_CreatePresentBarrierClient(dev.Get(), swapChain.Get(), &nvapi_PresentBarrierClientHandle)) {
                    Log("Failed to create Present Barrier Client.\n");
                    nvapi_PresentBarrierClientHandle = {};
                    nvapi_PresentBarrierClientHandleCreated = false;
                }
                else {
                    nvapi_PresentBarrierClientHandleCreated = true;
                }
            }
#endif
        }
        if (FAILED(factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN))) {
            return false;
        }

        if (FAILED(swapChain->SetMaximumFrameLatency(NUM_BACK_BUFFERS))) {
            return false;
        }

        if (swapChainWaitableObject != nullptr) {
            CloseHandle(swapChainWaitableObject);
            swapChainWaitableObject = nullptr;
        }
        swapChainWaitableObject = swapChain->GetFrameLatencyWaitableObject();
        if (swapChainWaitableObject == nullptr) {
            return false;
        }

        swapChainOccluded = false;
        for (size_t i = 0; i < NUM_BACK_BUFFERS; ++i) {
            swapChain->GetBuffer((UINT)i, IID_PPV_ARGS(&backbuffers[i]));
            dev->CreateRenderTargetView(backbuffers[i].Get(), nullptr, rtvDescHeap[i]->GetCPUDescriptorHandleForHeapStart());
        }

#ifdef NVAPI_ENABLED
        // Register backbuffers to NVAPI.
        if (nvapi_PresentBarrierClientHandleCreated) {
            std::scoped_lock<std::mutex> l{ app->mtx };

            std::array<ID3D12Resource*, NUM_BACK_BUFFERS> rawBackBuffers;
            std::transform(backbuffers.begin(), backbuffers.end(), rawBackBuffers.begin(), [](auto& a) { return a.Get(); });

            // Register the new back buffer resources
            if (NvAPI_D3D12_RegisterPresentBarrierResources(nvapi_PresentBarrierClientHandle,
                presentBarrierFence.Get(),
                rawBackBuffers.data(), (uint32_t)rawBackBuffers.size()) == NVAPI_OK) {
                Log("Failed to register present barrier resources.\n");
            }
        }
#endif

        currentSwapchainSize = { width, height };

        return true;
    }

    bool FullScreenStateTransition(BOOL fsState, IDXGIOutput* output = nullptr)
    {
        for (;;) {
            BOOL currentFs{};

            if (FAILED(swapChain->GetFullscreenState(&currentFs, nullptr))) {
                Log("Calling GetFullScreenState - Failed.\n");
                return false;
            }
            if (currentFs == fsState)
                return true;
            auto hr = swapChain->SetFullscreenState(fsState, output);
            if (FAILED(hr)) {
                Log("Calling SetFullScreenState - Failed.\n");
                break;
            }
            if (hr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) {
                Log("Calling SetFullScreenState - DXGI_STATUS_MODE_CHANGE_IN_PROGRESS returned. Retrying..\n");
                Sleep(10);
                continue;
            }
            break;
        }
        return true;
    }

    WindowModeTransitionStatus WindowModeTransition(HWND hWnd, const std::tuple<LONG_PTR, LONG_PTR>& defaultWindowStyle)
    {
        // Window mode change and swap chain modifications only happens here.
        // This thread is called from the Windows message pump thread, not the render thread.
        // Before entering this function, render thread need to be joined.

        RECT rc{};
        if (!GetClientRect(hWnd, &rc)) {
            Log("Failed to get client rect.\n");
            return WindowModeTransitionStatus::error;
        }

        // No window mode trasition.
        if (currentWindowMode == requestedWindowMode) {
            // Check the fullscreen status while in fullscreen mode.
            if (swapChain) {
                if (currentWindowMode == WindowMode::fullSceen) {
                    BOOL sts{ FALSE };
                    if (FAILED(swapChain->GetFullscreenState(&sts, nullptr))) {
                        Log("Failed to get fullscreen state.\n");
                        return WindowModeTransitionStatus::error;
                    }
                    if (!sts) {
                        // Full screen has been finished for somereason. (alt tabbing or something..)
                        requestedWindowMode = WindowMode::windowed;
                        internalWindowModeChange = true; // Tell the app it's cahnged from the window.
                    }
                }
            }

            // Check the client rect update.
            if (currentSwapchainSize[0] != rc.right || currentSwapchainSize[1] != rc.bottom) {
                if (! CreateSwapChain(hWnd, rc.right, rc.bottom)) {
                    Log("Failed to create swap chain.\n");
                    return WindowModeTransitionStatus::error;
                };
            }
        }
        if (currentWindowMode == requestedWindowMode)
            return WindowModeTransitionStatus::completed;

        // Transition is happening.
        // Take GPU <-> CPU sync.
        if (WaitForFence() != WAIT_OBJECT_0) {
            return WindowModeTransitionStatus::error;
        }

        BOOL currentFsState = currentWindowMode == WindowMode::fullSceen ? TRUE : FALSE;
        BOOL requestedFsState = requestedWindowMode == WindowMode::fullSceen ? TRUE : FALSE;
        BOOL setFsState = setWindowMode == WindowMode::fullSceen ? TRUE : FALSE;

        if (requestedWindowMode != setWindowMode) {
            Log("Changing Window Mode - Start.\n");

            if (currentWindowMode == WindowMode::windowed) {
                WINDOWPLACEMENT pls{};
                GetWindowPlacement(hWnd, &pls);
                storedWindowPosition = pls.rcNormalPosition;
            }
            if (requestedFsState != setFsState) {
                Log("Changing full screen state to %s\n", requestedFsState ? "TRUE" : "FALSE");
                if (!FullScreenStateTransition(requestedFsState, requestedFsState ? output.Get() : nullptr)) {
                    Log(L"Failed to set FullScreenSteate.\n");
                    return WindowModeTransitionStatus::error;
                }
            }

            // Window Stlye changes happen in window message pump. 
            if (requestedWindowMode == WindowMode::borderlessWindowed) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, (LONG_PTR)WS_VISIBLE);
                ShowWindow(hWnd, SW_SHOWMAXIMIZED);
                UpdateWindow(hWnd);
            }
            else if (requestedWindowMode == WindowMode::windowed) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, std::get<0>(defaultWindowStyle));
                SetWindowLongPtrW(hWnd, GWL_EXSTYLE, std::get<1>(defaultWindowStyle));

                const auto& rc{ storedWindowPosition };
                SetWindowPos(hWnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
                ShowWindow(hWnd, SW_SHOWNORMAL);
                UpdateWindow(hWnd);
            }

            // Get back to Windows message pump once to process messages.
            setWindowMode = requestedWindowMode;
            return WindowModeTransitionStatus::inProgress;
        }
        else if (requestedWindowMode != currentWindowMode) {
            // FS <-> Windowed transition always requires resizing the swap chain.
            // Window style change always changes client rect size.
            Log("Changing Window Mode - update swap chain.\n");
            if (!CreateSwapChain(hWnd, rc.right, rc.bottom)) {
                Log(L"Failed to resize/create swap chain after changing window mode.\n");
                return WindowModeTransitionStatus::error;
            }

            Log("Changing Window Mode - Calling an empty Present.\n");
            {
                HRESULT hr = swapChain->Present(1, 0);
                swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
                if (FAILED(hr))
                    return WindowModeTransitionStatus::error;
            }
            {
                if (FAILED(queue->Signal(fence.Get(), ++fenceLastSignaledValue)))
                    return WindowModeTransitionStatus::error;

                if (WaitForFence() != WAIT_OBJECT_0)
                    return WindowModeTransitionStatus::error;
            }

            currentWindowMode = requestedWindowMode;
            Log("Changing Window Mode - Finished.\n");
        }

        return WindowModeTransitionStatus::completed;
    }

    virtual void Render(HWND, ComPtr<ID3D12GraphicsCommandList>&) = 0;

    // This will be launched in present worker thread.
    void Present(HWND hWnd, std::atomic<bool>& returnStatus)
    {
        returnStatus.store(false);

        if (!dev)
            return;

        // Wait for the rendering completion for the current backbuffer index.
        {
            uint64_t cmpValue = fence->GetCompletedValue();
            if (fenceLastSignaledValue - cmpValue >= NUM_BACK_BUFFERS) {
                bool leavingPresentBarrier = false;
                for (;;) {
                    // Wait up to 2sec to detect present timeout.
                    auto sts = WaitForFence(leavingPresentBarrier, NUM_BACK_BUFFERS - 1, 2000);
                    if (sts == WAIT_OBJECT_0) {
                        break;
                    }
                    if (sts != WAIT_TIMEOUT) {
                        Log(L"An error detected while waiting for a fence.\n");
                        return;
                    }
                    Log(L"Present lock detected. Waited for more than 2 seconds.");
                    leavingPresentBarrier = true;
                }
            }
        }

        // Updating occlusion status.
        if (swapChainOccluded && swapChain->Present(0, DXGI_PRESENT_TEST) != DXGI_STATUS_OCCLUDED)
        {
            swapChainOccluded = false;
        }

        UINT backbufferIdx = swapChain->GetCurrentBackBufferIndex();
        cAllocator[backbufferIdx]->Reset();
        cList->Reset(cAllocator[backbufferIdx].Get(), nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = backbuffers[backbufferIdx].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cList->ResourceBarrier(1, &barrier);

        {
            constexpr std::array<float[4], 3> clearColors{ { { 0.4f, 0.3f, 0.3f, 1.0f }, { 0.3f, 0.4f, 0.3f, 1.0f }, { 0.3f, 0.3f, 0.4f, 1.0f } } };
            static_assert(clearColors.size() <= (size_t)WindowMode::numWindowMode);
            cList->ClearRenderTargetView(rtvDescHeap[backbufferIdx]->GetCPUDescriptorHandleForHeapStart(), clearColors[(size_t)currentWindowMode], 0, nullptr);
        }
        {
            auto rtvs{ rtvDescHeap[backbufferIdx]->GetCPUDescriptorHandleForHeapStart() };
            cList->OMSetRenderTargets(1, &rtvs, FALSE, nullptr);
        }

        {
            RECT rc{0, 0, (LONG)currentSwapchainSize[0], (LONG)currentSwapchainSize[1]};
            D3D12_VIEWPORT vp{0, 0, (float)rc.right, (float)rc.bottom, 0, 1.f};
            cList->RSSetViewports(1, &vp);
            cList->RSSetScissorRects(1, &rc);
        }

        Render(hWnd, cList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cList->ResourceBarrier(1, &barrier);
        cList->Close();

        {
            ID3D12CommandList* cListList[]{ cList.Get() };
            queue->ExecuteCommandLists(1, cListList);
        }

        {
            HRESULT hr = swapChain->Present(1, 0);
#if 0
            {
                // emurate present lock.
                static uint32_t i;
                if (++i % 512 == 511) {
                    Sleep(5000);
                }

            }
#endif
            swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
            if (FAILED(hr)) {
                Log("Present call failed with: %d.\n", hr);
                return;
            }
        }

        if (FAILED(queue->Signal(fence.Get(), ++fenceLastSignaledValue))) {
            Log("Setting a signal after Present call failed.\n");
            return;
        }

        returnStatus.store(true);
        return;
    }

    bool Terminate()
    {
        if (WaitForFence() != WAIT_OBJECT_0)
            return false;

        // Revert to windowed before releasing the swapchain.
        if (swapChain) {
            if (!FullScreenStateTransition(FALSE))
                return false;
        }

        if (shaderAssets) {
            bool sts = shaderAssets->Terminate();
            assert(sts);
            shaderAssets.reset();
        }

        // Descheaps
        for (auto& r : rtvDescHeap) {
            r.Reset();
        }
        for (auto& d : descHeap) {
            d.Reset();
        }

        // commands
        cList.Reset();
        for (auto& a : cAllocator) {
            a.Reset();
        }
        queue.Reset();

        // swapchain's backbuffers.
        for (auto& b : backbuffers) {
            b.Reset();
        }
        if (swapChainWaitableObject != nullptr) {
            CloseHandle(swapChainWaitableObject);
            swapChainWaitableObject = nullptr;
        }
        swapChainOccluded = false;

#ifdef NVAPI_ENABLED
        if (nvapi_PresentBarrierClientHandleCreated) {
            std::scoped_lock<std::mutex> l{ app->mtx };
            if (NvAPI_DestroyPresentBarrierClient(nvapi_PresentBarrierClientHandle) != NVAPI_OK) {
                Log("Failed to destroy Present Barrier Client.\n");
            }
            nvapi_PresentBarrierClientHandle = {};
            nvapi_PresentBarrierClientHandleCreated = false;;
        }
        presentBarrierFence.Reset();
#endif

        swapChain.Reset();

        // fence.
        fence.Reset();
        if (fenceEvent != nullptr) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        fenceLastSignaledValue = {};

        output.Reset();

        dev.Reset();

        factory.Reset();

        return true;
    }
};

class D3DContext_ImGuiBase : public D3DContext_Base
{
protected:
    ComPtr<ID3D12DescriptorHeap> imDescHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE descHeapCpuH{};
    D3D12_GPU_DESCRIPTOR_HANDLE descHeapGpuH{};
    UINT                        descHeapIncSize{};
    std::deque<std::tuple<int, int>> descHeapFreeIndices;
    bool imInitialized{ false };

public:
    bool Init_ImGui(HWND hWnd)
    {
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
#if 0
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
#endif
        // Setup Dear ImGui style
        ImGui::StyleColorsDark();

        // Setup Platform/Renderer backends
        ImGui_ImplWin32_Init(hWnd);

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = DESC_HEAP_SIZE;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&imDescHeap)))) {
               return false;
            }

            descHeapFreeIndices.clear();
            descHeapFreeIndices.push_front({ 0, desc.NumDescriptors });
            descHeapCpuH = imDescHeap->GetCPUDescriptorHandleForHeapStart();
            descHeapGpuH = imDescHeap->GetGPUDescriptorHandleForHeapStart();
            descHeapIncSize = dev->GetDescriptorHandleIncrementSize(desc.Type);
        }

        ImGui_ImplDX12_InitInfo info{};
        info.Device = dev.Get();
        info.CommandQueue = queue.Get();
        info.NumFramesInFlight = NUM_BACK_BUFFERS;
        info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;

        info.SrvDescriptorHeap = imDescHeap.Get();
        info.UserData = (void *)this;
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) -> void
            {
                D3DContext_ImGuiBase* thisPtr = reinterpret_cast<D3DContext_ImGuiBase*>(info->UserData);

                auto [idx, num] = thisPtr->descHeapFreeIndices.front();
                if (num == 1) {
                    thisPtr->descHeapFreeIndices.pop_front();
                }
                else {
                    thisPtr->descHeapFreeIndices.front() = { idx + 1, num - 1 };
                }
                outCpu->ptr = thisPtr->descHeapCpuH.ptr + (thisPtr->descHeapIncSize * idx);
                outGpu->ptr = thisPtr->descHeapGpuH.ptr + (thisPtr->descHeapIncSize * idx);
            }; 
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *info, D3D12_CPU_DESCRIPTOR_HANDLE inCpu, D3D12_GPU_DESCRIPTOR_HANDLE inGpu) -> void
            {
                D3DContext_ImGuiBase* thisPtr = reinterpret_cast<D3DContext_ImGuiBase*>(info->UserData);

                int cpuIdx = (int)((inCpu.ptr - thisPtr->descHeapCpuH.ptr) / thisPtr->descHeapIncSize);
                int gpuIdx = (int)((inGpu.ptr - thisPtr->descHeapGpuH.ptr) / thisPtr->descHeapIncSize);
                assert(cpuIdx == gpuIdx);

                bool processed = false;
                {
                    auto [idx, num] = thisPtr->descHeapFreeIndices.back();
                    if (idx + num == cpuIdx) {
                        thisPtr->descHeapFreeIndices.back() = { idx, num + 1 };
                        processed = true;
                    }
                }
                {
                    auto [idx, num] = thisPtr->descHeapFreeIndices.front();
                    if (idx == cpuIdx + 1) {
                        thisPtr->descHeapFreeIndices.front() = { cpuIdx, num + 1 };
                        processed = true;
                    }
                }
                if (!processed) {
                    thisPtr->descHeapFreeIndices.push_back({ cpuIdx, 1 });
                }
            };

        if (!ImGui_ImplDX12_Init(&info)) {
            Log(L"Failed to initialize ImGui.\n");
            return false;
        }
        imInitialized = true;
        return true;
    }

    bool Terminate_ImGui()
    {
        if (!imInitialized)
            return true;

        if (WaitForFence() != WAIT_OBJECT_0)
            return false;

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        imDescHeap.Reset();
        descHeapCpuH = {};
        descHeapGpuH = {};
        descHeapIncSize = {};
        descHeapFreeIndices.clear();

        imInitialized = false;

        return true;
    }

    bool PeekWindowMessage(HWND arg_hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (!imInitialized)
            return false;

        if (ImGui_ImplWin32_WndProcHandler(arg_hWnd, message, wParam, lParam)) {
            return true;
        }

        return false;
    }
};

class Window_Base
{
protected:
    std::thread thd{};
    enum class ThreadState : uint32_t {
        Initializing,
        Running,
        Terminated,
    };
    std::atomic<ThreadState>    thdState{ ThreadState::Initializing };

    virtual const std::wstring_view& WindowClassName() = 0;
    virtual ATOM& WindowClass() = 0;
    virtual D3DContext_ImGuiBase *GetD3DContext_ImGuiBase() = 0;

public:
    class PeekMessageContainer final
    {
    public:
        using PeekMessageFunc = std::function<void(HWND, UINT, WPARAM, LPARAM)>;
        using PeekMessageHandle = uint32_t;
        static constexpr PeekMessageHandle InvalidHandle{ 0xFFFFFFFFu };

    private:
        PeekMessageHandle lastHandle{ 0 };
        std::list<std::tuple<PeekMessageHandle, PeekMessageFunc>>  funcs;

    public:
        PeekMessageHandle Register(const std::function<void(HWND, UINT, WPARAM, LPARAM)>& f)
        {
            funcs.push_back({ lastHandle, f });
            return lastHandle++;
        }

        bool Unregister(const PeekMessageHandle h)
        {
            for (auto itr = funcs.begin(); itr != funcs.end(); ++itr) {
                if (std::get<0>(*itr) == h) {
                    funcs.erase(itr);
                    return true;
                }
            }
            return false;
        }

        void Call(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            for (auto& [handle, func] : funcs) {
                func(h, m, w, l);
            }
        }
    };

public:
    bool RegisterWindowClass(HINSTANCE hInst)
    {
        if (WindowClass() == 0) {
            // Register the window class.
            WNDCLASS wc = { };
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInst;
            wc.lpszClassName = WindowClassName().data();

            WindowClass() = RegisterClassW(&wc);
        }

        if (WindowClass() == 0) {
            Log(L"Failed to register application class.");
            return false;
        }
        return true;
    }

    static LRESULT __stdcall WndProc(HWND arg_hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        PeekMessageContainer *container = reinterpret_cast<PeekMessageContainer *>(GetWindowLongPtr(arg_hWnd, GWLP_USERDATA));
        if (container != nullptr) {
            container->Call(arg_hWnd, message, wParam, lParam);
        }

        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(arg_hWnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT*)lParam)->lpCreateParams);
        }

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

    bool Init(HINSTANCE hInst, std::shared_ptr<App>& inApp, const uint32_t listIdx, const bool withImGui)
    {
        if (!RegisterWindowClass(hInst)) {
            Log(L"Failed to regiser window class.");
            return false;
        }

        class ScopeGuard final {
            std::function<void()>   f;
        public:
            ScopeGuard(std::function<void()> inF) {
                f = inF;
            }
            ~ScopeGuard() {
                f();
            }
        };

        // Window thread.
        thd = std::thread([this, hInst, inApp, listIdx, withImGui]() -> void {
            // Make sure to change the thread state to Terminate whenever exitting from this scope.
            ScopeGuard threadGuard([this] { thdState.store(ThreadState::Terminated); });
            std::wstring wname;
            {
                std::scoped_lock<std::mutex> l{ inApp->mtx };
                wname = ToUTF16(inApp->ctx.displays.at(listIdx).description);
            }
            Log(L"Thread:%s - Start\n", wname.c_str());

            // naming the thread.
            if (FAILED(SetThreadDescription(GetCurrentThread(), wname.c_str())))
            {
                Log(L"Failed to set the thread name.");
                return;
            }

            // Create message pump for this thread.
            {
                MSG msg;
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
            }

            HWND hWnd{};
            PeekMessageContainer peekMsgContainer;
            // Guard scope to make sure to destroy the Window.
            {
                // Make sure DestroyWindow() is called when exit from this scope.
                ScopeGuard wndGuard([&hWnd] { if (hWnd != 0) DestroyWindow(hWnd); });

                // Register PeekMessageCallback to clear hWnd when WM_DESTROY received.
                peekMsgContainer.Register([&hWnd](HWND h, UINT m, WPARAM w, LPARAM l) {
                    if (m == WM_DESTROY)
                        hWnd = 0;
                    });

                // This is a pointer, but it points a member's instance.
                D3DContext_ImGuiBase* d3dctx = GetD3DContext_ImGuiBase();

                // Set the app and associated output.
                d3dctx->SetApp(inApp, listIdx);

                // Register PeekMessageCallbacks to peek messages in d3dctx.
                peekMsgContainer.Register([d3dctx](HWND h, UINT m, WPARAM w, LPARAM l) {
                    d3dctx->PeekWindowMessage(h, m, w, l);
                    });

                hWnd = CreateWindowExW(0, WindowClassName().data(), wname.c_str(),
                    WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInst, (void*)&peekMsgContainer);
                if (!hWnd)
                {
                    Log(L"Failed to create a window.");
                    return;
                }

                // Style and StyleEx
                std::tuple<LONG_PTR, LONG_PTR> defaultWindowStyle{ GetWindowLongPtrW(hWnd, GWL_STYLE), GetWindowLongPtrW(hWnd, GWL_EXSTYLE) };

                if (!d3dctx->CreateDeviceResources()) {
                    Log(L"Failed to initialize D3D device.");
                    return;
                }

                if (withImGui) {
                    if (!d3dctx->Init_ImGui(hWnd)) {
                        Log(L"Failed to initialize ImGUI.");
                        return;
                    }
                }

                if (!d3dctx->ShowWindowOnTheAssociatedOutput(hWnd)) {
                    Log(L"Failed to show window.");
                    return;
                }

                // Change the thread state. Unblocking the caller thread.
                thdState.store(ThreadState::Running);

                // Present thread.
                struct PresentThreadContext final {
                    std::binary_semaphore       startSemaphore{ 0 };
                    std::binary_semaphore       finishSemaphore{ 0 };
                    std::atomic<bool>           exitReq{ false };
                    std::atomic<bool>           sts{ true };
                    bool                        busy{ false };
                    std::thread                 thd;
                    std::chrono::high_resolution_clock::time_point lastPresent{};

                    void WMClose()
                    {
                        if (!thd.joinable())
                            return;

                        // Try to join the render thread before closing the window.
                        exitReq.store(true);
                        startSemaphore.release();
                        if (finishSemaphore.try_acquire_for(std::chrono::seconds(3))) {
                            thd.join();
                            if (!sts) {
                                Log(L"Present thread returned false after receiving WM_CLOSE\n");
                            }
                        }
                        else {
                            // Present thread timeout. Present blocked?
                            Log(L"Present thread blocked for 3 seconds after receiving WM_CLOSE. Aborting anyway.\n");
                            thd.detach(); // leave the thread as is. 
                        }
                    }

                    void CheckFinishStatus()
                    {
                        if (!busy)
                            return;
                        if (!thd.joinable())
                            return;

                        if (!finishSemaphore.try_acquire())
                            return;

                        // update the last present time.
                        lastPresent = std::chrono::high_resolution_clock::now();
                        busy = false;
                    }
                } presentCtx;
                presentCtx.thd = std::thread([&]() {
                    SetThreadDescription(GetCurrentThread(), L"Present Thread");
                    for (;;) {
                        presentCtx.startSemaphore.acquire();
                        if (presentCtx.exitReq.load()) {
                            presentCtx.finishSemaphore.release();
                            break;
                        }
                        d3dctx->Present(hWnd, presentCtx.sts);
                        presentCtx.finishSemaphore.release();
                        if (presentCtx.exitReq.load()) {
                            break;
                        }
                    }
                    });
                // Register peek message callback to join the Present thread when receiving WM_CLOSE message.
                peekMsgContainer.Register([&presentCtx](HWND h, UINT m, WPARAM w, LPARAM l) {
                    if (m == WM_CLOSE) {
                        presentCtx.WMClose();
                    }
                    });

                // Main Loop for the Window.
                for (;;) {
                    MSG msg;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        if (msg.message == WM_QUIT)
                            break;
                        // App side PeekMessage should be done in the WndProc function.
                        // Windows processes multiple message at once in DispatchMessage so that here is not a good place to peek all messages.
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (msg.message == WM_QUIT)
                        break;

                    // Try to join the present thread here to catch up the latest status.
                    presentCtx.CheckFinishStatus();

                    // Wait for 1ms and keep processing the Windows message loop while the Present thread is busy.
                    if (presentCtx.busy) {
                        Sleep(1);
                        continue;
                    }

                    if (!presentCtx.sts) {
                        Log(L"Present thread returned with an error.\n");
                        return;
                    }

                    // Window mode transition happens in the message loop thread while the Present thread is joined.
                    {
                        auto sts = d3dctx->WindowModeTransition(hWnd, defaultWindowStyle);
                        if (sts == D3DContext_Base::WindowModeTransitionStatus::error)
                        {
                            Log(L"Window mode transition failed.\n");
                            return;
                        }
                        if (sts == D3DContext_Base::WindowModeTransitionStatus::inProgress) {
                            // Need to get back to the windows message pump once.
                            continue;
                        }
                    }

                    // Check the duration from the last present.
                    {
                        float targetDurationMs{};
                        {
                            std::scoped_lock<std::mutex> l{ inApp->mtx };
                            targetDurationMs = inApp->ctx.displays.at(listIdx).threadWaitMs;
                        }
                        std::chrono::duration<float, std::milli> elapsedMs = std::chrono::high_resolution_clock::now() - presentCtx.lastPresent;

                        // Elapsed time is not reached to the target duration.
                        if (elapsedMs.count() < targetDurationMs)
                            continue;
                    }

                    // Invoking Present here.
                    presentCtx.busy = true;
                    presentCtx.startSemaphore.release();
                }
                // End of the main loop for the window.

                if (presentCtx.thd.joinable()) {
                    Log(L"Failed to??>???.");
                    return;
                }

                if (withImGui) {
                    if (!d3dctx->Terminate_ImGui()) {
                        Log(L"Failed to terminate ImGui.");
                        return;
                    }
                }
                if (!d3dctx->Terminate()) {
                    Log(L"Failed to terminate D3D device.");
                    return;
                }

                Log(L"Thread:%s - Join\n", wname.c_str());
            } // ScopeGuard.

        });

        // Wait untile the worker thread's window has been established.
        while (thdState.load() == ThreadState::Initializing) {
            Sleep(1);
        }

        return true;
    }

    void WaitForFinished()
    {
        thd.join();
    }

    bool Joinable()
    {
        return thdState == ThreadState::Terminated;
    }
};

class ControlWindow final : public Window_Base
{
protected:
    virtual const std::wstring_view& WindowClassName() override
    {
        static constexpr std::wstring_view s{ L"ControlWindowClass" };
        return s;
    };
    virtual ATOM& WindowClass() override
    {
        static ATOM atm{};
        return atm;
    };

    class D3DContext final : public D3DContext_ImGuiBase
    {
        uint32_t    logIdx{};

        virtual void Render(HWND hWnd, ComPtr<ID3D12GraphicsCommandList>& cl) override
        {
            if (imInitialized) {
                // Start the Dear ImGui frame
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                ImGuiIO& io = ImGui::GetIO(); (void)io;

                {
                    std::scoped_lock<std::mutex> l{ app->mtx };

                    ImGui::Begin("Adapter - Monitor List!");

                    for (auto& a : app->ctx.displays) {
                        ImGui::Checkbox(a.description.c_str(), &a.selected);
                    }

                    // Go fullscreen and try.
                    if (ImGui::Button("Test")) {
                        app->ctx.mode = App::Context::Mode::test;
                    }
                    if (ImGui::Button("Exit")) {
                        app->ctx.mode = App::Context::Mode::exit;
                    }
                    
                    ImGui_AddLogText(logIdx);

                    ImGui::End();
                }

                // Rendering
                ImGui::Render();
                auto descHeaps{ imDescHeap.Get() };
                cl->SetDescriptorHeaps(1, &descHeaps);

                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl.Get());
            }

            {
                std::scoped_lock<std::mutex> l{ app->mtx };
                
                if (app->ctx.mode != App::Context::Mode::control) {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
            }
        }
    } d3dCtx;

    virtual D3DContext_ImGuiBase* GetD3DContext_ImGuiBase() override
    {
        return &d3dCtx;
    };
};

class TestWindow final : public Window_Base
{
protected:
    virtual const std::wstring_view& WindowClassName() override
    {
        static constexpr std::wstring_view s{ L"TestWindowClass" };
        return s;
    };
    virtual ATOM& WindowClass() override
    {
        static ATOM atm{};
        return atm;
    };

    class D3DContext final : public D3DContext_ImGuiBase
    {
        uint32_t logIdx{};

        virtual void Render(HWND hWnd, ComPtr<ID3D12GraphicsCommandList>& cl) override
        {
#ifdef NVAPI_ENABLED
            // for all windows - check PB status and update.
            if (nvapi_PresentBarrierClientHandleCreated) {
                std::scoped_lock<std::mutex> l{ app->mtx };
                auto& display = app->ctx.displays.at(appListIdx);
                auto& sts = display.nvapi_PBStats;
                sts = { NV_PRESENT_BARRIER_FRAME_STATICS_VER1 , };
                if (NvAPI_QueryPresentBarrierFrameStatistics(nvapi_PresentBarrierClientHandle, &sts) != NVAPI_OK) {
                    Log("Failed to destroy Present Barrier Client.\n");
                    sts = { NV_PRESENT_BARRIER_FRAME_STATICS_VER1 , };
                }

                if (display.nvapi_PresentBarrierMode == PresentBarrierMode::join && sts.SyncMode == PRESENT_BARRIER_NOT_JOINED) {
                    NV_JOIN_PRESENT_BARRIER_PARAMS params{ NV_JOIN_PRESENT_BARRIER_PARAMS_VER1 , };
                    Log("Calling JoinPresentBarrier.\n");
                    if (NvAPI_JoinPresentBarrier(nvapi_PresentBarrierClientHandle, &params) != NVAPI_OK) {
                        Log("Failed to call JoinPresentBarrier.\n");
                    }
                }
                if (display.nvapi_PresentBarrierMode == PresentBarrierMode::leave && sts.SyncMode != PRESENT_BARRIER_NOT_JOINED) {
                    Log("Calling LeavePresentBarrier.\n");
                    if (NvAPI_LeavePresentBarrier(nvapi_PresentBarrierClientHandle) != NVAPI_OK) {
                        Log("Failed to call LeavePresentBarrier.\n");
                    }
                }
            }
#endif

            // display line.
            {
                if (!shaderAssets) {
                    shaderAssets = std::make_unique<ShaderAssets>();
                    bool sts = shaderAssets->Init(dev);
                    assert(sts);
                }

                auto [ptr, gpuPtr, size] = shaderAssets->GetUploadChunk();
                struct Vertex
                {
                    std::array<float, 4> pos;
                    std::array<float, 4> col;
                };
                std::span<Vertex, 6>  vb(reinterpret_cast<Vertex*>(ptr), 6);
                assert(size > vb.size_bytes());

                constexpr float lineWidth{ 0.05f };
                float linePos{};
                {
                    std::scoped_lock<std::mutex> l{ app->mtx };
                    linePos = 1.0f - float(app->ctx.globalCounter % 256) / 128.f;
                }

                std::array<float, 4> col{ 0.f, 1.f, 1.f, 1.f };
                std::array<float, 4> pos1{ -1.0f, linePos - lineWidth, 0.5f, 1.0f };
                std::array<float, 4> pos2{ 1.0f, linePos - lineWidth, 0.5f, 1.0f };
                std::array<float, 4> pos3{ 1.0f, linePos + lineWidth, 0.5f, 1.0f };
                std::array<float, 4> pos4{ -1.0f, linePos + lineWidth, 0.5f, 1.0f };
                vb[0] = { pos1, col };
                vb[1] = { pos2, col };
                vb[2] = { pos3, col };

                vb[3] = { pos3, col };
                vb[4] = { pos4, col };
                vb[5] = { pos1, col };

                D3D12_VERTEX_BUFFER_VIEW vView{ gpuPtr, (UINT)vb.size_bytes(), sizeof(Vertex) };
                cl->SetGraphicsRootSignature(shaderAssets->rootSig.Get());
                cl->SetPipelineState(shaderAssets->pso.Get());
                cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cl->IASetVertexBuffers(0, 1, &vView);
                cl->DrawInstanced((UINT)vb.size(), 1, 0, 0);
            }

            // Display UIs - primary window only.
            if (imInitialized) {
                // Start the Dear ImGui frame
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                {
                    std::scoped_lock<std::mutex> l{ app->mtx };

                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Once);
                    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_Once);

                    ImGui::Begin("Window Mode");

                    ImGui::Text("Global Counter: %d", app->ctx.globalCounter);

                    uint32_t idx{};
                    for (auto& d : app->ctx.displays) {
                        if (!d.selected)
                            continue;

                        ImGui::PushID(idx++);
                        ImGui::Text(d.description.c_str());

#ifdef NVAPI_ENABLED
                        {
                            std::string pbDesc;
                            auto& sts(d.nvapi_PBStats);
                            pbDesc += ToStr("PBSupported: %s, ", nvapi_PresentBarrierIsSupported ? "Yes" : "No ");
                            pbDesc += ToStr("PBHandle: %s, ", nvapi_PresentBarrierClientHandleCreated ? "Created" : "None   ");
                            pbDesc += ToStr("SyncMode: %s, ", [](const NV_PRESENT_BARRIER_SYNC_MODE& m) -> const char* {
                                switch (m) {
                                case PRESENT_BARRIER_NOT_JOINED:
                                    return "NOT_JOINED  ";
                                case PRESENT_BARRIER_SYNC_CLIENT:
                                    return "SYNC_CLIENT ";
                                case PRESENT_BARRIER_SYNC_SYSTEM:
                                    return "SYNC_SYSTEM ";
                                case PRESENT_BARRIER_SYNC_CLUSTER:
                                    return "SYNC_CLUSTER";
                                }
                                return "";
                                }(sts.SyncMode));
                            pbDesc += ToStr("PresentCount: %d, FlipSyncCount: %d, RefreshCount: %d",
                                sts.PresentCount, sts.PresentInSyncCount, sts.FlipInSyncCount, sts.RefreshCount);
                            ImGui::Text(pbDesc.c_str());
                        }
#endif

                        if (ImGui::Button("Fulscreen")) {
                            d.windowMode = WindowMode::fullSceen;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Borderless Windowed")) {
                            d.windowMode = WindowMode::borderlessWindowed;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Windowed")) {
                            d.windowMode = WindowMode::windowed;
                        }

                        ImGui::SliderFloat("Thread Wait(ms)", &d.threadWaitMs, 0.0f, 1000.0f);

#ifdef NVAPI_ENABLED
                        if (ImGui::Button("Join PresentBarrier")) {
                            d.nvapi_PresentBarrierMode = PresentBarrierMode::join;
                            if (!nvapi_PresentBarrierIsSupported) {
                                Log("PresentBarrier is not supported on this device.\n");
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Leave PresentBarrier")) {
                            d.nvapi_PresentBarrierMode = PresentBarrierMode::leave;
                            if (!nvapi_PresentBarrierIsSupported) {
                                Log("PresentBarrier is not supported on this device.\n");
                            }
                        }
#endif
                        ImGui::PopID();
                    }

                    if (ImGui::Button("Exit")) {
                        app->ctx.mode = App::Context::Mode::exit;
                    }

                    ImGui_AddLogText(logIdx);

                    ImGui::End();
                }

                // Rendering
                ImGui::Render();
                auto descHeaps{ imDescHeap.Get() };
                cl->SetDescriptorHeaps(1, &descHeaps);

                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl.Get());
            }

            // read app's states - for all windows.
            {
                App::Context::Mode  appMode{};
                WindowMode          wMode{};
                {
                    std::scoped_lock<std::mutex> l{ app->mtx };

                    if (internalWindowModeChange) {
                        // Window mode change event happened.
                        app->ctx.displays.at(appListIdx).windowMode = requestedWindowMode;
                        internalWindowModeChange = false;
                    }
                    appMode = app->ctx.mode;
                    wMode = app->ctx.displays.at(appListIdx).windowMode;
                }

                if (requestedWindowMode == currentWindowMode) {
                    // Window mode state transition has been completed now so that it can accept the request.
                    requestedWindowMode = wMode;
                }

                if (appMode != App::Context::Mode::test) {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
            }
        }
    } d3dCtx;

    virtual D3DContext_ImGuiBase* GetD3DContext_ImGuiBase() override
    {
        return &d3dCtx;
    };
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

    auto app = std::make_shared<App>();
    if (!app->Init(hInstance)) {
        Log(L"Failed to init the application.");
        return 1;
    }

    // Display list.
    for (size_t aIdx = 0; aIdx < app->adapters.size(); ++aIdx) {
        const auto& adapter{ app->adapters[aIdx] };
        const auto& adapterDesc{ adapter->desc };

        for (size_t mIdx = 0; mIdx < adapter->outputs.size(); ++mIdx) {
            const auto& output{ adapter->outputs[mIdx] };

            auto desc = ToStr("GPU:%s - Monitor:%s [%d x %d][%d / %d]", ToUTF8(adapterDesc.Description).c_str(), ToUTF8(output.desc.DeviceName).c_str(),
                output.currentModeDesc.Width, output.currentModeDesc.Height, output.currentModeDesc.RefreshRate.Denominator, output.currentModeDesc.RefreshRate.Numerator);

            app->ctx.displays.push_back({ false, WindowMode::windowed, (uint32_t)aIdx, (uint32_t)mIdx, desc });
        }
    }

    while (app->ctx.mode != App::Context::Mode::exit) {
        if (app->ctx.mode == App::Context::Mode::control) {
            // open control window;
            ControlWindow   w;
            if (! w.Init(hInstance, app, 0, true)) {
                Log(L"Failed to init a control window.");
                app->ctx.mode = App::Context::Mode::exit;
                break;
            };
            w.WaitForFinished();
        }
        if (app->ctx.mode == App::Context::Mode::test) {
            // open test windows
            std::vector<std::unique_ptr<TestWindow>> windows;
            uint32_t windowIdx{0};
            uint32_t listIdx{(uint32_t) -1};
            bool withImGui{ true };
            for (auto& d : app->ctx.displays) {
                ++listIdx;

                if (! d.selected)
                    continue;

                auto w = std::make_unique<TestWindow>();
                if (!w->Init(hInstance, app, listIdx, withImGui)) {
                    Log(L"Failed to init a test window.");
                    app->ctx.mode = App::Context::Mode::exit;
                    break;
                };
                windows.push_back(std::move(w));
                withImGui = false;
            }
            for (;;) {
                bool allJoinable{ true };
                for (auto& w : windows) {
                    allJoinable &= w->Joinable();
                }
                if (allJoinable)
                    break;
                {
                    std::scoped_lock<std::mutex> l{ app->mtx };
                    app->ctx.globalCounter++;
                }
                Sleep(5);
            }
            for (auto& w : windows) {
                w->WaitForFinished();
            }
        }
        {
            std::scoped_lock<std::mutex> l{ app->mtx };
            app->ctx.mode = App::Context::Mode::exit;
        }
    }

    app->Terminate();

    return 0;
}
