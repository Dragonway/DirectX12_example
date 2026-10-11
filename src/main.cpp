#include <shellapi.h>
#include <chrono>

#include "error.h"

using Microsoft::WRL::ComPtr;

constexpr auto NUM_FRAMES = 3;

uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

bool g_useWarp = false;
bool g_isInitialized = false;

// window
HWND g_hWnd;
RECT g_WindowRect;
bool g_Fullscreen = false;

// dxd12
ComPtr<ID3D12Device2> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain4> g_SwapChain;
ComPtr<ID3D12Resource> g_BackBuffers[NUM_FRAMES];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12CommandAllocator> g_CommandAllocator[NUM_FRAMES];
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;
UINT g_RTVDescriptorSize;
INT g_CurrentBackBufferIndex;

bool g_VSync = true;
bool g_TearingSupported = false;

// sync
ComPtr<ID3D12Fence> g_Fence;
uint64_t g_FenceValue = 0;
uint64_t g_FrameFenceValues[NUM_FRAMES];
HANDLE g_FenceEvent;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void ParseCmdArgs()
{
    int argc;
    wchar_t **argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    for (auto i = 0; i < argc; ++i)
    {
        if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
            g_ClientWidth = ::wcstol(argv[++i], nullptr, 10);

        if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
            g_ClientHeight = ::wcstol(argv[++i], nullptr, 10);

        if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
            g_useWarp = true;
    }

    ::LocalFree(argv);
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
    // always enable the debug layer before doing anything DX12 related
    // so all possible errors generated while creating DX12 objects are caught by the debug layer
    ComPtr<ID3D12Debug> debugInterface;
    WIN_CALL(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
    debugInterface->EnableDebugLayer();
#endif
}

void RegisterWindowClass(HINSTANCE hInst, const wchar_t *windowClassName)
{
    // register a window class for creating out render window with
    WNDCLASSEXW windowClass = {};

    windowClass.cbSize = sizeof(decltype(windowClass));
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WndProc;
    windowClass.cbClsExtra = 0;
    windowClass.cbWndExtra = 0;
    windowClass.hInstance = hInst;
    windowClass.hIcon = ::LoadIcon(hInst, nullptr);
    windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = nullptr;
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = ::LoadIcon(hInst, nullptr);

    static ATOM atom = ::RegisterClassExW(&windowClass);
    SANITY_CHECK(atom > 0);
}

HWND CreateWindow(const wchar_t *windowClassName, HINSTANCE hInst, const wchar_t *windowTitle, uint32_t width, uint32_t height)
{
    int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

    RECT windowRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    ::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    int windowX = std::max(0, (screenWidth - windowWidth));
    int windowY = std::max(0, (screenHeight - windowHeight));

    HWND hWnd = ::CreateWindowExW(
        NULL,
        windowClassName,
        windowTitle,
        WS_OVERLAPPEDWINDOW,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        NULL,
        NULL,
        hInst,
        nullptr);

    ASSERT(hWnd, "Failed to create window");

    return hWnd;
}

ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
    ComPtr<IDXGIFactory4> dxgiFactory;
    UINT createFactoryFlags = 0;
#if defined(_DEBUG)
    createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

    WIN_CALL(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

    ComPtr<IDXGIAdapter1> dxgiAdapter1;
    ComPtr<IDXGIAdapter4> dxgiAdapter4;

    if (useWarp)
    {
        WIN_CALL(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
        WIN_CALL(dxgiAdapter1.As(&dxgiAdapter4));
    }
    else
    {
        SIZE_T maxDedicatedVideoMemory = 0;
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
            dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

            // check to see if the adapter can create a D3D12 device without actually creating it
            // the adapter with the largest dedicated video memory is favored
            if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                continue;

            if (SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
                dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
            {
                maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
                WIN_CALL(dxgiAdapter1.As(&dxgiAdapter4));
            }
        }
    }

    return dxgiAdapter4;
}

ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
    ComPtr<ID3D12Device2> d3d12Device2;
    WIN_CALL(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)));

#if defined(_DEBUG)
    // enable debug messages
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(d3d12Device2.As(&infoQueue)))
    {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

        // suppress while category of messages
        // D3D12_MESSAGE_CATEGORY categories[] = {};

        // suppress messages based on their severity level
        D3D12_MESSAGE_SEVERITY severities[] = {
            D3D12_MESSAGE_SEVERITY_INFO,
        };

        // suppress individual messages by their id
        D3D12_MESSAGE_ID ids[] = {
            // warning occurs when a render target is cleared using a clear color that is not the optimized clear color specified during resource creation
            // if you want to clear a render target using an arbitrary clear color, you should disable this warning
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE, // don't know how to avoid this message

            // warnings occur when a frame is captured using the graphics debugger integrated in Visual Studio
            // since I think this bug will never be fixed in the debugger, it’s best to just ignore this warnings
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,   // occurs when using capture frame while graphics debugging
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE, // occurs when using capture frame while graphics debugging
        };

        D3D12_INFO_QUEUE_FILTER filter = {};
        // filter.DenyList.NumCategories = _countof(categories);
        // filter.DenyList.pCategoryList = categories;
        filter.DenyList.NumSeverities = _countof(severities);
        filter.DenyList.pSeverityList = severities;
        filter.DenyList.NumIDs = _countof(ids);
        filter.DenyList.pIDList = ids;

        WIN_CALL(infoQueue->PushStorageFilter(&filter));
    }
#endif

    return d3d12Device2;
}

ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
    ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    WIN_CALL(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

    return d3d12CommandQueue;
}

bool CheckTearingSupport()
{
    BOOL allowTearing = FALSE;

    // rather than create the DXGI 1.5 factory interface directly, we create DXGI 1.4 interface and query for the 1.5 interface
    // this is to enable the graphics debugging tools which will not support the 1.5 factory interface until a future update
    // (they probably support now)
    ComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory4.As(&factory5)))
        {
            if (FAILED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &allowTearing, sizeof(allowTearing))))
            {
                allowTearing = FALSE;
            }
        }
    }

    return allowTearing == TRUE;
}

ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd, ComPtr<ID3D12CommandQueue> commandQueue, uint32_t width, uint32_t height, uint32_t bufferCount)
{
    ComPtr<IDXGISwapChain4> dxgiSwapChain4;
    ComPtr<IDXGIFactory4> dxgiFactory4;
    UINT createFactoryFlags = 0;
#if defined(_DEBUG)
    createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

    WIN_CALL(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc = {1, 0};
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = bufferCount;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    WIN_CALL(dxgiFactory4->CreateSwapChainForHwnd(
        commandQueue.Get(),
        hWnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1));

    // disable the Alt+Enter fullscreen toggle feature
    // switching to fullscreen will be handled manually
    WIN_CALL(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

    WIN_CALL(swapChain1.As(&dxgiSwapChain4));
    return dxgiSwapChain4;
}

ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ComPtr<ID3D12Device2> device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
{
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;

    WIN_CALL(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

    return descriptorHeap;
}

void UpdateRenderTargetViews(ComPtr<ID3D12Device2> device, ComPtr<IDXGISwapChain4> swapChain, ComPtr<ID3D12DescriptorHeap> descriptorHeap)
{
    auto rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle{descriptorHeap->GetCPUDescriptorHandleForHeapStart()};

    for (int i = 0; i < NUM_FRAMES; ++i, rtvHandle.Offset(rtvDescriptorSize))
    {
        ComPtr<ID3D12Resource> backBuffer;
        WIN_CALL(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

        g_BackBuffers[i] = backBuffer;
    }
}

ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    WIN_CALL(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

    return commandAllocator;
}

ComPtr<ID3D12GraphicsCommandList> CreateCommandList(ComPtr<ID3D12Device2> device, ComPtr<ID3D12CommandAllocator> commandAllocator, D3D12_COMMAND_LIST_TYPE type)
{
    ComPtr<ID3D12GraphicsCommandList> commandList;
    WIN_CALL(device->CreateCommandList(0, type, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));
    WIN_CALL(commandList->Close());

    return commandList;
}

ComPtr<ID3D12Fence> CreateFence(ComPtr<ID3D12Device2> device)
{
    ComPtr<ID3D12Fence> fence;
    WIN_CALL(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

    return fence;
}

HANDLE CreateEventHandle()
{
    HANDLE fenceEvent;

    fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    ASSERT(fenceEvent, "Failed to create fence event");

    return fenceEvent;
}

uint64_t Signal(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence, uint64_t &fenceValue)
{
    uint64_t fenceValueForSignal = ++fenceValue;
    WIN_CALL(commandQueue->Signal(fence.Get(), fenceValueForSignal));

    return fenceValueForSignal;
}

void WaitForFenceValue(ComPtr<ID3D12Fence> fence, uint64_t fenceValue, HANDLE fenceEvent, std::chrono::milliseconds duration = std::chrono::milliseconds::max())
{
    if (fence->GetCompletedValue() < fenceValue)
    {
        WIN_CALL(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        ::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
    }
}

void Flush(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence, uint64_t &fenceValue, HANDLE fenceEvent)
{
    uint64_t fenceValueForSignal = Signal(commandQueue, fence, fenceValue);
    WaitForFenceValue(fence, fenceValueForSignal, fenceEvent);
}

void Update()
{
    static uint64_t frameCounter = 0;
    static double elapsedSec = 0.0;
    static std::chrono::high_resolution_clock clock;
    static auto t0 = clock.now();

    ++frameCounter;
    auto t1 = clock.now();
    auto deltaTime = t1 - t0;
    t0 = t1;

    elapsedSec += deltaTime.count() * 1e-9;

    if (elapsedSec > 1.0)
    {
        char buffer[500];
        auto fps = frameCounter / elapsedSec;
        std::snprintf(buffer, 500, "FPS: %f\n", fps);
        OutputDebugStringA(buffer);

        frameCounter = 0;
        elapsedSec = 0.0;
    }
}

void Render()
{
    auto commandAllocator = g_CommandAllocator[g_CurrentBackBufferIndex];
    auto backBuffer = g_BackBuffers[g_CurrentBackBufferIndex];

    commandAllocator->Reset();
    g_CommandList->Reset(commandAllocator.Get(), nullptr);

    // clear the render target
    {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        g_CommandList->ResourceBarrier(1, &barrier);

        FLOAT clearColor[] = {0.4f, 0.6f, 0.9f, 1.0f};
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv{g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), g_CurrentBackBufferIndex, g_RTVDescriptorSize};

        g_CommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    }

    // present
    {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);

        g_CommandList->ResourceBarrier(1, &barrier);

        WIN_CALL(g_CommandList->Close());

        ID3D12CommandList *const commandLists[] = {
            g_CommandList.Get()};
        g_CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

        UINT syncInterval = g_VSync ? 1 : 0;
        UINT presentFlags = g_TearingSupported && !g_VSync ? DXGI_PRESENT_ALLOW_TEARING : 0;
        WIN_CALL(g_SwapChain->Present(syncInterval, presentFlags));

        g_FrameFenceValues[g_CurrentBackBufferIndex] = Signal(g_CommandQueue, g_Fence, g_FenceValue);

        g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

        WaitForFenceValue(g_Fence, g_FrameFenceValues[g_CurrentBackBufferIndex], g_FenceEvent);
    }
}

void Resize(uint32_t width, uint32_t height)
{
    if (g_ClientWidth != width || g_ClientHeight != height)
    {
        // don't allow 0 size swap chain back buffers
        g_ClientWidth = std::max(1u, width);
        g_ClientHeight = std::max(1u, height);

        // flush the GPU queue to make sure the swap chain's back buffers are not being referenced by an in-flight command list
        Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

        for (int i = 0; i < NUM_FRAMES; ++i)
        {
            // any references to the back buffers must be released before the swap chain can be resized
            g_BackBuffers[i].Reset();
            g_FrameFenceValues[i] = g_FrameFenceValues[g_CurrentBackBufferIndex];
        }

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        WIN_CALL(g_SwapChain->GetDesc(&swapChainDesc));
        WIN_CALL(g_SwapChain->ResizeBuffers(
            NUM_FRAMES,
            g_ClientWidth,
            g_ClientHeight,
            swapChainDesc.BufferDesc.Format,
            swapChainDesc.Flags));

        g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

        UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);
    }
}

void SetFullscreen(bool fullscreen)
{
    if (g_Fullscreen != fullscreen)
    {
        g_Fullscreen = fullscreen;

        if (g_Fullscreen)
        {
            // store the current window dimensions so they can be restored when switching out of fullscreen state
            ::GetWindowRect(g_hWnd, &g_WindowRect);

            // set the window style to a borderless window so the client area fills the entire screen
            UINT windowStyle = WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

            ::SetWindowLongW(g_hWnd, GWL_STYLE, windowStyle);

            // query the name of the nearest display device for the window
            // this is required to set the fullscreen dimensions of the window when using a multi-monitor setup
            HMONITOR hMonitor = ::MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEX monitorInfo = {};
            monitorInfo.cbSize = sizeof(decltype(monitorInfo));
            ::GetMonitorInfo(hMonitor, &monitorInfo);

            ::SetWindowPos(
                g_hWnd,
                HWND_TOP,
                monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
            ::ShowWindow(g_hWnd, SW_MAXIMIZE);
        }
        else
        {
            // restore all the window decorators
            ::SetWindowLong(g_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
            ::SetWindowPos(
                g_hWnd,
                HWND_NOTOPMOST,
                g_WindowRect.left,
                g_WindowRect.top,
                g_WindowRect.right - g_WindowRect.left,
                g_WindowRect.bottom - g_WindowRect.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
            ::ShowWindow(g_hWnd, SW_NORMAL);
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_isInitialized)
    {
        switch (message)
        {
        case WM_PAINT:
            Update();
            Render();
            break;

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
        {
            bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            switch (wParam)
            {
            case 'V':
                g_VSync = !g_VSync;
                break;
            case VK_ESCAPE:
                ::PostQuitMessage(0);
                break;
            case VK_RETURN:
                if (alt)
                {
                case VK_F11:
                    SetFullscreen(!g_Fullscreen);
                }
                break;
            }
        }
        break;

        // the default window procedure will play a system notification sound
        // when pressing the Alt+Enter keyboard combination if this message is not handled
        case WM_SYSCHAR:
            break;

        case WM_SIZE:
        {
            RECT clientRect = {};
            ::GetClientRect(g_hWnd, &clientRect);

            int width = clientRect.right - clientRect.left;
            int height = clientRect.bottom - clientRect.top;

            Resize(width, height);
        }
        break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            break;

        default:
            return ::DefWindowProcW(hWnd, message, wParam, lParam);
        }
    }
    else
    {
        return ::DefWindowProcW(hWnd, message, wParam, lParam);
    }

    return 0;
}

int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    // windows 10 creators update adds per monitor v2 DPI awareness context
    // using this awareness context allows the client area of the window to achieve 100% scaling
    // while still allowing non-client window content to be rendered in a DPI sensitive fashion
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // window class name, used for registering / creatin the window
    const wchar_t *windowClassName = L"DXD12WindowClass";
    ParseCmdArgs();

    EnableDebugLayer();

    g_TearingSupported = CheckTearingSupport();

    RegisterWindowClass(hInstance, windowClassName);
    g_hWnd = CreateWindow(windowClassName, hInstance, L"DirectX 12 Example", g_ClientWidth, g_ClientHeight);

    // initialize the global window rect variable
    ::GetWindowRect(g_hWnd, &g_WindowRect);

    ComPtr<IDXGIAdapter4> dxgiAdapter4 = GetAdapter(g_useWarp);

    g_Device = CreateDevice(dxgiAdapter4);
    g_CommandQueue = CreateCommandQueue(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    g_SwapChain = CreateSwapChain(g_hWnd, g_CommandQueue, g_ClientWidth, g_ClientHeight, NUM_FRAMES);
    g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();
    g_RTVDescriptorHeap = CreateDescriptorHeap(g_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NUM_FRAMES);
    g_RTVDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);

    for (int i = 0; i < NUM_FRAMES; ++i)
        g_CommandAllocator[i] = CreateCommandAllocator(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);

    g_CommandList = CreateCommandList(g_Device, g_CommandAllocator[g_CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

    g_Fence = CreateFence(g_Device);
    g_FenceEvent = CreateEventHandle();

    g_isInitialized = true;

    ::ShowWindow(g_hWnd, SW_SHOW);

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
    }

    // make sure the command queue has finished all commands before closing
    Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

    ::CloseHandle(g_FenceEvent);

    return 0;
}
